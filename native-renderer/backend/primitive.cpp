#include "backend/primitive.h"

namespace nr {

bool BuildTriangleList(const PrimitiveInput& in, std::vector<uint32_t>& out) {
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

}  // namespace nr
