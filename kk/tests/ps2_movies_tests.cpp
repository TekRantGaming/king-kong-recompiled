#include "ps2_movies_logic.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)

int main() {
  using namespace kk::movies;
  const std::vector<int> levels{100, 200, 350, 400};
  CHECK(StoryBoundary(levels, 100, 200, true, false, false));
  CHECK(StoryBoundary(levels, 350, 400, true, false, false));
  CHECK(!StoryBoundary(levels, 100, 350, true, false, false));
  CHECK(!StoryBoundary(levels, 100, 100, true, false, false));
  CHECK(!StoryBoundary(levels, 200, 100, true, false, false));
  CHECK(!StoryBoundary(levels, 3, 100, true, false, false));
  CHECK(!StoryBoundary(levels, 400, 3, true, false, false));
  CHECK(!StoryBoundary(levels, 100, 200, false, false, false));
  CHECK(!StoryBoundary(levels, 100, 200, true, true, false));
  CHECK(!StoryBoundary(levels, 100, 200, true, false, true));
  CHECK(!StoryBoundary({}, 100, 200, true, false, false));
  constexpr uint32_t base = 0x90000000;
  std::map<uint32_t,uint32_t> memory{{base+0xBEC, 3}, {base+0x24, 100}, {base+0x28, 200},
      {base+0x2C, 350}, {base+0x280, 0xAA001234}, {base+0x284, 0xBB00ABCD}, {base+0x288, 0xCC005678}};
  auto read = [&](uint32_t p) { return memory[p]; };
  CHECK(WorldForLevel(base, 200, read) == 0xBB00ABCD);
  CHECK(WorldForLevel(base, 350, read) == 0xCC005678);
  CHECK(!WorldForLevel(base, 999, read));
  CHECK(!WorldForLevel(0, 200, read));
  CHECK(!WorldForLevel(0xFFFFFFF0, 200, read));
  CHECK(!WorldForLevel(base+1, 200, read));
  // Heap catalogs may live below the executable's 0x82000000 image.
  CHECK(WorldForLevel(0x40000000, 200, [&](uint32_t p) { return memory[p + 0x50000000]; }) == 0xBB00ABCD);
  memory[base+0xBEC] = 152;
  CHECK(!WorldForLevel(base, 200, read));
  memory[base+0xBEC] = 0;
  CHECK(!WorldForLevel(base, 200, read));
  memory[base+0xBEC] = 3;
  memory[base+0x2C] = 200;
  CHECK(!WorldForLevel(base, 200, read));
  CHECK(MovieName(0xBB00ABCD) == "BK_ABCD.mp4");
  CHECK(MovieName(0).empty());
  CHECK(MovieGain(50, 80, 60) == .3f);
  CHECK(MovieGain(100, 0, 100) == 0);
  CHECK(MovieGain(100, 100, 0) == 0);
  CHECK(MovieGain(200, 200, 200) == 1);
  CHECK(MovieGain(NAN, 100, 100) == 0);
  const auto wide = AspectFit(1920, 1200, 3840, 2160);
  CHECK(wide.x == 0 && wide.y == 60 && wide.w == 1920 && wide.h == 1080);
  const auto ultra = AspectFit(2560, 1080, 3840, 2160);
  CHECK(ultra.x == 320 && ultra.y == 0 && ultra.w == 1920 && ultra.h == 1080);
  CHECK(AspectFit(0, 0, 10, 10).w == 0);
  // A 24 FPS movie advances by timestamps at every display rate, with no
  // generated frames. The same second always selects source frame 24.
  for (double fps : {30., 60., 120., 144., 240., 517.}) {
    int selected = -1;
    for (int tick = 0; tick <= int(fps); ++tick) {
      const double time = tick / fps;
      while (selected < 100 && FrameDue(int64_t(selected+1)*10000000/24, time)) ++selected;
    }
    CHECK(selected == 24);
  }
  CHECK(!FrameDue(10000000, .999));
  CHECK(FrameDue(10000000, 1));
  CHECK(RelativePts(1250000, 1250000) == 0);
  CHECK(RelativePts(1666666, 1250000) == 416666);
  CHECK(RelativePts(1000000, 1250000) == 0);
  CHECK(RelativePts(INT64_MAX, INT64_MIN) == INT64_MAX);
  std::cout << checks << " PS2 movie logic checks passed\n";
}
