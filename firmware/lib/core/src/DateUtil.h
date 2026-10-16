// 日期换算。不依赖系统时区库：中国没有夏令时，时区只是固定偏移（设置里的 tzMin）。
// "day" 一律指 1970-01-01 起的天数。
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace et {

struct Ymd {
  int y, m, d;
};

// Howard Hinnant 的 days_from_civil / civil_from_days
inline int daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

inline Ymd civilFromDays(int z) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int y = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), static_cast<int>(m), static_cast<int>(d)};
}

// 0 = 周一 … 6 = 周日（1970-01-01 是周四）
inline int weekdayMon0(int day) {
  int r = (day + 3) % 7;
  return r < 0 ? r + 7 : r;
}

inline int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

inline std::string dayToIso(int day) {
  Ymd c = civilFromDays(day);
  char buf[16];
  snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.y, c.m, c.d);
  return buf;
}

// 解析失败返回 INT32_MIN
inline int isoToDay(const std::string& s) {
  int y, m, d;
  if (s.size() != 10 || sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return INT32_MIN;
  if (m < 1 || m > 12 || d < 1 || d > 31) return INT32_MIN;
  int day = daysFromCivil(y, m, d);
  Ymd back = civilFromDays(day);
  if (back.y != y || back.m != m || back.d != d) return INT32_MIN;
  return day;
}

}  // namespace et
