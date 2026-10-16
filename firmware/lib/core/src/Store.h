// Model 的持久化：几个小 JSON 文件，分开写，按键打勾时只写 today.json。
#pragma once
#include <string>

#include "Hal.h"
#include "Model.h"

namespace et {

namespace path {
constexpr const char* SETTINGS = "/state/settings.json";
constexpr const char* TEMPLATES = "/state/templates.json";
constexpr const char* OVERRIDES = "/state/overrides.json";
constexpr const char* TODAY = "/state/today.json";
constexpr const char* STATS = "/state/stats.json";
constexpr const char* FRAME = "/state/frame.rle";
}  // namespace path

class Store {
 public:
  explicit Store(Fs& fs) : fs_(fs) {}
  // 文件缺失或损坏的部分用默认值；返回 false 表示是全新设备（没有模板文件）
  bool load(Model& m);
  void saveSettings(const Model& m);
  void saveTemplates(const Model& m);
  void saveOverrides(const Model& m);
  void saveToday(const Model& m);
  void saveStats(const Model& m);
  void saveAll(const Model& m);

 private:
  Fs& fs_;
};

}  // namespace et
