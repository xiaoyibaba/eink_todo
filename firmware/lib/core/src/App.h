// 设备主流程：开机/唤醒 → 处理按键 → 空闲就深睡。家长网页的 API 也在这里。
//
// ESP32 深睡会丢掉全部内存，醒来相当于重新开机，所以 App 每次 boot 都从 flash 读状态。
// 模拟器为了模拟这一点，每次深睡都销毁 App 对象，醒来再新建一个。
#pragma once
#include <string>

#include "Hal.h"
#include "Model.h"
#include "Screen.h"
#include "Store.h"
#include "Ui.h"

namespace et {

constexpr uint32_t IDLE_MS = 30 * 1000;
constexpr uint32_t EDIT_IDLE_MS = 10 * 60 * 1000;
constexpr uint32_t LONG_PRESS_MS = 3000;
constexpr uint32_t CELEBRATE_MS = 4000;
constexpr size_t MAX_TEXT_CHARS = 10;
constexpr const char* AP_SSID = "EinkTodo-Setup";

class App {
 public:
  explicit App(Platform& p);
  void boot(WakeReason why, Key wakeKey);
  void loop();
  bool asleep() const { return asleep_; }
  HttpResponse http(const HttpRequest& req);

  // 给模拟器状态栏和测试用
  const Model& model() const { return m_; }
  const UiState& uiState() const { return ui_; }
  NetMode netMode() const { return net_; }
  const uint8_t* frame() { return screen_.canvas().data(); }

 private:
  struct KeyState {
    bool down = false;
    uint32_t since = 0, changedAt = 0;
    bool longFired = false;
    bool swallow = false;  // 唤醒用的那一下：松开时不算短按
  };

  void pollKeys();
  void onPress(Key k);
  void onShortRelease(Key k);
  void onLongPress(Key k);
  void render(bool forceFull = false);
  void goSleep();
  void enterEdit();
  void exitEdit();
  void touch();
  int firstUndone() const;
  void saveDay();

  HttpResponse apiState();
  HttpResponse apiGetDay(const HttpRequest& req);
  HttpResponse apiPutDay(const HttpRequest& req);
  HttpResponse apiDeleteDay(const HttpRequest& req);
  HttpResponse apiGetTemplates();
  HttpResponse apiPutTemplates(const HttpRequest& req);
  HttpResponse apiStars(const HttpRequest& req);
  HttpResponse apiSettings(const HttpRequest& req);
  HttpResponse apiWifi(const HttpRequest& req);

  Platform& p_;
  Assets assets_;
  Ui uiDraw_;
  Screen screen_;
  Store store_;
  Model m_;
  UiState ui_;
  KeyState keys_[3];
  uint32_t lastActivity_ = 0, celebrateUntil_ = 0;
  bool asleep_ = false, timeKnown_ = false;
  bool pendingExit_ = false, pendingRestartNet_ = false;
  NetMode net_ = NetMode::Off;
};

}  // namespace et
