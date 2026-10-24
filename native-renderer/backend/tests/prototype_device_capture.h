#pragma once
#include "backend/guest_memory.h"
#include <vector>

namespace nr::test::prototype {
// The exact read ranges and block order of ThreadedSink::DeviceBlocks. Keeping the sizes in the
// template makes full blocks fixed-size compares/copies; partial tails retain their exact size.
inline constexpr uint32_t kDeviceReadRanges[][2] = {{1152, 10272}, {11424, 12000}, {12532, 12944},
                                                   {13960, 13968}, {14060, 15596}};
template<uint32_t Bytes>
inline void CaptureDeviceBlock(const uint8_t* live, uint8_t* sent, uint32_t offset, uint16_t index,
                               bool all, std::vector<uint16_t>& blocks, std::vector<uint8_t>& bytes) {
  if (!all && (Bytes == 64 ? SameBytes64(live + offset, sent + offset)
                          : std::memcmp(live + offset, sent + offset, Bytes) == 0)) return;
  std::memcpy(sent + offset, live + offset, Bytes);
  blocks.push_back(index);
  bytes.insert(bytes.end(), live + offset, live + offset + Bytes);
}
template<uint32_t Start, uint32_t End>
inline void CaptureDeviceRange(const uint8_t* live, uint8_t* sent, bool all, uint16_t& index,
                              std::vector<uint16_t>& blocks, std::vector<uint8_t>& bytes) {
  constexpr uint32_t full_end = Start + ((End - Start) / 64) * 64;
  for (uint32_t offset = Start; offset < full_end; offset += 64)
    CaptureDeviceBlock<64>(live, sent, offset, index++, all, blocks, bytes);
  if constexpr (full_end < End)
    CaptureDeviceBlock<End - full_end>(live, sent, full_end, index++, all, blocks, bytes);
}
inline void CaptureDeviceImage(const uint8_t* live, uint8_t* sent, bool all,
                               std::vector<uint16_t>& blocks, std::vector<uint8_t>& bytes) {
  uint16_t index = 0;
  CaptureDeviceRange<1152, 10272>(live, sent, all, index, blocks, bytes);
  CaptureDeviceRange<11424, 12000>(live, sent, all, index, blocks, bytes);
  CaptureDeviceRange<12532, 12944>(live, sent, all, index, blocks, bytes);
  CaptureDeviceRange<13960, 13968>(live, sent, all, index, blocks, bytes);
  CaptureDeviceRange<14060, 15596>(live, sent, all, index, blocks, bytes);
}
}  // namespace nr::test::prototype
