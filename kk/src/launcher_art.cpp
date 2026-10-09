#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // first: its LoadImage macro renames art::LoadImage, as in the rest of the port
#endif

#include "launcher_art.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")
#endif

#include <rex/logging.h>

namespace kk::art {
namespace {

// Bumped when what's extracted changes, so it's extracted again.
constexpr const char* kArtVersion = "2";

// ------------------------------------------------------------------- LZO ---
// LZO1X decompression with bounds checks (as lzo1x_decompress_safe). Returns
// false if the data doesn't decompress to exactly out_len bytes.
bool Lzo1xDecompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
  size_t ip = 0, op = 0;
  auto have = [&](size_t n) { return in_len - ip >= n; };
  auto literals = [&](size_t n) {
    if (!have(n) || out_len - op < n) return false;
    std::memcpy(out + op, in + ip, n);
    ip += n;
    op += n;
    return true;
  };
  auto match = [&](size_t dist, size_t n) {
    if (dist == 0 || dist > op || out_len - op < n) return false;
    for (size_t i = 0; i < n; ++i, ++op) out[op] = out[op - dist];  // may overlap
    return true;
  };
  auto extend = [&](size_t& n, size_t base) {  // run length past a zero count
    while (have(1) && in[ip] == 0) {
      n += 255;
      ++ip;
    }
    if (!have(1)) return false;
    n += base + in[ip++];
    return true;
  };
  // After a match, 0-3 literals follow (state 0-3); after a literal run, 4.
  int state = 0;
  if (!have(1)) return false;
  if (in[0] > 17) {
    const size_t n = size_t(in[ip++]) - 17;
    if (!literals(n)) return false;
    state = n < 4 ? int(n) : 4;
  }
  for (;;) {
    if (!have(1)) return false;
    size_t t = in[ip++];
    if (t < 16) {
      if (state == 0) {
        if (t == 0 && !extend(t, 15)) return false;
        if (!literals(t + 3)) return false;
        state = 4;
        continue;
      }
      if (!have(1)) return false;
      const size_t b = in[ip++];
      const bool after_run = state == 4;
      if (!match((after_run ? 1 + 0x0800 : 1) + (t >> 2) + (b << 2), after_run ? 3 : 2)) return false;
    } else if (t >= 64) {
      if (!have(1)) return false;
      const size_t b = in[ip++];
      if (!match(1 + ((t >> 2) & 7) + (b << 3), (t >> 5) + 1)) return false;
    } else if (t >= 32) {
      size_t n = t & 31;
      if (n == 0 && !extend(n, 31)) return false;
      if (!have(2)) return false;
      const size_t d = (size_t(in[ip]) | (size_t(in[ip + 1]) << 8)) >> 2;
      ip += 2;
      if (!match(1 + d, n + 2)) return false;
    } else {
      size_t n = t & 7;
      if (n == 0 && !extend(n, 7)) return false;
      if (!have(2)) return false;
      const size_t d = ((t & 8) << 11) + ((size_t(in[ip]) | (size_t(in[ip + 1]) << 8)) >> 2);
      ip += 2;
      if (d == 0) return op == out_len;  // end of the stream
      if (!match(d + 0x4000, n + 2)) return false;
    }
    state = in[ip - 2] & 3;
    if (state && !literals(size_t(state))) return false;
  }
}

// -------------------------------------------------------------- bigfile ---
// KKTextures.bf is a Jade engine "BIG" file: a table of (offset, key) entries
// at 0x44, one per pack of textures. A pack is a run of chunks, each a 12-byte
// header (u32, decompressed size, stored size) and LZO1X data (or raw when the
// two sizes match); the first header's first u32 is the pack's size.
uint32_t LE32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t BE32(const uint8_t* p) { return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24; }

bool ReadPack(const std::filesystem::path& bigfile, uint32_t key, std::vector<uint8_t>& out, std::string& why) {
  std::ifstream f(bigfile, std::ios::binary);
  if (!f) return why = "can't open " + bigfile.filename().string(), false;
  f.seekg(0, std::ios::end);
  const uint64_t file_size = uint64_t(f.tellg());
  uint8_t header[0x44];
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "BIG", 4) != 0)
    return why = "not a Jade BIG file", false;
  const uint32_t count = LE32(header + 8);
  if (count == 0 || count > 65536) return why = "unexpected BIG file table", false;
  std::vector<uint8_t> table(size_t(count) * 8);
  if (!f.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) return why = "short BIG table", false;
  uint64_t offset = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (LE32(&table[i * 8 + 4]) == key) offset = LE32(&table[i * 8]);
  if (!offset || offset + 12 > file_size) return why = "front-end textures not found", false;
  uint8_t h[12];
  f.seekg(std::streamoff(offset));
  if (!f.read(reinterpret_cast<char*>(h), 4)) return why = "short pack", false;
  const uint64_t end = std::min<uint64_t>(offset + 4 + LE32(h), file_size);
  std::vector<uint8_t> stored;
  for (uint64_t pos = offset; pos + 12 <= end;) {
    f.seekg(std::streamoff(pos));
    if (!f.read(reinterpret_cast<char*>(h), 12)) break;
    const uint32_t size = LE32(h + 4), stored_size = LE32(h + 8);
    if (size == 0 || size > (1u << 24) || stored_size > size || pos + 12 + stored_size > end) break;
    stored.resize(stored_size);
    if (!f.read(reinterpret_cast<char*>(stored.data()), stored_size)) break;
    const size_t at = out.size();
    out.resize(at + size);
    if (stored_size == size) std::memcpy(&out[at], stored.data(), size);
    else if (!Lzo1xDecompress(stored.data(), stored_size, &out[at], size)) return why = "can't unpack textures", false;
    pos += 12 + stored_size;
  }
  return !out.empty() || (why = "empty pack", false);
}

// Xenos tiled layout (as Xenia's texture_util::GetTiledOffset2D).
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// The front end's logo: the pack's 512 x 256 8:8:8:8 texture (stored as the
// GPU reads it: tiled, big-endian ARGB words, upside down). A texture's record
// has the magic 0xCAD01234 and, 12 bytes on, "D2KK" with its size and format;
// its pixels start 32 bytes after "D2KK".
constexpr uint32_t kFrontEndPack = 0xFF80C45A;

Image FindLogo(const std::vector<uint8_t>& pack) {
  static const uint8_t kMagic[4] = {0x34, 0x12, 0xD0, 0xCA};
  for (size_t i = 12; i + 0x20 <= pack.size(); ++i) {
    if (std::memcmp(&pack[i], "D2KK", 4) != 0 || std::memcmp(&pack[i - 12], kMagic, 4) != 0) continue;
    const uint32_t w = BE32(&pack[i + 4]), h = BE32(&pack[i + 8]), fetch = BE32(&pack[i + 12]);
    if (w != 512 || h != 256 || (fetch & 0x3F) != 6 || ((fetch >> 6) & 3) != 2) continue;
    const size_t data = i + 0x20;
    if (data + size_t(w) * h * 4 > pack.size()) break;
    Image img;
    img.width = int(w);
    img.height = int(h);
    img.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      uint8_t* dst = &img.rgba[size_t(h - 1 - y) * w * 4];
      for (uint32_t x = 0; x < w; ++x, dst += 4) {
        const uint8_t* p = &pack[data + TiledOffset(x, y, w, 2)];
        dst[0] = p[1];
        dst[1] = p[2];
        dst[2] = p[3];
        dst[3] = p[0];
      }
    }
    return img;
  }
  return {};
}

// The front end's Xbox 360 button pictures: a 256 x 128 DXT5 sheet (tiled,
// 16-bit big-endian words, upside down), found by its checksum.
constexpr uint32_t kButtonSheetHash = 0x28E39DAD;  // FNV-1a of its 32 KB

uint32_t Fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
  return h;
}

Image FindButtonSheet(const std::vector<uint8_t>& pack) {
  static const uint8_t kMagic[4] = {0x34, 0x12, 0xD0, 0xCA};
  constexpr uint32_t kW = 256, kH = 128, kBlocksX = kW / 4, kBlocksY = kH / 4, kSize = kBlocksX * kBlocksY * 16;
  for (size_t i = 12; i + 0x20 + kSize <= pack.size(); ++i) {
    if (std::memcmp(&pack[i], "D2KK", 4) != 0 || std::memcmp(&pack[i - 12], kMagic, 4) != 0) continue;
    if (BE32(&pack[i + 4]) != kW || BE32(&pack[i + 8]) != kH || (BE32(&pack[i + 12]) & 0x3F) != 20) continue;
    const uint8_t* data = &pack[i + 0x20];
    if (Fnv1a(data, kSize) != kButtonSheetHash) continue;
    Image img;
    img.width = int(kW);
    img.height = int(kH);
    img.rgba.resize(size_t(kW) * kH * 4);
    for (uint32_t by = 0; by < kBlocksY; ++by)
      for (uint32_t bx = 0; bx < kBlocksX; ++bx) {
        uint8_t b[16];
        const uint8_t* src = data + TiledOffset(bx, by, kBlocksX, 4);
        for (int k = 0; k < 16; k += 2) {  // 16-bit big-endian words
          b[k] = src[k + 1];
          b[k + 1] = src[k];
        }
        // Alpha: two ends and 3-bit indices.
        uint8_t alpha[8] = {b[0], b[1]};
        if (b[0] > b[1])
          for (int k = 1; k < 7; ++k) alpha[k + 1] = uint8_t(((7 - k) * b[0] + k * b[1]) / 7);
        else {
          for (int k = 1; k < 5; ++k) alpha[k + 1] = uint8_t(((5 - k) * b[0] + k * b[1]) / 5);
          alpha[6] = 0;
          alpha[7] = 255;
        }
        uint64_t abits = 0;
        for (int k = 0; k < 6; ++k) abits |= uint64_t(b[2 + k]) << (8 * k);
        // Colour: two 5:6:5 ends and 2-bit indices (always four colours in DXT5).
        const uint16_t c0 = uint16_t(b[8] | b[9] << 8), c1 = uint16_t(b[10] | b[11] << 8);
        const uint32_t cbits = uint32_t(b[12]) | uint32_t(b[13]) << 8 | uint32_t(b[14]) << 16 | uint32_t(b[15]) << 24;
        int pal[4][3];
        for (int e = 0; e < 2; ++e) {
          const uint16_t c = e ? c1 : c0;
          pal[e][0] = ((c >> 11) & 31) * 255 / 31;
          pal[e][1] = ((c >> 5) & 63) * 255 / 63;
          pal[e][2] = (c & 31) * 255 / 31;
        }
        for (int ch = 0; ch < 3; ++ch) {
          pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
          pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
        }
        for (int py = 0; py < 4; ++py)
          for (int px = 0; px < 4; ++px) {
            const int k = py * 4 + px;
            const int* c = pal[(cbits >> (2 * k)) & 3];
            const uint32_t y = by * 4 + py, x = bx * 4 + px;
            uint8_t* d = &img.rgba[(size_t(kH - 1 - y) * kW + x) * 4];  // the right way up
            d[0] = uint8_t(c[0]);
            d[1] = uint8_t(c[1]);
            d[2] = uint8_t(c[2]);
            d[3] = alpha[(abits >> (3 * k)) & 7];
          }
      }
    return img;
  }
  return {};
}

// Where each button is on the sheet (x0, y0, x1, y1).
struct ButtonCut {
  const char* name;
  int x0, y0, x1, y1;
};
constexpr ButtonCut kButtonCuts[] = {
    {"b", 2, 2, 34, 34},       {"a", 36, 2, 68, 34},           {"x", 70, 2, 102, 34},
    {"y", 104, 2, 136, 34},    {"back", 140, 2, 176, 35},      {"start", 177, 3, 210, 36},
    {"dpad", 3, 38, 39, 74},   {"dpad_up", 3, 38, 39, 74},     {"stick", 82, 38, 118, 74},
    {"stick_click", 82, 38, 118, 74}, {"stick_move", 82, 38, 118, 74},
    {"lt", 70, 81, 96, 124},   {"rt", 102, 81, 128, 124},      {"lb", 134, 97, 193, 122},
    {"rb", 197, 97, 256, 122}};

Image Cut(const Image& src, int x0, int y0, int x1, int y1) {
  Image out;
  out.width = x1 - x0;
  out.height = y1 - y0;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y)
    std::memcpy(&out.rgba[size_t(y) * out.width * 4], &src.rgba[(size_t(y0 + y) * src.width + x0) * 4],
                size_t(out.width) * 4);
  return out;
}

// The part of an image that isn't transparent, with a small margin.
Image CropToContent(const Image& src, int margin) {
  int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x)
      if (src.rgba[(size_t(y) * src.width + x) * 4 + 3] > 8) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
  if (x1 < 0) return {};
  x0 = std::max(0, x0 - margin);
  y0 = std::max(0, y0 - margin);
  x1 = std::min(src.width - 1, x1 + margin);
  y1 = std::min(src.height - 1, y1 + margin);
  Image out;
  out.width = x1 - x0 + 1;
  out.height = y1 - y0 + 1;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y)
    std::memcpy(&out.rgba[size_t(y) * out.width * 4], &src.rgba[(size_t(y0 + y) * src.width + x0) * 4],
                size_t(out.width) * 4);
  return out;
}

// Scales up by a whole factor with a Catmull-Rom filter (on premultiplied
// colour, so the transparent edges don't darken).
Image Upscale(const Image& src, int factor) {
  const int w = src.width, h = src.height, W = w * factor, H = h * factor;
  std::vector<float> pre(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    const float a = src.rgba[i * 4 + 3] / 255.0f;
    for (int c = 0; c < 3; ++c) pre[i * 4 + c] = src.rgba[i * 4 + c] / 255.0f * a;
    pre[i * 4 + 3] = a;
  }
  auto weights = [](float t, float* wt) {
    const float t2 = t * t, t3 = t2 * t;
    wt[0] = (-t3 + 2 * t2 - t) * 0.5f;
    wt[1] = (3 * t3 - 5 * t2 + 2) * 0.5f;
    wt[2] = (-3 * t3 + 4 * t2 + t) * 0.5f;
    wt[3] = (t3 - t2) * 0.5f;
  };
  std::vector<float> mid(size_t(W) * h * 4);
  for (int x = 0; x < W; ++x) {
    const float sx = (x + 0.5f) / factor - 0.5f;
    const int ix = int(std::floor(sx));
    float wt[4];
    weights(sx - ix, wt);
    for (int y = 0; y < h; ++y)
      for (int c = 0; c < 4; ++c) {
        float v = 0;
        for (int k = 0; k < 4; ++k) v += wt[k] * pre[(size_t(y) * w + std::clamp(ix - 1 + k, 0, w - 1)) * 4 + c];
        mid[(size_t(y) * W + x) * 4 + c] = v;
      }
  }
  Image out;
  out.width = W;
  out.height = H;
  out.rgba.resize(size_t(W) * H * 4);
  for (int y = 0; y < H; ++y) {
    const float sy = (y + 0.5f) / factor - 0.5f;
    const int iy = int(std::floor(sy));
    float wt[4];
    weights(sy - iy, wt);
    for (int x = 0; x < W; ++x) {
      float v[4] = {};
      for (int k = 0; k < 4; ++k) {
        const float* p = &mid[(size_t(std::clamp(iy - 1 + k, 0, h - 1)) * W + x) * 4];
        for (int c = 0; c < 4; ++c) v[c] += wt[k] * p[c];
      }
      const float a = std::clamp(v[3], 0.0f, 1.0f);
      uint8_t* d = &out.rgba[(size_t(y) * W + x) * 4];
      for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(a > 0 ? v[c] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
      d[3] = uint8_t(a * 255.0f + 0.5f);
    }
  }
  return out;
}

// ------------------------------------------------------------ the video ---
#if defined(_WIN32)
template <typename T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// Frames of a video at the given times, through Media Foundation.
std::vector<Image> GrabStills(const std::filesystem::path& video, const std::vector<double>& seconds) {
  std::vector<Image> stills;
  IMFAttributes* attrs = nullptr;
  IMFSourceReader* reader = nullptr;
  IMFMediaType* type = nullptr;
  IMFMediaType* current = nullptr;
  MFCreateAttributes(&attrs, 1);
  if (attrs) attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  constexpr DWORD kVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  UINT32 w = 0, h = 0;
  if (SUCCEEDED(MFCreateSourceReaderFromURL(video.c_str(), attrs, &reader)) &&
      SUCCEEDED(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE)) &&
      SUCCEEDED(reader->SetStreamSelection(kVideo, TRUE)) && SUCCEEDED(MFCreateMediaType(&type)) &&
      SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) &&
      SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
      SUCCEEDED(reader->SetCurrentMediaType(kVideo, nullptr, type)) &&
      SUCCEEDED(reader->GetCurrentMediaType(kVideo, &current)) &&
      SUCCEEDED(MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h)) && w && h) {
    for (double t : seconds) {
      PROPVARIANT pos;
      InitPropVariantFromInt64(LONGLONG(t * 1e7), &pos);
      reader->SetCurrentPosition(GUID_NULL, pos);
      PropVariantClear(&pos);
      // Seeking lands on the key frame before; read on to the wanted one.
      IMFSample* sample = nullptr;
      for (int i = 0; i < 600; ++i) {
        Release(sample);
        DWORD flags = 0;
        LONGLONG ts = 0;
        if (FAILED(reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
          break;
        if (sample && ts >= LONGLONG(t * 1e7) - 100000) break;  // the frame at t (within 10 ms)
      }
      IMFMediaBuffer* buffer = nullptr;
      BYTE* data = nullptr;
      DWORD length = 0;
      if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) &&
          SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
        if (length >= w * h * 4) {
          Image img;
          img.width = int(w);
          img.height = int(h);
          img.rgba.resize(size_t(w) * h * 4);
          for (size_t i = 0; i < size_t(w) * h; ++i) {  // BGRX -> RGBA
            img.rgba[i * 4 + 0] = data[i * 4 + 2];
            img.rgba[i * 4 + 1] = data[i * 4 + 1];
            img.rgba[i * 4 + 2] = data[i * 4 + 0];
            img.rgba[i * 4 + 3] = 255;
          }
          stills.push_back(std::move(img));
        }
        buffer->Unlock();
      }
      Release(buffer);
      Release(sample);
    }
  }
  Release(current);
  Release(type);
  Release(reader);
  Release(attrs);
  return stills;
}
#endif

// The backdrops: Kong roaring at a V. rex, a valley on Skull Island, the
// mossy bridge in the jungle, 1930s New York from above and at street level,
// and a V. rex. The video is about 4 Mbit/s, so these are key frames (stored
// whole, and far sharper than the frames between them) of shots that hold up.
struct Still {
  const char* video;
  double seconds;
};
constexpr Still kStills[] = {{"Trailer.wmv", 135.218}, {"Trailer.wmv", 130.338}, {"Trailer.wmv", 108.900},
                             {"Trailer.wmv", 3.211},   {"Trailer.wmv", 12.303},  {"Trailer.wmv", 129.337}};

std::filesystem::path BackdropPath(const std::filesystem::path& dir, size_t i) {
  return dir / ("backdrop" + std::to_string(i + 1) + ".bmp");
}

}  // namespace

std::filesystem::path LauncherArtDir(const std::filesystem::path& user_data_root) {
  return CacheDir(user_data_root) / "art";
}

bool HasLauncherArt(const std::filesystem::path& dir) {
  std::ifstream f(dir / "art.txt");
  std::string line;
  return std::getline(f, line) && line == std::string("version=") + kArtVersion;
}

bool SaveImage(const Image& image, const std::filesystem::path& path) {
  if (!image) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const uint32_t pixels = uint32_t(image.width) * image.height * 4;
  std::string out(54, '\0');
  auto put = [&](size_t at, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) out[at + i] = char((v >> (8 * i)) & 0xFF);
  };
  out[0] = 'B';
  out[1] = 'M';
  put(2, 54 + pixels, 4);
  put(10, 54, 4);
  put(14, 40, 4);
  put(18, uint32_t(image.width), 4);
  put(22, uint32_t(-image.height), 4);  // top-down
  put(26, 1, 2);
  put(28, 32, 2);
  put(34, pixels, 4);
  out.resize(54 + pixels);
  for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {  // RGBA -> BGRA
    out[54 + i * 4 + 0] = char(image.rgba[i * 4 + 2]);
    out[54 + i * 4 + 1] = char(image.rgba[i * 4 + 1]);
    out[54 + i * 4 + 2] = char(image.rgba[i * 4 + 0]);
    out[54 + i * 4 + 3] = char(image.rgba[i * 4 + 3]);
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(out.data(), std::streamsize(out.size()));
  return bool(f);
}

bool ExtractLauncherArt(const std::filesystem::path& game_dir, const std::filesystem::path& dir, ArtProgress& progress) {
  progress.done = progress.failed = false;
  progress.fraction = 0;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  int saved = 0;
  std::string why;

  // The logo.
  {
    std::vector<uint8_t> pack;
    if (ReadPack(game_dir / "KKTextures.bf", kFrontEndPack, pack, why)) {
      progress.fraction = 0.25f;
      if (Image logo = FindLogo(pack)) {
        logo = Upscale(CropToContent(logo, 4), 3);
        if (SaveImage(logo, dir / "logo.bmp")) ++saved;
      } else {
        why = "the logo wasn't in the front end's textures";
      }
      // The Xbox 360 buttons, for the launcher's prompts with that setting.
      if (Image sheet = FindButtonSheet(pack)) {
        for (const ButtonCut& c : kButtonCuts)
          SaveImage(Upscale(Cut(sheet, c.x0, c.y0, c.x1, c.y1), 2), dir / "glyphs" / "xbox360" / (std::string(c.name) + ".bmp"));
      } else {
        REXLOG_WARN("KK: launcher art: the Xbox 360 button pictures weren't in the front end's textures");
      }
    }
    if (!why.empty()) REXLOG_WARN("KK: launcher art: logo: {}", why);
  }
  progress.fraction = 0.4f;

  // The backdrops.
#if defined(_WIN32)
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    size_t index = 0;
    for (size_t i = 0; i < std::size(kStills);) {
      // The stills from one video together.
      std::vector<double> times;
      const char* video = kStills[i].video;
      size_t j = i;
      for (; j < std::size(kStills) && std::strcmp(kStills[j].video, video) == 0; ++j) times.push_back(kStills[j].seconds);
      for (const Image& still : GrabStills(game_dir / "Video" / video, times))
        if (SaveImage(still, BackdropPath(dir, index++))) ++saved;
      i = j;
      progress.fraction = 0.4f + 0.6f * float(i) / float(std::size(kStills));
    }
    MFShutdown();
    if (index == 0) REXLOG_WARN("KK: launcher art: no stills from the videos (Media Foundation's WMV decoder missing?)");
  }
  if (SUCCEEDED(co)) CoUninitialize();
#endif

  progress.fraction = 1;
  if (saved == 0) {
    progress.message = why.empty() ? "No artwork could be read from the game files." : why;
    progress.failed = true;
    progress.busy = false;
    return false;
  }
  std::ofstream(dir / "art.txt", std::ios::trunc) << "version=" << kArtVersion << "\n";
  REXLOG_INFO("KK: launcher art: {} images from the game files", saved);
  progress.done = true;
  progress.busy = false;
  return true;
}

LauncherArt LoadLauncherArt(const std::filesystem::path& dir) {
  LauncherArt art;
  art.logo = LoadImage(dir / "logo.bmp");
  for (size_t i = 0; i < 16; ++i) {
    Image img = LoadImage(BackdropPath(dir, i));
    if (!img) break;
    art.backdrops.push_back(std::move(img));
  }
  return art;
}

}  // namespace kk::art
