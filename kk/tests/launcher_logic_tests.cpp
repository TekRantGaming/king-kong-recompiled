// Console checks of launcher logic that needs no game, window or device: the shader job's single
// progress bar (shader_progress.h), and the audio and subtitle options written to every save
// (profile_save_file.h), on made-up saves in a temporary folder.
#include "profile_save_file.h"
#include "shader_progress.h"
#include "graphics_presets.h"
#include "cursor_activity.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void GraphicsPresetChecks() {
  const char* expected[] = {"low", "medium", "high", "high", "low"};
  for (int i = 0; i < kk::kGraphicsPresetCount; ++i) {
    const auto& p = kk::kGraphicsPresets[i];
    Check(std::string_view(p.gi) == expected[i], "preset GI mapping");
    std::map<std::string, std::string> settings;
    auto get = [&](const char* name) { return settings[name]; };
    kk::ApplyGraphicsPresetSettings(p, [&](const char* name, const char* value) { settings[name] = value; });
    Check(kk::MatchesGraphicsPresetSettings(p, get), "applied preset matches");
    settings["native_gi"] = "off";
    Check(!kk::MatchesGraphicsPresetSettings(p, get), "different GI makes the preset Custom");
    Check(kk::MatchesGraphicsPresetSettings(p, get, false), "old preset is recognised without GI");
    settings["anisotropic_override"] = "-1";
    Check(!kk::MatchesGraphicsPresetSettings(p, get, false), "other custom settings do not match");
  }
}

void ShaderProgressChecks() {
  using kk::ShaderJobFraction;
  using kk::ShaderJobState;
  ShaderJobState s;
  Check(ShaderJobFraction(s) == 0.0f, "no steps yet: 0");
  // Two APIs: four steps, shaders 42% each and pipelines 8% each.
  s.steps = 4;
  s.step = 1;
  s.phase = 1;  // reading the database
  Check(ShaderJobFraction(s) == 0.0f, "step 1 reading: 0");
  s.phase = 2;
  s.total = 1000;
  s.done = 500;
  Check(Near(ShaderJobFraction(s), 0.42f * 0.5f * kk::kShaderCompileShare), "step 1 half compiled");
  s.phase = 3;
  Check(Near(ShaderJobFraction(s), 0.42f * kk::kShaderCompileShare), "step 1 writing");
  s.phase = 4;
  Check(Near(ShaderJobFraction(s), 0.42f), "step 1 finished");
  s.step = 2;
  s.step_kind = 1;
  s.done = s.total = 0;
  s.pass = 0;
  Check(Near(ShaderJobFraction(s), 0.42f), "step 2 starts where step 1 ended");
  s.total = 100;
  s.done = 50;
  Check(Near(ShaderJobFraction(s), 0.42f + 0.08f * 0.25f), "step 2 first pass half");
  s.pass = 1;
  s.done = 100;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 2 both passes done");
  s.step = 3;
  s.step_kind = 0;
  s.phase = 0;
  s.done = s.total = 0;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 3 starts at half");
  s.step = 4;
  s.step_kind = 1;
  s.pass = 1;
  s.total = s.done = 10;
  Check(Near(ShaderJobFraction(s), 1.0f), "the last step done: 100%");
  // One API (the other failed to start on this PC): two steps over the whole bar.
  ShaderJobState one;
  one.steps = 2;
  one.step = 2;
  one.step_kind = 1;
  Check(Near(ShaderJobFraction(one), 0.42f / 0.5f), "one API: its shaders are 84%");
  // Out-of-range reports stay on the bar.
  ShaderJobState odd;
  odd.steps = 2;
  odd.step = 7;
  odd.step_kind = 1;
  odd.pass = 5;
  odd.done = 50;
  odd.total = 10;
  Check(ShaderJobFraction(odd) <= 1.0f && ShaderJobFraction(odd) >= 0.0f, "out-of-range counts are clamped");

  // A whole job, sampled as the launcher polls it: the bar never goes back and ends at 100%.
  kk::ShaderProgressBar bar;
  float last = 0;
  bool monotonic = true;
  auto poll = [&](const ShaderJobState& st) {
    const float f = bar.Update(st);
    monotonic = monotonic && f >= last;
    last = f;
  };
  for (int api = 0; api < 2; ++api) {
    ShaderJobState st;
    st.steps = 4;
    st.step = api * 2 + 1;
    for (int phase = 0; phase <= 1; ++phase) { st.phase = phase; poll(st); }
    st.phase = 2;
    st.total = 17790;
    for (uint32_t d = 0; d <= st.total; d += 997) { st.done = d; poll(st); }
    st.phase = 3; poll(st);
    st.phase = 4; poll(st);
    st.step = api * 2 + 2;
    st.step_kind = 1;
    st.phase = 4;
    for (int pass = 0; pass < 2; ++pass) {
      st.pass = pass;
      st.done = st.total = 0;
      poll(st);
      // The pass queues more pipelines as it goes: the count goes back, the bar must not.
      st.total = 3000;
      st.done = 2000;
      poll(st);
      st.total = 3700;
      st.done = 2100;
      poll(st);
      st.done = 3700;
      poll(st);
    }
  }
  Check(monotonic, "the bar never goes back over a whole job");
  Check(Near(last, 1.0f), "a whole job ends at 100%");
  bar.Reset();
  Check(bar.Shown() == 0.0f, "a new job starts at 0");
}
namespace fs = std::filesystem;
namespace sf = kk::play::save_file;

std::vector<uint8_t> MadeUpSave(uint32_t seed, float volume, uint32_t subtitles) {
  std::vector<uint8_t> b(sf::kSaveSize);
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t((i * 131 + seed) & 0xFF);
  sf::WriteWord(b.data() + 0x28, 0x1234);  // a continue level: a used slot
  sf::WriteWord(b.data() + 0xA68, 4);
  for (int i = 0; i < 4; ++i) sf::WriteWord(b.data() + sf::kFileFields[i], std::bit_cast<uint32_t>(volume));
  sf::WriteWord(b.data() + sf::kFileFields[4], subtitles);
  return b;
}
void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
size_t CountFiles(const fs::path& dir, const std::string& prefix) {
  size_t n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().starts_with(prefix)) ++n;
  return n;
}
float Volume(const std::vector<uint8_t>& b, int option) {
  return std::bit_cast<float>(sf::ReadWord(b.data() + sf::kFileFields[option]));
}
// Only the option words differ from `before`.
bool OnlyOptionsChanged(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
  if (before.size() != after.size()) return false;
  for (size_t i = 0; i < before.size(); ++i) {
    bool option = false;
    for (uint32_t f : sf::kFileFields) option |= i >= f && i < f + 4;
    if (!option && before[i] != after[i]) return false;
  }
  return true;
}

void AllSavesChecks() {
  const fs::path root = fs::temp_directory_path() /
      ("kk-launcher-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto slot_file = [&root](int slot) { return root / ("slot" + std::to_string(slot + 1)) / "KKProfile.dat"; };
  const fs::path backups = root / "audio-option-backups";
  // Slots 1 and 2: saves. 3: none. 4: the game's empty header. 5: a save with an option out of range.
  const auto save1 = MadeUpSave(1, 1.0f, 0), save2 = MadeUpSave(2, 0.8f, 1);
  auto empty = MadeUpSave(4, 1.0f, 0);
  sf::WriteWord(empty.data() + 0x28, 0);
  auto broken = MadeUpSave(5, 1.0f, 0);
  sf::WriteWord(broken.data() + sf::kFileFields[4], 7);
  WriteFile(slot_file(0), save1);
  WriteFile(slot_file(1), save2);
  WriteFile(slot_file(3), empty);
  WriteFile(slot_file(4), broken);

  sf::Values values{};
  sf::Changed changed{};
  values[2] = 50;  // music
  changed[2] = true;
  values[4] = 1;  // subtitles on
  changed[4] = true;
  auto r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, -1);
  Check(r.written == 2, "both saves take the change");
  Check(r.failed == 1, "the save with an option out of range is refused");
  const auto after1 = ReadFile(slot_file(0)), after2 = ReadFile(slot_file(1));
  Check(Near(Volume(after1, 2), 0.5f) && Near(Volume(after2, 2), 0.5f), "music is 50% in both saves");
  Check(sf::ReadWord(after1.data() + sf::kFileFields[4]) == 1 && sf::ReadWord(after2.data() + sf::kFileFields[4]) == 1,
        "subtitles are on in both saves");
  Check(Near(Volume(after1, 0), 1.0f) && Near(Volume(after2, 0), 0.8f), "an option not changed keeps each save's value");
  Check(OnlyOptionsChanged(save1, after1) && OnlyOptionsChanged(save2, after2), "no other byte of a save changes");
  Check(!fs::exists(slot_file(2)), "an empty slot gets no save");
  Check(ReadFile(slot_file(3)) == empty, "the game's empty header is left alone");
  Check(ReadFile(slot_file(4)) == broken, "a refused save is left unchanged");
  Check(CountFiles(backups, "slot1-") == 1 && CountFiles(backups, "slot2-") == 1, "each changed save is backed up");
  Check(CountFiles(backups, "slot4-") == 0 && CountFiles(backups, "slot5-") == 0, "unchanged saves get no backup");
  {
    std::error_code ec;
    for (fs::directory_iterator it(backups, ec), end; !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.starts_with("slot1-")) Check(ReadFile(it->path()) == save1, "slot 1's backup is the save before");
      if (name.starts_with("slot2-")) Check(ReadFile(it->path()) == save2, "slot 2's backup is the save before");
    }
  }
  Check(CountFiles(slot_file(0).parent_path(), "KKProfile.audio-pending-") == 0, "no temporary file is left behind");

  // The game being played writes its own save (the game thread): skipped here.
  values[2] = 25;
  r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, 1);
  Check(r.written == 1 && Near(Volume(ReadFile(slot_file(0)), 2), 0.25f), "the other saves take the change");
  Check(ReadFile(slot_file(1)) == after2, "the game's own save is skipped");

  // The same value again makes no new backup; many changes keep only the newest three per save.
  r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, 1);
  Check(r.written == 1 && CountFiles(backups, "slot1-") == 2, "an unchanged value makes no new backup");
  for (int v = 10; v <= 40; v += 10) {
    values[2] = float(v);
    sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, -1);
  }
  Check(CountFiles(backups, "slot1-") == 3 && CountFiles(backups, "slot2-") == 3, "the newest three backups are kept");
  Check(Near(Volume(ReadFile(slot_file(0)), 2), 0.4f) && Near(Volume(ReadFile(slot_file(1)), 2), 0.4f),
        "the last value is in every save");
  Check(sf::OptionsValid(ReadFile(slot_file(0)).data(), sf::kFileFields), "the saves stay valid");

  std::error_code ec;
  fs::remove_all(root, ec);
}
}  // namespace

int main() {
  kk::CursorActivity cursor;
  kk::CursorActivity::Pad idle, stick, button;
  stick.axes[2] = 9; button.buttons = 1;
  Check(cursor.mouse, "pointer starts visible");
  cursor.Controller(idle, stick);
  Check(!cursor.mouse, "right stick hides pointer");
  cursor.Mouse();
  cursor.Controller(stick, stick);
  Check(cursor.mouse, "held controller does not override active mouse");
  cursor.Controller(stick, idle);
  Check(cursor.mouse, "controller release does not hide pointer");
  cursor.Controller(idle, button);
  Check(!cursor.mouse, "fresh button hides pointer");
  cursor.Mouse();
  Check(cursor.mouse, "mouse restores pointer");
  GraphicsPresetChecks();
  ShaderProgressChecks();
  AllSavesChecks();
  std::cout << checks << " launcher checks passed\n";
  return 0;
}
