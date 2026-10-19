#pragma once
#if defined(KK_DEV_TOOLS)
#include <cstdint>
#include <rex/hook.h>
namespace kk {
// Only the explicit spawn path arms these probes. End runs inside its existing
// preserved guest Scratch frame, after the helper's mode-13 reset has returned.
void DevPuppetVariantBegin(const uint8_t* base, uint32_t source, uint32_t native_source, uint64_t epoch);
void DevPuppetVariantEnd(PPCContext& ctx, uint8_t* base, uint32_t copy, uint32_t world);
void DevPuppetVariantReset();
}
#endif
