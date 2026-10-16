#include "Model.h"

#include <iterator>

#include "DateUtil.h"

namespace et {

std::string Model::newId() {
  static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string s;
  uint32_t n = nextId++;
  do {
    s.insert(s.begin(), digits[n % 36]);
    n /= 36;
  } while (n);
  return "i" + s;
}

void Model::loadDefaults() {
  struct T { const char* text; const char* icon; };
  const std::vector<T> weekday = {
      {"口算20题", "kousuan"}, {"语文阅读", "yuedu"}, {"英语听读", "tingdu"},
      {"练字一页", "lianzi"}, {"整理书包", "shubao"}};
  const std::vector<T> weekend = {{"阅读30分钟", "yuedu"}, {"跳绳200个", "tiaosheng"}, {"练琴", "lianqin"}};
  for (int d = 0; d < 7; ++d) {
    templates[d].clear();
    for (auto& t : d < 5 ? weekday : weekend) templates[d].push_back({newId(), t.text, t.icon, false});
  }
}

int Model::logicalDay(int64_t utc) const {
  return static_cast<int>(floorDiv(localSecs(utc) - int64_t(settings.resetHour) * 3600, 86400));
}

int64_t Model::nextRolloverUtc(int64_t utc) const {
  int64_t nextLocal = int64_t(logicalDay(utc) + 1) * 86400 + int64_t(settings.resetHour) * 3600;
  return nextLocal - int64_t(settings.tzMin) * 60;
}

DayPlan Model::planFor(int day) const {
  auto it = overrides.find(day);
  if (it != overrides.end()) return it->second;
  DayPlan p;
  p.items = templates[weekdayMon0(day)];
  return p;
}

void Model::applyToToday(const DayPlan& plan, bool custom) {
  std::vector<Item> items;
  for (const Item& src : plan.items) {
    Item it = src;
    it.done = false;
    for (const Item& old : today.items)
      if (old.id == it.id) it.done = old.done;
    items.push_back(it);
  }
  today.items = std::move(items);
  today.rest = plan.rest;
  today.custom = custom;
}

void Model::refreshToday() {
  if (today.day < 0) return;
  applyToToday(planFor(today.day), overrides.count(today.day) > 0);
}

void Model::pushHistory(int day, DayStatus st, int done, int total) {
  stats.history.push_back({day, st, uint8_t(done), uint8_t(total)});
  while (stats.history.size() > HISTORY_DAYS) stats.history.erase(stats.history.begin());
}

bool Model::rollover(int64_t utc) {
  const int d = logicalDay(utc);
  if (today.day == d) return false;
  if (today.day >= 0 && d > today.day) {
    // 结算旧的一天
    if (!counts()) {
      pushHistory(today.day, DayStatus::Off, 0, 0);
    } else if (allDone()) {
      stats.stars += 1;
      stats.streak += 1;
      pushHistory(today.day, DayStatus::Done, int(doneCount()), int(today.items.size()));
    } else {
      stats.streak = 0;
      pushHistory(today.day, DayStatus::Missed, int(doneCount()), int(today.items.size()));
    }
    // 中间没开机（比如没电）的日子：有任务就算没完成
    if (d - today.day > 60) {
      stats.streak = 0;
    } else {
      for (int g = today.day + 1; g < d; ++g) {
        DayPlan p = planFor(g);
        if (p.rest || p.items.empty()) {
          pushHistory(g, DayStatus::Off, 0, 0);
        } else {
          stats.streak = 0;
          pushHistory(g, DayStatus::Missed, 0, int(p.items.size()));
        }
      }
    }
  }
  // 时钟往回跳（d < today.day）时不结算，直接按新日期生成
  for (auto it = overrides.begin(); it != overrides.end();) it = it->first < d ? overrides.erase(it) : std::next(it);
  today = Today{};
  today.day = d;
  applyToToday(planFor(d), overrides.count(d) > 0);
  return true;
}

bool Model::allDone() const {
  if (today.items.empty()) return false;
  for (auto& i : today.items)
    if (!i.done) return false;
  return true;
}

size_t Model::doneCount() const {
  size_t n = 0;
  for (auto& i : today.items) n += i.done;
  return n;
}

bool Model::toggle(size_t idx) {
  if (idx >= today.items.size()) return false;
  bool before = allDone();
  today.items[idx].done = !today.items[idx].done;
  return !before && allDone() && counts();
}

std::array<uint8_t, 7> Model::weekView() const {
  std::array<uint8_t, 7> v{};
  if (today.day < 0) return v;
  const int monday = today.day - weekdayMon0(today.day);
  for (int i = 0; i < 7; ++i) {
    const int day = monday + i;
    if (day > today.day) {
      v[i] = 0;
    } else if (day == today.day) {
      v[i] = !counts() ? 3 : allDone() ? 1 : 4;
    } else {
      v[i] = 0;
      for (auto& h : stats.history)
        if (h.day == day) v[i] = h.status == DayStatus::Done ? 1 : h.status == DayStatus::Missed ? 2 : 3;
    }
  }
  return v;
}

}  // namespace et
