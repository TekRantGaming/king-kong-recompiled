#include "coop_grid_guard.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace guard = kk::coop::grid_guard;
namespace {
void Check(bool ok, const char* why) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint64_t Pair(uint32_t hi, uint32_t lo) { return uint64_t(hi) << 32 | lo; }
guard::Line Valid() {
  // Partner position retained on the captured guest stack in the 28f dump.
  return {Pair(0x42A4DA95, 0x42BF394A), Pair(0x41674674, 0),
          Pair(0x42A50000, 0x42BF0000), 0, 4};
}
}

int main() {
  auto line = Valid();
  const auto p = line.Points();
  Check(p[0] > 82 && p[0] < 83 && p[1] > 95 && p[1] < 96 && p[2] > 14 && p[2] < 15,
        "packed ABI decodes the captured finite position");
  Check(!guard::Refuse(true, line), "ordinary native query passes");
  line.to_xy = line.from_xy; line.to_z = line.from_z;
  Check(!guard::Refuse(true, line), "native zero length query still passes");

  // Both signs of NaN were captured on the guest stack. Include signalling
  // NaNs and infinities in every used word to cover both endpoints and all axes.
  for (uint32_t invalid : {0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7F800000u, 0xFF800000u}) {
    for (unsigned axis = 0; axis < 6; ++axis) {
      line = Valid();
      uint64_t* words[] = {&line.from_xy, &line.from_xy, &line.from_z, &line.to_xy, &line.to_xy, &line.to_z};
      const unsigned shift = axis % 3 == 1 ? 0 : 32;
      *words[axis] = (*words[axis] & ~(uint64_t(0xFFFFFFFF) << shift)) | (uint64_t(invalid) << shift);
      Check(guard::Refuse(true, line), "invalid coordinate is blocked before native grid conversion");
      Check(!guard::Refuse(false, line), "disabled co op preserves the native call");
    }
  }
  line = Valid();
  line.from_z |= 0x7FC00000; line.to_z |= 0xFF800000;
  Check(!guard::Refuse(true, line), "unused GPR padding may contain arbitrary bits");
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    line = Valid(); line.shorten = invalid;
    Check(guard::Refuse(true, line), "invalid shortening cannot generate NaN endpoints inside the helper");
  }
  for (double shorten : {0., -4., 4.}) {
    line = Valid(); line.shorten = shorten;
    Check(!guard::Refuse(true, line), "both native shortening modes pass");
  }
  line = {0, 0, Pair(0x7F7FFFFF, 0xFF7FFFFF), 0, 0};
  Check(!guard::Refuse(true, line), "finite values get no arbitrary world bounds");

  guard::Reports reports;
  Check(reports.Note(100), "first invalid query is reported");
  for (int i = 0; i < 10000; ++i)
    Check(!reports.Note(100 + double(i) / 1000), "repeated invalid queries do not flood the shipping log");
  Check(!reports.Note(129.999) && reports.Note(130) && reports.refused == 10003,
        "next report includes the total refused count after thirty seconds");
  std::puts("coop grid guard OK");
}
