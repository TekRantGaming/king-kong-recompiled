// The source is an immutable device snapshot, separate from both cache arrays.
#pragma once
#include "backend/cpu_simd.h"
#include "backend/guest_memory.h"
#include <array>
namespace nr {
inline bool UpdateShaderConstantsScalar(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                                         std::array<float, 1024>& last) {
  bool changed = false;
  for (uint32_t block = 0; block < 4096; block += 64) {
    if (SameBytes64(raw + block, last_raw.data() + block)) continue;
    changed = true;
    std::memcpy(last_raw.data() + block, raw + block, 64);
    for (uint32_t i = block / 4; i < block / 4 + 16; ++i) last[i] = LoadBEFloat(raw + i * 4);
  }
  return changed;
}
#if NR_AVX2_CODE
__attribute__((target("avx2"), noinline)) inline bool Avx2Constants(
    const uint8_t* raw, std::array<uint8_t, 4096>& last_raw, std::array<float, 1024>& last) {
  const auto shuffle = _mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
                                       3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
  bool changed = false;
  for (uint32_t block = 0; block < 4096; block += 64) {
    const auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(raw + block));
    const auto b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(raw + block + 32));
    const auto diff = _mm256_or_si256(
        _mm256_xor_si256(a, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(last_raw.data() + block))),
        _mm256_xor_si256(b, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(last_raw.data() + block + 32))));
    if (_mm256_testz_si256(diff, diff)) continue;
    changed = true;
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(last_raw.data() + block), a);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(last_raw.data() + block + 32), b);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(last.data() + block / 4), _mm256_shuffle_epi8(a, shuffle));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(last.data() + block / 4 + 8), _mm256_shuffle_epi8(b, shuffle));
  }
  return changed;
}
#endif
inline bool UpdateShaderConstants(const uint8_t* raw, std::array<uint8_t, 4096>& last_raw,
                                   std::array<float, 1024>& last) {
#if NR_AVX2_CODE
  if (CpuAvx2Available()) return Avx2Constants(raw, last_raw, last);
#endif
  return UpdateShaderConstantsScalar(raw, last_raw, last);
}
}  // namespace nr
