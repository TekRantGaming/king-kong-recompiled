#pragma once
#include <cstdint>
struct PPCContext;
namespace kk::coop {
void ObjectsLoaded(uint8_t* base);
void ObjectsBeforeAi(PPCContext& ctx,uint8_t* base,uint32_t world);
void ObjectsAfterAi(PPCContext& ctx,uint8_t* base,uint32_t world);
void ObjectsDestroy(PPCContext& ctx,uint8_t* base);
// Observe the final, filtered local gameplay pad. Never writes native input.
void ObjectsPad(uint32_t user,const uint8_t* state,bool connected);
struct ObjectEnqueue {
  uint32_t type=0,source=0,payload=0,output=0,caller=0;
  float seconds=0;
  uint64_t previous=0;
};
ObjectEnqueue ObjectsBeginEnqueue(const PPCContext& input,const uint8_t* base);
void ObjectsEnqueued(const ObjectEnqueue& input,const uint8_t* base);
void ObjectsFirearm(const PPCContext& input,const uint8_t* base);
#if defined(KK_DEV_TOOLS)
void ObjectsDevCamera(PPCContext& ctx,uint8_t* base,uint32_t actor,uint32_t caller);
// Persistent progress bits (door endpoints, leaves): hash and set list for diagnostics.
void ObjectsDevProgressBits(const uint8_t* base,const char* where);
#endif
}
