// The REX_DEV_FRAME_LOG writer: line formats, the clock and frame rules, the option parsing.
// No GPU, no game data. The lines are written against the examples in docs/testing.md and
// patches 0003 / 0012 in tools/rexglue-patches.

#include <string>
#include <vector>

#include "backend/frame_log.h"
#include "backend/tests/check.h"

using namespace nr;
using namespace nr::test;

namespace {

struct Capture {
  std::vector<std::string> lines;
  double now = 0.0;
  FrameLog Make(const char* option) {
    return FrameLog(FrameLogConfig::Parse(option), [this](const std::string& l) { lines.push_back(l); },
                    [this] { return now; });
  }
};

FrameLogDraw SampleDraw() {
  FrameLogDraw d;
  d.ps_hash = 0x00AB00CD00EF0012ull;
  d.vs_hash = 0x1234567890ABCDEFull;
  d.pitch = 1280;
  d.msaa = 2;
  d.z_test = true;
  d.z_write = true;
  d.z_func = 3;
  d.depth_base = 1120;
  d.depth_format = 1;
  d.offset_x = 0;
  d.offset_y = -8;
  d.scissor_w = 1280;
  d.scissor_h = 720;
  d.targets = {{0, 0, 6}, {2, 560, 15}};
  d.count = 4821;
  return d;
}

}  // namespace

TEST(FrameLog_OptionParsing) {
  CHECK(!FrameLogConfig::Parse(nullptr).enabled);
  CHECK(!FrameLogConfig::Parse("").enabled);
  FrameLogConfig a = FrameLogConfig::Parse("12.5");
  CHECK(a.enabled);
  CHECK_NEAR(a.start_after, 12.5, 1e-9);
  CHECK_EQ(a.frames, 1);
  FrameLogConfig b = FrameLogConfig::Parse("30,3");
  CHECK(b.enabled);
  CHECK_NEAR(b.start_after, 30.0, 1e-9);
  CHECK_EQ(b.frames, 3);
  CHECK_EQ(FrameLogConfig::Parse("5,0").frames, 1);   // at least one frame
  CHECK_EQ(FrameLogConfig::Parse("5,-4").frames, 1);
  CHECK(!FrameLogConfig::Parse("-1").enabled);        // negative: off, as in the Xenos plugin
  CHECK(FrameLogConfig::Parse("0").enabled);          // 0: the first swap
}

TEST(FrameLog_DrawLineFormat) {
  // The Vulkan form of patch 0012 (the depth function after the z state).
  CHECK_EQ(FrameLog::FormatDraw(7, SampleDraw()),
           std::string("Frame log: draw 7 ps 00AB00CD00EF0012 vs 1234567890ABCDEF pitch 1280 msaa 2 z test+write func 3 "
                       "@1120:f1 off 0,-8 scissor 1280x720 rt0@0:f6 rt2@560:f15 count 4821"));
  FrameLogDraw plain;
  plain.pitch = 640;
  plain.depth_format = 0;
  plain.scissor_w = 640;
  plain.scissor_h = 480;
  plain.count = 3;
  // No pixel shader, no depth test, no colour targets.
  CHECK_EQ(FrameLog::FormatDraw(0, plain),
           std::string("Frame log: draw 0 ps 0000000000000000 vs 0000000000000000 pitch 640 msaa 1 z off func 0 "
                       "@0:f0 off 0,0 scissor 640x480 count 3"));
  plain.z_test = true;
  CHECK(FrameLog::FormatDraw(0, plain).find(" z test func ") != std::string::npos);
}

TEST(FrameLog_ResolveLineFormat) {
  FrameLogResolve r;
  r.source_index = 0;
  r.source_base = 0;
  r.source_format = 6;
  r.dest_address = 0x1A2B3000;
  r.dest_format = 6;
  r.dest_pitch = 1280;
  r.dest_height = 720;
  r.command = 1;
  r.clear_color = true;
  r.offset_y = 8;
  r.surface_pitch = 1280;
  r.msaa = 1;
  CHECK_EQ(FrameLog::FormatResolve(2, r),
           std::string("Frame log: resolve 2 rt0@0:f6 -> 1A2B3000 format 6 pitch 1280 height 720 command 1 clear color 1 "
                       "depth 0 off 0,8 surface pitch 1280 msaa 1"));
  FrameLogResolve depth;
  depth.from_depth = true;
  depth.source_base = 1120;
  depth.source_format = 1;
  depth.dest_address = 0x2000;
  depth.command = 0;
  depth.clear_depth = true;
  CHECK_EQ(FrameLog::FormatResolve(0, depth),
           std::string("Frame log: resolve 0 depth@1120:f1 -> 00002000 format 0 pitch 0 height 0 command 0 clear color 0 "
                       "depth 1 off 0,0 surface pitch 0 msaa 1"));
}

TEST(FrameLog_NothingWhenOff) {
  Capture c;
  FrameLog log = c.Make(nullptr);
  CHECK(!log.enabled());
  for (int i = 0; i < 3; ++i) {
    log.OnSwap();
    log.Draw(SampleDraw());
    log.Resolve({});
  }
  CHECK(c.lines.empty());
}

TEST(FrameLog_StartsAtTheFirstSwapAfterTheDelay) {
  Capture c;
  FrameLog log = c.Make("10");
  // The clock starts at the first draw (not at construction).
  c.now = 100.0;
  log.Draw(SampleDraw());  // not active: nothing written
  CHECK(!log.active());
  c.now = 105.0;
  log.OnSwap();
  CHECK(!log.active());  // 5 s in: too early
  c.now = 110.0;
  log.OnSwap();
  CHECK(log.active());  // exactly 10 s: start
  CHECK_EQ(c.lines.size(), size_t(1));
  CHECK_EQ(c.lines[0], std::string("Frame log: frame start"));
}

TEST(FrameLog_OneFrameBetweenTwoSwaps) {
  Capture c;
  FrameLog log = c.Make("0");
  log.OnSwap();  // clock starts and the delay is 0: the frame starts
  log.Draw(SampleDraw());
  log.DrawNotDrawn(true);
  log.Draw(SampleDraw());
  FrameLogResolve r;
  r.dest_address = 0x1000;
  log.Resolve(r);
  log.OnSwap();  // ends it
  log.Draw(SampleDraw());  // after the last frame: quiet
  log.OnSwap();
  log.OnSwap();
  CHECK_EQ(c.lines.size(), size_t(6));
  if (c.lines.size() != 6) return;
  CHECK_EQ(c.lines[0], std::string("Frame log: frame start"));
  CHECK(c.lines[1].rfind("Frame log: draw 0 ps", 0) == 0);
  CHECK_EQ(c.lines[2], std::string("Frame log: draw 0 pipeline placeholder (skipped)"));
  CHECK(c.lines[3].rfind("Frame log: draw 1 ps", 0) == 0);
  CHECK(c.lines[4].rfind("Frame log: resolve 0 rt0@", 0) == 0);
  CHECK_EQ(c.lines[5], std::string("Frame log: frame end, 2 draws, 1 resolves"));
  CHECK(!log.active());
}

TEST(FrameLog_SeveralFramesRestartTheNumbers) {
  Capture c;
  FrameLog log = c.Make("0,2");
  log.OnSwap();
  log.Draw(SampleDraw());
  log.OnSwap();  // frame 1 ends; frame 2 starts at the next swap only (as the Xenos log does)
  CHECK(log.active());
  log.Draw(SampleDraw());
  log.Draw(SampleDraw());
  log.DrawNotDrawn(false);
  log.OnSwap();
  CHECK(!log.active());
  std::vector<std::string> want = {"Frame log: frame start",
                                   "",  // draw 0
                                   "Frame log: frame end, 1 draws, 0 resolves",
                                   "",  // draw 0
                                   "",  // draw 1
                                   "Frame log: draw 1 pipeline missing",
                                   "Frame log: frame end, 2 draws, 0 resolves"};
  CHECK_EQ(c.lines.size(), want.size());
  for (size_t i = 0; i < want.size() && i < c.lines.size(); ++i) {
    if (!want[i].empty()) CHECK_EQ(c.lines[i], want[i]);
  }
  CHECK(c.lines.size() > 3 && c.lines[3].rfind("Frame log: draw 0 ps", 0) == 0);
  CHECK(c.lines.size() > 4 && c.lines[4].rfind("Frame log: draw 1 ps", 0) == 0);
}

TEST(FrameLog_NotDrawnNeedsALoggedDraw) {
  Capture c;
  FrameLog log = c.Make("0");
  log.OnSwap();
  log.DrawNotDrawn(true);  // no draw logged yet: nothing to refer to
  CHECK_EQ(c.lines.size(), size_t(1));
}

NR_TEST_MAIN()
