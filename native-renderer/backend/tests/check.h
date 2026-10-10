// A minimal test runner (no external test framework in the container or the
// SDK bundle): TEST(name) registers a test, CHECK / CHECK_EQ / CHECK_NEAR
// record failures and keep going, main() comes from NR_TEST_MAIN().
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace nr::test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Cases() {
  static std::vector<Case> cases;
  return cases;
}
inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }
};

inline void Fail(const char* file, int line, const std::string& what) {
  ++Failures();
  std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, what.c_str());
}

template <typename T>
void Print(std::ostream& s, const T& v) {
  if constexpr (std::is_arithmetic_v<T>) {
    s << +v;  // chars and bytes as numbers
  } else {
    s << v;
  }
}

template <typename A, typename B>
void CheckEq(const A& a, const B& b, const char* sa, const char* sb, const char* file, int line) {
  if (!(a == b)) {
    std::ostringstream s;
    s << sa << " == " << sb << " (";
    Print(s, a);
    s << " vs ";
    Print(s, b);
    s << ")";
    Fail(file, line, s.str());
  }
}

inline int RunAll(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const Case& c : Cases()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    int before = Failures();
    std::printf("[ RUN  ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    std::printf("[ %s ] %s\n", Failures() == before ? " OK " : "FAIL", c.name);
    ++run;
  }
  std::printf("%d tests, %d failed checks\n", run, Failures());
  return Failures() == 0 && run > 0 ? 0 : 1;
}

}  // namespace nr::test

#define TEST(name)                                                   \
  static void name();                                                \
  static ::nr::test::Registrar name##_registrar(#name, &name);       \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::nr::test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b) ::nr::test::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    double _a = (a), _b = (b);                                                         \
    if (!(std::fabs(_a - _b) <= (tol))) {                                              \
      std::ostringstream _s;                                                           \
      _s << #a " ~= " #b " (" << _a << " vs " << _b << ", tolerance " << (tol) << ")"; \
      ::nr::test::Fail(__FILE__, __LINE__, _s.str());                                  \
    }                                                                                  \
  } while (0)

#define NR_TEST_MAIN() \
  int main(int argc, char** argv) { return ::nr::test::RunAll(argc, argv); }
