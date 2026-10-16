#include "Font.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace et {

uint32_t nextCodepoint(const std::string& s, size_t& i) {
  uint8_t c = static_cast<uint8_t>(s[i]);
  int n;
  uint32_t cp;
  if (c < 0x80) { ++i; return c; }
  if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else { ++i; return 0xFFFD; }
  for (int k = 1; k <= n; ++k) {
    uint8_t b = i + k < s.size() ? static_cast<uint8_t>(s[i + k]) : 0;
    if ((b & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (b & 0x3F);
  }
  i += n + 1;
  return cp;
}

size_t codepointCount(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) { nextCodepoint(s, i); ++n; }
  return n;
}

Font::~Font() { free(whole_); }

bool Font::load(Fs& fs, const std::string& path) {
  file_ = fs.open(path);
  if (!file_) return false;
  uint8_t hdr[16];
  if (file_->readAt(0, hdr, 16) != 16 || memcmp(hdr, "EFNT", 4) != 0) return false;
  uint32_t count;
  memcpy(&count, hdr + 8, 4);
  index_.resize(count);
  if (file_->readAt(16, index_.data(), count * sizeof(Entry)) != count * sizeof(Entry)) return false;
  size_t total = file_->size();
  whole_ = static_cast<uint8_t*>(malloc(total));  // ESP32 上大块分配会自动落到 PSRAM
  if (whole_ && file_->readAt(0, whole_, total) != total) { free(whole_); whole_ = nullptr; }
  if (whole_) file_.reset();
  size_ = hdr[5];
  emAscent_ = hdr[6];
  return true;
}

const Font::Entry* Font::find(uint32_t cp) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), cp,
                             [](const Entry& e, uint32_t v) { return e.cp < v; });
  return (it != index_.end() && it->cp == cp) ? &*it : nullptr;
}

int Font::advance(uint32_t cp) const {
  const Entry* e = find(cp);
  return e ? e->adv : size_ / 2;
}

int Font::measure(const std::string& s) const {
  int w = 0;
  for (size_t i = 0; i < s.size();) w += advance(nextCodepoint(s, i));
  return w;
}

void Font::drawGlyph(Canvas& c, const Entry& e, int x, int baseline, Color col) const {
  if (!e.w || !e.h) return;
  size_t n = size_t((e.w + 7) / 8) * e.h;
  const uint8_t* bits;
  if (whole_) {
    bits = whole_ + e.off;
  } else {
    scratch_.resize(n);
    file_->readAt(e.off, scratch_.data(), n);
    bits = scratch_.data();
  }
  c.blit(bits, e.w, e.h, x + e.xoff, baseline + e.yoff, col);
}

int Font::draw(Canvas& c, int x, int yTop, const std::string& s, Color col, int maxW) const {
  const int baseline = yTop + emAscent_;
  const uint32_t ELL = 0x2026;  // …
  const bool truncate = maxW > 0 && measure(s) > maxW;
  const int limit = truncate ? maxW - advance(ELL) : INT_MAX;
  auto one = [&](uint32_t cp, int cx) {
    if (const Entry* e = find(cp)) drawGlyph(c, *e, cx, baseline, col);
    else c.rect(cx + 2, yTop + 2, advance(cp) - 4, size_ - 4, 2, col);  // 字库里没有的字画个方框
  };
  int cx = x;
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = nextCodepoint(s, i);
    int adv = advance(cp);
    if (cx - x + adv > limit) break;
    one(cp, cx);
    cx += adv;
  }
  if (truncate) {
    one(ELL, cx);
    cx += advance(ELL);
  }
  return cx - x;
}

}  // namespace et
