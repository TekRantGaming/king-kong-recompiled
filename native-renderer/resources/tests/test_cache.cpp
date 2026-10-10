// Upload policy: create on first bind, re-upload only when guest memory was written AND its content changed,
// sampler-only differences share one host texture, resolves make the host copy the truth.
#include <cstring>
#include <vector>

#include "kknr/texture_cache.h"
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
