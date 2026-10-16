// 单元测试：pio test -e test（在 firmware/ 目录下跑，字库从 data/ 读）
#include <unity.h>

#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "App.h"
#include "DateUtil.h"
#include "EpdEmu.h"
#include "Model.h"
#include "Screen.h"

using namespace et;

// ---------------------------------------------------------------- 测试用平台

class MemFile : public File {
 public:
  explicit MemFile(std::string d) : d_(std::move(d)) {}
  size_t size() override { return d_.size(); }
  size_t readAt(uint32_t off, void* buf, size_t n) override {
    if (off >= d_.size()) return 0;
    n = std::min(n, d_.size() - off);
    memcpy(buf, d_.data() + off, n);
    return n;
  }

 private:
  std::string d_;
};

// /state/ 在内存里；其他路径只读，取自 firmware/data/
class TestFs : public Fs {
 public:
  std::map<std::string, std::string> mem;
  int writes = 0;
  std::unique_ptr<File> open(const std::string& p) override {
    std::string d;
    if (!readAll(p, d)) return nullptr;
    return std::make_unique<MemFile>(d);
  }
  bool readAll(const std::string& p, std::string& out) override {
    if (p.rfind("/state/", 0) == 0) {
      auto it = mem.find(p);
      if (it == mem.end()) return false;
      out = it->second;
      return true;
    }
    auto it = cache_.find(p);
    if (it == cache_.end()) {
      std::ifstream f("data" + p, std::ios::binary);
      if (!f) return false;
      std::stringstream ss;
      ss << f.rdbuf();
      it = cache_.emplace(p, ss.str()).first;
    }
    out = it->second;
    return true;
  }
  bool writeAtomic(const std::string& p, const std::string& d) override {
    mem[p] = d;
    writes++;
    return true;
  }
  bool remove(const std::string& p) override { return mem.erase(p) > 0; }

 private:
  static std::map<std::string, std::string> cache_;
};
std::map<std::string, std::string> TestFs::cache_;

class TestPlatform : public Platform {
 public:
  uint32_t ms = 0;
  int64_t utc = 0;
  bool keys[3] = {false, false, false};
  int64_t sleptUntil = -1;
  bool wifiOk = true;
  HttpHandler handler;
  TestFs fsImpl;
  EpdEmu epdImpl;
  RtcData rtcData;

  uint32_t millis() override { return ms; }
  int64_t nowUtc() override { return utc; }
  bool syncTime() override { return utc != 0; }
  bool keyDown(Key k) override { return keys[int(k) - 1]; }
  int batteryPercent() override { return 80; }
  NetMode startNetwork(std::string& url) override {
    url = wifiOk ? "http://192.168.1.50" : "http://192.168.4.1";
    return wifiOk ? NetMode::Station : NetMode::SetupAp;
  }
  void stopNetwork() override {}
  void pollNetwork() override {}
  void setHttpHandler(HttpHandler h) override { handler = h; }
  bool saveWifi(const std::string&, const std::string&) override { return true; }
  void deepSleep(int64_t wakeAt) override { sleptUntil = wakeAt; }
  RtcData& rtc() override { return rtcData; }
  Fs& fs() override { return fsImpl; }
  Epd& epd() override { return epdImpl; }
  void log(const char*) override {}
};

// 2026-10-16 18:00 北京时间（周五）
const int64_t FRI_1800 = int64_t(daysFromCivil(2026, 10, 16)) * 86400 + 10 * 3600;

struct Rig {
  TestPlatform p;
  std::unique_ptr<App> app;
  Rig() { p.utc = FRI_1800; }
  void boot(WakeReason w, Key k = Key::None) {
    app = std::make_unique<App>(p);
    p.sleptUntil = -1;
    app->boot(w, k);
  }
  void run(uint32_t forMs) {
    for (uint32_t t = 0; t < forMs && !app->asleep(); t += 10) {
      p.ms += 10;
      app->loop();
    }
  }
  void press(Key k, uint32_t holdMs = 100) {
    p.keys[int(k) - 1] = true;
    run(holdMs);
    p.keys[int(k) - 1] = false;
    run(60);
  }
  // 屏上实际显示的必须和固件以为的画面一致
  void assertPanelMatches() {
    TEST_ASSERT_EQUAL_INT(0, p.epdImpl.errors);
    TEST_ASSERT_EQUAL_MEMORY(app->frame(), p.epdImpl.physical(), FB_BYTES);
  }
  HttpResponse http(const char* method, const char* path, const std::string& body = "",
                    std::map<std::string, std::string> q = {}) {
    HttpRequest r;
    r.method = method;
    r.path = path;
    r.body = body;
    r.query = q;
    return p.handler(r);
  }
};

// ---------------------------------------------------------------- Model

Model fresh() {
  Model m;
  for (int d = 0; d < 7; ++d) m.templates[d] = {{"a", "口算", "kousuan", false}, {"b", "阅读", "yuedu", false}};
  m.templates[6].clear();  // 周日没任务
  return m;
}

void test_date_roundtrip() {
  for (int d = -1000; d < 30000; d += 37) TEST_ASSERT_EQUAL_INT(d, isoToDay(dayToIso(d)));
  TEST_ASSERT_EQUAL_INT(4, weekdayMon0(daysFromCivil(2026, 10, 16)));  // 周五
  TEST_ASSERT_EQUAL_INT(INT32_MIN, isoToDay("2026-02-30"));
}

void test_logical_day_switches_at_reset_hour() {
  Model m = fresh();
  const int fri = daysFromCivil(2026, 10, 16);
  // 周六凌晨 3:59 还算周五，4:00 算周六
  int64_t sat0359 = int64_t(fri + 1) * 86400 + (3 * 3600 + 59 * 60) - 8 * 3600;
  TEST_ASSERT_EQUAL_INT(fri, m.logicalDay(sat0359));
  TEST_ASSERT_EQUAL_INT(fri + 1, m.logicalDay(sat0359 + 60));
  TEST_ASSERT_EQUAL_INT64(sat0359 + 60, m.nextRolloverUtc(FRI_1800));
}

void test_stars_and_streak() {
  Model m = fresh();
  m.rollover(FRI_1800);
  TEST_ASSERT_FALSE(m.toggle(0));
  TEST_ASSERT_TRUE(m.toggle(1));  // 最后一条：变成全部完成
  TEST_ASSERT_EQUAL_INT(1, m.displayStars());
  TEST_ASSERT_EQUAL_INT(1, m.displayStreak());
  m.toggle(1);  // 取消：星星收回
  TEST_ASSERT_EQUAL_INT(0, m.displayStars());
  m.toggle(1);
  m.rollover(FRI_1800 + 86400);  // 周六
  TEST_ASSERT_EQUAL_INT(1, m.stats.stars);
  TEST_ASSERT_EQUAL_INT(1, m.stats.streak);
  TEST_ASSERT_FALSE(m.today.items[0].done);
  // 周六没做完 → 周日结算时归零；周日没任务，不加不断
  m.rollover(FRI_1800 + 2 * 86400);
  TEST_ASSERT_EQUAL_INT(0, m.stats.streak);
  TEST_ASSERT_FALSE(m.counts());
  m.rollover(FRI_1800 + 3 * 86400);
  TEST_ASSERT_EQUAL_INT(0, m.stats.streak);
  TEST_ASSERT_EQUAL_INT(1, m.stats.stars);
}

void test_rest_day_keeps_streak() {
  Model m = fresh();
  const int fri = daysFromCivil(2026, 10, 16);
  m.rollover(FRI_1800);
  m.toggle(0);
  m.toggle(1);
  m.overrides[fri + 1] = DayPlan{true, {}};  // 周六休息
  m.rollover(FRI_1800 + 86400);
  TEST_ASSERT_TRUE(m.today.rest);
  m.rollover(FRI_1800 + 2 * 86400);  // 周日没任务
  m.rollover(FRI_1800 + 3 * 86400);  // 周一
  TEST_ASSERT_EQUAL_INT(1, m.stats.streak);
  TEST_ASSERT_TRUE(m.overrides.empty());  // 过期的单独设置清掉
}

void test_gap_days_break_streak() {
  Model m = fresh();
  m.rollover(FRI_1800);
  m.toggle(0);
  m.toggle(1);
  m.stats.streak = 5;
  m.rollover(FRI_1800 + 4 * 86400);  // 没电关机三天，中间周六有任务
  TEST_ASSERT_EQUAL_INT(0, m.stats.streak);
  TEST_ASSERT_EQUAL_INT(1, m.stats.stars);
  TEST_ASSERT_EQUAL_INT(4, int(m.stats.history.size()));
}

void test_template_change_keeps_done_flags() {
  Model m = fresh();
  m.rollover(FRI_1800);
  m.toggle(1);
  m.templates[4] = {{"b", "阅读", "yuedu", false}, {"c", "跳绳", "tiaosheng", false}};
  m.refreshToday();
  TEST_ASSERT_EQUAL_INT(2, int(m.today.items.size()));
  TEST_ASSERT_TRUE(m.today.items[0].done);
  TEST_ASSERT_FALSE(m.today.items[1].done);
}

void test_week_view() {
  Model m = fresh();
  m.rollover(FRI_1800 - 4 * 86400);  // 周一
  m.toggle(0);
  m.toggle(1);
  m.rollover(FRI_1800 - 3 * 86400);  // 周二没做
  m.rollover(FRI_1800);              // 周五（周三周四没开机）
  auto w = m.weekView();
  uint8_t expect[7] = {1, 2, 2, 2, 4, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, w.data(), 7);
}

// ---------------------------------------------------------------- 屏幕刷新

void test_rle_roundtrip() {
  std::vector<uint8_t> a(FB_BYTES, 0xFF), b(FB_BYTES);
  for (size_t i = 0; i < FB_BYTES; i += 97) a[i] = uint8_t(i);
  std::string enc = rleEncode(a.data());
  TEST_ASSERT_TRUE(rleDecode(enc, b.data()));
  TEST_ASSERT_EQUAL_MEMORY(a.data(), b.data(), FB_BYTES);
  TEST_ASSERT_FALSE(rleDecode(enc.substr(0, enc.size() - 2), b.data()));
}

void test_partial_after_deep_sleep_is_correct() {
  TestFs fs;
  EpdEmu epd;
  RtcData rtc;
  auto paint = [](Canvas& c, int v) {
    c.clear();
    c.fillRect(10 + v * 30, 10, 200, 100, BLACK);
  };
  {
    Screen s(epd, fs, rtc);
    s.begin(true);
    paint(s.canvas(), 0);
    s.present();
    TEST_ASSERT_EQUAL_INT(1, epd.fullCount);
    s.sleep();
  }
  {  // 深睡醒来：新的 Screen 对象（内存全丢），这次应该走局刷且画面正确
    Screen s(epd, fs, rtc);
    s.begin(false);
    paint(s.canvas(), 1);
    s.present();
    TEST_ASSERT_EQUAL_INT(1, epd.partialCount);
    TEST_ASSERT_EQUAL_MEMORY(s.canvas().data(), epd.physical(), FB_BYTES);
    paint(s.canvas(), 2);
    s.present();
    TEST_ASSERT_EQUAL_MEMORY(s.canvas().data(), epd.physical(), FB_BYTES);
    TEST_ASSERT_EQUAL_INT(0, epd.errors);
  }
}

// 反向验证：如果醒来后不把上一帧写回 old RAM，模拟的控制器必须能暴露出残留像素
void test_emulator_catches_missing_previous() {
  EpdEmu epd;
  std::vector<uint8_t> a(FB_BYTES, 0xFF), b(FB_BYTES, 0xFF);
  memset(a.data(), 0x00, FB_BYTES / 2);  // 上半屏黑
  epd.begin(true);
  epd.writeNew(a.data());
  epd.refreshFull();
  epd.hibernate();
  epd.begin(false);
  epd.writeNew(b.data());  // 全白，但漏了 writePrevious
  epd.refreshPartial({0, 0, W, H});
  TEST_ASSERT_TRUE(memcmp(epd.physical(), b.data(), FB_BYTES) != 0);
}

void test_forced_full_refresh_every_n() {
  TestFs fs;
  EpdEmu epd;
  RtcData rtc;
  Screen s(epd, fs, rtc);
  s.begin(true);
  // 第 1 次全刷，之后每 20 次局刷插 1 次全刷：第 1、22、43 次是全刷
  for (int i = 0; i < 2 * (FULL_REFRESH_EVERY + 1) + 1; ++i) {
    s.canvas().clear();
    s.canvas().fillRect(i * 8, 0, 8, 8, BLACK);
    s.present();
  }
  TEST_ASSERT_EQUAL_INT(3, epd.fullCount);
}

// ---------------------------------------------------------------- App 流程

void test_app_check_all_and_celebrate() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  TEST_ASSERT_TRUE(r.app->model().counts());
  const int n = int(r.app->model().today.items.size());
  r.assertPanelMatches();
  for (int i = 0; i < n; ++i) {
    r.press(Key::Ok);
    if (i < n - 1) r.press(Key::Down);
  }
  TEST_ASSERT_TRUE(r.app->model().allDone());
  TEST_ASSERT_EQUAL_INT(int(View::Celebrate), int(r.app->uiState().view));
  r.run(CELEBRATE_MS + 100);
  TEST_ASSERT_EQUAL_INT(int(View::Main), int(r.app->uiState().view));
  r.assertPanelMatches();
  r.run(IDLE_MS + 100);
  TEST_ASSERT_TRUE(r.app->asleep());
  TEST_ASSERT_EQUAL_INT64(r.app->model().nextRolloverUtc(r.p.utc), r.p.sleptUntil);
}

void test_wake_key_only_shows_cursor() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  r.run(IDLE_MS + 100);
  TEST_ASSERT_TRUE(r.app->asleep());
  // 按确认键唤醒：不应该勾选任何东西
  r.p.keys[2] = true;
  r.boot(WakeReason::Key, Key::Ok);
  r.run(100);
  r.p.keys[2] = false;
  r.run(100);
  TEST_ASSERT_EQUAL_INT(0, int(r.app->model().doneCount()));
  TEST_ASSERT_TRUE(r.app->uiState().showCursor);
  r.assertPanelMatches();
  r.press(Key::Ok);  // 第二下才勾
  TEST_ASSERT_EQUAL_INT(1, int(r.app->model().doneCount()));
  r.assertPanelMatches();
}

void test_long_press_enters_edit_and_web_validation() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  r.press(Key::Ok, LONG_PRESS_MS + 100);
  TEST_ASSERT_EQUAL_INT(int(View::Edit), int(r.app->uiState().view));
  TEST_ASSERT_EQUAL_INT(0, int(r.app->model().doneCount()));  // 长按不算勾选

  auto res = r.http("GET", "/api/state");
  TEST_ASSERT_EQUAL_INT(200, res.status);
  const std::string today = dayToIso(r.app->model().today.day);
  res = r.http("PUT", "/api/day", R"({"rest":false,"items":[{"text":"画画😀","icon":"huahua"}]})", {{"date", today}});
  TEST_ASSERT_EQUAL_INT(400, res.status);
  TEST_ASSERT_TRUE(res.body.find("显示不了") != std::string::npos);
  res = r.http("PUT", "/api/day", R"({"items":[{"text":"一二三四五六七八九十十一"}]})", {{"date", today}});
  TEST_ASSERT_EQUAL_INT(400, res.status);
  res = r.http("PUT", "/api/day", R"({"items":[{"text":" 背古诗 ","icon":"nope"}]})", {{"date", today}});
  TEST_ASSERT_EQUAL_INT(200, res.status);
  TEST_ASSERT_EQUAL_STRING("背古诗", r.app->model().today.items[0].text.c_str());
  TEST_ASSERT_EQUAL_STRING("star", r.app->model().today.items[0].icon.c_str());
  res = r.http("PUT", "/api/day", R"({"items":[]})", {{"date", "2020-01-01"}});
  TEST_ASSERT_EQUAL_INT(400, res.status);

  r.http("POST", "/api/exit");
  r.run(50);
  TEST_ASSERT_EQUAL_INT(int(View::Main), int(r.app->uiState().view));
  r.assertPanelMatches();
  r.run(IDLE_MS + 100);
  TEST_ASSERT_TRUE(r.app->asleep());
  TEST_ASSERT_EQUAL_INT(503, r.http("GET", "/api/state").status);  // 睡着了不响应
}

void test_edit_mode_times_out() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  r.press(Key::Ok, LONG_PRESS_MS + 100);
  r.run(EDIT_IDLE_MS + 1000);
  TEST_ASSERT_EQUAL_INT(int(NetMode::Off), int(r.app->netMode()));
}

void test_state_survives_power_loss() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  r.press(Key::Ok);
  TestFs saved = r.p.fsImpl;
  r.p = TestPlatform();  // 新硬件状态（RTC 内存也丢）
  r.p.utc = FRI_1800 + 60;
  r.p.fsImpl = saved;
  r.boot(WakeReason::PowerOn);
  TEST_ASSERT_EQUAL_INT(1, int(r.app->model().doneCount()));
  r.assertPanelMatches();
}

void test_timer_wake_rolls_over_and_sleeps() {
  Rig r;
  r.boot(WakeReason::PowerOn);
  r.run(IDLE_MS + 100);
  const int64_t wake = r.p.sleptUntil;
  r.p.utc = wake + 1;
  r.boot(WakeReason::Timer);
  TEST_ASSERT_TRUE(r.app->asleep());
  TEST_ASSERT_EQUAL_INT(daysFromCivil(2026, 10, 17), r.app->model().today.day);
  TEST_ASSERT_EQUAL_INT64(wake + 86400, r.p.sleptUntil);
  r.assertPanelMatches();
}

void test_no_wifi_goes_to_setup() {
  Rig r;
  r.p.utc = 0;
  r.p.wifiOk = false;
  r.boot(WakeReason::PowerOn);
  TEST_ASSERT_EQUAL_INT(int(View::NoTime), int(r.app->uiState().view));
  r.press(Key::Ok, LONG_PRESS_MS + 100);
  TEST_ASSERT_EQUAL_INT(int(View::Setup), int(r.app->uiState().view));
  r.assertPanelMatches();
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_date_roundtrip);
  RUN_TEST(test_logical_day_switches_at_reset_hour);
  RUN_TEST(test_stars_and_streak);
  RUN_TEST(test_rest_day_keeps_streak);
  RUN_TEST(test_gap_days_break_streak);
  RUN_TEST(test_template_change_keeps_done_flags);
  RUN_TEST(test_week_view);
  RUN_TEST(test_rle_roundtrip);
  RUN_TEST(test_partial_after_deep_sleep_is_correct);
  RUN_TEST(test_emulator_catches_missing_previous);
  RUN_TEST(test_forced_full_refresh_every_n);
  RUN_TEST(test_app_check_all_and_celebrate);
  RUN_TEST(test_wake_key_only_shows_cursor);
  RUN_TEST(test_long_press_enters_edit_and_web_validation);
  RUN_TEST(test_edit_mode_times_out);
  RUN_TEST(test_state_survives_power_loss);
  RUN_TEST(test_timer_wake_rolls_over_and_sleeps);
  RUN_TEST(test_no_wifi_goes_to_setup);
  return UNITY_END();
}
