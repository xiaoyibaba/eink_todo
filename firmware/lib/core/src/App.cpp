#include "App.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <set>

#include "DateUtil.h"

namespace et {

namespace {

const Note TICK[] = {{1568, 40}};
const Note UNTICK[] = {{784, 40}};
const Note FANFARE[] = {{523, 120}, {659, 120}, {784, 120}, {1047, 320}};

const Key KEYS[3] = {Key::Up, Key::Down, Key::Ok};

HttpResponse json(const JsonDocument& doc, int status = 200) {
  HttpResponse r;
  r.status = status;
  serializeJson(doc, r.body);
  return r;
}

HttpResponse error(int status, const std::string& msg) {
  JsonDocument doc;
  doc["error"] = msg;
  return json(doc, status);
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

void itemsTo(JsonArray arr, const std::vector<Item>& items, bool withDone) {
  for (auto& it : items) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = it.id;
    o["text"] = it.text;
    o["icon"] = it.icon;
    if (withDone) o["done"] = it.done;
  }
}

}  // namespace

App::App(Platform& p)
    : p_(p), uiDraw_(assets_), screen_(p.epd(), p.fs(), p.rtc()), store_(p.fs()) {
  p_.setHttpHandler([this](const HttpRequest& r) { return http(r); });
}

void App::boot(WakeReason why, Key wakeKey) {
  if (!assets_.load(p_.fs())) p_.log("assets missing: run tools/build_assets.py and upload data/");
  if (!store_.load(m_)) {
    m_.loadDefaults();
    store_.saveAll(m_);
  }
  RtcData& rtc = p_.rtc();
  if (why == WakeReason::PowerOn || rtc.magic != RTC_MAGIC) {
    rtc = RtcData{};
    rtc.magic = RTC_MAGIC;
  }
  screen_.begin(why == WakeReason::PowerOn);
  ui_.battery = p_.batteryPercent();
  rtc.battery = static_cast<int8_t>(ui_.battery);

  int64_t now = p_.nowUtc();
  // 按键唤醒要快，不联网；定时唤醒（每天换日）和开机时校一次时
  if (why != WakeReason::Key || now == 0) {
    if (p_.syncTime()) now = p_.nowUtc();
  }
  lastActivity_ = p_.millis();
  timeKnown_ = now != 0;
  if (!timeKnown_) {
    ui_.view = View::NoTime;
    render(true);
    return;
  }
  if (m_.rollover(now)) {
    store_.saveToday(m_);
    store_.saveStats(m_);
    store_.saveOverrides(m_);
  }
  ui_.view = View::Main;
  ui_.cursor = firstUndone();
  switch (why) {
    case WakeReason::Timer:
      ui_.showCursor = false;
      render(true);  // 每天换日全刷一次，顺便清残影
      goSleep();
      return;
    case WakeReason::Key: {
      ui_.showCursor = true;
      // 唤醒的那一下只负责亮光标；如果一直按着确认键，照样能长按进设置
      int i = static_cast<int>(wakeKey) - 1;
      if (i >= 0 && i < 3 && p_.keyDown(wakeKey)) {
        keys_[i].down = true;
        keys_[i].since = keys_[i].changedAt = p_.millis();
        keys_[i].swallow = true;
      }
      render();
      break;
    }
    case WakeReason::PowerOn:
      ui_.showCursor = true;
      render(true);
      break;
  }
}

int App::firstUndone() const {
  for (size_t i = 0; i < m_.today.items.size(); ++i)
    if (!m_.today.items[i].done) return static_cast<int>(i);
  return 0;
}

void App::touch() { lastActivity_ = p_.millis(); }

void App::render(bool forceFull) {
  int64_t now = p_.nowUtc();
  if (now) ui_.localSecs = m_.localSecs(now);
  uiDraw_.render(screen_.canvas(), m_, ui_);
  screen_.present(forceFull);
}

void App::saveDay() { store_.saveToday(m_); }

void App::loop() {
  if (asleep_) return;
  if (net_ != NetMode::Off) p_.pollNetwork();
  pollKeys();
  if (asleep_) return;

  const uint32_t ms = p_.millis();
  if (pendingExit_) {
    pendingExit_ = false;
    exitEdit();
  }
  if (pendingRestartNet_) {
    pendingRestartNet_ = false;
    p_.stopNetwork();
    net_ = NetMode::Off;
    enterEdit();
  }
  const int64_t now = p_.nowUtc();
  if (now && timeKnown_ && m_.rollover(now)) {
    store_.saveToday(m_);
    store_.saveStats(m_);
    store_.saveOverrides(m_);
    ui_.cursor = firstUndone();
    if (ui_.view == View::Main || ui_.view == View::Celebrate) {
      ui_.view = View::Main;
      render(true);
    }
  }
  if (ui_.view == View::Celebrate && static_cast<int32_t>(ms - celebrateUntil_) >= 0) {
    ui_.view = View::Main;
    render(true);
  }
  if (net_ != NetMode::Off) {
    if (ms - lastActivity_ > EDIT_IDLE_MS) exitEdit();
  } else if (ms - lastActivity_ > IDLE_MS && ui_.view != View::Celebrate) {
    goSleep();
  }
}

void App::pollKeys() {
  const uint32_t ms = p_.millis();
  for (int i = 0; i < 3; ++i) {
    KeyState& k = keys_[i];
    const bool level = p_.keyDown(KEYS[i]);
    if (level != k.down && ms - k.changedAt >= 25) {  // 25ms 去抖
      k.changedAt = ms;
      k.down = level;
      if (level) {
        k.since = ms;
        k.longFired = false;
        onPress(KEYS[i]);
      } else {
        if (!k.longFired && !k.swallow) onShortRelease(KEYS[i]);
        k.swallow = false;
      }
    }
    if (k.down && !k.longFired && KEYS[i] == Key::Ok && ms - k.since >= LONG_PRESS_MS) {
      k.longFired = true;
      onLongPress(Key::Ok);
    }
  }
}

void App::onPress(Key k) {
  touch();
  if (ui_.view == View::Celebrate) {
    keys_[static_cast<int>(k) - 1].swallow = true;
    ui_.view = View::Main;
    render(true);
    return;
  }
  if (ui_.view != View::Main || !m_.counts()) return;
  if (!ui_.showCursor) {
    keys_[static_cast<int>(k) - 1].swallow = true;
    ui_.showCursor = true;
    render();
    return;
  }
  const int n = static_cast<int>(m_.today.items.size());
  if (k == Key::Up) ui_.cursor = (ui_.cursor - 1 + n) % n;
  if (k == Key::Down) ui_.cursor = (ui_.cursor + 1) % n;
  if (k != Key::Ok) render();
}

void App::onShortRelease(Key k) {
  touch();
  if (k != Key::Ok) return;
  if (ui_.view == View::Edit || ui_.view == View::Setup) {
    exitEdit();
    return;
  }
  if (ui_.view != View::Main || !m_.counts() || !ui_.showCursor) return;
  const size_t idx = static_cast<size_t>(ui_.cursor);
  const bool finished = m_.toggle(idx);
  saveDay();
  if (finished) {
    if (m_.settings.buzzer) p_.beep(FANFARE, 4);
    ui_.view = View::Celebrate;
    celebrateUntil_ = p_.millis() + CELEBRATE_MS;
    render(true);
    return;
  }
  if (m_.settings.buzzer) p_.beep(m_.today.items[idx].done ? TICK : UNTICK, 1);
  render();
}

void App::onLongPress(Key k) {
  touch();
  if (k == Key::Ok && net_ == NetMode::Off) enterEdit();
}

void App::enterEdit() {
  std::string url;
  net_ = p_.startNetwork(url);
  if (net_ == NetMode::Off) net_ = NetMode::SetupAp;
  ui_.url = url;
  ui_.apSsid = AP_SSID;
  if (net_ == NetMode::Station && !timeKnown_ && p_.syncTime()) timeKnown_ = true;
  ui_.view = net_ == NetMode::Station ? View::Edit : View::Setup;
  ui_.showCursor = false;
  render(true);
  touch();
}

void App::exitEdit() {
  if (net_ == NetMode::Station && p_.syncTime()) timeKnown_ = true;
  p_.stopNetwork();
  net_ = NetMode::Off;
  const int64_t now = p_.nowUtc();
  if (timeKnown_ && now) {
    if (m_.rollover(now)) store_.saveAll(m_);
    ui_.view = View::Main;
    ui_.cursor = std::min(ui_.cursor, std::max(0, static_cast<int>(m_.today.items.size()) - 1));
    ui_.showCursor = true;
  } else {
    ui_.view = View::NoTime;
  }
  render(true);  // 二维码面积大，全刷避免残影
  touch();
}

void App::goSleep() {
  if (net_ != NetMode::Off) {
    p_.stopNetwork();
    net_ = NetMode::Off;
  }
  ui_.showCursor = false;
  if (ui_.view == View::Celebrate || ui_.view == View::Edit || ui_.view == View::Setup)
    ui_.view = timeKnown_ ? View::Main : View::NoTime;
  render();
  saveDay();
  screen_.sleep();
  asleep_ = true;
  const int64_t now = p_.nowUtc();
  p_.deepSleep(now && timeKnown_ ? m_.nextRolloverUtc(now) : 0);
}

// ---------------------------------------------------------------- 网页 API

namespace {

// 校验网页提交的任务列表。出错返回错误信息。
std::string parseItems(JsonArrayConst arr, Model& m, const Font& font, std::vector<Item>& out) {
  out.clear();
  if (arr.isNull()) return "缺少任务列表";
  if (arr.size() > MAX_ITEMS) return "每天最多 " + std::to_string(MAX_ITEMS) + " 条任务";
  std::set<std::string> seen;
  for (JsonObjectConst o : arr) {
    Item it;
    it.text = trim(o["text"] | "");
    if (it.text.empty()) return "有任务没写内容";
    if (codepointCount(it.text) > MAX_TEXT_CHARS)
      return "「" + it.text + "」太长了，最多 " + std::to_string(MAX_TEXT_CHARS) + " 个字";
    for (size_t i = 0; i < it.text.size();) {
      size_t start = i;
      uint32_t cp = nextCodepoint(it.text, i);
      if (!font.has(cp)) return "「" + it.text + "」里的「" + it.text.substr(start, i - start) + "」屏幕显示不了";
    }
    it.icon = o["icon"] | "star";
    if (!isTaskIcon(it.icon)) it.icon = "star";
    it.id = o["id"] | "";
    bool idOk = it.id.size() > 1 && it.id[0] == 'i' && it.id.size() < 12 && !seen.count(it.id);
    for (char ch : it.id) idOk = idOk && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z'));
    if (!idOk) it.id = m.newId();
    seen.insert(it.id);
    out.push_back(it);
  }
  return "";
}

}  // namespace

HttpResponse App::http(const HttpRequest& req) {
  if (net_ == NetMode::Off) return error(503, "设备不在编辑模式");
  touch();
  const std::string& p = req.path;
  const std::string& m = req.method;
  if (p == "/api/state" && m == "GET") return apiState();
  if (p == "/api/day" && m == "GET") return apiGetDay(req);
  if (p == "/api/day" && m == "PUT") return apiPutDay(req);
  if (p == "/api/day" && m == "DELETE") return apiDeleteDay(req);
  if (p == "/api/templates" && m == "GET") return apiGetTemplates();
  if (p == "/api/templates" && m == "PUT") return apiPutTemplates(req);
  if (p == "/api/stars" && m == "POST") return apiStars(req);
  if (p == "/api/settings" && m == "PUT") return apiSettings(req);
  if (p == "/api/wifi" && m == "POST") return apiWifi(req);
  if (p == "/api/exit" && m == "POST") {
    pendingExit_ = true;
    return json(JsonDocument());
  }
  return error(404, "没有这个接口");
}

HttpResponse App::apiState() {
  JsonDocument doc;
  doc["mode"] = net_ == NetMode::Station ? "edit" : "setup";
  doc["timeKnown"] = timeKnown_;
  if (timeKnown_) {
    JsonObject t = doc["today"].to<JsonObject>();
    t["date"] = dayToIso(m_.today.day);
    t["weekday"] = weekdayMon0(m_.today.day);
    t["rest"] = m_.today.rest;
    t["custom"] = m_.today.custom;
    itemsTo(t["items"].to<JsonArray>(), m_.today.items, true);
    doc["tomorrow"] = dayToIso(m_.today.day + 1);
  }
  doc["stars"] = m_.displayStars();
  doc["streak"] = m_.displayStreak();
  JsonArray week = doc["week"].to<JsonArray>();
  for (uint8_t v : m_.weekView()) week.add(v);
  JsonArray hist = doc["history"].to<JsonArray>();
  for (auto& h : m_.stats.history) {
    JsonObject o = hist.add<JsonObject>();
    o["date"] = dayToIso(h.day);
    o["s"] = int(h.status);
    o["d"] = h.done;
    o["t"] = h.total;
  }
  doc["settings"]["resetHour"] = m_.settings.resetHour;
  doc["settings"]["buzzer"] = m_.settings.buzzer;
  doc["battery"] = ui_.battery;
  JsonArray icons = doc["icons"].to<JsonArray>();
  for (auto& i : taskIcons()) {
    JsonObject o = icons.add<JsonObject>();
    o["name"] = i.name;
    o["label"] = i.label;
  }
  doc["limits"]["maxItems"] = MAX_ITEMS;
  doc["limits"]["maxChars"] = MAX_TEXT_CHARS;
  return json(doc);
}

HttpResponse App::apiGetDay(const HttpRequest& req) {
  auto q = req.query.find("date");
  int day = q == req.query.end() ? INT32_MIN : isoToDay(q->second);
  if (day == INT32_MIN) return error(400, "日期格式不对");
  JsonDocument doc;
  doc["date"] = dayToIso(day);
  doc["weekday"] = weekdayMon0(day);
  doc["isToday"] = day == m_.today.day;
  doc["custom"] = m_.overrides.count(day) > 0;
  if (day == m_.today.day) {
    doc["rest"] = m_.today.rest;
    itemsTo(doc["items"].to<JsonArray>(), m_.today.items, true);
  } else {
    DayPlan pl = m_.planFor(day);
    doc["rest"] = pl.rest;
    itemsTo(doc["items"].to<JsonArray>(), pl.items, false);
  }
  return json(doc);
}

HttpResponse App::apiPutDay(const HttpRequest& req) {
  auto q = req.query.find("date");
  int day = q == req.query.end() ? INT32_MIN : isoToDay(q->second);
  if (day == INT32_MIN) return error(400, "日期格式不对");
  if (day < m_.today.day) return error(400, "不能改已经过去的日子");
  JsonDocument body;
  if (deserializeJson(body, req.body)) return error(400, "请求格式不对");
  DayPlan plan;
  plan.rest = body["rest"] | false;
  std::string err = parseItems(body["items"], m_, assets_.f32, plan.items);
  if (!err.empty()) return error(400, err);
  m_.overrides[day] = plan;
  store_.saveOverrides(m_);
  if (day == m_.today.day) {
    m_.refreshToday();
    ui_.cursor = 0;
    saveDay();
  }
  store_.saveTemplates(m_);  // nextId 可能变了
  return apiGetDay(req);
}

HttpResponse App::apiDeleteDay(const HttpRequest& req) {
  auto q = req.query.find("date");
  int day = q == req.query.end() ? INT32_MIN : isoToDay(q->second);
  if (day == INT32_MIN) return error(400, "日期格式不对");
  m_.overrides.erase(day);
  store_.saveOverrides(m_);
  if (day == m_.today.day) {
    m_.refreshToday();
    ui_.cursor = 0;
    saveDay();
  }
  return apiGetDay(req);
}

HttpResponse App::apiGetTemplates() {
  JsonDocument doc;
  JsonArray days = doc["days"].to<JsonArray>();
  for (auto& t : m_.templates) itemsTo(days.add<JsonArray>(), t, false);
  return json(doc);
}

HttpResponse App::apiPutTemplates(const HttpRequest& req) {
  JsonDocument body;
  if (deserializeJson(body, req.body)) return error(400, "请求格式不对");
  JsonArrayConst days = body["days"];
  if (days.size() != 7) return error(400, "需要 7 天的模板");
  std::array<std::vector<Item>, 7> next;
  static const char* names[7] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
  for (int d = 0; d < 7; ++d) {
    std::string err = parseItems(days[d], m_, assets_.f32, next[d]);
    if (!err.empty()) return error(400, std::string(names[d]) + "：" + err);
  }
  m_.templates = next;
  store_.saveTemplates(m_);
  if (!m_.today.custom) {
    m_.refreshToday();
    ui_.cursor = 0;
    saveDay();
  }
  return apiGetTemplates();
}

HttpResponse App::apiStars(const HttpRequest& req) {
  JsonDocument body;
  if (deserializeJson(body, req.body)) return error(400, "请求格式不对");
  int delta = body["delta"] | 0;
  if (delta < -100 || delta > 100) return error(400, "一次最多调整 100 颗");
  m_.stats.stars = std::max(0, m_.stats.stars + delta);
  store_.saveStats(m_);
  return apiState();
}

HttpResponse App::apiSettings(const HttpRequest& req) {
  JsonDocument body;
  if (deserializeJson(body, req.body)) return error(400, "请求格式不对");
  int h = body["resetHour"] | m_.settings.resetHour;
  if (h < 0 || h > 23) return error(400, "换日时间要在 0–23 点之间");
  m_.settings.resetHour = h;
  m_.settings.buzzer = body["buzzer"] | m_.settings.buzzer;
  store_.saveSettings(m_);
  return apiState();
}

HttpResponse App::apiWifi(const HttpRequest& req) {
  JsonDocument body;
  if (deserializeJson(body, req.body)) return error(400, "请求格式不对");
  std::string ssid = trim(body["ssid"] | "");
  std::string pass = body["pass"] | "";
  if (ssid.empty()) return error(400, "WiFi 名称不能为空");
  if (!p_.saveWifi(ssid, pass)) return error(500, "保存失败");
  pendingRestartNet_ = true;  // 回完这个请求再断开热点去连家里的 WiFi
  return json(JsonDocument());
}

}  // namespace et
