// 业务数据和规则（纯逻辑，不碰硬件，单元测试覆盖这里）。
// 规则见 docs/需求.md「每日逻辑」。
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace et {

constexpr size_t MAX_ITEMS = 8;
constexpr size_t HISTORY_DAYS = 30;

struct Item {
  std::string id, text, icon;
  bool done = false;
};

// 某一天的安排：来自星期模板，或家长对这一天的单独设置
struct DayPlan {
  bool rest = false;
  std::vector<Item> items;  // done 无意义
};

struct Today {
  int day = -1;  // 1970-01-01 起的天数；-1 = 还没生成过
  bool rest = false;
  bool custom = false;  // 是否来自家长对当天的单独设置
  std::vector<Item> items;
};

enum class DayStatus : uint8_t { Missed = 0, Done = 1, Off = 2 };  // Off = 休息日或没有任务

struct HistEntry {
  int day;
  DayStatus status;
  uint8_t done, total;
};

struct Stats {
  int stars = 0;   // 截至昨天累计（含家长手动调整）
  int streak = 0;  // 截至昨天的连续天数
  std::vector<HistEntry> history;  // 按日期升序，最多 HISTORY_DAYS 条
};

struct Settings {
  int resetHour = 4;
  int tzMin = 480;  // UTC+8
};

class Model {
 public:
  std::array<std::vector<Item>, 7> templates;  // 0 = 周一
  std::map<int, DayPlan> overrides;            // day -> 单独设置
  Today today;
  Stats stats;
  Settings settings;
  uint32_t nextId = 1;

  std::string newId();
  void loadDefaults();  // 第一次开机的示例模板

  // 当前时刻属于哪一个「逻辑日」：每天 resetHour 点换日
  int logicalDay(int64_t utc) const;
  int64_t localSecs(int64_t utc) const { return utc + int64_t(settings.tzMin) * 60; }
  // 下一次换日的 UTC 时间
  int64_t nextRolloverUtc(int64_t utc) const;

  // 跨日时结算昨天（及中间没开机的日子）并生成今天。返回是否换了日。
  bool rollover(int64_t utc);

  DayPlan planFor(int day) const;
  // 把一份安排套到今天，按 id 保留已勾选状态
  void applyToToday(const DayPlan& plan, bool custom);
  // 模板或单独设置改了之后，重新生成今天（保留勾选）
  void refreshToday();

  bool counts() const { return !today.rest && !today.items.empty(); }
  bool allDone() const;
  size_t doneCount() const;
  int displayStars() const { return stats.stars + (counts() && allDone() ? 1 : 0); }
  int displayStreak() const { return stats.streak + (counts() && allDone() ? 1 : 0); }
  // 勾选/取消；返回 true 表示这一下让今天变成了全部完成
  bool toggle(size_t idx);
  // 本周一到周日每天的状态：0 未来，1 完成，2 没完成，3 休息/无任务，4 今天进行中
  std::array<uint8_t, 7> weekView() const;

 private:
  void pushHistory(int day, DayStatus st, int done, int total);
};

}  // namespace et
