// Upload policy: create on first bind, re-upload only when guest memory was written AND its content changed,
// sampler-only differences share one host texture, resolves make the host copy the truth.
#include <cstring>
#include <vector>

#include "kknr/texture_cache.h"
#include "kknr/resident_snapshot.h"
#include "test.h"

using namespace kknr;
using kknr_test::FakeGuest;

namespace {

constexpr uint32_t kBase = 0x00200000, kMips = 0x00300000;

TextureFetch Fetch64(uint32_t base = kBase, uint32_t mip = kMips) {
  TextureFetchDesc d;
  d.format = TextureFormat::k_8_8_8_8;
  d.endian = Endian::k8in32;
  d.tiled = true;
  d.packed_mips = true;
  d.width = d.height = 64;
  d.base_address = base;
  d.mip_address = mip;
  d.max_level = 6;
  return MakeTextureFetch(d);
}

}  // namespace

TEST(texture_cache_upload_only_on_change) {
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  std::vector<std::pair<uint32_t, uint32_t>> watched;
  cache.watch_range = [&](uint32_t a, uint32_t n) { watched.push_back({a, n}); };

  const TextureFetch f = Fetch64();
  TextureCache::BindResult r = cache.Bind(f, memory, 1);
  CHECK(r.action == BindAction::kCreateAndUpload);
  cache.OnUploaded(*r.entry, memory);
  CHECK_EQ(r.entry->uploads, 1u);
  CHECK_EQ(watched.size(), size_t(2));  // base and mips
  if (watched.size() == 2) {
    CHECK_EQ(watched[0].first, kBase);
    CHECK_EQ(watched[1].first, kMips);
  }

  // No write: nothing to do.
  CHECK(cache.Bind(f, memory, 2).action == BindAction::kUseExisting);
  // A write somewhere else: nothing to do.
  cache.InvalidateRange(0x00500000, 4096);
  CHECK(!r.entry->dirty);
  // A write of the same bytes: dirty, but the hash shows no change; no upload.
  cache.InvalidateRange(kBase + 100, 4);
  CHECK(r.entry->dirty);
  CHECK(cache.Bind(f, memory, 3).action == BindAction::kUseExisting);
  CHECK(!r.entry->dirty);
  // A real change in the mip region: re-upload.
  guest.bytes[kMips + 8] = 0x5A;
  cache.InvalidateRange(kMips + 8, 1);
  CHECK(cache.Bind(f, memory, 4).action == BindAction::kReupload);
  cache.OnUploaded(*r.entry, memory);
  CHECK_EQ(r.entry->uploads, 2u);
  CHECK(cache.Bind(f, memory, 5).action == BindAction::kUseExisting);
}

TEST(texture_cache_watches_once_until_a_write) {
  // Arming a watch is costly (every page of the range is protected): an entry still watched is not
  // watched again, by a check, an upload or a resolve; a write disarms it.
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  int calls = 0;
  cache.watch_range = [&](uint32_t, uint32_t) { ++calls; };
  const TextureFetch f = Fetch64();
  TextureCache::BindResult r = cache.Bind(f, memory, 1);
  cache.OnUploaded(*r.entry, memory);
  CHECK_EQ(calls, 2);  // base and mips
  CHECK(r.entry->watched);
  cache.MarkGpuWritten(f, 2);  // a resolve into it, every frame in the game
  cache.MarkGpuWritten(f, 3);
  CHECK_EQ(calls, 2);
  cache.InvalidateRange(kBase + 100, 4);
  CHECK(!r.entry->watched);
  CHECK(cache.Bind(f, memory, 4).action == BindAction::kReupload);  // the resolve's copy is stale now
  cache.OnUploaded(*r.entry, memory);
  CHECK_EQ(calls, 4);
  CHECK(r.entry->watched);
}

TEST(texture_cache_key_ignores_sampler_state) {
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  TextureFetch a = Fetch64();
  TextureFetch b = a;
  b.words[0] |= 2u << 10;                 // clamp x
  b.words[3] |= 1u << 19 | 0x123u << 1;   // mag filter, swizzle
  b.words[4] |= 0x3Fu << 12;              // LOD bias
  b.words[5] |= 1;                        // border colour
  TextureCache::BindResult ra = cache.Bind(a, memory, 1);
  TextureCache::BindResult rb = cache.Bind(b, memory, 1);
  CHECK(ra.entry == rb.entry);
  CHECK_EQ(cache.size(), size_t(1));
  // A different mip address or format is another texture.
  cache.Bind(Fetch64(kBase, kMips + 0x10000), memory, 1);
  CHECK_EQ(cache.size(), size_t(2));
}

TEST(texture_cache_gpu_written_and_trim) {
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  int released = 0;
  cache.release_host = [&](TextureCache::Entry&) { ++released; };
  const TextureFetch f = Fetch64();
  // A resolve target: the host copy is the truth until the CPU writes there.
  TextureCache::Entry& e = cache.MarkGpuWritten(f, 1);
  CHECK(e.gpu_written);
  CHECK(cache.Bind(f, memory, 2).action == BindAction::kUseExisting);
  guest.bytes[kBase] = 1;
  cache.InvalidateRange(kBase, 1);
  CHECK(cache.Bind(f, memory, 3).action == BindAction::kReupload);
  // Unused for longer than max_age: dropped, host resource released.
  cache.Bind(Fetch64(0x00400000, 0), memory, 10);
  CHECK_EQ(cache.Trim(20, 5), size_t(2));
  CHECK_EQ(released, 2);
  CHECK_EQ(cache.size(), size_t(0));
  // Dropped entries no longer react to writes (no dangling page index).
  cache.InvalidateRange(kBase, 1 << 20);
}

TEST(buffer_cache_policy) {
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  BufferCache cache;
  BufferCache::BindResult r = cache.Bind(0x10000, 4096, 7, memory, 1);
  CHECK(r.action == BindAction::kCreateAndUpload);
  cache.OnUploaded(*r.entry, memory);
  CHECK(cache.Bind(0x10000, 4096, 7, memory, 2).action == BindAction::kUseExisting);
  // The same bytes through another swap plan are another host buffer.
  CHECK(cache.Bind(0x10000, 4096, 8, memory, 2).action == BindAction::kCreateAndUpload);
  cache.InvalidateRange(0x10010, 4);
  CHECK(cache.Bind(0x10000, 4096, 7, memory, 3).action == BindAction::kUseExisting);
  guest.bytes[0x10010] = 9;
  cache.InvalidateRange(0x10010, 4);
  CHECK(cache.Bind(0x10000, 4096, 7, memory, 4).action == BindAction::kReupload);
  CHECK_EQ(cache.Trim(100, 10), size_t(2));
}

TEST(hash_bytes_sensitivity) {
  std::vector<uint8_t> a(1000, 0), b(1000, 0);
  CHECK_EQ(HashBytes(a.data(), a.size()), HashBytes(b.data(), b.size()));
  for (size_t i : {size_t(0), size_t(31), size_t(32), size_t(999)}) {
    b = a;
    b[i] = 1;
    CHECK(HashBytes(a.data(), a.size()) != HashBytes(b.data(), b.size()));
  }
  CHECK(HashBytes(a.data(), 999) != HashBytes(a.data(), 1000));
}

TEST(cache_reuses_checked_hash_and_invalidates_it) {
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  ResourceWork work;
  TextureCache textures; BufferCache buffers;
  textures.profile = buffers.profile = &work;
  const auto fetch = Fetch64();
  auto* t = textures.Bind(fetch, memory, 0).entry;
  auto* b = buffers.Bind(kBase, 4096, 0, memory, 0).entry;
  textures.OnUploaded(*t, memory); buffers.OnUploaded(*b, memory);
  CHECK_EQ(work.calls[ResourceWork::Hash], 2u);
  guest.bytes[kBase + 8] = 7;
  textures.InvalidateRange(kBase, 4096); buffers.InvalidateRange(kBase, 4096);
  CHECK(textures.Bind(fetch, memory, 1).action == BindAction::kReupload);
  CHECK(buffers.Bind(kBase, 4096, 0, memory, 1).action == BindAction::kReupload);
  CHECK_EQ(work.calls[ResourceWork::Hash], 4u);
  textures.OnUploaded(*t, memory); buffers.OnUploaded(*b, memory);
  CHECK_EQ(work.calls[ResourceWork::Hash], 4u);
  // Another write between a check and upload cannot reuse the earlier hash.
  guest.bytes[kBase + 9] = 3;
  textures.InvalidateRange(kBase, 4096); buffers.InvalidateRange(kBase, 4096);
  textures.Bind(fetch, memory, 2); buffers.Bind(kBase, 4096, 0, memory, 2);
  guest.bytes[kBase + 10] = 4;
  textures.InvalidateRange(kBase, 4096); buffers.InvalidateRange(kBase, 4096);
  textures.OnUploaded(*t, memory); buffers.OnUploaded(*b, memory);
  CHECK_EQ(work.calls[ResourceWork::Hash], 8u);
  CHECK(textures.Bind(fetch, memory, 3).action == BindAction::kUseExisting);
  CHECK(buffers.Bind(kBase, 4096, 0, memory, 3).action == BindAction::kUseExisting);
}

TEST(cache_watch_coalesces_only_a_contiguous_union) {
  FakeGuest guest;
  GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  auto* entry = cache.Bind(Fetch64(), memory, 0).entry;
  std::vector<std::pair<uint32_t, uint32_t>> calls;
  cache.watch_range = [&](uint32_t p, uint32_t n) { calls.emplace_back(p,n); };
  for (uint32_t second : {0x1800u, 0x2000u, 0x3000u}) {
    entry->ranges[0] = {0x1000, 0x1000}; entry->ranges[1] = {second, 0x1000};
    entry->watched = false; calls.clear();
    cache.EnsureWatched(*entry);
    CHECK_EQ(calls.size(), second <= 0x2000 ? 1u : 2u);
    CHECK_EQ(calls[0].first, 0x1000u);
    CHECK_EQ(calls[0].second, second <= 0x2000 ? second : 0x1000u);
    cache.OnUploaded(*entry, uint64_t(123));
    CHECK_EQ(calls.size(), second <= 0x2000 ? 1u : 2u);
  }
}

TEST(resident_snapshot_owns_bytes_preserves_word_padding_and_rejects_truncation) {
  std::vector<uint8_t> live(64, 0x39), storage;
  GuestMemory memory{live.data(), live.size(), 0x1000};
  const GuestRange ranges[2] = {{0x1000, 7}, {0x1020, 13}};
  ResidentSnapshot snapshot;
  CHECK(!snapshot.Capture(ranges, memory, storage, 4096));
  CHECK(snapshot.Capture(ranges, memory, storage, 8192));
  const auto hash = snapshot.Hash();
  live[7] = 0xAB;
  CHECK_EQ(snapshot.memory.At(0x1000, 8)[7], 0x39u);
  CHECK_EQ(snapshot.Hash(), hash);
  std::fill(live.begin(), live.end(), 0xFC);
  CHECK_EQ(snapshot.memory.At(0x1000, 8)[0], 0x39u);
  GuestMemory truncated{live.data(), 7, 0x1000};
  CHECK(!snapshot.Capture(ranges, truncated, storage, 8192));
}
