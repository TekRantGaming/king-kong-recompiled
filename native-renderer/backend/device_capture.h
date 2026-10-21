// The game's immutable draw-time device image, encoded in the original block order.
#pragma once
#include "backend/render_thread.h"
#include "backend/cpu_simd.h"
namespace nr {
#if NR_AVX2_CODE
__attribute__((target("avx2"), noinline)) inline void Avx2Capture(
    const uint8_t* live, uint8_t* sent, bool all,
    std::vector<uint16_t>& out, std::vector<uint8_t>& bytes) {
  const auto& blocks = ThreadedSink::DeviceBlocks();
  for (size_t i = 0; i < blocks.size(); ++i) {
    const auto& b = blocks[i];
    if (!all) {
      if (b.size == 64) {
        const auto diff = _mm256_or_si256(
          _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(live + b.offset)),
                           _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sent + b.offset))),
          _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(live + b.offset + 32)),
                           _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sent + b.offset + 32))));
        if (_mm256_testz_si256(diff, diff)) continue;
      } else if (std::memcmp(live + b.offset, sent + b.offset, b.size) == 0) continue;
    }
    std::memcpy(sent + b.offset, live + b.offset, b.size);
    out.push_back(uint16_t(i));
    bytes.insert(bytes.end(), live + b.offset, live + b.offset + b.size);
  }
}
#endif
inline bool TryCaptureDeviceAvx2(const uint8_t* live, uint8_t* sent, bool all,
                                 std::vector<uint16_t>& out, std::vector<uint8_t>& bytes) {
#if NR_AVX2_CODE
  if (CpuAvx2Available()) {
    Avx2Capture(live, sent, all, out, bytes);
    return true;
  }
#endif
  return false;
}
}  // namespace nr
