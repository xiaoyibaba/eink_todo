// 电脑上的模拟器。跑的是和真机同一份 core 代码，只是把硬件换成：
//   屏幕 → SDL 窗口（按 UC8179 语义模拟局刷/全刷，见 EpdEmu）
//   按键 → 键盘 ↑ ↓ 回车
//   flash → /state/ 映射到 firmware/.sim/state/，其余路径只读映射到 data/（字库、网页）
//   WiFi → 本机 HTTP 服务，同一局域网的手机可以直接扫屏幕上的二维码
//   深睡 → 销毁 App 对象（模拟内存全丢），按键或到点再新建
//
// 交互：  ./program                      （在 firmware/ 目录下运行）
// 脚本：  ./program --script s.txt --out shots/   （无窗口、虚拟时间，用于自动验证）
#include <SDL.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "App.h"
#include "DateUtil.h"
#include "EpdEmu.h"
#include "Font.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"
#include "vendor/httplib.h"

using namespace et;

namespace {

std::string g_dataDir = "data", g_stateDir = ".sim/state";

void mkdirs(const std::string& p) {
  std::string cur;
  std::stringstream ss(p);
  std::string part;
  if (!p.empty() && p[0] == '/') cur = "/";
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    cur += part + "/";
    mkdir(cur.c_str(), 0755);
  }
}

// ------------------------------------------------------------------ 文件系统

class DiskFile : public File {
 public:
  explicit DiskFile(const std::string& p) : f_(p, std::ios::binary) {
    f_.seekg(0, std::ios::end);
    size_ = static_cast<size_t>(f_.tellg());
  }
  bool ok() const { return static_cast<bool>(f_); }
  size_t size() override { return size_; }
  size_t readAt(uint32_t off, void* buf, size_t n) override {
    f_.clear();
    f_.seekg(off);
    f_.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f_.gcount());
  }

 private:
  std::ifstream f_;
  size_t size_ = 0;
};

class SimFs : public Fs {
 public:
  std::string real(const std::string& p) const {
    if (p.rfind("/state/", 0) == 0) return g_stateDir + p.substr(6);
    return g_dataDir + p;
  }
  std::unique_ptr<File> open(const std::string& p) override {
    auto f = std::make_unique<DiskFile>(real(p));
    if (!f->ok()) return nullptr;
    return f;
  }
  bool readAll(const std::string& p, std::string& out) override {
    std::ifstream f(real(p), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
  }
  bool writeAtomic(const std::string& p, const std::string& d) override {
    if (p.rfind("/state/", 0) != 0) return false;
    std::string dst = real(p), tmp = dst + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      f.write(d.data(), static_cast<std::streamsize>(d.size()));
    }
    return std::rename(tmp.c_str(), dst.c_str()) == 0;
  }
  bool remove(const std::string& p) override { return std::remove(real(p).c_str()) == 0; }
};

// ------------------------------------------------------------------ 屏幕

class SimEpd : public EpdEmu {
 public:
  bool verbose = true;
  uint32_t lastRefreshTick = 0;
  void refreshFull() override {
    EpdEmu::refreshFull();
    if (verbose) printf("[屏幕] 全刷\n");
    lastRefreshTick = SDL_GetTicks();
  }
  void refreshPartial(Rect r) override {
    EpdEmu::refreshPartial(r);
    if (verbose) printf("[屏幕] 局刷 x=%d y=%d w=%d h=%d\n", r.x, r.y, r.w, r.h);
    lastRefreshTick = SDL_GetTicks();
  }
};

// ------------------------------------------------------------------ 平台

std::string lanIp() {
  std::string best = "127.0.0.1";
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return best;
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
    best = buf;
    if (strcmp(i->ifa_name, "en0") == 0) break;
  }
  freeifaddrs(ifs);
  return best;
}

class SimPlatform : public Platform {
 public:
  // 时间：脚本模式用虚拟时钟，交互模式用真实时钟；两者都可以整体拨快
  bool scripted = false;
  uint32_t virtMs = 0;
  int64_t virtUtcBase = 0;
  int64_t utcShift = 0;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  bool keys[3] = {false, false, false};
  int battery = 80;
  bool wifiConfigured = true;
  int port = 8090;
  bool sleeping = false;
  int64_t wakeAt = 0;
  HttpHandler handler;
  SimFs fsImpl;
  SimEpd epdImpl;
  RtcData rtcData;

  uint32_t millis() override {
    if (scripted) return virtMs;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  int64_t nowUtc() override {
    if (scripted) return virtUtcBase + virtMs / 1000 + utcShift;
    return static_cast<int64_t>(time(nullptr)) + utcShift;
  }
  bool syncTime() override {
    printf("[网络] NTP 校时%s\n", wifiConfigured ? "成功" : "失败（没有 WiFi）");
    return wifiConfigured;
  }
  bool keyDown(Key k) override { return keys[int(k) - 1]; }
  int batteryPercent() override { return battery; }
  NetMode startNetwork(std::string& url) override {
    if (wifiConfigured) {
      url = "http://" + lanIp() + ":" + std::to_string(port);
      printf("[网络] 已连上 WiFi，家长网页：%s\n", url.c_str());
      return NetMode::Station;
    }
    url = "http://192.168.4.1";
    printf("[网络] 没有 WiFi 配置，开热点 %s（模拟器里网页仍在 http://localhost:%d）\n", AP_SSID, port);
    return NetMode::SetupAp;
  }
  void stopNetwork() override { printf("[网络] 关闭 WiFi\n"); }
  void pollNetwork() override {}
  void setHttpHandler(HttpHandler h) override { handler = std::move(h); }
  bool saveWifi(const std::string& ssid, const std::string&) override {
    printf("[网络] 保存 WiFi：%s\n", ssid.c_str());
    wifiConfigured = true;
    return true;
  }
  void deepSleep(int64_t at) override {
    sleeping = true;
    wakeAt = at;
    if (at) {
      int64_t local = at + 8 * 3600;
      Ymd d = civilFromDays(static_cast<int>(floorDiv(local, 86400)));
      int s = static_cast<int>(local - floorDiv(local, 86400) * 86400);
      printf("[电源] 深睡，按键或 %02d-%02d %02d:%02d 定时唤醒\n", d.m, d.d, s / 3600, s / 60 % 60);
    } else {
      printf("[电源] 深睡，只能按键唤醒\n");
    }
  }
  RtcData& rtc() override { return rtcData; }
  Fs& fs() override { return fsImpl; }
  Epd& epd() override { return epdImpl; }
  void log(const char* m) override { printf("[固件] %s\n", m); }
};

// ------------------------------------------------------------------ 设备（App 的生命周期）

struct Device {
  SimPlatform plat;
  std::unique_ptr<App> app;
  std::recursive_mutex mu;

  void boot(WakeReason why, Key k = Key::None) {
    app.reset();  // 模拟内存全丢
    plat.sleeping = false;
    const char* names[] = {"上电", "按键", "定时"};
    printf("[电源] %s启动\n", names[int(why)]);
    app = std::make_unique<App>(plat);
    app->boot(why, k);
  }
  void step() {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!app) return;
    if (plat.sleeping) {
      for (int i = 0; i < 3; ++i)
        if (plat.keys[i]) return boot(WakeReason::Key, Key(i + 1));
      if (plat.wakeAt && plat.nowUtc() >= plat.wakeAt) boot(WakeReason::Timer);
      return;
    }
    app->loop();
  }
  HttpResponse http(const HttpRequest& r) {
    std::lock_guard<std::recursive_mutex> l(mu);
    if (!app || plat.sleeping) return {503, "application/json", R"({"error":"设备在睡眠。长按确认键 3 秒进入家长设置"})"};
    return plat.handler(r);
  }
};

std::string localTimeStr(int64_t utc) {
  int64_t local = utc + 8 * 3600;
  int day = static_cast<int>(floorDiv(local, 86400));
  int s = static_cast<int>(local - int64_t(day) * 86400);
  Ymd d = civilFromDays(day);
  char buf[64];
  snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", d.y, d.m, d.d, s / 3600, s / 60 % 60, s % 60);
  return buf;
}

// "2026-10-16 18:00" → UTC（按北京时间）
bool parseLocal(const std::string& s, int64_t& utc) {
  int y, mo, d, h = 0, mi = 0;
  if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3) return false;
  utc = int64_t(daysFromCivil(y, mo, d)) * 86400 + h * 3600 + mi * 60 - 8 * 3600;
  return true;
}

void writePng(const uint8_t* fb, const std::string& path) {
  std::vector<uint8_t> gray(W * H);
  for (int i = 0; i < W * H; ++i) gray[i] = (fb[i >> 3] & (0x80 >> (i & 7))) ? 0xFF : 0x00;
  stbi_write_png(path.c_str(), W, H, 1, gray.data(), W);
}

void startServer(Device& dev, httplib::Server& svr, int port) {
  svr.set_mount_point("/", g_dataDir + "/web");
  auto api = [&dev](const httplib::Request& req, httplib::Response& res) {
    HttpRequest r;
    r.method = req.method;
    r.path = req.path;
    r.body = req.body;
    for (auto& kv : req.params) r.query[kv.first] = kv.second;
    HttpResponse out = dev.http(r);
    res.status = out.status;
    res.set_content(out.body, (out.type + "; charset=utf-8").c_str());
  };
  svr.Get(R"(/api/.*)", api);
  svr.Put(R"(/api/.*)", api);
  svr.Post(R"(/api/.*)", api);
  svr.Delete(R"(/api/.*)", api);
  std::thread([&svr, port] {
    if (!svr.listen("0.0.0.0", port)) fprintf(stderr, "HTTP 端口 %d 打不开\n", port);
  }).detach();
}

// ------------------------------------------------------------------ 脚本模式

int runScript(Device& dev, const std::string& script, const std::string& outDir, int64_t startUtc) {
  SimPlatform& p = dev.plat;
  p.scripted = true;
  p.virtUtcBase = startUtc;
  p.epdImpl.verbose = true;
  mkdirs(outDir);
  std::ifstream in(script);
  if (!in) {
    fprintf(stderr, "打不开脚本 %s\n", script.c_str());
    return 2;
  }
  auto run = [&](uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      p.virtMs += 10;
      dev.step();
    }
  };
  auto keyOf = [](const std::string& s) { return s == "up" ? Key::Up : s == "down" ? Key::Down : Key::Ok; };
  dev.boot(WakeReason::PowerOn);
  int failures = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    printf("\n> %s   （%s）\n", line.c_str(), localTimeStr(p.nowUtc()).c_str());
    if (cmd == "key") {
      std::string k;
      ls >> k;
      p.keys[int(keyOf(k)) - 1] = true;
      run(120);
      p.keys[int(keyOf(k)) - 1] = false;
      run(100);
    } else if (cmd == "hold") {
      std::string k;
      double sec;
      ls >> k >> sec;
      p.keys[int(keyOf(k)) - 1] = true;
      run(static_cast<uint32_t>(sec * 1000));
      p.keys[int(keyOf(k)) - 1] = false;
      run(100);
    } else if (cmd == "wait") {
      double sec;
      ls >> sec;
      run(static_cast<uint32_t>(sec * 1000));
    } else if (cmd == "jump") {  // 拨快墙上时钟（不逐毫秒跑），用来跨天
      std::string t;
      std::getline(ls >> std::ws, t);
      int64_t target;
      if (parseLocal(t, target)) p.utcShift += target - p.nowUtc();
      run(20);
    } else if (cmd == "shot") {
      std::string name;
      ls >> name;
      writePng(p.epdImpl.physical(), outDir + "/" + name + ".png");
      printf("  截图 %s/%s.png\n", outDir.c_str(), name.c_str());
    } else if (cmd == "http") {
      HttpRequest r;
      std::string target;
      ls >> r.method >> target;
      std::getline(ls >> std::ws, r.body);
      size_t q = target.find('?');
      r.path = target.substr(0, q);
      if (q != std::string::npos) {
        std::stringstream qs(target.substr(q + 1));
        std::string kv;
        while (std::getline(qs, kv, '&')) {
          size_t e = kv.find('=');
          r.query[kv.substr(0, e)] = e == std::string::npos ? "" : kv.substr(e + 1);
        }
      }
      HttpResponse res = dev.http(r);
      std::string body = res.body.size() > 400 ? res.body.substr(0, 400) + "…" : res.body;
      printf("  HTTP %d %s\n", res.status, body.c_str());
      run(50);
    } else if (cmd == "serve") {  // 打开 HTTP 服务，按真实时间跑 N 秒（给浏览器/curl 测网页用）
      double sec;
      ls >> sec;
      static httplib::Server svr;
      static bool started = false;
      if (!started) {
        startServer(dev, svr, p.port);
        started = true;
      }
      printf("  网页 http://localhost:%d ，运行 %.0f 秒\n", p.port, sec);
      auto t0 = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(sec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
          std::lock_guard<std::recursive_mutex> l(dev.mu);
          p.virtMs += 10;
        }
        dev.step();
      }
    } else if (cmd == "battery") {
      ls >> p.battery;
    } else if (cmd == "wifi") {
      std::string v;
      ls >> v;
      p.wifiConfigured = v == "on";
    } else if (cmd == "power-cycle") {
      p.rtcData = RtcData{};
      dev.boot(WakeReason::PowerOn);
    } else if (cmd == "expect") {  // expect sleeping|awake|view <名字>|done <n>|errors 0
      std::string what, val;
      ls >> what >> val;
      bool ok = true;
      std::string got;
      if (what == "sleeping") { ok = p.sleeping; got = p.sleeping ? "sleeping" : "awake"; }
      else if (what == "awake") { ok = !p.sleeping; got = p.sleeping ? "sleeping" : "awake"; }
      else if (what == "view") {
        const char* names[] = {"Main", "Celebrate", "Edit", "Setup", "NoTime"};
        got = p.sleeping ? "(sleeping)" : names[int(dev.app->uiState().view)];
        ok = got == val;
      } else if (what == "done") {
        got = std::to_string(dev.app->model().doneCount());
        ok = got == val;
      } else if (what == "stars") {
        got = std::to_string(dev.app->model().displayStars());
        ok = got == val;
      } else if (what == "panel") {  // 屏上实际显示 == 固件以为的画面
        ok = p.epdImpl.errors == 0 && memcmp(dev.app->frame(), p.epdImpl.physical(), FB_BYTES) == 0;
        got = ok ? "一致" : ("不一致 " + p.epdImpl.lastError);
      }
      printf("  %s 期望 %s %s，实际 %s\n", ok ? "✅" : "❌", what.c_str(), val.c_str(), got.c_str());
      failures += !ok;
    } else {
      printf("  未知命令\n");
      failures++;
    }
  }
  printf("\n脚本结束：全刷 %d 次，局刷 %d 次，控制器错误 %d 次，断言失败 %d 个\n", p.epdImpl.fullCount,
         p.epdImpl.partialCount, p.epdImpl.errors, failures);
  return failures || p.epdImpl.errors ? 1 : 0;
}

// ------------------------------------------------------------------ 交互模式

constexpr int BAR_H = 40;
const uint32_t PAPER = 0xFFF2F0EA, INK = 0xFF1C1C1C, BAR_BG = 0xFF2B2B2B, BAR_FG = 0xFFE8E8E8;

int runInteractive(Device& dev, int port) {
  SimPlatform& p = dev.plat;
  p.epdImpl.fullMs = 1200;  // GxEPD2 源码里这块屏的刷新时间
  p.epdImpl.partialMs = 450;
  p.epdImpl.delayFn = [](uint32_t ms) { SDL_Delay(ms); };

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL 初始化失败：%s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow("eink-todo 模拟器", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W,
                                     H + 2 * BAR_H, SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, W, H + 2 * BAR_H);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H + 2 * BAR_H);

  // 状态栏用同一套点阵字库画
  Font f24;
  f24.load(p.fsImpl, "/fonts/sans24.bin");
  std::vector<uint8_t> hudBuf(FB_BYTES);
  Canvas hud(hudBuf.data());

  httplib::Server svr;
  startServer(dev, svr, port);
  printf("家长网页（编辑模式下可用）：http://localhost:%d  /  http://%s:%d\n", port, lanIp().c_str(), port);
  printf("按键：↑ ↓ = 上下，回车/空格 = 确认（按住 3 秒 = 长按）\n");
  printf("调试：T = 时间 +1 小时，N = 跳到换日前 10 秒，B = 电量 -10%%，W = WiFi 有/无，P = 断电重启，Esc = 退出\n\n");

  dev.boot(WakeReason::PowerOn);
  std::vector<uint32_t> px(W * (H + 2 * BAR_H));
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        std::lock_guard<std::recursive_mutex> l(dev.mu);
        switch (e.key.keysym.sym) {
          case SDLK_ESCAPE: running = false; break;
          case SDLK_t: p.utcShift += 3600; printf("[调试] 时间 +1 小时 → %s\n", localTimeStr(p.nowUtc()).c_str()); break;
          case SDLK_n: {
            int64_t target = dev.app->model().nextRolloverUtc(p.nowUtc()) - 10;
            p.utcShift += target - p.nowUtc();
            printf("[调试] 跳到 %s\n", localTimeStr(p.nowUtc()).c_str());
            break;
          }
          case SDLK_b: p.battery = p.battery <= 10 ? 100 : p.battery - 10; printf("[调试] 电量 %d%%（下次唤醒生效）\n", p.battery); break;
          case SDLK_w: p.wifiConfigured = !p.wifiConfigured; printf("[调试] WiFi %s\n", p.wifiConfigured ? "有" : "无"); break;
          case SDLK_p: p.rtcData = RtcData{}; dev.boot(WakeReason::PowerOn); break;
          default: break;
        }
      }
    }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    {
      std::lock_guard<std::recursive_mutex> l(dev.mu);
      p.keys[0] = ks[SDL_SCANCODE_UP];
      p.keys[1] = ks[SDL_SCANCODE_DOWN];
      p.keys[2] = ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_SPACE];
    }
    dev.step();

    // 画面：上状态栏 / 墨水屏 / 下帮助栏
    std::string top, bottom = "↑↓ 选择   回车 确认（按住3秒=长按）   T +1小时   N 跳到换日   B 电量   W WiFi   P 断电";
    {
      std::lock_guard<std::recursive_mutex> l(dev.mu);
      top = localTimeStr(p.nowUtc()).substr(5, 11) + "   ";
      if (p.sleeping) top += "深睡中（按任意键唤醒）";
      else if (dev.app->netMode() != NetMode::Off) top += "家长编辑模式";
      else top += "醒着";
      top += "   电量 " + std::to_string(p.battery) + "%   全刷 " + std::to_string(p.epdImpl.fullCount) +
             "  局刷 " + std::to_string(p.epdImpl.partialCount);
      if (p.epdImpl.errors) top += "   控制器错误 " + std::to_string(p.epdImpl.errors);
    }
    auto bar = [&](const std::string& text, int y0) {
      hud.clear(WHITE);
      f24.draw(hud, 12, 8, text, BLACK, W - 24);
      for (int y = 0; y < BAR_H; ++y)
        for (int x = 0; x < W; ++x) px[(y0 + y) * W + x] = hud.get(x, y) ? BAR_BG : BAR_FG;
    };
    bar(top, 0);
    bar(bottom, H + BAR_H);
    const uint8_t* fb = p.epdImpl.physical();
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) px[(y + BAR_H) * W + x] = (fb[(y * W + x) >> 3] & (0x80 >> (x & 7))) ? PAPER : INK;
    // 刚刷新过的区域用红框标一下（只在窗口里，截图里没有）
    if (SDL_GetTicks() - p.epdImpl.lastRefreshTick < 500 && !p.epdImpl.lastWasFull) {
      Rect r = p.epdImpl.lastRefresh;
      for (int x = r.x; x < r.x + r.w; ++x) {
        px[(r.y + BAR_H) * W + x] = 0xFFE53935;
        px[(r.y + r.h - 1 + BAR_H) * W + x] = 0xFFE53935;
      }
      for (int y = r.y; y < r.y + r.h; ++y) {
        px[(y + BAR_H) * W + r.x] = 0xFFE53935;
        px[(y + BAR_H) * W + r.x + r.w - 1] = 0xFFE53935;
      }
    }
    SDL_UpdateTexture(tex, nullptr, px.data(), W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
    SDL_Delay(10);
  }
  svr.stop();
  SDL_Quit();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string script, outDir = "shots", start;
  int port = 8090;
  bool reset = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--script") script = next();
    else if (a == "--out") outDir = next();
    else if (a == "--start") start = next();
    else if (a == "--port") port = std::stoi(next());
    else if (a == "--data") g_dataDir = next();
    else if (a == "--state") g_stateDir = next();
    else if (a == "--reset") reset = true;
    else {
      printf("用法：program [--script 文件 --out 截图目录 --start \"2026-10-16 18:00\"] [--port 8090] [--reset]\n");
      return 2;
    }
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (reset || !script.empty()) {  // 脚本模式每次从全新状态开始
    for (const char* f : {"settings.json", "templates.json", "overrides.json", "today.json", "stats.json", "frame.rle", "wifi.json"})
      std::remove((g_stateDir + "/" + f).c_str());
  }
  mkdirs(g_stateDir);

  static Device dev;
  dev.plat.port = port;
  if (!script.empty()) {
    int64_t utc = 0;
    if (start.empty() || !parseLocal(start, utc)) utc = time(nullptr);
    return runScript(dev, script, outDir, utc);
  }
  if (!start.empty()) {
    int64_t utc;
    if (parseLocal(start, utc)) dev.plat.utcShift = utc - time(nullptr);
  }
  return runInteractive(dev, port);
}
