// Keep AVX2 instructions in separate functions, entered only after the OS feature check.
#pragma once
#if (defined(__clang__) || defined(__GNUC__)) && (defined(__x86_64__) || defined(_M_X64))
#define NR_AVX2_CODE 1
#include <immintrin.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#else
#define NR_AVX2_CODE 0
#endif
namespace nr {
inline bool CpuAvx2Available() {
#if NR_AVX2_CODE
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
}  // namespace nr
