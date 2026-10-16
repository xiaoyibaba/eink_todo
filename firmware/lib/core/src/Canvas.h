// 800x480 单色帧缓冲，格式同 GxEPD2：1 bit/像素，bit7 在左，1 = 白。
#pragma once
#include <cstdint>
#include <cstring>

#include "Hal.h"

namespace et {

enum Color : uint8_t { BLACK = 0, WHITE = 1 };

class Canvas {
 public:
  explicit Canvas(uint8_t* buf) : buf_(buf) {}
  uint8_t* data() { return buf_; }
  const uint8_t* data() const { return buf_; }

  void clear(Color c = WHITE) { memset(buf_, c == WHITE ? 0xFF : 0x00, FB_BYTES); }
  void pixel(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t& b = buf_[(y * W + x) >> 3];
    uint8_t m = 0x80 >> (x & 7);
    if (c == WHITE) b |= m; else b &= ~m;
  }
  bool get(int x, int y) const { return buf_[(y * W + x) >> 3] & (0x80 >> (x & 7)); }

  void fillRect(int x, int y, int w, int h, Color c);
  void rect(int x, int y, int w, int h, int thick, Color c);  // 空心框，线宽向内
  void invertRect(int x, int y, int w, int h);
  // 区域内黑点只留一半（棋盘格），看起来是灰色。墨水屏只有黑白，用它表示「已完成、淡化」
  void halftone(int x, int y, int w, int h);
  void thickLine(int x0, int y0, int x1, int y1, int thick, Color c);
  // 1 位图（行按字节对齐，bit7 在左，1 = 着色）
  void blit(const uint8_t* bits, int w, int h, int x, int y, Color c);

 private:
  uint8_t* buf_;
};

}  // namespace et
