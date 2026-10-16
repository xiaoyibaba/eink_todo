#include "EpdEmu.h"

#include <cstring>

namespace et {

EpdEmu::EpdEmu() : old_(FB_BYTES), new_(FB_BYTES), phys_(FB_BYTES, 0xFF) { scramble(); }

void EpdEmu::scramble() {
  for (auto* v : {&old_, &new_})
    for (auto& b : *v) {
      seed_ = seed_ * 1103515245 + 12345;
      b = static_cast<uint8_t>(seed_ >> 16);
    }
}

bool EpdEmu::check(const char* op) {
  if (!asleep_) return true;
  errors++;
  lastError = std::string(op) + " while controller is in deep sleep (missing begin/reset)";
  return false;
}

void EpdEmu::begin(bool) { asleep_ = false; }  // 复位后 RAM 仍是随机值

void EpdEmu::writePrevious(const uint8_t* fb) {
  if (check("writePrevious")) memcpy(old_.data(), fb, FB_BYTES);
}

void EpdEmu::writeNew(const uint8_t* fb) {
  if (check("writeNew")) memcpy(new_.data(), fb, FB_BYTES);
}

void EpdEmu::refreshFull() {
  if (!check("refreshFull")) return;
  phys_ = new_;
  old_ = new_;
  fullCount++;
  lastRefresh = {0, 0, W, H};
  lastWasFull = true;
  if (delayFn && fullMs) delayFn(fullMs);
}

void EpdEmu::refreshPartial(Rect r) {
  if (!check("refreshPartial")) return;
  for (int y = r.y; y < r.y + r.h && y < H; ++y)
    for (int x = r.x; x < r.x + r.w && x < W; ++x) {
      size_t i = (y * W + x) >> 3;
      uint8_t m = 0x80 >> (x & 7);
      if ((old_[i] ^ new_[i]) & m) phys_[i] = (phys_[i] & ~m) | (new_[i] & m);
    }
  old_ = new_;
  partialCount++;
  lastRefresh = r;
  lastWasFull = false;
  if (delayFn && partialMs) delayFn(partialMs);
}

void EpdEmu::hibernate() {
  if (!check("hibernate")) return;
  asleep_ = true;
  scramble();
}

}  // namespace et
