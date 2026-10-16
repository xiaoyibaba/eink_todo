#include "Canvas.h"

#include <cstdlib>

namespace et {

void Canvas::fillRect(int x, int y, int w, int h, Color c) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  for (int yy = y0; yy < y1; ++yy)
    for (int xx = x0; xx < x1; ++xx) pixel(xx, yy, c);
}

void Canvas::rect(int x, int y, int w, int h, int t, Color c) {
  fillRect(x, y, w, t, c);
  fillRect(x, y + h - t, w, t, c);
  fillRect(x, y, t, h, c);
  fillRect(x + w - t, y, t, h, c);
}

void Canvas::invertRect(int x, int y, int w, int h) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
  for (int yy = y0; yy < y1; ++yy)
    for (int xx = x0; xx < x1; ++xx) buf_[(yy * W + xx) >> 3] ^= 0x80 >> (xx & 7);
}

void Canvas::halftone(int x, int y, int w, int h) {
  for (int yy = y; yy < y + h; ++yy)
    for (int xx = x + ((x + yy) & 1); xx < x + w; xx += 2) pixel(xx, yy, WHITE);
}

void Canvas::thickLine(int x0, int y0, int x1, int y1, int t, Color c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, r = t / 2;
  for (;;) {
    fillRect(x0 - r, y0 - r, t, t, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void Canvas::blit(const uint8_t* bits, int w, int h, int x, int y, Color c) {
  int stride = (w + 7) / 8;
  for (int yy = 0; yy < h; ++yy) {
    const uint8_t* row = bits + yy * stride;
    for (int xx = 0; xx < w; ++xx)
      if (row[xx >> 3] & (0x80 >> (xx & 7))) pixel(x + xx, y + yy, c);
  }
}

}  // namespace et
