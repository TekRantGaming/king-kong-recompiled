// This file is UTF-8.
#include "launcher_text.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>

namespace kk::text {
namespace {

struct Entry {
  const char* en;
  const char* de;
  const char* es;
  const char* fr;
  const char* it;
};

#include "launcher_text_table.inc"

const Entry* Find(std::string_view english) {
  static const auto* map = [] {
    auto* m = new std::unordered_map<std::string_view, const Entry*>();
    for (const Entry& e : kEntries) m->emplace(e.en, &e);
    return m;
  }();
  const auto it = map->find(english);
  return it == map->end() ? nullptr : it->second;
}

#if defined(KK_DEV_TOOLS)
// Developer aid: KK_DEV_TEXT_MISSES=<file> lists text drawn without a
// translation (in a language other than English), once each.
void NoteMiss(std::string_view english) {
  static const char* path = std::getenv("KK_DEV_TEXT_MISSES");
  if (!path || !*path || english.empty()) return;
  static std::mutex mutex;
  static std::set<std::string, std::less<>> seen;
  std::lock_guard lock(mutex);
  if (!seen.emplace(english).second) return;
  std::ofstream(path, std::ios::app) << english << "\n";
}
#endif

const char* Pick(const Entry* e, Lang lang) {
  switch (lang) {
    case Lang::kGerman: return e->de;
    case Lang::kSpanish: return e->es;
    case Lang::kFrench: return e->fr;
    case Lang::kItalian: return e->it;
    default: return e->en;
  }
}

}  // namespace

Lang Current() {
  // The Xbox 360 language numbers the game's Language setting uses.
  switch (std::atoi(rex::cvar::GetFlagByName("user_language").c_str())) {
    case 3: return Lang::kGerman;
    case 4: return Lang::kFrench;
    case 5: return Lang::kSpanish;
    case 6: return Lang::kItalian;
    default: return Lang::kEnglish;
  }
}

const char* T(const char* english) {
  const Lang lang = Current();
  if (lang == Lang::kEnglish || !english) return english;
  if (const Entry* e = Find(english)) {
    const char* t = Pick(e, lang);
    return t && *t ? t : english;
  }
#if defined(KK_DEV_TOOLS)
  NoteMiss(english);
#endif
  return english;
}

std::string T(const std::string& english) { return T(english.c_str()); }

std::string F(const char* english, std::initializer_list<std::string> args) {
  std::string out = T(english);
  size_t i = 0;
  for (const std::string& arg : args) {
    const std::string key = "{" + std::to_string(i++) + "}";
    for (size_t at = 0; (at = out.find(key, at)) != std::string::npos; at += arg.size()) out.replace(at, key.size(), arg);
  }
  return out;
}

std::string Upper(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 'a' && c <= 'z') {
      s[i] = char(c - 'a' + 'A');
    } else if (c == 0xC3 && i + 1 < s.size()) {
      // Latin-1 letters à..þ (U+00E0..U+00FE, but not ÷) to À..Þ; ß stays.
      const unsigned char d = static_cast<unsigned char>(s[i + 1]);
      if (d >= 0xA0 && d <= 0xBE && d != 0xB7) s[i + 1] = char(d - 0x20);
      ++i;
    }
  }
  return s;
}

}  // namespace kk::text
