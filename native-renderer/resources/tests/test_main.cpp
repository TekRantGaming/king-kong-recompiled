#include <cstring>

#include "test.h"

namespace kknr_test {
std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}
int g_failures = 0;
const char* g_current = "";
}  // namespace kknr_test

int main(int argc, char** argv) {
  using namespace kknr_test;
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const Case& c : Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    g_current = c.name;
    const int before = g_failures;
    c.fn();
    ++run;
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("%d tests, %d failed checks\n", run, g_failures);
  return g_failures ? 1 : 0;
}
