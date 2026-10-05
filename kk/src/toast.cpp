#include "toast.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/immediate_drawer.h>

#include "art.h"
#include "platform.h"
#include "settings.h"

namespace kk {
namespace {

// Timeline of one toast, in seconds.
constexpr double kOrbIn = 0.30, kUnfold = 0.40, kHold = 4.6, kFold = 0.35, kOrbOut = 0.25;
constexpr double kTotal = kOrbIn + kUnfold + kHold + kFold + kOrbOut;

float EaseOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}
float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2; }

constexpr double kPi = 3.14159265358979323846;

// --- Chime: an original two-note "unlock" sound, synthesized at runtime ------
std::vector<uint8_t> MakeChimeWav(float volume) {
  constexpr int kRate = 44100;
  constexpr double kLength = 1.6;
  std::vector<float> mix(size_t(kRate * kLength), 0.0f);
  auto bell = [&](double start, double freq, double gain, double decay) {
    const size_t s0 = size_t(start * kRate);
    for (size_t i = s0; i < mix.size(); ++i) {
      const double t = double(i - s0) / kRate;
      const double env = std::min(1.0, t / 0.006) * std::exp(-t / decay);
      const double v = std::sin(2 * kPi * freq * t) + 0.45 * std::sin(2 * kPi * freq * 2.0 * t) * std::exp(-t / 0.15) +
                       0.18 * std::sin(2 * kPi * freq * 3.01 * t) * std::exp(-t / 0.08);
      mix[i] += float(gain * env * v);
    }
  };
  // A soft low swell, then a rising pair of bell tones with a shimmer on top.
  bell(0.00, 392.00, 0.22, 0.30);
  bell(0.02, 783.99, 0.55, 0.40);
  bell(0.13, 1174.66, 0.60, 0.55);
  bell(0.13, 1567.98, 0.18, 0.30);
  // Short echo for space.
  const size_t delay = size_t(0.17 * kRate);
  for (size_t i = mix.size(); i-- > delay;) mix[i] += 0.28f * mix[i - delay];
  float peak = 1e-6f;
  for (float v : mix) peak = std::max(peak, std::abs(v));
  const float scale = 0.70f * std::clamp(volume, 0.0f, 1.0f) / peak;

  const uint32_t data_bytes = uint32_t(mix.size() * 2);
  std::vector<uint8_t> wav(44 + data_bytes);
  auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
  auto put16 = [&](size_t o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
  std::memcpy(&wav[0], "RIFF", 4);
  put32(4, 36 + data_bytes);
  std::memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, 1);
  put32(24, kRate);
  put32(28, kRate * 2);
  put16(32, 2);
  put16(34, 16);
  std::memcpy(&wav[36], "data", 4);
  put32(40, data_bytes);
  for (size_t i = 0; i < mix.size(); ++i)
    put16(44 + i * 2, uint16_t(int16_t(std::clamp(mix[i] * scale, -1.0f, 1.0f) * 32767.0f)));
  return wav;
}

std::filesystem::path PortAchievementFile(const std::filesystem::path& user_dir) {
  return art::CacheDir(user_dir) / "port_achievements.txt";
}

}  // namespace

std::filesystem::path SoundsDir(const std::filesystem::path& user_dir) { return user_dir / "sounds"; }

std::vector<std::filesystem::path> ListSounds(const std::filesystem::path& user_dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(SoundsDir(user_dir), ec)) {
    auto ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (e.is_regular_file(ec) && ext == ".wav") out.push_back(e.path().filename());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string SoundLabel(const std::filesystem::path& file) {
  std::string name = file.stem().string();
  bool word_start = true;
  for (char& c : name) {
    if (c == '_' || c == '-') c = ' ';
    if (word_start && c != ' ') c = char(std::toupper(static_cast<unsigned char>(c)));
    word_start = c == ' ';
  }
  return name;
}

namespace {

// Reads a 16-bit PCM WAV (tolerating the 0xFFFFFFFF sizes written by streaming
// encoders) and returns a clean WAV with the volume applied, or empty if the
// format isn't 16-bit PCM.
std::vector<uint8_t> LoadWavScaled(const std::filesystem::path& path, float volume) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  std::vector<uint8_t> in(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(in.data()), in.size());
  if (in.size() < 12 || std::memcmp(in.data(), "RIFF", 4) || std::memcmp(in.data() + 8, "WAVE", 4)) return {};
  const uint8_t* fmt = nullptr;
  size_t data_off = 0, data_len = 0;
  for (size_t o = 12; o + 8 <= in.size();) {
    uint32_t len;
    std::memcpy(&len, &in[o + 4], 4);
    if (!std::memcmp(&in[o], "fmt ", 4) && len >= 16 && o + 8 + 16 <= in.size()) fmt = &in[o + 8];
    if (!std::memcmp(&in[o], "data", 4)) {
      data_off = o + 8;
      data_len = std::min<size_t>(len, in.size() - data_off);
      break;
    }
    o += 8 + size_t(len) + (len & 1);
  }
  if (!fmt || !data_off) return {};
  uint16_t format, channels, bits;
  uint32_t rate;
  std::memcpy(&format, fmt, 2);
  std::memcpy(&channels, fmt + 2, 2);
  std::memcpy(&rate, fmt + 4, 4);
  std::memcpy(&bits, fmt + 14, 2);
  if (format != 1 || bits != 16 || channels == 0) return {};
  data_len &= ~size_t(channels * 2 - 1);

  std::vector<uint8_t> wav(44 + data_len);
  auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
  auto put16 = [&](size_t o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
  std::memcpy(&wav[0], "RIFF", 4);
  put32(4, uint32_t(36 + data_len));
  std::memcpy(&wav[8], "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, channels);
  put32(24, rate);
  put32(28, rate * channels * 2);
  put16(32, uint16_t(channels * 2));
  put16(34, 16);
  std::memcpy(&wav[36], "data", 4);
  put32(40, uint32_t(data_len));
  for (size_t i = 0; i + 1 < data_len; i += 2) {
    int16_t s;
    std::memcpy(&s, &in[data_off + i], 2);
    const int16_t scaled = int16_t(std::clamp(float(s) * volume, -32768.0f, 32767.0f));
    std::memcpy(&wav[44 + i], &scaled, 2);
  }
  return wav;
}

}  // namespace

void PlayAchievementSound(const std::filesystem::path& user_dir) {
#if defined(_WIN32)
  if (!REXCVAR_GET(kk_achievement_sound)) return;
  // PlaySound reads SND_MEMORY data while playing asynchronously, so keep the
  // buffer alive and only rebuild it when the sound or volume changes.
  static std::vector<uint8_t> wav;
  static std::string wav_key;
  const int volume = std::clamp(REXCVAR_GET(kk_achievement_volume), 0, 100);
  std::filesystem::path file;
  if (const std::string& name = REXCVAR_GET(kk_achievement_sound_file); !name.empty() && !user_dir.empty()) {
    std::error_code ec;
    file = SoundsDir(user_dir) / name;
    if (!std::filesystem::exists(file, ec)) file.clear();
  }
  const std::string key = file.string() + "|" + std::to_string(volume);
  if (key != wav_key) {
    PlaySoundW(nullptr, nullptr, 0);  // stop before replacing the buffer
    wav = file.empty() ? std::vector<uint8_t>{} : LoadWavScaled(file, volume / 100.0f);
    if (wav.empty()) wav = MakeChimeWav(volume / 100.0f);
    wav_key = key;
  }
  PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#else
  (void)user_dir;
#endif
}

AchievementToast::AchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                                   std::filesystem::path game_dir, std::filesystem::path user_dir)
    : AchievementNotificationDialog(drawer),
      immediate_(immediate),
      game_dir_(std::move(game_dir)),
      user_dir_(std::move(user_dir)) {
  icon_paths_ = art::AchievementIcons(game_dir_);
  icon_paths_[0] = art::TitleIconPath(game_dir_);
}

AchievementToast::~AchievementToast() = default;

void AchievementToast::Push(const rex::system::AchievementEvent& event) {
  Show(event.achievement.label, event.achievement.gamerscore, event.achievement.id);
}

void AchievementToast::Show(std::string title, uint32_t gamerscore, uint32_t icon_id) {
  if (!REXCVAR_GET(kk_achievement_toasts)) return;
  std::lock_guard lock(mutex_);
  queue_.push_back({std::move(title), gamerscore, icon_id});
}

rex::ui::ImmediateTexture* AchievementToast::Icon(uint32_t id) {
  if (auto it = icons_.find(id); it != icons_.end()) return it->second.get();
  auto& slot = icons_[id];
  auto path = icon_paths_.find(id);
  if (path == icon_paths_.end() || !immediate_) return nullptr;
  const auto img = art::LoadImage(path->second);
  if (img)
    slot = immediate_->CreateTexture(uint32_t(img.width), uint32_t(img.height),
                                     rex::ui::ImmediateTextureFilter::kLinear, false, img.rgba.data());
  return slot.get();
}

void AchievementToast::OnDraw(ImGuiIO& io) {
  Toast toast;
  const double now = ImGui::GetTime();
  {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return;
    if (queue_.front().started < 0) {
      queue_.front().started = now;
      PlayAchievementSound(user_dir_);
    }
    if (now - queue_.front().started > kTotal) {
      queue_.pop_front();
      return;
    }
    toast = queue_.front();
  }

  const float t = float(now - toast.started);
  const float s = ImGui::GetFontSize() / 18.0f;
  const UiFonts& fonts = GetUiFonts();
  ImFont* regular = fonts.regular ? fonts.regular : ImGui::GetFont();
  ImFont* semibold = fonts.semibold ? fonts.semibold : regular;

  // Phase values: orb scale (0..1) and bar unfold (0..1).
  float orb = 1.0f, unfold = 1.0f;
  if (t < kOrbIn) {
    orb = EaseOutBack(t / float(kOrbIn));
    unfold = 0.0f;
  } else if (t < kOrbIn + kUnfold) {
    unfold = EaseInOut((t - float(kOrbIn)) / float(kUnfold));
  } else if (t > kTotal - kOrbOut) {
    unfold = 0.0f;
    orb = 1.0f - EaseInOut((t - float(kTotal - kOrbOut)) / float(kOrbOut));
  } else if (t > kTotal - kOrbOut - kFold) {
    unfold = 1.0f - EaseInOut((t - float(kTotal - kOrbOut - kFold)) / float(kFold));
  }

  const std::string line1 = "Achievement unlocked";
  const std::string line2 = toast.gamerscore ? std::to_string(toast.gamerscore) + "G - " + toast.title : toast.title;
  const float h = 70 * s, r = h * 0.5f;
  const float text_w = std::max(regular->CalcTextSizeA(16 * s, FLT_MAX, 0, line1.c_str()).x,
                                semibold->CalcTextSizeA(20 * s, FLT_MAX, 0, line2.c_str()).x);
  const float full_w = h + 22 * s + text_w + 34 * s;
  const float w = h + (full_w - h) * unfold;
  const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.86f);
  const float x0 = center.x - full_w * 0.5f, y0 = center.y - r;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (unfold > 0.0f) {
    const ImVec2 b0(x0, y0), b1(x0 + w, y0 + h);
    dl->AddRectFilled(ImVec2(b0.x + 3 * s, b0.y + 5 * s), ImVec2(b1.x + 3 * s, b1.y + 5 * s), IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(b0, b1, IM_COL32(30, 30, 34, 245), r);
    dl->AddRectFilled(ImVec2(b0.x + 2 * s, b0.y + 2 * s), ImVec2(b1.x - 2 * s, b0.y + h * 0.5f),
                      IM_COL32(255, 255, 255, 16), r - 2 * s, ImDrawFlags_RoundCornersTop);
    dl->AddRect(b0, b1, IM_COL32(255, 255, 255, 40), r, 0, 1.2f * s);
    // Text fades in once the bar is mostly open.
    const float alpha = std::clamp((unfold - 0.6f) / 0.4f, 0.0f, 1.0f);
    if (alpha > 0.0f) {
      const float tx = x0 + h + 18 * s;
      dl->PushClipRect(b0, ImVec2(b1.x - 14 * s, b1.y), true);
      dl->AddText(regular, 16 * s, ImVec2(tx, y0 + 12 * s), IM_COL32(200, 205, 210, int(255 * alpha)), line1.c_str());
      dl->AddText(semibold, 20 * s, ImVec2(tx, y0 + 33 * s), IM_COL32(255, 255, 255, int(255 * alpha)), line2.c_str());
      dl->PopClipRect();
    }
  }
  // The orb: a green ring around the achievement's icon.
  if (orb > 0.0f) {
    const ImVec2 c(x0 + r, y0 + r);
    const float rr = (r + 4 * s) * orb;
    dl->AddCircleFilled(c, rr + 2 * s, IM_COL32(0, 0, 0, 80), 48);
    dl->AddCircleFilled(c, rr, IM_COL32(124, 194, 66, 255), 48);
    dl->AddCircleFilled(c, rr * 0.86f, IM_COL32(18, 22, 18, 255), 48);
    if (auto* icon = Icon(toast.icon_id)) {
      const float ir = rr * 0.80f;
      dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(icon)), ImVec2(c.x - ir, c.y - ir),
                          ImVec2(c.x + ir, c.y + ir), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ir);
    }
    dl->AddCircle(c, rr, IM_COL32(190, 240, 140, 180), 48, 1.5f * s);
  }
}

const std::vector<PortAchievement>& PortAchievements() {
  static const std::vector<PortAchievement> kList = {
      {"welcome", "Welcome to Skull Island",
       "Started King Kong on PC for the first time. Open the launcher's Achievements page to track the game's 9."},
  };
  return kList;
}

bool IsPortAchievementUnlocked(const std::filesystem::path& user_dir, const char* id) {
  std::ifstream f(PortAchievementFile(user_dir));
  std::string line;
  while (std::getline(f, line))
    if (line == id) return true;
  return false;
}

bool UnlockPortAchievement(const std::filesystem::path& user_dir, const char* id) {
  if (user_dir.empty() || IsPortAchievementUnlocked(user_dir, id)) return false;
  std::error_code ec;
  std::filesystem::create_directories(art::CacheDir(user_dir), ec);
  std::ofstream(PortAchievementFile(user_dir), std::ios::app) << id << "\n";
  return true;
}

}  // namespace kk
