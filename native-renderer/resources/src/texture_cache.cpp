#include "kknr/texture_cache.h"

#include <algorithm>
#include <cstring>

namespace kknr {

namespace {

constexpr uint32_t kPageShift = 16;  // 64 KB buckets for range lookups

inline uint64_t Mix(uint64_t v) {
  v ^= v >> 33;
  v *= 0xFF51AFD7ED558CCDull;
  v ^= v >> 33;
  v *= 0xC4CEB9FE1A85EC53ull;
  v ^= v >> 33;
  return v;
}

template <typename E>
void IndexEntryRanges(std::unordered_map<uint32_t, std::vector<E*>>& pages, E* e, bool add) {
  for (const GuestRange& r : e->ranges) {
    if (!r.bytes) continue;
    const uint32_t first = r.start >> kPageShift, last = (r.start + r.bytes - 1) >> kPageShift;
    for (uint32_t p = first; p <= last; ++p) {
      std::vector<E*>& list = pages[p];
      if (add) {
        if (std::find(list.begin(), list.end(), e) == list.end()) list.push_back(e);
      } else {
        list.erase(std::remove(list.begin(), list.end(), e), list.end());
        if (list.empty()) pages.erase(p);
      }
    }
  }
}

template <typename E>
void InvalidateIndexed(std::unordered_map<uint32_t, std::vector<E*>>& pages, uint32_t physical, uint32_t bytes) {
  if (!bytes) return;
  const uint32_t first = physical >> kPageShift, last = (physical + bytes - 1) >> kPageShift;
  for (uint32_t p = first; p <= last; ++p) {
    auto it = pages.find(p);
    if (it == pages.end()) continue;
    for (E* e : it->second)
      for (const GuestRange& r : e->ranges)
        if (r.Overlaps(physical, bytes)) e->dirty = true;
  }
}

uint64_t HashResident(const ResidentEntry& e, const GuestMemory& memory) {
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (const GuestRange& r : e.ranges) {
    if (!r.bytes) continue;
    const uint8_t* p = memory.At(r.start, r.bytes);
    h = p ? HashBytes(p, r.bytes, h) : Mix(h ^ r.start);
  }
  return h;
}

void Watch(const std::function<void(uint32_t, uint32_t)>& watch, const ResidentEntry& e) {
  if (!watch) return;
  for (const GuestRange& r : e.ranges)
    if (r.bytes) watch(r.start, r.bytes);
}

}  // namespace

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  // Four independent 64-bit lanes over 32-byte chunks, then the tail; fast enough to re-check a texture.
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t lanes[4] = {seed ^ 0x243F6A8885A308D3ull, seed ^ 0x13198A2E03707344ull, seed ^ 0xA4093822299F31D0ull,
                       seed ^ 0x082EFA98EC4E6C89ull};
  size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    for (int k = 0; k < 4; ++k) {
      uint64_t v;
      std::memcpy(&v, p + i + 8 * k, 8);
      lanes[k] = (lanes[k] ^ v) * 0x9E3779B97F4A7C15ull;
      lanes[k] ^= lanes[k] >> 29;
    }
  }
  uint64_t h = Mix(lanes[0]) ^ Mix(lanes[1] + 1) ^ Mix(lanes[2] + 2) ^ Mix(lanes[3] + 3) ^ uint64_t(size);
  for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return Mix(h);
}

TextureKey MakeTextureKey(const TextureFetch& fetch) {
  TextureKey k;
  k.words[0] = fetch.words[0] & ~(0x1FFu << 10);          // drop clamp x / y / z
  k.words[1] = fetch.words[1] & ~(1u << 11);              // drop the nearest-clamp policy
  k.words[2] = fetch.words[2];                            // size
  k.words[3] = fetch.words[3] & 1u;                       // number format only (swizzle, filters: view / sampler)
  k.words[4] = fetch.words[4] & (0xFFu << 2);             // mip min / max level
  k.words[5] = fetch.words[5] & ~0x1FFu;                  // dimension, packed mips, mip address
  return k;
}

TextureCache::Entry* TextureCache::Find(const TextureFetch& fetch) {
  auto it = entries_.find(MakeTextureKey(fetch));
  return it == entries_.end() ? nullptr : it->second.get();
}

TextureCache::Entry& TextureCache::Create(const TextureFetch& fetch, const TextureKey& key) {
  auto e = std::make_unique<Entry>();
  e->key = key;
  e->fetch = fetch;
  const TextureRanges r = GetTextureRanges(fetch, options);
  e->ranges[0] = {r.base, r.base_bytes};
  e->ranges[1] = {r.mip, r.mip_bytes};
  Entry& ref = *e;
  entries_[key] = std::move(e);
  IndexRanges(&ref, true);
  return ref;
}

void TextureCache::IndexRanges(Entry* e, bool add) { IndexEntryRanges(pages_, e, add); }

uint64_t TextureCache::HashRanges(const ResidentEntry& e, const GuestMemory& memory) const {
  return HashResident(e, memory);
}

TextureCache::BindResult TextureCache::Bind(const TextureFetch& fetch, const GuestMemory& memory, uint64_t frame) {
  const TextureKey key = MakeTextureKey(fetch);
  BindResult result;
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    result.entry = &Create(fetch, key);
    result.entry->last_used_frame = frame;
    result.action = BindAction::kCreateAndUpload;
    return result;
  }
  Entry& e = *it->second;
  e.last_used_frame = frame;
  result.entry = &e;
  if (!e.dirty) return result;
  // Written since the last upload: only a real change of content costs an upload.
  const uint64_t hash = HashRanges(e, memory);
  if (hash == e.content_hash && !e.gpu_written) {
    e.dirty = false;
    Watch(watch_range, e);
    return result;
  }
  result.action = BindAction::kReupload;
  return result;
}

void TextureCache::OnUploaded(Entry& e, const GuestMemory& memory) {
  e.content_hash = HashRanges(e, memory);
  e.dirty = false;
  e.gpu_written = false;
  ++e.uploads;
  Watch(watch_range, e);
}

void TextureCache::InvalidateRange(uint32_t physical, uint32_t bytes) { InvalidateIndexed(pages_, physical, bytes); }

TextureCache::Entry& TextureCache::MarkGpuWritten(const TextureFetch& fetch, uint64_t frame) {
  const TextureKey key = MakeTextureKey(fetch);
  auto it = entries_.find(key);
  Entry& e = it == entries_.end() ? Create(fetch, key) : *it->second;
  e.gpu_written = true;
  e.dirty = false;
  e.content_hash = 0;
  e.last_used_frame = frame;
  Watch(watch_range, e);  // a later CPU write makes guest memory the truth again
  return e;
}

size_t TextureCache::Trim(uint64_t frame, uint64_t max_age) {
  size_t dropped = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& e = *it->second;
    if (frame > e.last_used_frame + max_age) {
      if (release_host) release_host(e);
      IndexRanges(&e, false);
      it = entries_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

BufferCache::BindResult BufferCache::Bind(uint32_t physical, uint32_t bytes, uint64_t conversion_id,
                                          const GuestMemory& memory, uint64_t frame) {
  const uint64_t key = Mix(uint64_t(physical) << 32 ^ bytes) ^ Mix(conversion_id + 0x51ED27);
  BindResult result;
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    auto e = std::make_unique<Entry>();
    e->key = key;
    e->physical = physical;
    e->bytes = bytes;
    e->conversion_id = conversion_id;
    e->ranges[0] = {physical, bytes};
    e->last_used_frame = frame;
    result.entry = e.get();
    IndexEntryRanges(pages_, e.get(), true);
    entries_[key] = std::move(e);
    result.action = BindAction::kCreateAndUpload;
    return result;
  }
  Entry& e = *it->second;
  e.last_used_frame = frame;
  result.entry = &e;
  if (!e.dirty) return result;
  if (HashResident(e, memory) == e.content_hash) {
    e.dirty = false;
    Watch(watch_range, e);
    return result;
  }
  result.action = BindAction::kReupload;
  return result;
}

void BufferCache::OnUploaded(Entry& e, const GuestMemory& memory) {
  e.content_hash = HashResident(e, memory);
  e.dirty = false;
  ++e.uploads;
  Watch(watch_range, e);
}

void BufferCache::InvalidateRange(uint32_t physical, uint32_t bytes) { InvalidateIndexed(pages_, physical, bytes); }

size_t BufferCache::Trim(uint64_t frame, uint64_t max_age) {
  size_t dropped = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& e = *it->second;
    if (frame > e.last_used_frame + max_age) {
      if (release_host) release_host(e);
      IndexEntryRanges(pages_, &e, false);
      it = entries_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  return dropped;
}

}  // namespace kknr
