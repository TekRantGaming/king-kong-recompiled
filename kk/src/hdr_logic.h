// HDR follows Windows (owner, 17 and 18 Oct; DECISIONS.md "HDR follows Windows"): the pure rules, kept apart
// from the Windows queries (hdr_display.cpp) so the unit tests (tests/hdr_tests.cpp) can check them anywhere.
//
//  - kk_hdr: off (the default since Beta 14: HDR is experimental, the F8 panel's "HDR (experimental)" switches
//    between off and on), on, auto (Follow Windows, the default before). Follow Windows asks for HDR only when Windows HDR is
//    detected on for the game's screen; On always asks (the presenter still needs Windows HDR for that screen and
//    falls back to SDR without it); Off never asks. The answer goes into the presenter's present_hdr.
//  - kk_hdr_levels: auto (the default) or manual. Automatic takes Peak brightness from the Windows HDR Calibration
//    app's colour profile (its MHC2 tag; else the peak the screen reports through DXGI) and Paper white from
//    Windows' "SDR content brightness" (the SDR white level). Manual keeps the player's present_hdr_peak and
//    present_hdr_paper_white.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kk::hdr {

// The presenter's slider ranges and defaults (native-renderer/docs/hdr.md).
constexpr float kPeakMin = 80, kPeakMax = 10000, kPeakDefault = 1000;
constexpr float kPaperMin = 80, kPaperMax = 500, kPaperDefault = 200;

enum class Mode { kFollow, kOn, kOff };
inline Mode ParseMode(std::string_view v) { return v == "on" ? Mode::kOn : v == "off" ? Mode::kOff : Mode::kFollow; }

// What a colour profile says about the screen's brightness (cd/m2, "nits").
struct Luminance {
  float min_nits = 0, max_nits = 0, full_frame_nits = 0;
  bool calibrated = false;  // the profile has an MHC2 tag (Windows HDR Calibration writes one)
};

struct DisplayInfo {
  bool detected = false;        // the game's screen was found (DXGI output)
  bool hdr_on = false;          // Windows HDR is on for it (DXGI colour space G2084 / BT.2020)
  bool hdr_supported = false;   // the screen can do HDR (DisplayConfig advanced colour)
  float dxgi_min_nits = 0, dxgi_max_nits = 0, dxgi_full_frame_nits = 0;  // what DXGI reports for it
  float sdr_white_nits = 0;     // Windows' SDR content brightness; 0: not known
  std::optional<Luminance> profile;  // from the screen's default advanced colour profile
  std::string profile_name, monitor_name;
};

// The SDR white level DisplayConfig gives (DISPLAYCONFIG_SDR_WHITE_LEVEL): 1000 = 80 nits.
inline float SdrWhiteNits(uint32_t level) { return level == 0 ? 0.0f : float(level) / 1000.0f * 80.0f; }

namespace detail {
inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline float S15Fixed16(const uint8_t* p) { return float(int32_t(Be32(p))) / 65536.0f; }
inline bool Finite(float v) { return std::isfinite(v); }
}  // namespace detail

// Reads an ICC profile's brightness tags: MHC2 (Microsoft's hardware calibration tag: min and peak luminance,
// s15Fixed16 at bytes 12 and 16) and lumi (XYZ type: Y = the full-frame luminance). Missing or broken data:
// nullopt. Never reads outside `size`.
inline std::optional<Luminance> ParseIccLuminance(const uint8_t* data, size_t size) {
  using detail::Be32;
  if (!data || size < 132 || std::string_view(reinterpret_cast<const char*>(data) + 36, 4) != "acsp")
    return std::nullopt;
  const size_t declared = Be32(data);
  if (declared < 132 || declared > size) return std::nullopt;
  const uint32_t count = Be32(data + 128);
  if (count > (declared - 132) / 12) return std::nullopt;
  Luminance l;
  bool any = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* t = data + 132 + size_t(i) * 12;
    const std::string_view sig(reinterpret_cast<const char*>(t), 4);
    const size_t off = Be32(t + 4), len = Be32(t + 8);
    if (off > declared || len > declared - off) continue;
    const uint8_t* d = data + off;
    if (sig == "MHC2" && len >= 20 && std::string_view(reinterpret_cast<const char*>(d), 4) == "MHC2") {
      const float mn = detail::S15Fixed16(d + 12), mx = detail::S15Fixed16(d + 16);
      if (detail::Finite(mn) && detail::Finite(mx) && mx > 0 && mn >= 0 && mn < mx) {
        l.min_nits = mn;
        l.max_nits = mx;
        l.calibrated = true;
        any = true;
      }
    } else if (sig == "lumi" && len >= 20 && std::string_view(reinterpret_cast<const char*>(d), 4) == "XYZ ") {
      const float y = detail::S15Fixed16(d + 12);
      if (detail::Finite(y) && y > 0) {
        l.full_frame_nits = y;
        any = true;
      }
    }
  }
  if (!any) return std::nullopt;
  if (l.max_nits <= 0) l.max_nits = l.full_frame_nits;  // lumi alone
  return l;
}

// Whether to ask the presenter for HDR (present_hdr).
inline bool WantHdr(Mode mode, const DisplayInfo& info) {
  switch (mode) {
    case Mode::kOn: return true;
    case Mode::kOff: return false;
    case Mode::kFollow: return info.detected && info.hdr_on;
  }
  return false;
}

struct Levels {
  float peak = kPeakDefault, paper_white = kPaperDefault;
};

enum class PeakSource { kCalibration, kScreen, kDefault };
inline PeakSource AutoPeakSource(const DisplayInfo& info) {
  if (info.profile && info.profile->calibrated && info.profile->max_nits >= kPeakMin) return PeakSource::kCalibration;
  if (info.detected && info.dxgi_max_nits >= kPeakMin) return PeakSource::kScreen;
  return PeakSource::kDefault;
}

// Automatic Peak brightness and Paper white: the calibration's peak (else the screen's reported peak, else the
// default), Windows' SDR white level for Paper white (else the default); both within the presenter's ranges and
// Paper white never above Peak.
inline Levels AutoLevels(const DisplayInfo& info) {
  Levels l;
  switch (AutoPeakSource(info)) {
    case PeakSource::kCalibration: l.peak = info.profile->max_nits; break;
    case PeakSource::kScreen: l.peak = info.dxgi_max_nits; break;
    case PeakSource::kDefault: l.peak = kPeakDefault; break;
  }
  l.peak = std::clamp(std::round(l.peak), kPeakMin, kPeakMax);
  l.paper_white = detail::Finite(info.sdr_white_nits) && info.sdr_white_nits >= kPaperMin ? info.sdr_white_nits
                                                                                          : kPaperDefault;
  l.paper_white = std::clamp(std::round(l.paper_white), kPaperMin, std::min(kPaperMax, l.peak));
  return l;
}

// Task 262: the display's black level and full-frame (large area) brightness for the presenter's highlight
// expansion (present_hdr_black, present_hdr_full_frame), in Automatic and Manual alike (they describe the screen,
// not a preference): the Windows HDR Calibration profile when calibrated, else what DXGI reports, else 0 (= not
// known: no black lift, and large bright areas may reach Peak brightness).
struct DisplayRange {
  float black = 0, full_frame = 0;
};
inline DisplayRange AutoRange(const DisplayInfo& info) {
  DisplayRange r;
  const bool cal = info.profile && info.profile->calibrated;
  const float black = cal && detail::Finite(info.profile->min_nits) && info.profile->max_nits > 0
                          ? info.profile->min_nits : info.detected ? info.dxgi_min_nits : 0;
  const float full = cal && detail::Finite(info.profile->full_frame_nits) && info.profile->full_frame_nits >= kPaperMin
                         ? info.profile->full_frame_nits : info.detected ? info.dxgi_full_frame_nits : 0;
  r.black = detail::Finite(black) ? std::clamp(black, 0.0f, 5.0f) : 0;
  r.full_frame = detail::Finite(full) && full >= kPaperMin ? std::min(full, kPeakMax) : 0;
  return r;
}

}  // namespace kk::hdr
