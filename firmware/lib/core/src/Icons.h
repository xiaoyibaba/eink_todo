// 读 tools/build_assets.py 生成的 icons.bin。文件很小（约 8KB），整个读进内存。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Hal.h"

namespace et {

struct IconInfo {
  const char* name;
  const char* label;
};
// 任务图标列表（顺序同网页上的选择器）
const std::vector<IconInfo>& taskIcons();
bool isTaskIcon(const std::string& name);

class Icons {
 public:
  bool load(Fs& fs, const std::string& path);
  // 找不到时画一个空心方框，返回 false
  bool draw(Canvas& c, const std::string& name, int x, int y, Color col) const;
  int width(const std::string& name) const;

 private:
  struct Icon {
    std::string name;
    int w, h;
    std::vector<uint8_t> bits;
  };
  const Icon* find(const std::string& name) const;
  std::vector<Icon> icons_;
};

}  // namespace et
