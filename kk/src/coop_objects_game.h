#pragma once
#include <cstdint>
struct PPCContext;
namespace kk::coop {
void ObjectsLoaded(uint8_t* base);
void ObjectsBeforeAi(PPCContext& ctx,uint8_t* base,uint32_t world);
void ObjectsAfterAi(PPCContext& ctx,uint8_t* base,uint32_t world);
void ObjectsObserveHit(const PPCContext& ctx,const uint8_t* base);
}
