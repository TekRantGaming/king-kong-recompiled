// Compact, immutable guest bytes for a joined resource job. The caller retains
// storage until the job finishes. Truncated regions use the live converter's
// existing fallback; a compact view must not invent readable endian swap units.
#pragma once

#include <algorithm>
#include <cstring>
#include "kknr/texture_cache.h"

namespace kknr {

struct ResidentSnapshot {
  GuestRange original[2]{}, relocated[2]{};
  bool hashable[2]{};
  GuestMemory memory;

  bool Capture(const GuestRange ranges[2], const GuestMemory& source,
               std::vector<uint8_t>& storage, uint64_t limit) {
    uint64_t size = 0;
    for (unsigned i = 0; i < 2; ++i) {
      original[i] = ranges[i];
      relocated[i] = {ranges[i].bytes ? uint32_t(4096 + size) : 0, ranges[i].bytes};
      hashable[i] = source.At(ranges[i].start, ranges[i].bytes) != nullptr;
      if (ranges[i].bytes && source.Available(ranges[i].start) <
          ((uint64_t(ranges[i].bytes) + 3) & ~uint64_t(3))) return false;
      size = (size + ranges[i].bytes + 4095) & ~uint64_t(4095);
      if (size > limit) return false;
    }
    storage.resize(size);
    std::fill(storage.begin(), storage.end(), 0);
    for (unsigned i = 0; i < 2; ++i) if (ranges[i].bytes) {
      const size_t n = (uint64_t(ranges[i].bytes) + 3) & ~uint64_t(3);
      std::memcpy(storage.data() + relocated[i].start - 4096, source.At(ranges[i].start, uint32_t(n)), n);
    }
    memory = {storage.data(), size, 4096};
    return true;
  }
  uint64_t Hash() const {
    const uint8_t* data[2]{};
    for (unsigned i = 0; i < 2; ++i)
      if (hashable[i]) data[i] = memory.At(relocated[i].start, relocated[i].bytes);
    return HashGuestRanges(original, data);
  }
  TextureFetch Relocate(TextureFetch fetch) const {
    fetch.words[1] = (fetch.words[1] & 0xFFF) | relocated[0].start;
    fetch.words[5] = (fetch.words[5] & 0xFFF) | relocated[1].start;
    return fetch;
  }
};

}  // namespace kknr
