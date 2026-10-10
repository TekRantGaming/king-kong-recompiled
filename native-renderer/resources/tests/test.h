// A minimal test registry (no third-party framework): TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <algorithm>
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

#define CHECK_NEAR(a, b, tolerance)                                                                       \
  do {                                                                                                    \
    const double va = double(a);                                                                          \
    const double vb = double(b);                                                                          \
    if (!(va - vb <= (tolerance) && vb - va <= (tolerance)))                                              \
      kknr_test::Fail(__FILE__, __LINE__,                                                                 \
                      std::string(#a " ~= " #b ": ") + std::to_string(va) + " vs " + std::to_string(vb)); \
  } while (0)

namespace kknr_test {

// Deterministic pseudo-random bytes (xorshift64*), so failures reproduce.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t Next() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  }
  void Fill(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) p[i] = uint8_t(Next() >> 56);
  }
};

// A fake 64 MB physical guest memory for converter tests.
struct FakeGuest {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(64u << 20, 0);
  void Put(uint32_t address, const std::vector<uint8_t>& data) {
    std::copy(data.begin(), data.end(), bytes.begin() + address);
  }
};

}  // namespace kknr_test
