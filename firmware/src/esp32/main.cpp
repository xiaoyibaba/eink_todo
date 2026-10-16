// XIAO ESP32S3 真机固件（Wokwi 仿真跑的也是这一份）。
// 引脚分配见 docs/hardware.md，改接线必须两边一起改。
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GxEPD2.h>
#include <GxEPD2_EPD.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <gdey/GxEPD2_750_GDEY075T7.h>
#include <sys/time.h>

#include "App.h"

using namespace et;

namespace pins {
constexpr int BAT_ADC = 1;   // D0，电池分压后
constexpr int KEY_UP = 2;    // D1
constexpr int KEY_DOWN = 3;  // D2
constexpr int KEY_OK = 4;    // D3
constexpr int EPD_BUSY = 5;  // D4
constexpr int EPD_RST = 6;   // D5
// D6（GPIO43）空着：它是 UART0 TX，开机时 ROM 会往上打日志，不能接屏幕的 DC（Wokwi 里 DC 会一直是高电平）
constexpr int EPD_CS = 44;   // D7
constexpr int EPD_SCK = 7;   // D8
constexpr int EPD_DC = 8;    // D9
constexpr int EPD_MOSI = 9;  // D10
}  // namespace pins

constexpr int64_t TIME_VALID_AFTER = 1700000000;  // 2023 年以后才算校过时
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;

RTC_DATA_ATTR RtcData g_rtc;

// 刷屏时固件在等 BUSY，不轮询按键。用中断把这期间的按下先锁存住，否则孩子按快了会丢键
volatile bool g_latched[3] = {false, false, false};
void IRAM_ATTR onKeyUp() { g_latched[0] = true; }
void IRAM_ATTR onKeyDown() { g_latched[1] = true; }
void IRAM_ATTR onKeyOk() { g_latched[2] = true; }

// ------------------------------------------------------------------ LittleFS

class LfsFile : public et::File {
 public:
  explicit LfsFile(fs::File f) : f_(f) {}
  ~LfsFile() override { f_.close(); }
  size_t size() override { return f_.size(); }
  size_t readAt(uint32_t off, void* buf, size_t n) override {
    if (!f_.seek(off)) return 0;
    return f_.read(static_cast<uint8_t*>(buf), n);
  }

 private:
  fs::File f_;
};

class LfsFs : public Fs {
 public:
  std::unique_ptr<et::File> open(const std::string& p) override {
    if (!LittleFS.exists(p.c_str())) return nullptr;
    fs::File f = LittleFS.open(p.c_str(), "r");
    if (!f) return nullptr;
    return std::unique_ptr<et::File>(new LfsFile(f));
  }
  bool readAll(const std::string& p, std::string& out) override {
#ifdef FS_TRACE
    Serial.printf("fs read %s\n", p.c_str());
#endif
    if (!LittleFS.exists(p.c_str())) return false;
    fs::File f = LittleFS.open(p.c_str(), "r");
    if (!f) return false;
    out.resize(f.size());
    size_t n = f.read(reinterpret_cast<uint8_t*>(&out[0]), out.size());
    f.close();
    return n == out.size();
  }
  bool writeAtomic(const std::string& p, const std::string& d) override {
    std::string tmp = p + ".tmp";
    fs::File f = LittleFS.open(tmp.c_str(), "w");
    if (!f) return false;
    size_t n = f.write(reinterpret_cast<const uint8_t*>(d.data()), d.size());
    f.close();
    if (n != d.size()) return false;
    if (LittleFS.rename(tmp.c_str(), p.c_str())) return true;
    LittleFS.remove(p.c_str());  // 个别 VFS 实现不允许覆盖
    return LittleFS.rename(tmp.c_str(), p.c_str());
  }
  bool remove(const std::string& p) override { return LittleFS.remove(p.c_str()); }
};

// ------------------------------------------------------------------ 屏幕

class GxEpd : public Epd {
 public:
  GxEpd() : epd_(pins::EPD_CS, pins::EPD_DC, pins::EPD_RST, pins::EPD_BUSY) {}
  void begin(bool coldBoot) override {
    SPI.begin(pins::EPD_SCK, -1, pins::EPD_MOSI, pins::EPD_CS);
    epd_.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
    // initial=false：屏上内容已知（深睡醒来），允许直接局刷。init 里总会硬复位，把控制器从深睡叫醒
    epd_.init(0, coldBoot, 2, false);
  }
  void writePrevious(const uint8_t* fb) override { epd_.writeImageToPrevious(fb, 0, 0, W, H); }
  void writeNew(const uint8_t* fb) override { epd_.writeImage(fb, 0, 0, W, H); }
  void refreshFull() override { epd_.refresh(false); }
  void refreshPartial(Rect r) override { epd_.refresh(r.x, r.y, r.w, r.h); }
  void hibernate() override { epd_.hibernate(); }

 private:
  GxEPD2_750_GDEY075T7 epd_;
};

// ------------------------------------------------------------------ 平台


class EspPlatform : public Platform {
 public:
  uint32_t millis() override { return ::millis(); }
  int64_t nowUtc() override {
    int64_t t = time(nullptr);
    return t > TIME_VALID_AFTER ? t : 0;
  }
  bool syncTime() override {
    bool ownWifi = WiFi.status() != WL_CONNECTED;
    if (ownWifi && !connectStation()) return false;
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    configTime(0, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
    uint32_t t0 = ::millis();
    bool ok = false;
    // configTime 是异步的：等系统时间被 SNTP 更新
    while (::millis() - t0 < 8000) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ok = true;
        break;
      }
      delay(100);
    }
    if (ownWifi) wifiOff();
    log(ok ? "NTP ok" : "NTP failed");
    return ok;
  }
  bool keyDown(Key k) override {
    int i = static_cast<int>(k) - 1;
    int pin = k == Key::Up ? pins::KEY_UP : k == Key::Down ? pins::KEY_DOWN : pins::KEY_OK;
    bool level = digitalRead(pin) == LOW;
    // 锁存的按下只报告一次「按下」，下一次轮询起按真实电平走，App 自然会看到一次松开
    bool latched = g_latched[i];
    g_latched[i] = false;
    return level || latched;
  }
  int batteryPercent() override {
    // 1:1 分压：引脚电压 × 2 = 电池电压。锂电粗略曲线
    uint32_t mv = 0;
    for (int i = 0; i < 8; ++i) mv += analogReadMilliVolts(pins::BAT_ADC);
    mv = mv / 8 * 2;
    if (mv < 2500) return -1;  // 没接电池（只插 USB）或分压没焊
    static const uint16_t curve[][2] = {{3300, 0}, {3600, 10}, {3700, 30}, {3800, 55}, {3900, 70}, {4000, 85}, {4150, 100}};
    if (mv <= curve[0][0]) return 0;
    for (int i = 1; i < 7; ++i)
      if (mv <= curve[i][0])
        return curve[i - 1][1] + (mv - curve[i - 1][0]) * (curve[i][1] - curve[i - 1][1]) / (curve[i][0] - curve[i - 1][0]);
    return 100;
  }

  NetMode startNetwork(std::string& url) override {
    if (connectStation()) {
      MDNS.begin("todo");
      MDNS.addService("http", "tcp", 80);
      url = "http://" + std::string(WiFi.localIP().toString().c_str());
      mode_ = NetMode::Station;
    } else {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID);
      dns_.start(53, "*", WiFi.softAPIP());  // 所有域名都指向自己：手机连上热点会自动弹出配网页
      url = "http://" + std::string(WiFi.softAPIP().toString().c_str());
      mode_ = NetMode::SetupAp;
    }
    startServer();
    return mode_;
  }
  void stopNetwork() override {
    server_.stop();
    if (mode_ == NetMode::SetupAp) dns_.stop();
    MDNS.end();
    wifiOff();
    mode_ = NetMode::Off;
  }
  void pollNetwork() override {
    if (mode_ == NetMode::SetupAp) dns_.processNextRequest();
    server_.handleClient();
  }
  void setHttpHandler(HttpHandler h) override { handler_ = h; }
  bool saveWifi(const std::string& ssid, const std::string& pass) override {
    Preferences p;
    if (!p.begin("wifi", false)) return false;
    bool ok = p.putString("ssid", ssid.c_str()) && (pass.empty() || p.putString("pass", pass.c_str()));
    if (pass.empty()) p.remove("pass");
    p.end();
    return ok;
  }

  void deepSleep(int64_t wakeAtUtc) override {
    uint64_t mask = (1ULL << pins::KEY_UP) | (1ULL << pins::KEY_DOWN) | (1ULL << pins::KEY_OK);
    // 深睡期间靠 RTC 域的内部上拉保持按键为高电平
    for (int pin : {pins::KEY_UP, pins::KEY_DOWN, pins::KEY_OK}) {
      rtc_gpio_pullup_en(static_cast<gpio_num_t>(pin));
      rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(pin));
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    int64_t now = nowUtc();
    if (wakeAtUtc > 0 && now > 0) {
      int64_t sec = wakeAtUtc - now;
      if (sec < 1) sec = 1;
      esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sec) * 1000000ULL);
    }
    Serial.printf("deep sleep, wake in %lld s\n", wakeAtUtc > 0 && now > 0 ? (long long)(wakeAtUtc - now) : -1LL);
    Serial.flush();
    esp_deep_sleep_start();
  }
  RtcData& rtc() override { return g_rtc; }
  Fs& fs() override { return fs_; }
  Epd& epd() override { return epd_; }
  void log(const char* m) override { Serial.println(m); }

 private:
  bool connectStation() {
    String ssid, pass;
    Preferences p;
    if (p.begin("wifi", true)) {  // 从没保存过时命名空间不存在，begin 会失败，当作没配置
      ssid = p.getString("ssid", "");
      pass = p.getString("pass", "");
      p.end();
    }
#ifdef WOKWI_SIM
    if (ssid.isEmpty()) ssid = "Wokwi-GUEST";  // Wokwi 的虚拟热点
#endif
    if (ssid.isEmpty()) return false;
    WiFi.mode(WIFI_STA);
#ifdef WOKWI_SIM
    WiFi.begin(ssid.c_str(), pass.c_str(), 6);
#else
    WiFi.begin(ssid.c_str(), pass.c_str());
#endif
    uint32_t t0 = ::millis();
    while (WiFi.status() != WL_CONNECTED && ::millis() - t0 < WIFI_TIMEOUT_MS) delay(100);
    if (WiFi.status() == WL_CONNECTED) return true;
    wifiOff();
    return false;
  }
  void wifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  void startServer() {
    server_.serveStatic("/", LittleFS, "/web/");
    server_.onNotFound([this] { handleAny(); });
    server_.begin();
  }
  void handleAny() {
    String uri = server_.uri();
    if (!uri.startsWith("/api/")) {
      // 配网热点下，手机探测联网的请求都跳到首页，触发自动弹窗
      server_.sendHeader("Location", "/", true);
      server_.send(302, "text/plain", "");
      return;
    }
    HttpRequest req;
    static const char* methods[] = {"GET", "PUT", "POST", "DELETE"};
    HTTPMethod m = server_.method();
    req.method = m == HTTP_GET ? methods[0] : m == HTTP_PUT ? methods[1] : m == HTTP_POST ? methods[2] : m == HTTP_DELETE ? methods[3] : "OTHER";
    req.path = uri.c_str();
    for (int i = 0; i < server_.args(); ++i) {
      if (server_.argName(i) == "plain") req.body = server_.arg(i).c_str();
      else req.query[server_.argName(i).c_str()] = server_.arg(i).c_str();
    }
    HttpResponse res = handler_ ? handler_(req) : HttpResponse{503, "application/json", "{}"};
    server_.send(res.status, (res.type + "; charset=utf-8").c_str(), res.body.c_str());
  }

  LfsFs fs_;
  GxEpd epd_;
  WebServer server_{80};
  DNSServer dns_;
  HttpHandler handler_;
  NetMode mode_ = NetMode::Off;
};

EspPlatform* g_plat;
App* g_app;

void setup() {
  Serial.begin(115200);
  pinMode(pins::KEY_UP, INPUT_PULLUP);
  pinMode(pins::KEY_DOWN, INPUT_PULLUP);
  pinMode(pins::KEY_OK, INPUT_PULLUP);
  for (int pin : {pins::KEY_UP, pins::KEY_DOWN, pins::KEY_OK}) rtc_gpio_deinit(static_cast<gpio_num_t>(pin));
  setenv("TZ", "UTC0", 1);  // 时区由 core 自己按设置换算
  tzset();
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  LittleFS.mkdir("/state");
  Serial.printf("fs: %u/%u bytes used, psram free %u\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(), (unsigned)ESP.getFreePsram());

  WakeReason why = WakeReason::PowerOn;
  Key key = Key::None;
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      uint64_t st = esp_sleep_get_ext1_wakeup_status();
      why = WakeReason::Key;
      key = st & (1ULL << pins::KEY_OK) ? Key::Ok : st & (1ULL << pins::KEY_UP) ? Key::Up : Key::Down;
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER: why = WakeReason::Timer; break;
    default: break;
  }
  Serial.printf("boot: reason=%d key=%d\n", int(why), int(key));
  attachInterrupt(pins::KEY_UP, onKeyUp, FALLING);
  attachInterrupt(pins::KEY_DOWN, onKeyDown, FALLING);
  attachInterrupt(pins::KEY_OK, onKeyOk, FALLING);
  g_plat = new EspPlatform();
  g_app = new App(*g_plat);
  g_app->boot(why, key);
  Serial.printf("booted: heap free %u, psram free %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

void loop() {
  g_app->loop();
  delay(5);
}
