#pragma once
#include "backend/guest_memory.h"
#include <array>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace nr::test::prototype {
// Byte conversion only: NaN payloads, signed zero and unaligned guest addresses are preserved.
inline bool UpdateShaderConstants(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                                  std::array<float, 1024>& last) {
  bool changed = false;
  for (uint32_t block = 0; block < 4096; block += 64) {
#if defined(__SSE2__) || defined(_M_X64)
    __m128i values[4];
    __m128i diff = _mm_setzero_si128();
    for (unsigned j = 0; j < 4; ++j) {
      values[j] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(raw + block + j * 16));
      diff = _mm_or_si128(diff, _mm_xor_si128(values[j],
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(last_raw.data() + block + j * 16))));
    }
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xffff) continue;
    changed = true;
    for (unsigned j = 0; j < 4; ++j) {
      _mm_storeu_si128(reinterpret_cast<__m128i*>(last_raw.data() + block + j * 16), values[j]);
      auto v = _mm_or_si128(_mm_srli_epi16(values[j], 8), _mm_slli_epi16(values[j], 8));
      v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
      v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(last.data() + block / 4 + j * 4), v);
    }
#else
    if (SameBytes64(raw + block, last_raw.data() + block)) continue;
    changed = true;
    std::memcpy(last_raw.data() + block, raw + block, 64);
    for (uint32_t i = block / 4; i < block / 4 + 16; ++i) last[i] = LoadBEFloat(raw + i * 4);
#endif
  }
  return changed;
}
}  // namespace nr::test::prototype
