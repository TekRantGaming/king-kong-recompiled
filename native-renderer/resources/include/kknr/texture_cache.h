// Upload policy for guest resources: which host texture / buffer a bind needs, and whether it must be
// (re)uploaded. Backend-agnostic: entries carry an opaque host handle the renderer fills in.
//
// Policy (docs/formats.md, "Upload policy"):
// - A host texture is identified by the fetch-constant fields that change its data (TextureKey); sampler
//   state and the swizzle are view / sampler properties, not part of it.
// - Created and uploaded on first bind. Afterwards it is re-uploaded only when its guest memory was written
//   (InvalidateRange, fed by the SDK's physical-memory write watches or by Unlock) AND the content hash of its
//   guest ranges changed. Writes of identical data, or no writes, cost nothing at bind time.
// - Resolve destinations are GPU-written (MarkGpuWritten): their host copy is the truth and guest memory is
//   never read for them (our renderer does not write resolves back to guest memory) until the CPU writes there.
// - Buffers (index / vertex) follow the same rules keyed by (address, size, conversion).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "kknr/texture_convert.h"
#include "kknr/xenos.h"

namespace kknr {

uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0);

struct TextureKey {
  uint32_t words[6] = {};
  bool operator==(const TextureKey& o) const {
    for (int i = 0; i < 6; ++i)
      if (words[i] != o.words[i]) return false;
    return true;
  }
};
struct TextureKeyHash {
  size_t operator()(const TextureKey& k) const { return size_t(HashBytes(k.words, sizeof(k.words))); }
};
// The fetch constant without the sampler / view fields (clamp, filters, LOD, border, swizzle, exp adjust).
TextureKey MakeTextureKey(const TextureFetch& fetch);

// A guest memory range owned by an entry, physical addresses.
struct GuestRange {
  uint32_t start = 0, bytes = 0;
  bool Overlaps(uint32_t s, uint32_t n) const { return bytes && n && s < start + bytes && start < s + n; }
};

// Shared bookkeeping for entries that live in guest memory: dirty marking by range, content hashes, LRU.
struct ResidentEntry {
  GuestRange ranges[2];
  uint64_t content_hash = 0;
  uint64_t last_used_frame = 0;
  uint32_t uploads = 0;        // how many times the host copy was (re)built
  bool dirty = false;          // guest memory written since the last upload / check
  bool gpu_written = false;    // a resolve wrote the host copy; guest memory is stale
  void* host = nullptr;        // the renderer's host resource
};

enum class BindAction : uint8_t {
  kUseExisting,      // host copy is current
  kCreateAndUpload,  // new entry: create the host resource and upload
  kReupload,         // guest data changed: upload again (same host resource description)
};

class TextureCache {
 public:
  struct Entry : ResidentEntry {
    TextureKey key;
    TextureFetch fetch;  // as first seen (for conversion)
  };
  struct BindResult {
    Entry* entry = nullptr;
    BindAction action = BindAction::kUseExisting;
  };

  // Called when an entry needs its guest ranges watched for CPU writes again (after an upload or check).
  std::function<void(uint32_t physical, uint32_t bytes)> watch_range;
  // Called before an entry is dropped, to free its host resource.
  std::function<void(Entry&)> release_host;

  BindResult Bind(const TextureFetch& fetch, const GuestMemory& memory, uint64_t frame);
  // The renderer reports a finished upload (or creation) so the entry's state and watches are current.
  void OnUploaded(Entry& entry, const GuestMemory& memory);

  // Guest memory [physical, physical + bytes) was written by the CPU (write watch or Unlock).
  void InvalidateRange(uint32_t physical, uint32_t bytes);
  // A resolve wrote this texture on the GPU.
  Entry& MarkGpuWritten(const TextureFetch& fetch, uint64_t frame);

  // Drops entries unused for more than max_age frames. Returns how many were dropped.
  size_t Trim(uint64_t frame, uint64_t max_age);
  size_t size() const { return entries_.size(); }
  Entry* Find(const TextureFetch& fetch);

 private:
  Entry& Create(const TextureFetch& fetch, const TextureKey& key);
  void IndexRanges(Entry* e, bool add);
  uint64_t HashRanges(const ResidentEntry& e, const GuestMemory& memory) const;

  std::unordered_map<TextureKey, std::unique_ptr<Entry>, TextureKeyHash> entries_;
  // 64 KB page -> entries touching it, for InvalidateRange.
  std::unordered_map<uint32_t, std::vector<Entry*>> pages_;
};

// Index and vertex buffers: one host buffer per (guest range, conversion id). conversion_id identifies the
// swap plan (index size, or a hash of the vertex swap ranges and stride).
class BufferCache {
 public:
  struct Entry : ResidentEntry {
    uint64_t key = 0;
    uint32_t physical = 0, bytes = 0;
    uint64_t conversion_id = 0;
  };
  struct BindResult {
    Entry* entry = nullptr;
    BindAction action = BindAction::kUseExisting;
  };
  std::function<void(uint32_t physical, uint32_t bytes)> watch_range;
  std::function<void(Entry&)> release_host;

  BindResult Bind(uint32_t physical, uint32_t bytes, uint64_t conversion_id, const GuestMemory& memory,
                  uint64_t frame);
  void OnUploaded(Entry& entry, const GuestMemory& memory);
  void InvalidateRange(uint32_t physical, uint32_t bytes);
  size_t Trim(uint64_t frame, uint64_t max_age);
  size_t size() const { return entries_.size(); }

 private:
  std::unordered_map<uint64_t, std::unique_ptr<Entry>> entries_;
  std::unordered_map<uint32_t, std::vector<Entry*>> pages_;
};

}  // namespace kknr
