// UC8179 控制器的行为模型（只给模拟器和单元测试用，真机不链接）。
//
// 模拟的是会导致「画面不对」的那部分语义：
// - old / new 两块 RAM；全刷：所有像素驱动成 new；局刷：窗口内只驱动 old != new 的像素
// - 刷新后 new 拷到 old（GxEPD2 设置了 N2OCP）
// - 深睡（hibernate）后两块 RAM 内容随机；深睡状态下写入会被忽略并记错
// physical() 是「玻璃上实际显示的东西」，模拟器窗口和截图都画它。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Hal.h"

namespace et {

class EpdEmu : public Epd {
 public:
  EpdEmu();
  void begin(bool coldBoot) override;
  void writePrevious(const uint8_t* fb) override;
  void writeNew(const uint8_t* fb) override;
  void refreshFull() override;
  void refreshPartial(Rect r) override;
  void hibernate() override;

  const uint8_t* physical() const { return phys_.data(); }
  int fullCount = 0, partialCount = 0, errors = 0;
  Rect lastRefresh;
  bool lastWasFull = false;
  std::string lastError;
  // 交互模式下模拟刷新耗时（毫秒）；测试和脚本模式为 0
  uint32_t fullMs = 0, partialMs = 0;
  void (*delayFn)(uint32_t) = nullptr;

 private:
  bool check(const char* op);
  void scramble();
  std::vector<uint8_t> old_, new_, phys_;
  bool asleep_ = true;
  uint32_t seed_ = 12345;
};

}  // namespace et
