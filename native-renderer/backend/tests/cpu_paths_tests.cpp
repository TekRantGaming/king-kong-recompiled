#include <array>
#include <cstring>
#include "backend/pipeline_workers.h"
#include "backend/primitive.h"
#include "backend/guest_memory.h"
#include "backend/tests/check.h"
#include "kknr/buffers.h"

using namespace nr;

TEST(WorkerBudgets) {
  CHECK_EQ(PipelineWorkerCount(0, 0), 1u);
  CHECK_EQ(PipelineWorkerCount(1, 1), 1u);
  CHECK_EQ(PipelineWorkerCount(4, 4), 1u);
  CHECK_EQ(PipelineWorkerCount(6, 6), 1u);
  CHECK_EQ(PipelineWorkerCount(8, 4), 2u);
  CHECK_EQ(PipelineWorkerCount(12, 6), 3u);
  CHECK_EQ(PipelineWorkerCount(16, 8), 4u);
  CHECK_EQ(PipelineWorkerCount(16, 2), 1u);
  CHECK_EQ(PipelineWorkerCount(128, 64), 4u);
}

#ifdef _WIN32
TEST(WorkerCountUsesRestrictedProcessAffinity) {
  DWORD_PTR original = 0, system = 0;
  CHECK(GetProcessAffinityMask(GetCurrentProcess(), &original, &system));
  if (!original) return;
  struct Restore {
    DWORD_PTR mask;
    ~Restore() { SetProcessAffinityMask(GetCurrentProcess(), mask); }
  } restore{original};
  const DWORD_PTR one = original & (~original + 1);
  CHECK(SetProcessAffinityMask(GetCurrentProcess(), one));
  CHECK_EQ(ProcessPipelineWorkerCount(), 1u);
}
#endif

TEST(DeclarationScratchMatchesFieldsAndRetainsStorage) {
  std::array<uint8_t, 36 + 65 * 12> data{};
  StoreBE32(data.data() + 8, 65);
  for (unsigned i = 0; i < 65; ++i) {
    auto* p = data.data() + 36 + i * 12;
    StoreBE16(p, uint16_t(i % 16));
    StoreBE16(p + 2, uint16_t(i * 4));
    StoreBE32(p + 4, kknr::kDeclShort4N.value);
    p[8] = 1; p[9] = 2; p[10] = uint8_t(i); p[11] = 3;
  }
  std::vector<kknr::VertexElement> out;
  kknr::DecodeVertexDeclaration(data.data(), out, data.size());
  CHECK_EQ(out.size(), size_t(64));
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i].stream, i % 16);
    CHECK_EQ(out[i].offset, i * 4);
    CHECK_EQ(out[i].type.value, kknr::kDeclShort4N.value);
    CHECK_EQ(out[i].method, 1); CHECK_EQ(out[i].usage, 2);
    CHECK_EQ(out[i].usage_index, i); CHECK_EQ(out[i].extra, 3);
  }
  const auto* storage = out.data();
  const auto capacity = out.capacity();
  StoreBE16(data.data() + 36 + 3 * 12, 0xFF);
  kknr::DecodeVertexDeclaration(data.data(), out, data.size());
  CHECK_EQ(out.size(), size_t(3));
  CHECK(out.data() == storage); CHECK_EQ(out.capacity(), capacity);
  StoreBE32(data.data() + 36 + 12 + 4, 0xFFFFFFFF);
  kknr::DecodeVertexDeclaration(data.data(), out, data.size());
  CHECK_EQ(out.size(), size_t(1));
  kknr::DecodeVertexDeclaration(data.data(), out, 36);
  CHECK(out.empty());
  kknr::DecodeVertexDeclaration(nullptr, out);
  CHECK(out.empty()); CHECK_EQ(out.capacity(), capacity);
}

TEST(IndexScratchMatchesOldUploadAndClearsPadding) {
  std::array<uint8_t, 260> data{};
  for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 37 + 11);
  std::vector<uint8_t> scratch;
  kknr::ConvertIndexBuffer(data.data(), scratch, 260, true);
  const auto* storage = scratch.data();
  const auto capacity = scratch.capacity();
  for (bool wide : {false, true}) {
    for (uint32_t bytes : {260u, 128u, 7u, 6u, 3u, 2u, 1u, 0u, 260u}) {
      std::vector<uint8_t> reference((size_t(bytes) + 3) & ~size_t(3), 0);
      kknr::ConvertIndices(data.data(), reference.data(), bytes / (wide ? 4 : 2), wide);
      kknr::ConvertIndexBuffer(data.data(), scratch, bytes, wide);
      CHECK(scratch == reference);
      CHECK(scratch.data() == storage); CHECK_EQ(scratch.capacity(), capacity);
      for (uint32_t i = 0; i < bytes / (wide ? 4 : 2); ++i) {
        if (wide) {
          uint32_t actual; std::memcpy(&actual, scratch.data() + i * 4, 4);
          CHECK_EQ(actual, LoadBE32(data.data() + i * 4));
        } else {
          uint16_t actual; std::memcpy(&actual, scratch.data() + i * 2, 2);
          CHECK_EQ(actual, LoadBE16(data.data() + i * 2));
        }
      }
    }
  }
}

// Frozen pre-task implementation, used to compare primitive expansion below.
bool ReferenceTriangleList(const PrimitiveInput&, std::vector<uint32_t>&);

TEST(PrimitiveConversionMatchesBeforeForAllDrawShapes) {
  std::array<uint8_t, 128> data{};
  std::vector<uint32_t> out, reference;
  out.reserve(256);
  const auto* storage = out.data();
  for (auto shape : {GuestPrimitive::kTriangleList, GuestPrimitive::kTriangleStrip,
                     GuestPrimitive::kTriangleFan, GuestPrimitive::kQuadList,
                     GuestPrimitive::kPointList}) {
    for (bool wide : {false, true}) {
      for (int reset_at = -1; reset_at < 16; ++reset_at) {
        for (unsigned i = 0; i < 32; ++i) {
          const uint32_t v = int(i) == reset_at ? (wide ? 0xFFFFFFFF : 0xFFFF) : i % 11;
          if (wide) StoreBE32(data.data() + i * 4, v);
          else StoreBE16(data.data() + i * 2, uint16_t(v));
        }
        for (bool indexed : {false, true}) {
          for (uint32_t count = 0; count < 20; ++count) {
            for (int base : {-3, 0, 7}) {
              PrimitiveInput in{shape, indexed ? data.data() : nullptr, wide, 2, count, base};
              CHECK_EQ(BuildTriangleList(in, out), ReferenceTriangleList(in, reference));
              CHECK(out == reference); CHECK(out.data() == storage);
            }
          }
        }
      }
    }
  }
}

NR_TEST_MAIN()





bool ReferenceTriangleList(const PrimitiveInput& in, std::vector<uint32_t>& out) {
  out.clear();
  const uint32_t reset = in.index32 ? 0xFFFFFFFFu : 0xFFFFu;
  // Raw guest vertex numbers before the base vertex; reset marks restarts.
  std::vector<uint32_t> raw(in.count);
  for (uint32_t i = 0; i < in.count; ++i) {
    if (in.index_data) {
      raw[i] = in.index32 ? LoadBE32(in.index_data + size_t(in.start + i) * 4)
                          : LoadBE16(in.index_data + size_t(in.start + i) * 2);
    } else {
      raw[i] = in.start + i;
    }
  }
  const bool indexed = in.index_data != nullptr;
  auto is_reset = [&](uint32_t v) { return indexed && v == reset; };
  auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (is_reset(a) || is_reset(b) || is_reset(c)) return;
    out.push_back(uint32_t(int64_t(a) + in.base_vertex));
    out.push_back(uint32_t(int64_t(b) + in.base_vertex));
    out.push_back(uint32_t(int64_t(c) + in.base_vertex));
  };
  const uint32_t n = in.count;
  switch (in.primitive) {
    case GuestPrimitive::kTriangleList:
      for (uint32_t i = 0; i + 2 < n; i += 3) emit(raw[i], raw[i + 1], raw[i + 2]);
      return true;
    case GuestPrimitive::kTriangleStrip: {
      // Winding alternates; a reset restarts the strip (and its parity).
      uint32_t run = 0;
      for (uint32_t i = 0; i < n; ++i) {
        if (is_reset(raw[i])) {
          run = 0;
          continue;
        }
        if (++run >= 3) {
          if (((run - 3) & 1) == 0) {
            emit(raw[i - 2], raw[i - 1], raw[i]);
          } else {
            emit(raw[i - 1], raw[i - 2], raw[i]);
          }
        }
      }
      return true;
    }
    case GuestPrimitive::kTriangleFan:
      for (uint32_t i = 1; i + 1 < n; ++i) emit(raw[0], raw[i], raw[i + 1]);
      return true;
    case GuestPrimitive::kQuadList:
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        emit(raw[i], raw[i + 1], raw[i + 2]);
        emit(raw[i], raw[i + 2], raw[i + 3]);
      }
      return true;
    default:
      return false;
  }
}


