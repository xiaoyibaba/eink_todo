// 屏幕刷新管理：决定全刷还是局刷，记住屏上当前是什么。
//
// 关键点：控制器深睡（hibernate）后 old/new 两块 RAM 都会丢。醒来第一次局刷前，
// 必须把「屏上现在的画面」写回 old RAM，否则差分局刷会漏驱动像素、留下残影。
// 所以每次睡前把屏上画面压缩存进 flash（/data/frame.rle），醒来读回。
#pragma once
#include <cstdint>
#include <string>

#include "Canvas.h"
#include "Hal.h"

namespace et {

constexpr int FULL_REFRESH_EVERY = 20;  // 局刷累计次数到了就插一次全刷，防残影

std::string rleEncode(const uint8_t* fb);
bool rleDecode(const std::string& in, uint8_t* fb);
Rect diffRect(const uint8_t* a, const uint8_t* b);  // x、w 按 8 像素对齐

class Screen {
 public:
  Screen(Epd& epd, Fs& fs, RtcData& rtc);
  ~Screen();
  void begin(bool coldBoot);
  Canvas& canvas() { return canvas_; }
  // 把 canvas 推到屏上。返回是否真的刷新了（画面没变就不刷）。
  bool present(bool forceFull = false);
  void sleep();  // 睡前调用：保存屏上画面，控制器深睡
  uint16_t partialCount() const { return rtc_.partialCount; }

 private:
  Epd& epd_;
  Fs& fs_;
  RtcData& rtc_;
  uint8_t* frame_;
  uint8_t* shown_;
  Canvas canvas_;
  bool shownValid_ = false;  // shown_ 是否等于屏上实际内容
  bool oldRamValid_ = false; // 控制器 old RAM 是否等于 shown_
  bool shownDirty_ = false;  // shown_ 改过、还没存盘
};

}  // namespace et
