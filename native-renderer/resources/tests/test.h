// A minimal test registry (no third-party framework): TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace kknr_test {

struct Case {
  const char* name;
  std::function<void()> fn;
};
std::vector<Case>& Registry();
extern int g_failures;
extern const char* g_current;

struct Register {
  Register(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline void Fail(const char* file, int line, const std::string& what) {
  ++g_failures;
  std::printf("  FAIL %s (%s:%d): %s\n", g_current, file, line, what.c_str());
}

}  // namespace kknr_test

#define KKNR_CAT2(a, b) a##b
#define KKNR_CAT(a, b) KKNR_CAT2(a, b)
#define TEST(name)                                                                        \
  static void name();                                                                     \
  static kknr_test::Register KKNR_CAT(register_, name)(#name, name);                      \
  static void name()

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) kknr_test::Fail(__FILE__, __LINE__, "CHECK " #cond); \
  } while (0)

#define CHECK_EQ(a, b)                                                                                    \
  do {                                                                                                    \
    const auto va = (a);                                                                                  \
    const auto vb = (b);                                                                                  \
    if (!(va == vb))                                                                                      \
      kknr_test::Fail(__FILE__, __LINE__,                                                                 \
                      std::string(#a " == " #b ": ") + std::to_string(uint64_t(va)) + " vs " +           \
                          std::to_string(uint64_t(vb)));                                                  \
  } while (0)
