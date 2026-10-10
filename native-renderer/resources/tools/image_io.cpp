#include "image_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace kknr_tools {

using namespace kknr;

namespace {

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBE(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBE(png, uint32_t(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  PutBE(png, Crc32(png.data() + start, png.size() - start));
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

float Snorm(int32_t v, int32_t max) { return std::max(float(v) / float(max), -1.0f); }

// One texel of an uncompressed host format -> RGBA floats (absent channels 0, alpha 1).
void LoadTexel(HostFormat f, const uint8_t* p, float c[4]) {
  c[0] = c[1] = c[2] = 0.0f;
  c[3] = 1.0f;
  auto u16 = [&](int i) {
    uint16_t v;
    std::memcpy(&v, p + 2 * i, 2);
    return v;
  };
  auto u32 = [&](int i) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    return v;
  };
  auto f32 = [&](int i) {
    float v;
    std::memcpy(&v, p + 4 * i, 4);
    return v;
  };
  using H = HostFormat;
  switch (f) {
    case H::R8_UNORM: case H::RG8_UNORM: case H::RGBA8_UNORM: {
      const int n = f == H::R8_UNORM ? 1 : f == H::RG8_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = p[i] / 255.0f;
      break;
    }
    case H::R8_SNORM: case H::RG8_SNORM: case H::RGBA8_SNORM: {
      const int n = f == H::R8_SNORM ? 1 : f == H::RG8_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = Snorm(int8_t(p[i]), 127);
      break;
    }
    case H::R16_UNORM: case H::RG16_UNORM: case H::RGBA16_UNORM: {
      const int n = f == H::R16_UNORM ? 1 : f == H::RG16_UNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = u16(i) / 65535.0f;
      break;
    }
    case H::R16_SNORM: case H::RG16_SNORM: case H::RGBA16_SNORM: {
      const int n = f == H::R16_SNORM ? 1 : f == H::RG16_SNORM ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = Snorm(int16_t(u16(i)), 32767);
      break;
    }
    case H::R16_FLOAT: case H::RG16_FLOAT: case H::RGBA16_FLOAT: {
      const int n = f == H::R16_FLOAT ? 1 : f == H::RG16_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = HalfToFloat(u16(i));
      break;
    }
    case H::R32_FLOAT: case H::RG32_FLOAT: case H::RGBA32_FLOAT: {
      const int n = f == H::R32_FLOAT ? 1 : f == H::RG32_FLOAT ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = f32(i);
      break;
    }
    case H::R32_UINT: case H::RG32_UINT: case H::RGBA32_UINT: {
      const int n = f == H::R32_UINT ? 1 : f == H::RG32_UINT ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = float(std::min<uint32_t>(u32(i), 255)) / 255.0f;
      break;
    }
    case H::R32_SINT: case H::RG32_SINT: case H::RGBA32_SINT: {
      const int n = f == H::R32_SINT ? 1 : f == H::RG32_SINT ? 2 : 4;
      for (int i = 0; i < n; ++i) c[i] = float(std::clamp<int32_t>(int32_t(u32(i)), 0, 255)) / 255.0f;
      break;
    }
    case H::BGRA4_UNORM: {
      const uint16_t v = u16(0);
      c[2] = (v & 15) / 15.0f, c[1] = ((v >> 4) & 15) / 15.0f, c[0] = ((v >> 8) & 15) / 15.0f, c[3] = (v >> 12) / 15.0f;
      break;
    }
    case H::B5G6R5_UNORM: {
      const uint16_t v = u16(0);
      c[2] = (v & 31) / 31.0f, c[1] = ((v >> 5) & 63) / 63.0f, c[0] = (v >> 11) / 31.0f;
      break;
    }
    case H::B5G5R5A1_UNORM: {
      const uint16_t v = u16(0);
      c[2] = (v & 31) / 31.0f, c[1] = ((v >> 5) & 31) / 31.0f, c[0] = ((v >> 10) & 31) / 31.0f, c[3] = float(v >> 15);
      break;
    }
    case H::R10G10B10A2_UNORM: {
      const uint32_t v = u32(0);
      for (int i = 0; i < 3; ++i) c[i] = ((v >> (10 * i)) & 1023) / 1023.0f;
      c[3] = (v >> 30) / 3.0f;
      break;
    }
    default:
      break;
  }
}

}  // namespace

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  return h;
}

bool WritePng(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
  // Filter byte 0 per row, then zlib with stored (uncompressed) deflate blocks: big files, no dependency.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (width * 4 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + size_t(y) * width * 4, rgba.begin() + size_t(y + 1) * width * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size() || at == 0; at += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  PutBE(z, b << 16 | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBE(ihdr, width);
  PutBE(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  return WriteFile(path, png);
}

uint32_t DxgiFormat(HostFormat f) {
  using H = HostFormat;
  switch (f) {
    case H::R8_UNORM: return 61;
    case H::R8_SNORM: return 63;
    case H::RG8_UNORM: return 49;
    case H::RG8_SNORM: return 51;
    case H::R16_UNORM: return 56;
    case H::R16_SNORM: return 58;
    case H::R16_FLOAT: return 54;
    case H::BGRA4_UNORM: return 115;
    case H::B5G6R5_UNORM: return 85;
    case H::B5G5R5A1_UNORM: return 86;
    case H::RGBA8_UNORM: return 28;
    case H::RGBA8_SNORM: return 31;
    case H::R10G10B10A2_UNORM: return 24;
    case H::RG16_UNORM: return 35;
    case H::RG16_SNORM: return 37;
    case H::RG16_FLOAT: return 34;
    case H::R32_UINT: return 42;
    case H::R32_SINT: return 43;
    case H::R32_FLOAT: return 41;
    case H::RGBA16_FLOAT: return 10;
    case H::RGBA16_UNORM: return 11;
    case H::RGBA16_SNORM: return 13;
    case H::RG32_UINT: return 17;
    case H::RG32_SINT: return 18;
    case H::RG32_FLOAT: return 16;
    case H::RGBA32_UINT: return 3;
    case H::RGBA32_SINT: return 4;
    case H::RGBA32_FLOAT: return 2;
    case H::BC1_UNORM: return 71;
    case H::BC2_UNORM: return 74;
    case H::BC3_UNORM: return 77;
    case H::BC4_UNORM: return 80;
    case H::BC4_SNORM: return 81;
    case H::BC5_UNORM: return 83;
    case H::BC5_SNORM: return 84;
    default: return 0;
  }
}

bool WriteDds(const std::string& path, const HostTextureData& t) {
  const HostTexturePlan& p = t.plan;
  const HostFormatInfo& info = GetHostFormatInfo(p.format);
  std::vector<uint8_t> out;
  auto put = [&](uint32_t v) {
    for (int s = 0; s < 32; s += 8) out.push_back(uint8_t(v >> s));
  };
  const bool cube = p.dimension == Dimension::kCube, volume = p.dimension == Dimension::k3D;
  put(0x20534444);  // "DDS "
  put(124);
  put(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (volume ? 0x800000 : 0));
  put(p.height);
  put(p.width);
  put(0);
  put(volume ? p.depth : 0);
  put(p.levels);
  for (int i = 0; i < 11; ++i) put(0);
  put(32);
  put(0x4);         // DDPF_FOURCC
  put(0x30315844);  // "DX10"
  for (int i = 0; i < 5; ++i) put(0);
  put(0x1000 | 0x8 | 0x400000);
  put(cube ? 0x200 | 0xFC00 : (volume ? 0x200000 : 0));
  put(0);
  put(0);
  put(0);
  put(DxgiFormat(p.format));
  put(volume ? 4 : (p.dimension == Dimension::k1D ? 2 : 3));
  put(cube ? 0x4 : 0);
  put(cube ? 1 : p.layers);
  put(0);
  // Layer-major (each layer's mip chain); levels below min_level are zero.
  for (uint32_t layer = 0; layer < p.layers; ++layer)
    for (uint32_t level = 0; level < p.levels; ++level) {
      if (const HostSubresource* s = t.Find(level, layer)) {
        out.insert(out.end(), t.bytes.begin() + s->offset, t.bytes.begin() + s->offset + s->size);
        continue;
      }
      const uint32_t w = std::max(p.width >> level, 1u), h = std::max(p.height >> level, 1u);
      const uint32_t d = volume ? std::max(p.depth >> level, 1u) : 1;
      const size_t bytes = size_t((w + info.block_size - 1) / info.block_size) *
                           ((h + info.block_size - 1) / info.block_size) * info.bytes_per_block * d;
      out.insert(out.end(), bytes, 0);
    }
  return WriteFile(path, out);
}

std::vector<uint8_t> HostToRgba8(const HostTextureData& t, const HostSubresource& sub, uint32_t z) {
  const HostFormat f = t.plan.format;
  const HostFormatInfo& info = GetHostFormatInfo(f);
  const uint32_t w = sub.width, h = sub.height;
  std::vector<float> rgba(size_t(w) * h * 4, 0.0f);
  const uint8_t* slice = t.bytes.data() + sub.offset + size_t(z) * sub.depth_pitch;
  if (info.block_size == 4) {
    for (uint32_t by = 0; by < (h + 3) / 4; ++by)
      for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
        const uint8_t* b = slice + size_t(by) * sub.row_pitch + size_t(bx) * info.bytes_per_block;
        float texels[16][4] = {};
        uint8_t u8[64];
        int8_t s8[32];
        switch (f) {
          case HostFormat::BC1_UNORM: case HostFormat::BC2_UNORM: case HostFormat::BC3_UNORM:
            if (f == HostFormat::BC1_UNORM) DecodeBC1(b, u8);
            else if (f == HostFormat::BC2_UNORM) DecodeBC2(b, u8);
            else DecodeBC3(b, u8);
            for (int i = 0; i < 16; ++i)
              for (int k = 0; k < 4; ++k) texels[i][k] = u8[4 * i + k] / 255.0f;
            break;
          case HostFormat::BC4_UNORM:
            DecodeBC4(b, u8);
            for (int i = 0; i < 16; ++i) texels[i][0] = u8[i] / 255.0f, texels[i][3] = 1;
            break;
          case HostFormat::BC4_SNORM:
            DecodeBC4Signed(b, s8);
            for (int i = 0; i < 16; ++i) texels[i][0] = Snorm(s8[i], 127), texels[i][3] = 1;
            break;
          case HostFormat::BC5_UNORM:
            DecodeBC5(b, u8);
            for (int i = 0; i < 16; ++i) texels[i][0] = u8[2 * i] / 255.0f, texels[i][1] = u8[2 * i + 1] / 255.0f, texels[i][3] = 1;
            break;
          case HostFormat::BC5_SNORM:
            DecodeBC5Signed(b, s8);
            for (int i = 0; i < 16; ++i) texels[i][0] = Snorm(s8[2 * i], 127), texels[i][1] = Snorm(s8[2 * i + 1], 127), texels[i][3] = 1;
            break;
          default:
            break;
        }
        for (uint32_t ty = 0; ty < 4 && by * 4 + ty < h; ++ty)
          for (uint32_t tx = 0; tx < 4 && bx * 4 + tx < w; ++tx)
            std::memcpy(&rgba[(size_t(by * 4 + ty) * w + bx * 4 + tx) * 4], texels[ty * 4 + tx], 16);
      }
  } else {
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
        LoadTexel(f, slice + size_t(y) * sub.row_pitch + size_t(x) * info.bytes_per_block, &rgba[(size_t(y) * w + x) * 4]);
  }
  const bool is_signed = f == HostFormat::R8_SNORM || f == HostFormat::RG8_SNORM || f == HostFormat::RGBA8_SNORM ||
                         f == HostFormat::R16_SNORM || f == HostFormat::RG16_SNORM || f == HostFormat::RGBA16_SNORM ||
                         f == HostFormat::BC4_SNORM || f == HostFormat::BC5_SNORM;
  std::vector<uint8_t> out(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i)
    for (int k = 0; k < 4; ++k) {
      const uint8_t s = SwizzleComponent(t.plan.view_swizzle, k);
      float v = s < 4 ? rgba[i * 4 + s] : (s == 5 ? 1.0f : 0.0f);
      if (is_signed && s < 4) v = v * 0.5f + 0.5f;
      out[i * 4 + k] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }
  return out;
}

}  // namespace kknr_tools
