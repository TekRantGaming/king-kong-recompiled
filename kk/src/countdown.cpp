// This file is UTF-8.
#include "countdown.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "http.h"
#include "launcher_text.h"

namespace kk::countdown {
namespace {

// Days since 1970-01-01 for a date in the proleptic Gregorian calendar
// (H. Hinnant's days_from_civil) and back.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

struct Civil {
  int64_t y;
  unsigned m, d;
};
Civil CivilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2), m, d};
}

int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// 0 = Sunday.
int WeekdayOf(int64_t days) { return int(((days % 7) + 7 + 4) % 7); }

// UK clocks: GMT, or BST (UTC+1) from 01:00 UTC on the last Sunday of March
// to 01:00 UTC on the last Sunday of October.
int64_t UkOffsetSeconds(int64_t utc) {
  const Civil c = CivilFromDays(FloorDiv(utc, 86400));
  auto last_sunday_1am = [&](unsigned month) {
    const int64_t last = DaysFromCivil(c.y, month, 31);  // March and October both have 31 days
    return (last - WeekdayOf(last)) * 86400 + 3600;
  };
  return utc >= last_sunday_1am(3) && utc < last_sunday_1am(10) ? 3600 : 0;
}

const char* const kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

std::string TwoDigits(int v) {
  char b[8];
  std::snprintf(b, sizeof(b), "%02d", v);
  return b;
}

// The string value of "key": "value" (no escapes: a value with a backslash is
// not used).
std::optional<std::string> JsonString(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  for (size_t k = json.find(needle); k != std::string::npos; k = json.find(needle, k + 1)) {
    size_t c = json.find_first_not_of(" \t\r\n", k + needle.size());
    if (c == std::string::npos || json[c] != ':') continue;
    const size_t q = json.find_first_not_of(" \t\r\n", c + 1);
    if (q == std::string::npos || json[q] != '"') return std::nullopt;
    const size_t end = json.find('"', q + 1);
    if (end == std::string::npos) return std::nullopt;
    std::string v = json.substr(q + 1, end - q - 1);
    if (v.find('\\') != std::string::npos) return std::nullopt;
    return v;
  }
  return std::nullopt;
}

}  // namespace

std::optional<int64_t> ParseUtc(const std::string& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, n = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &n) != 5) return std::nullopt;
  std::string rest = text.substr(size_t(n));
  if (rest.size() >= 3 && rest[0] == ':') {
    int m = 0;
    if (std::sscanf(rest.c_str(), ":%2d%n", &s, &m) != 1) return std::nullopt;
    rest.erase(0, size_t(m));
  }
  if (rest != "Z" && rest != "+00:00") return std::nullopt;
  if (y < 2026 || y > 2035 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59 || h < 0 ||
      mi < 0 || s < 0)
    return std::nullopt;
  const int64_t days = DaysFromCivil(y, unsigned(mo), unsigned(d));
  if (CivilFromDays(days).d != unsigned(d)) return std::nullopt;  // 31 June and the like
  return days * 86400 + h * 3600 + mi * 60 + s;
}

std::optional<LaunchInfo> ParseLaunchJson(const std::string& json) {
  const auto utc_text = JsonString(json, "utc");
  if (!utc_text) return std::nullopt;
  const auto utc = ParseUtc(*utc_text);
  if (!utc) return std::nullopt;
  LaunchInfo info;
  info.utc = *utc;
  info.utc_text = *utc_text;
  if (const auto title = JsonString(json, "title"); title && !title->empty() && title->size() <= 80)
    info.title = *title;
  return info;
}

std::shared_ptr<Fetch> StartFetch() {
  auto fetch = std::make_shared<Fetch>();
  std::thread([fetch] {
    std::vector<uint8_t> data;
    if (HttpGet(kLaunchJsonUrl, data, nullptr, nullptr, 5000) && data.size() < 16384)
      fetch->info = ParseLaunchJson(std::string(data.begin(), data.end()));
    fetch->done = true;
  }).detach();
  return fetch;
}

int64_t NowUtc() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string CountdownText(int64_t left) {
  if (left < 0) left = 0;
  const int64_t days = left / 86400;
  const int h = int(left / 3600 % 24), m = int(left / 60 % 60), s = int(left % 60);
  return "T- " + std::to_string(days) + "d " + TwoDigits(h) + ":" + TwoDigits(m) + ":" + TwoDigits(s);
}

std::string DateLine(int64_t utc) {
  using text::F;
  using text::T;
  // UK time, from the rule above (the same on every PC).
  const int64_t uk = utc + UkOffsetSeconds(utc);
  const int64_t uk_days = FloorDiv(uk, 86400);
  const Civil c = CivilFromDays(uk_days);
  const int uk_h = int((uk - uk_days * 86400) / 3600), uk_m = int((uk - uk_days * 86400) / 60 % 60);
  std::string line = F("{0} {1} {2}, {3} UK time", {T(kWeekdays[WeekdayOf(uk_days)]), std::to_string(c.d),
                                                    T(kMonths[c.m - 1]), TwoDigits(uk_h) + ":" + TwoDigits(uk_m)});
  if (!line.empty() && line[0] >= 'a' && line[0] <= 'z') line[0] = char(line[0] - 'a' + 'A');  // "vendredi" starts it

  // The player's own clock.
  const std::time_t t = std::time_t(utc);
  std::tm local{};
#if defined(_WIN32)
  if (localtime_s(&local, &t) != 0) return line;
#else
  if (!localtime_r(&t, &local)) return line;
#endif
  if (local.tm_mday == int(c.d) && local.tm_hour == uk_h && local.tm_min == uk_m) return line;
  std::string when = TwoDigits(local.tm_hour) + ":" + TwoDigits(local.tm_min);
  if (local.tm_mday != int(c.d)) when = std::string(T(kWeekdays[local.tm_wday % 7])) + " " + when;
  return line + " " + F("({0} your time)", {when});
}

}  // namespace kk::countdown
