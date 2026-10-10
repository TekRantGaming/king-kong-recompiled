// Guest primitive lists to host triangle lists: the vertex numbers a draw
// uses, read from the big-endian guest index buffer (or counted, for
// non-indexed draws), with the base vertex applied and strips, fans and quads
// expanded. The placeholder pipeline only draws triangle lists.
#pragma once

#include <cstdint>
#include <vector>

#include "backend/guest_layout.h"

namespace nr {

struct PrimitiveInput {
  GuestPrimitive primitive = GuestPrimitive::kTriangleList;
  // Indexed draws: the guest index buffer's data (big-endian), its index
  // size, the first index and the count. Non-indexed: index_data is null,
  // start is the first vertex.
  const uint8_t* index_data = nullptr;
  bool index32 = false;
  uint32_t start = 0;
  uint32_t count = 0;
  int32_t base_vertex = 0;
};

// Appends a triangle list to `out` (vertex numbers). Returns false for
// primitive types the placeholder does not draw (points, lines, rectangles).
// Strips and fans skip triangles that use the 360's reset index (0xFFFF /
// 0xFFFFFFFF), which is how the hardware restarts them when reset is enabled.
bool BuildTriangleList(const PrimitiveInput& in, std::vector<uint32_t>& out);

}  // namespace nr
