// Button prompts in the style of other controllers (kk_button_prompts).
//
// The game draws every button prompt ("\p16\a" in its text is the A button)
// from one 256x128 picture sheet: DXT5, Xbox 360 tiled, 16-bit byte-swapped and
// stored upside down. This finds that sheet in the console's memory, redraws
// each button on it with an image from <exe folder>/glyphs/<style>/, and writes
// it back; the GPU picks the change up like any texture the game updates. The
// game can load the sheet again (or elsewhere), so it is checked every second.
//
// Keyboard prompts show the first key bound to each button (keybind_* cvars),
// from glyphs/keyboard/<key>.png.

#include "glyphs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/image_decode.h>

#include "settings.h"

namespace kk {
namespace {

constexpr uint32_t kWidth = 256, kHeight = 128;
constexpr uint32_t kBlocksX = kWidth / 4, kBlocksY = kHeight / 4;
constexpr uint32_t kSheetBytes = kBlocksX * kBlocksY * 16;  // 32 KB
constexpr uint32_t kPhysicalSize = 0x20000000;
// The guest's views of physical memory (64 KB, 16 MB and 4 KB pages).
constexpr uint32_t kPhysicalViews[] = {0xA0000000, 0xC0000000, 0xE0000000};

// Identifies the game's original sheet: 32 bytes at +256 (unique on the page
// grid) and a hash of all of it.
constexpr uint32_t kProbeOffset = 256;
constexpr std::array<uint8_t, 32> kProbe = {
    0x00, 0xff, 0x92, 0x49, 0x49, 0x24, 0x24, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x36, 0xa2, 0x46, 0xff, 0xfe, 0xff, 0xff, 0xbd, 0xf8, 0x18, 0xa3, 0xfd, 0xf5, 0x0b, 0x2f};
constexpr uint64_t kOriginalHash = 0x3e520fc9af17500dull;

struct Rgba {
  uint8_t r, g, b, a;
};
using Canvas = std::vector<Rgba>;  // kWidth x kHeight, rows as stored (upside down)

// Where each button sits on the sheet, in stored (upside-down) pixels.
struct Slot {
  const char* name;
  int x0, y0, x1, y1;
};
constexpr Slot kSlots[] = {
    {"b", 2, 94, 34, 126},        {"a", 36, 94, 68, 126},          {"x", 70, 94, 102, 126},
    {"y", 104, 94, 136, 126},     {"back", 141, 93, 176, 126},     {"start", 178, 93, 213, 126},
    {"dpad", 3, 54, 39, 90},      {"dpad_up", 42, 54, 79, 90},     {"stick", 82, 54, 118, 90},
    {"stick_click", 120, 54, 156, 90}, {"stick_move", 158, 54, 194, 90},
    {"bumper_l", 196, 65, 255, 90}, {"bumper_r", 197, 36, 256, 61}, {"trigger_l", 6, 4, 32, 47},
    {"trigger_r", 38, 4, 64, 47}, {"lt", 70, 4, 96, 47},           {"rt", 102, 4, 128, 47},
    {"lb", 134, 6, 193, 31},      {"rb", 197, 6, 256, 31},
};

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// Xenos 2D tiling (as in Xenia's texture_util::GetTiledOffset2D), in blocks.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2) {
  pitch = (pitch + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// --- DXT5 ---------------------------------------------------------------------

Rgba From565(uint16_t c) {
  return {uint8_t((c >> 11 & 31) * 255 / 31), uint8_t((c >> 5 & 63) * 255 / 63), uint8_t((c & 31) * 255 / 31), 255};
}
uint16_t To565(int r, int g, int b) {
  return uint16_t((std::clamp(r, 0, 255) * 31 + 127) / 255 << 11 | (std::clamp(g, 0, 255) * 63 + 127) / 255 << 5 |
                  (std::clamp(b, 0, 255) * 31 + 127) / 255);
}

// Block bytes are little-endian here (after undoing the 16-bit swap).
void DecodeBlock(const uint8_t* b, Rgba out[16]) {
  const int a0 = b[0], a1 = b[1];
  int alpha[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) alpha[i + 1] = ((7 - i) * a0 + i * a1) / 7;
  } else {
    for (int i = 1; i < 5; ++i) alpha[i + 1] = ((5 - i) * a0 + i * a1) / 5;
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t abits = 0;
  for (int i = 0; i < 6; ++i) abits |= uint64_t(b[2 + i]) << (8 * i);
  const uint16_t c0 = uint16_t(b[8] | b[9] << 8), c1 = uint16_t(b[10] | b[11] << 8);
  const Rgba e0 = From565(c0), e1 = From565(c1);
  const Rgba pal[4] = {e0, e1,
                       {uint8_t((2 * e0.r + e1.r) / 3), uint8_t((2 * e0.g + e1.g) / 3), uint8_t((2 * e0.b + e1.b) / 3), 255},
                       {uint8_t((e0.r + 2 * e1.r) / 3), uint8_t((e0.g + 2 * e1.g) / 3), uint8_t((e0.b + 2 * e1.b) / 3), 255}};
  const uint32_t cbits = uint32_t(b[12] | b[13] << 8 | b[14] << 16 | uint32_t(b[15]) << 24);
  for (int i = 0; i < 16; ++i) {
    out[i] = pal[cbits >> (2 * i) & 3];
    out[i].a = uint8_t(alpha[abits >> (3 * i) & 7]);
  }
}

void EncodeBlock(const Rgba in[16], uint8_t* b) {
  // Alpha: 8-value ramp between the block's extremes.
  int amin = 255, amax = 0;
  for (int i = 0; i < 16; ++i) amin = std::min<int>(amin, in[i].a), amax = std::max<int>(amax, in[i].a);
  b[0] = uint8_t(amax);
  b[1] = uint8_t(amin);
  uint64_t abits = 0;
  if (amax > amin) {
    int ramp[8] = {amax, amin};
    for (int i = 1; i < 7; ++i) ramp[i + 1] = ((7 - i) * amax + i * amin) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k)
        if (std::abs(ramp[k] - in[i].a) < std::abs(ramp[best] - in[i].a)) best = k;
      abits |= uint64_t(best) << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) b[2 + i] = uint8_t(abits >> (8 * i));

  // Colour: ends of the block's main axis, from the visible pixels only.
  float mean[3] = {}, n = 0;
  for (int i = 0; i < 16; ++i)
    if (in[i].a) mean[0] += in[i].r, mean[1] += in[i].g, mean[2] += in[i].b, n += 1;
  uint16_t c0 = 0, c1 = 0;
  uint32_t cbits = 0;
  if (n > 0) {
    for (float& m : mean) m /= n;
    float cov[6] = {};
    for (int i = 0; i < 16; ++i) {
      if (!in[i].a) continue;
      const float d[3] = {in[i].r - mean[0], in[i].g - mean[1], in[i].b - mean[2]};
      cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
      cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {  // power iteration
      const float v[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                          cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                          cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
      const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (len < 1e-6f) break;
      for (int k = 0; k < 3; ++k) axis[k] = v[k] / len;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; ++i) {
      if (!in[i].a) continue;
      const float t = (in[i].r - mean[0]) * axis[0] + (in[i].g - mean[1]) * axis[1] + (in[i].b - mean[2]) * axis[2];
      lo = std::min(lo, t), hi = std::max(hi, t);
    }
    auto at = [&](float t, int k) { return int(std::lround(mean[k] + axis[k] * t)); };
    c0 = To565(at(hi, 0), at(hi, 1), at(hi, 2));
    c1 = To565(at(lo, 0), at(lo, 1), at(lo, 2));
    if (c0 < c1) std::swap(c0, c1);
    if (c0 != c1) {
      const Rgba e0 = From565(c0), e1 = From565(c1);
      const int pal[4][3] = {{e0.r, e0.g, e0.b},
                             {e1.r, e1.g, e1.b},
                             {(2 * e0.r + e1.r) / 3, (2 * e0.g + e1.g) / 3, (2 * e0.b + e1.b) / 3},
                             {(e0.r + 2 * e1.r) / 3, (e0.g + 2 * e1.g) / 3, (e0.b + 2 * e1.b) / 3}};
      for (int i = 0; i < 16; ++i) {
        int best = 0, best_d = INT32_MAX;
        for (int k = 0; k < 4; ++k) {
          const int dr = pal[k][0] - in[i].r, dg = pal[k][1] - in[i].g, db = pal[k][2] - in[i].b;
          const int d = dr * dr + dg * dg + db * db;
          if (d < best_d) best = k, best_d = d;
        }
        cbits |= uint32_t(best) << (2 * i);
      }
    }
  }
  b[8] = uint8_t(c0), b[9] = uint8_t(c0 >> 8), b[10] = uint8_t(c1), b[11] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) b[12 + i] = uint8_t(cbits >> (8 * i));
}

Canvas DecodeSheet(const uint8_t* guest) {
  Canvas c(kWidth * kHeight);
  for (uint32_t by = 0; by < kBlocksY; ++by)
    for (uint32_t bx = 0; bx < kBlocksX; ++bx) {
      uint8_t block[16];
      const uint8_t* src = guest + TiledOffset(bx, by, kBlocksX, 4);
      for (int i = 0; i < 16; i += 2) block[i] = src[i + 1], block[i + 1] = src[i];  // 8-in-16 swap
      Rgba px[16];
      DecodeBlock(block, px);
      for (int i = 0; i < 16; ++i) c[(by * 4 + i / 4) * kWidth + bx * 4 + i % 4] = px[i];
    }
  return c;
}

std::vector<uint8_t> EncodeSheet(const Canvas& c) {
  std::vector<uint8_t> out(kSheetBytes);
  for (uint32_t by = 0; by < kBlocksY; ++by)
    for (uint32_t bx = 0; bx < kBlocksX; ++bx) {
      Rgba px[16];
      for (int i = 0; i < 16; ++i) px[i] = c[(by * 4 + i / 4) * kWidth + bx * 4 + i % 4];
      uint8_t block[16];
      EncodeBlock(px, block);
      uint8_t* dst = out.data() + TiledOffset(bx, by, kBlocksX, 4);
      for (int i = 0; i < 16; i += 2) dst[i] = block[i + 1], dst[i + 1] = block[i];
    }
  return out;
}

// --- Composing ------------------------------------------------------------------

struct Picture {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

std::optional<Picture> LoadPng(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
  int w = 0, h = 0;
  Picture p;
  p.rgba = rex::ui::DecodeImageRGBA(data.data(), data.size(), w, h);
  if (p.rgba.empty() || w <= 0 || h <= 0) return std::nullopt;
  p.width = uint32_t(w);
  p.height = uint32_t(h);
  return p;
}

// Draws `pic` into the slot, fitted and centred, flipped to the sheet's
// upside-down storage. The slot is cleared first.
void DrawInto(Canvas& c, const Slot& s, const Picture& pic) {
  const int w = s.x1 - s.x0, h = s.y1 - s.y0;
  for (int y = s.y0; y < s.y1; ++y)
    for (int x = s.x0; x < std::min<int>(s.x1, kWidth); ++x) c[y * kWidth + x] = {0, 0, 0, 0};
  const float scale = std::min(float(w) / pic.width, float(h) / pic.height);
  const float dw = pic.width * scale, dh = pic.height * scale;
  const float ox = s.x0 + (w - dw) / 2, oy = s.y0 + (h - dh) / 2;
  for (int y = s.y0; y < s.y1; ++y)
    for (int x = s.x0; x < std::min<int>(s.x1, kWidth); ++x) {
      // Average the picture's pixels under this sheet pixel (premultiplied).
      const float u0 = (x - ox) / scale, u1 = (x + 1 - ox) / scale;
      const float v0 = (dh - (y + 1 - oy)) / scale, v1 = (dh - (y - oy)) / scale;  // flipped
      float acc[4] = {}, wsum = 0;
      for (int py = int(std::floor(v0)); py < int(std::ceil(v1)); ++py)
        for (int px = int(std::floor(u0)); px < int(std::ceil(u1)); ++px) {
          if (px < 0 || py < 0 || px >= int(pic.width) || py >= int(pic.height)) {
            wsum += 1;
            continue;
          }
          const uint8_t* p = &pic.rgba[(size_t(py) * pic.width + px) * 4];
          const float a = p[3] / 255.0f;
          acc[0] += p[0] * a, acc[1] += p[1] * a, acc[2] += p[2] * a, acc[3] += a;
          wsum += 1;
        }
      if (wsum <= 0 || acc[3] <= 0) continue;
      c[y * kWidth + x] = {uint8_t(acc[0] / acc[3]), uint8_t(acc[1] / acc[3]), uint8_t(acc[2] / acc[3]),
                           uint8_t(std::lround(255 * acc[3] / wsum))};
    }
}

// Keyboard: the image for a key bound to the button (ignoring modifiers). When
// several are bound (A is "Semicolon,Space"), a punctuation key is only used if
// nothing else is, since Space reads better than ";".
std::filesystem::path KeyImage(const std::filesystem::path& dir, const char* keybind) {
  static constexpr const char* kPunctuation[] = {"Semicolon", "Quote",  "Comma",    "Period",   "Slash", "Backslash",
                                                 "LBracket",  "RBracket", "Backtick", "Minus", "Plus"};
  const std::string binds = rex::cvar::GetFlagByName(keybind);
  std::filesystem::path fallback;
  size_t start = 0;
  while (start <= binds.size()) {
    size_t end = binds.find(',', start);
    if (end == std::string::npos) end = binds.size();
    std::string key = binds.substr(start, end - start);
    if (const size_t plus = key.rfind('+'); plus != std::string::npos) key = key.substr(plus + 1);
    key.erase(0, key.find_first_not_of(' '));
    key.erase(key.find_last_not_of(' ') + 1);
    std::error_code ec;
    if (!key.empty() && std::filesystem::exists(dir / (key + ".png"), ec)) {
      if (std::find(std::begin(kPunctuation), std::end(kPunctuation), key) == std::end(kPunctuation))
        return dir / (key + ".png");
      if (fallback.empty()) fallback = dir / (key + ".png");
    }
    start = end + 1;
  }
  return fallback;
}

std::filesystem::path SlotImage(const std::filesystem::path& dir, bool keyboard, const char* slot) {
  static constexpr std::pair<const char*, const char*> kAliases[] = {
      {"trigger_l", "lt"}, {"trigger_r", "rt"}, {"bumper_l", "lb"}, {"bumper_r", "rb"}};
  std::string name = slot;
  for (auto& [from, to] : kAliases)
    if (name == from) name = to;
  if (!keyboard) return dir / (name + ".png");
  static constexpr std::pair<const char*, const char*> kKeys[] = {
      {"a", "keybind_a"}, {"b", "keybind_b"}, {"x", "keybind_x"}, {"y", "keybind_y"},
      {"back", "keybind_back"}, {"start", "keybind_start"}, {"lt", "keybind_left_trigger"},
      {"rt", "keybind_right_trigger"}, {"lb", "keybind_left_shoulder"}, {"rb", "keybind_right_shoulder"},
      {"dpad", "keybind_dpad_up"}, {"dpad_up", "keybind_dpad_up"}, {"stick", "keybind_lstick_up"},
      {"stick_click", "keybind_lstick_press"}, {"stick_move", "keybind_lstick_up"}};
  for (auto& [s, bind] : kKeys)
    if (name == s) return KeyImage(dir, bind);
  return {};
}

std::vector<uint8_t> BuildSheet(const uint8_t* original, const std::string& style) {
  const auto dir = rex::filesystem::GetExecutableFolder() / "glyphs" / style;
  const bool keyboard = style == "keyboard";
  Canvas c = DecodeSheet(original);
  int drawn = 0;
  for (const Slot& s : kSlots) {
    const auto path = SlotImage(dir, keyboard, s.name);
    if (path.empty()) continue;
    if (auto pic = LoadPng(path)) {
      DrawInto(c, s, *pic);
      ++drawn;
    }
  }
  if (!drawn) {
    REXLOG_WARN("KK: no button images found in {}", dir.string());
    return {};
  }
  return EncodeSheet(c);
}

// --- Finding and patching ---------------------------------------------------------

bool Readable(const uint8_t* p, size_t n) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  return p + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
#else
  (void)p, (void)n;
  return true;  // the physical memory mapping is fully backed
#endif
}

// Physical addresses of every copy of the game's original sheet.
std::vector<uint32_t> FindOriginals(const uint8_t* physical) {
  std::vector<uint32_t> found;
  for (uint32_t region = 0; region < kPhysicalSize; region += 0x10000) {
    if (!Readable(physical + region, 0x10000)) continue;
    for (uint32_t page = region; page < region + 0x10000; page += 0x1000) {
      if (std::memcmp(physical + page + kProbeOffset, kProbe.data(), kProbe.size()) != 0) continue;
      if (page + kSheetBytes > kPhysicalSize || !Readable(physical + page, kSheetBytes)) continue;
      if (Fnv1a(physical + page, kSheetBytes) == kOriginalHash) found.push_back(page);
    }
  }
  return found;
}

std::atomic<bool> g_running{false};

}  // namespace

void StartButtonPrompts(rex::memory::Memory* memory) {
  uint8_t* physical_membase = memory->physical_membase();
  const std::string style = REXCVAR_GET(kk_button_prompts);
  if (style.empty() || style == "xbox360" || g_running.exchange(true)) return;
  std::thread([memory, physical_membase, style] {
    std::vector<uint8_t> sheet;
    // The game keeps more than one copy (and loads it again later), so look
    // for originals every second.
    while (true) {
      for (const uint32_t at : FindOriginals(physical_membase)) {
        if (sheet.empty()) {
          sheet = BuildSheet(physical_membase + at, style);
          if (sheet.empty()) return;
        }
        // Put the whole sheet in place first (this path isn't watched), then
        // rewrite one byte per page through the game's views of the memory: the
        // GPU's cache watches those, and each write makes it reload the page.
        // In this order it can only ever reload the finished sheet.
        std::memcpy(physical_membase + at, sheet.data(), kSheetBytes);
        for (const uint32_t view : kPhysicalViews) {
          for (uint32_t page = 0; page < kSheetBytes; page += 0x1000) {
            volatile uint8_t* p = memory->TranslateVirtual<volatile uint8_t*>(view + at + page);
            *p = *p;
          }
        }
        REXLOG_INFO("KK: {} button prompts applied at physical {:08X}", style, at);
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();
}

}  // namespace kk
