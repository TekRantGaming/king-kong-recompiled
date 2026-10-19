#pragma once
#include "coop_protocol.h"
#include <algorithm>

namespace kk::coop {
// A personal save may gain only the chapter unlock word. Never import any part
// of the host's serialized world, timestamps, name, continue point or scores.
inline bool GrantTogetherProgress(std::vector<uint8_t>& bytes,uint64_t completed) {
  if (bytes.size()!=kSessionSaveBytes || (completed>>42)!=0) return false;
  auto word=[&](size_t p) { return uint32_t(bytes[p])<<24 | uint32_t(bytes[p+1])<<16 | uint32_t(bytes[p+2])<<8 | bytes[p+3]; };
  if (!word(0x28) || word(0x34)>=42) return false;
  uint32_t unlock=word(0x34);
  for (uint32_t i=0;i<41;++i) if (completed & (uint64_t(1)<<i)) unlock=std::max(unlock,i+1);
  for (int i=0;i<4;++i) bytes[0x34+i]=uint8_t(unlock>>(24-i*8));
  return true;
}
}  // namespace kk::coop
