#pragma once
#include "backend/render_thread.h"
#include <array>
#if (defined(__clang__) || defined(__GNUC__)) && (defined(__x86_64__) || defined(_M_X64))
#define NR_HOTSPOT_AVX2 1
#include <immintrin.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#else
#define NR_HOTSPOT_AVX2 0
#endif

namespace nr::test::prototype {
inline bool Avx2Available() {
#if NR_HOTSPOT_AVX2
#if defined(_WIN32) && defined(PF_AVX2_INSTRUCTIONS_AVAILABLE)
  static const bool available = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
#elif !defined(_WIN32)
  static const bool available = __builtin_cpu_supports("avx2");
#else
  static const bool available = false;
#endif
  return available;
#else
  return false;
#endif
}
#if NR_HOTSPOT_AVX2
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
}  // namespace nr::test::prototype
