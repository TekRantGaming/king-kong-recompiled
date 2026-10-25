// Compatibility names for the measured SIMD paths; product code owns their implementations.
#pragma once
#include "backend/device_capture.h"
#include "backend/shader_constants.h"
#define NR_HOTSPOT_AVX2 NR_AVX2_CODE
namespace nr::test::prototype {
inline bool Avx2Available() { return nr::CpuAvx2Available(); }
#if NR_HOTSPOT_AVX2
using nr::Avx2Constants;
using nr::Avx2Capture;
#endif
}  // namespace nr::test::prototype
