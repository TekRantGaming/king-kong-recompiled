// Mips kept in the base's packed tail (TextureOptions::mips_from_base_tail): textures whose whole chain fits
// in one 32x32 tail (8x8 and 16x16 8:8:8:8, 16x16 DXT1 in the census) bound with mip address 0 and max level
// 3 or 4. Default: level 0 only (the SDK). With the option: every level from the base's tail.
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "kknr/guest_texture.h"
#include "kknr/texture_cache.h"
#include "kknr/texture_convert.h"
#include "kknr/tiling.h"
#include "test.h"

using namespace kknr;
using kknr_test::FakeGuest;
using kknr_test::Rng;

namespace {

constexpr uint32_t kBase = 0x00200000;

TextureFetchDesc TailDesc(TextureFormat format, uint32_t size, uint32_t max_level, bool tiled = true,
                          Dimension dim = Dimension::k2D) {
  TextureFetchDesc d;
  d.format = format;
  d.endian = GetFormatInfo(format).kind == FormatKind::kBlock4x4 ? Endian::k8in16 : Endian::k8in32;
  d.dimension = dim;
  d.tiled = tiled;
  d.packed_mips = true;
  d.width = d.height = size;
  d.base_address = kBase;
  d.mip_address = 0;
  d.max_level = max_level;
  return d;
}

TextureOptions WithTail() {
  TextureOptions o;
  o.mips_from_base_tail = true;
  return o;
}

std::string Name(const TextureFetchDesc& d) {
  return std::string(GetFormatInfo(d.format).name) + " " + std::to_string(d.width) + (d.tiled ? " tiled" : " linear") +
         " " + DimensionName(d.dimension);
}

}  // namespace

TEST(tail_mips_default_is_level_0_only) {
  // The census shapes, as the SDK reads them.
  for (auto [format, size, max] : {std::tuple{TextureFormat::k_8_8_8_8, 8u, 3u},
                                    std::tuple{TextureFormat::k_8_8_8_8, 16u, 4u},
                                    std::tuple{TextureFormat::k_DXT1, 16u, 4u}}) {
    const TextureFetch f = MakeTextureFetch(TailDesc(format, size, max));
    CHECK(HasMipsInBaseTail(f));
    const TextureLevels l = GetTextureLevels(f);
    CHECK_EQ(l.max_level, 0u);
    CHECK(!l.mips_in_base_tail);
    HostTexturePlan p;
    CHECK(PlanHostTexture(f, p));
    CHECK_EQ(p.levels, 1u);
    const TextureLevels t = GetTextureLevels(f, WithTail());
    CHECK_EQ(t.max_level, max);
    CHECK(t.mips_in_base_tail);
    CHECK_EQ(t.mip_address, 0u);
    CHECK(PlanHostTexture(f, p, nullptr, WithTail()));
    CHECK_EQ(p.levels, max + 1);
  }
  // Not such a texture: a 64x64 (tail starts at level 2), a texture with a mip address, no packed mips.
  TextureFetchDesc d = TailDesc(TextureFormat::k_8_8_8_8, 64, 6);
  CHECK(!HasMipsInBaseTail(MakeTextureFetch(d)));
  CHECK_EQ(GetTextureLevels(MakeTextureFetch(d), WithTail()).max_level, 0u);
  d = TailDesc(TextureFormat::k_8_8_8_8, 16, 4);
  d.packed_mips = false;
  CHECK(!HasMipsInBaseTail(MakeTextureFetch(d)));
  d = TailDesc(TextureFormat::k_8_8_8_8, 16, 4);
  d.mip_address = 0x00300000;
  CHECK(!HasMipsInBaseTail(MakeTextureFetch(d)));
  CHECK(!GetTextureLevels(MakeTextureFetch(d), WithTail()).mips_in_base_tail);
}

TEST(tail_mips_positions) {
  // 16x16 one texel per block: level 0 sits at x = 16 of the tail, 8x8 at x = 8, 4x4 at x = 4, then 2x2 at
  // y = 8 and 1x1 at y = 4 (the SDK's tail picture), all in the base's storage.
  const GuestLayout l = ComputeGuestLayout(MakeTextureFetch(TailDesc(TextureFormat::k_8_8_8_8, 16, 4)), WithTail());
  CHECK(l.mips_in_base_tail);
  CHECK_EQ(l.packed_level, 0u);
  const uint32_t expect[5][2] = {{16, 0}, {8, 0}, {4, 0}, {0, 8}, {0, 4}};
  for (uint32_t level = 0; level <= 4; ++level) {
    LevelSource s;
    CHECK(GetLevelSource(l, level, 0, s));
    CHECK(s.storage == &l.base);
    CHECK(!s.from_mips);
    CHECK_EQ(s.x_blocks, expect[level][0]);
    CHECK_EQ(s.y_blocks, expect[level][1]);
  }
  CHECK_EQ(l.mips_extent_bytes, 0u);
  // Without the option the base's extent covers level 0 only; with it, the whole chain.
  const GuestLayout plain = ComputeGuestLayout(MakeTextureFetch(TailDesc(TextureFormat::k_8_8_8_8, 16, 4)));
  CHECK(l.base_extent_bytes >= plain.base_extent_bytes);
  const TextureRanges r = GetTextureRanges(MakeTextureFetch(TailDesc(TextureFormat::k_8_8_8_8, 16, 4)), WithTail());
  CHECK_EQ(r.base_bytes, l.base_extent_bytes);
  CHECK_EQ(r.mip_bytes, 0u);
}

TEST(tail_mips_same_bytes_as_a_mip_tail_at_the_base_address) {
  // The hardware question: with mip address 0, where are levels 1+? Reading them from the base's tail is
  // the same as pointing the mip address at the base: the mips' tail is laid out like level 0's (packed level
  // 0), so every level lands on the same bytes. Checked block for block for the census shapes, tiled and
  // linear, 2D and cube.
  int checked = 0;
  for (TextureFormat format : {TextureFormat::k_8_8_8_8, TextureFormat::k_DXT1, TextureFormat::k_8,
                               TextureFormat::k_DXT4_5}) {
    for (uint32_t size : {4u, 8u, 16u}) {
      for (bool tiled : {true, false}) {
        for (Dimension dim : {Dimension::k2D, Dimension::kCube}) {
          uint32_t max = 0;
          for (uint32_t s = size; s > 1; s >>= 1) ++max;
          TextureFetchDesc a = TailDesc(format, size, max, tiled, dim);
          TextureFetchDesc b = a;
          b.mip_address = kBase;
          const GuestLayout la = ComputeGuestLayout(MakeTextureFetch(a), WithTail());
          const GuestLayout lb = ComputeGuestLayout(MakeTextureFetch(b));
          bool same = la.mips_in_base_tail && la.max_level == lb.max_level;
          for (uint32_t level = 1; same && level <= la.max_level; ++level)
            for (uint32_t layer = 0; same && layer < la.layers; ++layer) {
              LevelSource sa, sb;
              same &= GetLevelSource(la, level, layer, sa) && GetLevelSource(lb, level, layer, sb);
              for (uint32_t y = 0; same && y < sa.height_blocks; ++y)
                for (uint32_t x = 0; same && x < sa.width_blocks; ++x)
                  same &= sa.layer_offset_bytes +
                              BlockOffset(la, *sa.storage, x + sa.x_blocks, y + sa.y_blocks, sa.z) ==
                          sb.layer_offset_bytes +
                              BlockOffset(lb, *sb.storage, x + sb.x_blocks, y + sb.y_blocks, sb.z);
            }
          if (!same) kknr_test::Fail(__FILE__, __LINE__, "levels differ from a mip tail at the base: " + Name(a));
          ++checked;
        }
      }
    }
  }
  CHECK_EQ(checked, 48);
}

TEST(tail_mips_round_trip_and_convert) {
  // Random blocks for every level, laid out with the option, read back, no two levels sharing bytes, and
  // ConvertTexture returning each level's blocks (kCopy formats).
  for (auto [format, size, max, dim] :
       {std::tuple{TextureFormat::k_8_8_8_8, 8u, 3u, Dimension::k2D}, std::tuple{TextureFormat::k_8_8_8_8, 16u, 4u, Dimension::k2D},
        std::tuple{TextureFormat::k_DXT1, 16u, 4u, Dimension::k2D}, std::tuple{TextureFormat::k_8_8_8_8, 16u, 4u, Dimension::kCube},
        std::tuple{TextureFormat::k_DXN, 16u, 4u, Dimension::k2D}}) {
    const TextureFetchDesc d = TailDesc(format, size, max, true, dim);
    const TextureFetch f = MakeTextureFetch(d);
    const GuestLayout layout = ComputeGuestLayout(f, WithTail());
    const uint32_t bpb = GetFormatInfo(format).BytesPerBlock();
    Rng rng(size * 31 + uint32_t(format));
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> data;
    for (uint32_t level = 0; level <= max; ++level)
      for (uint32_t layer = 0; layer < layout.layers; ++layer) {
        std::vector<uint8_t>& v = data[{level, layer}];
        v.resize(GetLevelBlockExtent(layout, level).Count() * bpb);
        rng.Fill(v.data(), v.size());
      }
    GuestTextureImage image;
    CHECK(EncodeGuestTexture(
        f, [&](uint32_t level, uint32_t layer) { return data[{level, layer}].data(); }, image, WithTail()));
    CHECK(image.mips.empty());
    // Ownership of every byte of the base region.
    std::vector<int> owner(image.base.size(), -1);
    bool overlap = false;
    int id = 0;
    for (const auto& [key, blocks] : data) {
      LevelSource s;
      GetLevelSource(layout, key.first, key.second, s);
      for (uint32_t y = 0; y < s.height_blocks; ++y)
        for (uint32_t x = 0; x < s.width_blocks; ++x) {
          const size_t o = s.layer_offset_bytes + BlockOffset(layout, *s.storage, x + s.x_blocks, y + s.y_blocks, 0);
          for (uint32_t b = 0; b < bpb; ++b) {
            overlap |= owner[o + b] >= 0;
            owner[o + b] = id;
          }
        }
      ++id;
    }
    if (overlap) kknr_test::Fail(__FILE__, __LINE__, "levels share bytes: " + Name(d));
    static FakeGuest guest;
    std::fill_n(guest.bytes.begin() + kBase, 1u << 20, 0);
    guest.Put(kBase, image.base);
    const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
    HostTextureData t;
    std::string why;
    CHECK(ConvertTexture(f, memory, t, &why, WithTail()));
    CHECK_EQ(t.plan.levels, max + 1);
    for (const auto& [key, blocks] : data) {
      const HostSubresource* s = t.Find(key.first, key.second);
      if (!s || s->size != blocks.size() || std::memcmp(t.bytes.data() + s->offset, blocks.data(), s->size) != 0)
        kknr_test::Fail(__FILE__, __LINE__, "level " + std::to_string(key.first) + " differs: " + Name(d));
    }
    // Without the option the same memory gives level 0 only, with the same level 0.
    HostTextureData t0;
    CHECK(ConvertTexture(f, memory, t0));
    CHECK_EQ(t0.subresources.size(), size_t(layout.layers));
    const HostSubresource* a = t0.Find(0, 0);
    const HostSubresource* b = t.Find(0, 0);
    CHECK(a && b && a->size == b->size &&
          std::memcmp(t0.bytes.data() + a->offset, t.bytes.data() + b->offset, a->size) == 0);
  }
}

TEST(tail_mips_cache_watches_the_whole_tail) {
  // With the option, a write to a small level inside the base's tail but past level 0's own extent must
  // still invalidate the texture.
  const TextureFetch f = MakeTextureFetch(TailDesc(TextureFormat::k_8_8_8_8, 16, 4));
  const GuestLayout layout = ComputeGuestLayout(f, WithTail());
  LevelSource s;
  GetLevelSource(layout, 3, 0, s);
  const uint32_t level3 = kBase + s.layer_offset_bytes + BlockOffset(layout, *s.storage, s.x_blocks, s.y_blocks, 0);
  FakeGuest guest;
  const GuestMemory memory{guest.bytes.data(), guest.bytes.size()};
  TextureCache cache;
  cache.options = WithTail();
  TextureCache::BindResult r = cache.Bind(f, memory, 1);
  cache.OnUploaded(*r.entry, memory);
  guest.bytes[level3] = 0x77;
  cache.InvalidateRange(level3, 1);
  CHECK(cache.Bind(f, memory, 2).action == BindAction::kReupload);
}
