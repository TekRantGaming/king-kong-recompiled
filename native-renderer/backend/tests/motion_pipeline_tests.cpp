#include "backend/pipeline_cache.h"
#include "backend/tests/check.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace nr;

TEST(MotionPipeline_PreservesVertexAndRasterState) {
  PipelineRecord draw;
  draw.vs_hash = 123;
  draw.vs_input = 456;
  draw.topology = 3;
  draw.depth_format = 40;
  draw.mode_control = 0x804;
  draw.clip_control = 0x10000;
  draw.poly_scale = 0x3f800000;
  draw.poly_offset = 0x40000000;
  draw.flags = kRecordScaled | kRecordFlipFrontFace;
  const auto motion = MotionPipelineRecord(draw, 83);
  CHECK_EQ(motion.vs_hash, draw.vs_hash);
  CHECK_EQ(motion.vs_input, draw.vs_input);
  CHECK_EQ(motion.topology, draw.topology);
  CHECK_EQ(motion.depth_format, draw.depth_format);
  CHECK_EQ(motion.mode_control, draw.mode_control);
  CHECK_EQ(motion.clip_control, draw.clip_control);
  CHECK_EQ(motion.poly_scale, draw.poly_scale);
  CHECK_EQ(motion.poly_offset, draw.poly_offset);
  CHECK_EQ(motion.flags, kRecordMotion | kRecordFlipFrontFace);
  CHECK_EQ(motion.color_format[0], 83u);
  CHECK_EQ(motion.color_count, 1u);
  CHECK_EQ(motion.color_mask[0], 3u);
  CHECK_EQ(motion.color_control[0], 0x10001u);
  CHECK_EQ(motion.depth_control, 34u);
  CHECK_EQ(motion.ps_hash, 0u);
  CHECK_EQ(motion.ps_input, 0u);
  CHECK_EQ(motion.ref_mask, 0u);
}

TEST(MotionPipeline_DeduplicatesScaleAndPixelVariants) {
  PipelineRecord plain;
  plain.vs_hash = 123;
  plain.vs_input = 456;
  plain.depth_format = 40;
  PipelineRecord scaled = plain;
  scaled.flags = kRecordScaled;
  scaled.ps_hash = 789;
  scaled.ps_input = 1011;
  scaled.color_mask[0] = 15;
  scaled.ref_mask = 0xffffff;
  auto first = MotionPipelineRecord(plain, 83);
  auto second = MotionPipelineRecord(scaled, 83);
  CHECK(first == second);
  CHECK_EQ(PipelineKey(first), PipelineKey(second));
  PipelineListMerger merger;
  CHECK_EQ(merger.AddRecords({first, second}), size_t(1));
}

TEST(MotionPipeline_SeparatesLayoutsAndTranslations) {
  PipelineRecord draw;
  draw.vs_hash = 123;
  auto motion = MotionPipelineRecord(draw, 83);
  auto ordinary = motion;
  ordinary.flags = 0;
  CHECK(PipelineKey(motion) != PipelineKey(ordinary));
  auto translated = motion;
  ++translated.vs_input;
  CHECK(PipelineKey(motion) != PipelineKey(translated));
  auto flipped = motion;
  flipped.flags |= kRecordFlipFrontFace;
  CHECK(PipelineKey(motion) != PipelineKey(flipped));
}

TEST(MotionPipeline_SeparatesDepthAndRasterVariants) {
  PipelineRecord draw;
  draw.vs_hash = 123;
  const auto original = MotionPipelineRecord(draw, 83);
  for (int field = 0; field < 7; ++field) {
    auto changed = draw;
    switch (field) {
      case 0: ++changed.topology; break;
      case 1: ++changed.depth_format; break;
      case 2: ++changed.mode_control; break;
      case 3: ++changed.clip_control; break;
      case 4: ++changed.poly_scale; break;
      case 5: ++changed.poly_offset; break;
      case 6: ++changed.vs_input; break;
    }
    CHECK(PipelineKey(original) != PipelineKey(MotionPipelineRecord(changed, 83)));
  }
  CHECK(PipelineKey(original) != PipelineKey(MotionPipelineRecord(draw, 84)));
  CHECK(original == MotionPipelineRecord(original, 83));
}

TEST(MotionPipeline_RecordReloadAndMergeKeepsRealVariants) {
  PipelineRecord draw;
  draw.vs_hash = 123;
  draw.vs_input = 456;
  draw.ps_hash = 789;
  draw.ps_input = 1011;
  draw.depth_format = 40;
  const auto motion = MotionPipelineRecord(draw, 83);
  auto other = motion;
  ++other.vs_input;
  const char* dir = std::getenv("NR_MOTION_TEST_DIR");
  const auto path = std::filesystem::path(dir ? dir : ".") /
      ("motion-pipeline-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".list");
  PipelineCacheFile cache;
  CHECK(cache.Open(path.string()));
  CHECK(cache.Append(draw));
  CHECK(cache.Append(motion));
  CHECK(!cache.Append(MotionPipelineRecord(draw, 83)));
  CHECK(cache.Append(other));
  cache.Close();
  CHECK(cache.Open(path.string()));
  CHECK_EQ(cache.loaded().size(), size_t(3));
  if (cache.loaded().size() == 3) {
    CHECK(cache.loaded()[0] == draw);
    CHECK(cache.loaded()[1] == motion);
    CHECK(cache.loaded()[2] == other);
    CHECK_EQ(PipelineKey(cache.loaded()[1]), PipelineKey(motion));
  }
  PipelineListMerger merger;
  CHECK_EQ(merger.AddRecords(cache.loaded()), size_t(3));
  CHECK_EQ(merger.AddRecords({motion, other}), size_t(0));
  // Retain this synthetic fixture for review; do not remove files during this task.
}

NR_TEST_MAIN()
