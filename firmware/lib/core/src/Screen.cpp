#include "Screen.h"

#include <cstdlib>
#include <cstring>

#include "Store.h"

namespace et {

std::string rleEncode(const uint8_t* fb) {
  std::string out = "FRM1";
  for (size_t i = 0; i < FB_BYTES;) {
    uint8_t v = fb[i];
    size_t run = 1;
    while (i + run < FB_BYTES && fb[i + run] == v && run < 255) ++run;
    out.push_back(static_cast<char>(run));
    out.push_back(static_cast<char>(v));
    i += run;
  }
  return out;
}

bool rleDecode(const std::string& in, uint8_t* fb) {
  if (in.size() < 4 || in.compare(0, 4, "FRM1") != 0 || (in.size() - 4) % 2) return false;
  size_t o = 0;
  for (size_t i = 4; i < in.size(); i += 2) {
    size_t run = static_cast<uint8_t>(in[i]);
    if (run == 0 || o + run > FB_BYTES) return false;
    memset(fb + o, static_cast<uint8_t>(in[i + 1]), run);
    o += run;
  }
  return o == FB_BYTES;
}

Rect diffRect(const uint8_t* a, const uint8_t* b) {
  constexpr int BW = W / 8;
  int x0 = BW, x1 = -1, y0 = H, y1 = -1;
  for (int y = 0; y < H; ++y) {
    const uint8_t* ra = a + y * BW;
    const uint8_t* rb = b + y * BW;
    if (memcmp(ra, rb, BW) == 0) continue;
    if (y < y0) y0 = y;
    y1 = y;
    for (int x = 0; x < BW; ++x)
      if (ra[x] != rb[x]) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
      }
  }
  if (y1 < 0) return {};
  return {x0 * 8, y0, (x1 - x0 + 1) * 8, y1 - y0 + 1};
}

Screen::Screen(Epd& epd, Fs& fs, RtcData& rtc)
    : epd_(epd),
      fs_(fs),
      rtc_(rtc),
      frame_(static_cast<uint8_t*>(malloc(FB_BYTES))),
      shown_(static_cast<uint8_t*>(malloc(FB_BYTES))),
      canvas_(frame_) {
  canvas_.clear();
}

Screen::~Screen() {
  free(frame_);
  free(shown_);
}

void Screen::begin(bool coldBoot) {
  epd_.begin(coldBoot);
  shownValid_ = false;
  oldRamValid_ = false;
  if (!coldBoot) {
    std::string raw;
    shownValid_ = fs_.readAll(path::FRAME, raw) && rleDecode(raw, shown_);
  }
  if (!shownValid_) rtc_.partialCount = FULL_REFRESH_EVERY;  // 屏上内容未知，下一次必须全刷
}

bool Screen::present(bool forceFull) {
  const bool full = forceFull || !shownValid_ || rtc_.partialCount >= FULL_REFRESH_EVERY;
  if (full) {
    epd_.writeNew(frame_);
    epd_.refreshFull();
    rtc_.partialCount = 0;
  } else {
    Rect r = diffRect(shown_, frame_);
    if (r.empty()) return false;
    if (!oldRamValid_) epd_.writePrevious(shown_);
    // 整帧写 new RAM：刷新后控制器会把 new 拷到 old，整帧都对才不会留下脏数据
    epd_.writeNew(frame_);
    epd_.refreshPartial(r);
    rtc_.partialCount++;
  }
  memcpy(shown_, frame_, FB_BYTES);
  shownValid_ = true;
  oldRamValid_ = true;
  shownDirty_ = true;
  return true;
}

void Screen::sleep() {
  if (shownDirty_ && shownValid_) {
    fs_.writeAtomic(path::FRAME, rleEncode(shown_));
    shownDirty_ = false;
  }
  epd_.hibernate();
  oldRamValid_ = false;
}

}  // namespace et
