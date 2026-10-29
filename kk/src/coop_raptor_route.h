#pragma once
#include "coop_mut.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace kk::coop::raptor_route {
using Point3 = std::array<float,3>;
inline bool Finite(Point3 p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
// The native matrix builder normalizes both supplied vectors. A zero vector
// produces 0 * infinity and poisons the actor basis before its next movement.
inline bool Normalizable(Point3 p) {
  const float squared=(p[2]*p[2]+p[1]*p[1])+p[0]*p[0];
  return Finite(p) && std::isfinite(squared) && squared>0;
}
inline bool RefuseOrientation(bool enabled,bool raptor,Point3 first,Point3 second) {
  if (COOP_MUT_IF("raptor_orientation_zero")) return false;
  return enabled && raptor && (!Normalizable(first) || !Normalizable(second));
}
// Reset movement only. Keep the state, threat handle, health, target choice,
// transform and all story fields. The native destination selector runs again.
template<class U32,class Put32,class PutFloat,class Clear>
bool ResetRoute(U32 u32,Put32 put,PutFloat putf,Clear native_clear,uint32_t vars,Point3 own) {
  if (COOP_MUT_IF("raptor_route_recovery") || !Finite(own)) return false;
  // The native graph list occupies +7028 through +7144. A corrupt count must
  // not turn its clear loop into a second unbounded walk.
  if (u32(vars+6776)>30) put(vars+6776,0);
  native_clear();
  put(vars+2552,1); // recompute the cell path
  put(vars+2556,0); // stop this frame
  put(vars+2644,0); // discard cached cells
  if (!COOP_MUT_IF("raptor_route_reselect")) put(vars+6760,UINT32_MAX);
  put(vars+6732,u32(vars+6732)&~0x00225E08u);
  putf(vars+5176,0); putf(vars+6932,0);
  for (uint32_t offset : {2560u,2572u,2584u})
    for (uint32_t axis=0;axis<3;++axis) putf(vars+offset+4*axis,own[axis]);
  return true;
}
} // namespace kk::coop::raptor_route
