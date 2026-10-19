// Task 219 (owner, 18 Oct): HDR follows Windows (hdr_logic.h: the default decision, the Windows HDR Calibration
// profile's brightness, the automatic Peak brightness and Paper white) and the setup graphics pages' pictures made
// from the game files (setup_compare.h). No window, game or GPU needed. It also prints what this PC's screen
// reports (hdr_display.cpp, information only) and reads any HDR Calibration profiles in the Windows colour folder
// (each must read as calibrated).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "hdr_display.h"
#include "hdr_logic.h"
#include "setup_compare.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
bool Near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}
void PutFixed(std::vector<uint8_t>& b, size_t at, float v) { Put32(b, at, uint32_t(int32_t(std::lround(v * 65536.0f)))); }

// A profile as the Windows HDR Calibration app writes it (only the parts read): header, tag table, lumi, MHC2.
std::vector<uint8_t> Profile(bool lumi, bool mhc2, float min_nits, float max_nits, float full_frame) {
  const uint32_t count = uint32_t(lumi) + uint32_t(mhc2);
  std::vector<uint8_t> b(132 + 12 * count, 0);
  std::memcpy(&b[36], "acsp", 4);
  Put32(b, 128, count);
  size_t tag = 132;
  auto add = [&](const char* sig, std::vector<uint8_t> data) {
    const size_t off = b.size();
    b.insert(b.end(), data.begin(), data.end());
    std::memcpy(&b[tag], sig, 4);
    Put32(b, tag + 4, uint32_t(off));
    Put32(b, tag + 8, uint32_t(data.size()));
    tag += 12;
  };
  if (lumi) {
    std::vector<uint8_t> d(20, 0);
    std::memcpy(&d[0], "XYZ ", 4);
    PutFixed(d, 8, full_frame), PutFixed(d, 12, full_frame), PutFixed(d, 16, full_frame);
    add("lumi", d);
  }
  if (mhc2) {
    std::vector<uint8_t> d(36, 0);
    std::memcpy(&d[0], "MHC2", 4);
    Put32(d, 8, 2);
    PutFixed(d, 12, min_nits), PutFixed(d, 16, max_nits);
    add("MHC2", d);
  }
  Put32(b, 0, uint32_t(b.size()));
  return b;
}

void ProfileTests() {
  using kk::hdr::ParseIccLuminance;
  {
    const auto b = Profile(true, true, 0.05f, 1015.0f, 650.0f);
    const auto l = ParseIccLuminance(b.data(), b.size());
    Check(l && l->calibrated, "a calibration profile is read as calibrated");
    Check(l && Near(l->min_nits, 0.05f, 0.001f) && Near(l->max_nits, 1015.0f) && Near(l->full_frame_nits, 650.0f),
          "MHC2 min/peak and lumi full frame are read");
  }
  {
    const auto b = Profile(true, false, 0, 0, 400.0f);
    const auto l = ParseIccLuminance(b.data(), b.size());
    Check(l && !l->calibrated && Near(l->max_nits, 400.0f), "lumi alone: not calibrated, its value as the peak");
  }
  {
    const auto b = Profile(false, false, 0, 0, 0);
    Check(!ParseIccLuminance(b.data(), b.size()), "a profile without brightness tags gives nothing");
  }
  {
    auto b = Profile(true, true, 0, 1000.0f, 1000.0f);
    std::memcpy(&b[36], "xxxx", 4);
    Check(!ParseIccLuminance(b.data(), b.size()), "not an ICC profile: nothing");
  }
  {
    const auto b = Profile(true, true, 0, 1000.0f, 1000.0f);
    Check(!ParseIccLuminance(b.data(), b.size() - 1), "a cut-off file (shorter than its header says): nothing");
    Check(!ParseIccLuminance(b.data(), 100), "shorter than a header: nothing");
    Check(!ParseIccLuminance(nullptr, 0), "no data: nothing");
  }
  {
    auto b = Profile(false, true, 0, 1000.0f, 0);
    Put32(b, 132 + 4, 0x7fffff00);  // the MHC2 tag points outside the file
    Check(!ParseIccLuminance(b.data(), b.size()), "a tag outside the file is not read");
  }
  {
    auto b = Profile(false, true, 500.0f, 400.0f, 0);  // min above peak: broken
    Check(!ParseIccLuminance(b.data(), b.size()), "a broken MHC2 (min above peak) is ignored");
    auto c = Profile(true, false, 0, 0, 0);
    Check(!ParseIccLuminance(c.data(), c.size()), "lumi 0 is ignored");
  }
  {
    auto b = Profile(true, true, 0, 1000.0f, 1000.0f);
    Put32(b, 128, 0x40000000);  // a tag count far beyond the file
    Check(!ParseIccLuminance(b.data(), b.size()), "an impossible tag count is refused");
  }
}

void DecisionTests() {
  using namespace kk::hdr;
  Check(ParseMode("auto") == Mode::kFollow && ParseMode("") == Mode::kFollow && ParseMode("junk") == Mode::kFollow,
        "auto, empty or unknown is Follow Windows");
  Check(ParseMode("on") == Mode::kOn && ParseMode("off") == Mode::kOff, "on and off");
  DisplayInfo hdr;
  hdr.detected = hdr.hdr_on = hdr.hdr_supported = true;
  DisplayInfo sdr;
  sdr.detected = true;
  sdr.hdr_supported = true;
  DisplayInfo none;  // nothing detected (no DXGI output, other platform without the property)
  Check(WantHdr(Mode::kFollow, hdr), "Follow Windows: HDR when Windows HDR is on");
  Check(!WantHdr(Mode::kFollow, sdr), "Follow Windows: SDR when Windows HDR is off");
  Check(!WantHdr(Mode::kFollow, none), "Follow Windows: SDR when it cannot be detected");
  Check(WantHdr(Mode::kOn, sdr) && WantHdr(Mode::kOn, none), "On always asks (the presenter still falls back)");
  Check(!WantHdr(Mode::kOff, hdr), "Off never asks, even with Windows HDR on");

  Check(Near(SdrWhiteNits(1000), 80) && Near(SdrWhiteNits(2500), 200) && Near(SdrWhiteNits(6000), 480),
        "SDR white level: 1000 = 80 nits");
  Check(SdrWhiteNits(0) == 0, "no SDR white level: 0");

  // Automatic levels: the calibration's peak first, then the screen's reported peak, then the default.
  DisplayInfo d = hdr;
  d.dxgi_max_nits = 600;
  d.sdr_white_nits = 240;
  Luminance cal;
  cal.calibrated = true;
  cal.max_nits = 4000;
  cal.full_frame_nits = 4000;
  d.profile = cal;
  Check(AutoPeakSource(d) == PeakSource::kCalibration && Near(AutoLevels(d).peak, 4000) &&
            Near(AutoLevels(d).paper_white, 240),
        "calibrated: peak from the profile, paper white from Windows' SDR brightness");
  d.profile->calibrated = false;
  Check(AutoPeakSource(d) == PeakSource::kScreen && Near(AutoLevels(d).peak, 600), "no calibration: the screen's peak");
  d.profile.reset();
  d.dxgi_max_nits = 0;
  Check(AutoPeakSource(d) == PeakSource::kDefault && Near(AutoLevels(d).peak, kPeakDefault), "nothing: the default peak");
  d.sdr_white_nits = 0;
  Check(Near(AutoLevels(d).paper_white, kPaperDefault), "no SDR white level: the default paper white");

  // Task 262: black level and full-frame brightness: calibration, else DXGI, else 0 (not known).
  {
    DisplayInfo r = hdr;
    r.dxgi_min_nits = 0.05f;
    r.dxgi_full_frame_nits = 350;
    Luminance c;
    c.calibrated = true;
    c.min_nits = 0.002f;
    c.max_nits = 800;
    c.full_frame_nits = 300;
    r.profile = c;
    Check(Near(AutoRange(r).black, 0.002f) && Near(AutoRange(r).full_frame, 300), "range from the calibration");
    r.profile->calibrated = false;
    Check(Near(AutoRange(r).black, 0.05f) && Near(AutoRange(r).full_frame, 350), "range from DXGI");
    r.detected = false;
    r.profile.reset();
    Check(AutoRange(r).black == 0 && AutoRange(r).full_frame == 0, "nothing known: 0 (no black lift, no cap)");
    r.detected = true;
    r.dxgi_min_nits = std::numeric_limits<float>::quiet_NaN();
    r.dxgi_full_frame_nits = 20;
    Check(AutoRange(r).black == 0 && AutoRange(r).full_frame == 0, "broken values ignored");
  }
  d.sdr_white_nits = 480;
  d.dxgi_max_nits = 20000;
  Check(Near(AutoLevels(d).peak, kPeakMax) && Near(AutoLevels(d).paper_white, 480), "peak clamped to 10000 nits");
  d.sdr_white_nits = 700;
  Check(Near(AutoLevels(d).paper_white, kPaperMax), "paper white clamped to 500 nits");
  d.dxgi_max_nits = 300;
  d.sdr_white_nits = 400;
  Check(Near(AutoLevels(d).peak, 300) && Near(AutoLevels(d).paper_white, 300), "paper white never above the peak");
  d.dxgi_max_nits = 40;  // a nonsense report below the slider range
  Check(AutoPeakSource(d) == PeakSource::kDefault, "a peak below 80 nits is not used");
  d.sdr_white_nits = std::nanf("");
  Check(Near(AutoLevels(d).paper_white, kPaperDefault), "a non-finite SDR white level is not used");
}

// --------------------------------------------------------------------------- setup pictures ---
using kk::art::Image;
Image Checker(int w, int h, int cell) {
  Image im;
  im.width = w, im.height = h;
  im.rgba.resize(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t v = ((x / cell + y / cell) & 1) ? 230 : 25;
      uint8_t* p = &im.rgba[(size_t(y) * w + x) * 4];
      p[0] = p[1] = p[2] = v, p[3] = 255;
    }
  return im;
}
Image Flat(int w, int h, uint8_t v) {
  Image im;
  im.width = w, im.height = h;
  im.rgba.assign(size_t(w) * h * 4, v);
  return im;
}
// Detail: the summed difference between neighbouring pixels, in rows y0..y1.
double Detail(const Image& im, int y0, int y1) {
  double sum = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = 1; x < im.width; ++x)
      sum += std::abs(int(im.rgba[(size_t(y) * im.width + x) * 4]) - int(im.rgba[(size_t(y) * im.width + x - 1) * 4]));
  return sum;
}

void PictureTests() {
  namespace c = kk::compare;
  const Image flat = Flat(64, 36, 120), check = Checker(128, 72, 2);
  Check(c::Resize(check, 32, 18).width == 32 && c::Resize(check, 32, 18).height == 18, "Resize gives the size asked");
  Check(c::Fit(Checker(200, 50, 4), 160, 90).width == 160 && c::Fit(Checker(200, 50, 4), 160, 90).height == 90,
        "Fit gives the shape asked");
  Check(c::FewerPixels(flat, 2).rgba == flat.rgba, "fewer pixels leaves a flat picture as it is");
  Check(Detail(c::FewerPixels(check, 2), 0, 72) < Detail(check, 0, 72) * 0.5, "fewer pixels loses fine detail");
  Check(c::Sharpen(flat, 1.2f).rgba == flat.rgba, "sharpening leaves a flat picture as it is");
  const Image soft = c::Resize(c::Resize(Checker(128, 72, 8), 64, 36), 128, 72);
  Check(Detail(c::Sharpen(soft, 1.2f), 0, 72) > Detail(soft, 0, 72), "sharpening adds contrast at edges");
  const Image jag = c::Edges(check, 96, 54, 6, 3, false), smooth = c::Edges(check, 96, 54, 6, 3, true);
  Check(jag.width == 96 && smooth.height == 54, "edges: the size asked");
  Check(Detail(smooth, 0, 54) < Detail(jag, 0, 54), "edges: the smooth picture has softer steps than the jagged one");
  // The floor: 16x filtering keeps more detail in the distance (top half of the floor) than plain filtering.
  const Image tex = Checker(256, 256, 4);
  const Image plain = c::Ground(tex, 320, 180, false), aniso = c::Ground(tex, 320, 180, true);
  const int horizon = int(180 * 0.22f);
  Check(Detail(aniso, horizon + 10, horizon + 50) > Detail(plain, horizon + 10, horizon + 50) * 2,
        "16x filtering keeps the far floor sharper than plain filtering");
  Check(c::Ground(Image{}, 32, 18, true).width == 32 && c::Resize(Image{}, 8, 8).width == 8,
        "no picture: a blank of the size asked, no crash");
}

// Information only: this PC's screen and any Windows HDR Calibration profiles.
void PrintThisPc() {
  const kk::hdr::DisplayInfo d = kk::hdr::QueryDisplay(nullptr);
  std::printf("this PC (primary screen): detected=%d name='%s' windows_hdr=%d hdr_capable=%d dxgi=%.4f/%.0f/%.0f nits "
              "sdr_white=%.0f nits profile='%s'",
              d.detected, d.monitor_name.c_str(), d.hdr_on, d.hdr_supported, d.dxgi_min_nits, d.dxgi_max_nits,
              d.dxgi_full_frame_nits, d.sdr_white_nits, d.profile_name.c_str());
  if (d.profile)
    std::printf(" calibrated=%d %.3f/%.0f/%.0f nits", d.profile->calibrated, d.profile->min_nits, d.profile->max_nits,
                d.profile->full_frame_nits);
  const kk::hdr::Levels l = kk::hdr::AutoLevels(d);
  std::printf("\n  -> Follow Windows: %s, automatic peak %.0f nits, paper white %.0f nits, calibration app %s\n",
              kk::hdr::WantHdr(kk::hdr::Mode::kFollow, d) ? "HDR" : "SDR", l.peak, l.paper_white,
              kk::hdr::CalibrationAppInstalled() ? "installed" : "not installed");
#if defined(_WIN32)
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator("C:/Windows/System32/spool/drivers/color", ec)) {
    const std::string name = e.path().filename().string();
    if (name.rfind("HDR Calibrated", 0) != 0) continue;
    std::ifstream f(e.path(), std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto lum = kk::hdr::ParseIccLuminance(b.data(), b.size());
    std::printf("  profile '%s': %s", name.c_str(), lum ? "" : "no brightness tags\n");
    if (lum) std::printf("calibrated=%d %.3f/%.0f/%.0f nits\n", lum->calibrated, lum->min_nits, lum->max_nits,
                         lum->full_frame_nits);
    Check(lum && lum->calibrated, "a Windows HDR Calibration profile on this PC reads as calibrated");
  }
#endif
}
}  // namespace

int main() {
  ProfileTests();
  DecisionTests();
  PictureTests();
  PrintThisPc();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("hdr and setup picture tests passed\n");
  return 0;
}
