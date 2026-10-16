#include "Icons.h"

#include <cstring>

namespace et {

const std::vector<IconInfo>& taskIcons() {
  static const std::vector<IconInfo> list = {
#define X(n, l) {n, l},
#include "IconList.inc"
#undef X
  };
  return list;
}

bool isTaskIcon(const std::string& name) {
  for (auto& i : taskIcons())
    if (name == i.name) return true;
  return false;
}

bool Icons::load(Fs& fs, const std::string& path) {
  std::string raw;
  if (!fs.readAll(path, raw) || raw.size() < 8 || raw.compare(0, 4, "EICN") != 0) return false;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(raw.data());
  uint16_t count;
  memcpy(&count, p + 4, 2);
  icons_.clear();
  for (int i = 0; i < count; ++i) {
    const uint8_t* e = p + 8 + 28 * i;
    if (8 + 28 * (i + 1) > static_cast<int>(raw.size())) return false;
    Icon ic;
    ic.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 20));
    uint16_t w, h;
    uint32_t off;
    memcpy(&w, e + 20, 2);
    memcpy(&h, e + 22, 2);
    memcpy(&off, e + 24, 4);
    ic.w = w;
    ic.h = h;
    size_t n = size_t((w + 7) / 8) * h;
    if (off + n > raw.size()) return false;
    ic.bits.assign(p + off, p + off + n);
    icons_.push_back(std::move(ic));
  }
  return true;
}

const Icons::Icon* Icons::find(const std::string& name) const {
  for (auto& i : icons_)
    if (i.name == name) return &i;
  return nullptr;
}

int Icons::width(const std::string& name) const {
  const Icon* i = find(name);
  return i ? i->w : 0;
}

bool Icons::draw(Canvas& c, const std::string& name, int x, int y, Color col) const {
  const Icon* i = find(name);
  if (!i) {
    c.rect(x, y, 40, 40, 2, col);
    return false;
  }
  c.blit(i->bits.data(), i->w, i->h, x, y, col);
  return true;
}

}  // namespace et
