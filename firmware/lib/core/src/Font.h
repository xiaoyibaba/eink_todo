// 读 tools/build_assets.py 生成的点阵字库（格式见该脚本）。
// 内存够（有 PSRAM）就整个文件读进内存；不够就只读索引，字形按需从文件读。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Hal.h"

namespace et {

// 逐个取 UTF-8 码点；非法字节返回 0xFFFD
uint32_t nextCodepoint(const std::string& s, size_t& i);
size_t codepointCount(const std::string& s);

class Font {
 public:
  bool load(Fs& fs, const std::string& path);
  bool loaded() const { return size_ != 0; }
  int size() const { return size_; }
  bool has(uint32_t cp) const { return find(cp) != nullptr; }
  int measure(const std::string& utf8) const;
  // yTop 是方块字的顶边，文字占 [yTop, yTop+size)。maxW > 0 时超出部分截断并补「…」。
  int draw(Canvas& c, int x, int yTop, const std::string& utf8, Color col, int maxW = 0) const;
  int drawCentered(Canvas& c, int cx, int yTop, const std::string& utf8, Color col) const {
    int w = measure(utf8);
    return draw(c, cx - w / 2, yTop, utf8, col);
  }

 private:
#pragma pack(push, 1)
  struct Entry {
    uint32_t cp, off;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t adv, pad[3];
  };
#pragma pack(pop)
  static_assert(sizeof(Entry) == 16, "font index entry must be 16 bytes");

  const Entry* find(uint32_t cp) const;
  int advance(uint32_t cp) const;
  void drawGlyph(Canvas& c, const Entry& e, int x, int baseline, Color col) const;

  int size_ = 0, emAscent_ = 0;
  std::vector<Entry> index_;
  uint8_t* whole_ = nullptr;  // 整个文件（可能为空）
  std::unique_ptr<File> file_;
  mutable std::vector<uint8_t> scratch_;

 public:
  ~Font();
};

}  // namespace et
