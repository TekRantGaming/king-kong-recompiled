#include "backend/pipeline_cache.h"
#include "backend/tests/check.h"

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

NR_TEST_MAIN()
