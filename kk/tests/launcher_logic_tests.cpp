// Console checks of launcher logic that needs no game, window or device: the shader job's single
// progress bar (shader_progress.h), and the audio and subtitle options written to every save
// (profile_save_file.h), on made-up saves in a temporary folder.
#include "profile_save_file.h"
#include "shader_progress.h"
#include "graphics_presets.h"
#include "cursor_activity.h"
#include "slider_input.h"
#include "extras.h"
#include "dev_launcher_chain.h"
#include "dev_movie_input.h"
#include "video_modes.h"

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

void ExtrasChecks() {
  using namespace kk::play;
  ExtraProgress fresh;
  Check(kExtras.size() == 15, "the retail Extras page has fifteen entries");
  // Independently transcribed from sub_824CBFE8, loc_824CCD08's id switch.
  // Modes return from that switch; their fields are read by sub_825EE130.
  constexpr ExtraInfo original[] = {
      {19, ExtraType::kMovie, 0x820AE520, -1, -1},
      {18, ExtraType::kLevel, 7, -1, -1},
      {12, ExtraType::kLevel, 54, -1, 5},
      {1, ExtraType::kMode, 0, -1, 15, 0, 0, 0x4DA4},
      {15, ExtraType::kLevel, 55, -1, 1000},
      {9, ExtraType::kLevel, 57, -1, 50},
      {5, ExtraType::kLevel, 50, -1, 75},
      {8, ExtraType::kMode, 0, -1, 100, 0, 0, 0x4DA0},
      {6, ExtraType::kLevel, 51, 20000, -1},
      {7, ExtraType::kLevel, 56, 50000, -1},
      {0, ExtraType::kMovie, 0x820AE4F0, 75000, -1},
      {10, ExtraType::kLevel, 52, 100000, -1},
      {11, ExtraType::kLevel, 53, 150000, -1},
      {3, ExtraType::kMovie, 0x820AE508, -1, 2000},
      {2, ExtraType::kLevel, 2003, 250000, 100, 1, 1},
  };
  constexpr uint32_t original_selection[] = {0, 2, 3, 0, 5, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15};
  for (size_t i = 0; i < kExtras.size(); ++i) {
    const auto& want = original[i];
    const auto* got = FindExtra(want.id);
    Check(kExtras[i].id == want.id && got, "original Extras order and id lookup");
    Check(got->type == want.type && got->level == want.level,
          "every original id selects its exact level or movie address");
    Check(got->start_point == want.start_point && got->session_flag == want.session_flag &&
              got->mode_offset == want.mode_offset,
          "every original id preserves its start point and session or mode flags");
    Check(got->score == want.score && got->progress == want.progress,
          "every original id retains its own unlock requirements");
    Check(ExtraSelection(want.id) == original_selection[i],
          "only level bonuses write their original one-based selection to G+0x5680");
  }
  Check(!ExtraSelection(-1) && !ExtraSelection(4) && !ExtraSelection(13) && !ExtraSelection(20),
        "invalid bonuses do not write the selection field");
  Check(!FindExtra(-1) && !FindExtra(4) && !FindExtra(13) && !FindExtra(20), "unused and invalid bonus ids are not launchable");
  std::map<int, bool> ids;
  for (const auto& e : kExtras) {
    Check(ids.emplace(e.id, true).second, "each retail bonus appears once");
    Check(ExtraUnlocked(e, fresh) == (e.id == 19 || e.id == 18), "new profile availability");
    ExtraProgress p;
    p.museum = true;
    Check(ExtraUnlocked(e, p), "the original museum cheat unlocks each bonus");
    p = {};
    if (e.progress > 100) {
      p.percent = 100; p.score = 1000000;
      Check(!ExtraUnlocked(e, p), "progress and score cannot unlock an online-code bonus");
      p.online_codes = 1u << e.id;
      Check(ExtraUnlocked(e, p), "the matching saved online-code flag unlocks its bonus");
      p.online_codes = 1u << (e.id + 1);
      Check(!ExtraUnlocked(e, p), "an unrelated code flag does not unlock the bonus");
    } else {
      p.percent = std::max(0, e.progress); p.score = std::max(0, e.score);
      Check(ExtraUnlocked(e, p) == (float(p.percent) * 0.01f >= float(e.progress) / 100.0f),
            "unlock comparison matches the game's single-precision progress calculation");
      if (e.progress > 0) { --p.percent; Check(!ExtraUnlocked(e, p), "progress below threshold remains locked"); p.percent = 100; }
      if (e.score > 0) { --p.score; Check(!ExtraUnlocked(e, p), "score below threshold remains locked"); }
    }
  }
  const auto* ending = FindExtra(2);
  Check(ending && ending->level == 2003 && ending->score == 250000 && ending->progress == 100,
        "alternate ending uses its original level, score and completion requirements");
}

void GraphicsPresetChecks() {
  const char* expected[] = {"low", "medium", "high", "high", "low"};
  const char* distances[] = {"near", "medium", "far", "full", "near"};
  for (int i = 0; i < kk::kGraphicsPresetCount; ++i) {
    const auto& p = kk::kGraphicsPresets[i];
    Check(std::string_view(p.gi) == expected[i], "preset GI mapping");
    Check(std::string_view(p.gi_distance) == distances[i], "preset GI distance mapping");
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
  std::map<std::string, std::string> settings;
  auto get = [&](const char* name) { return settings[name]; };
  auto set = [&](const char* name, const char* value) { settings[name] = value; };
  auto match = [&] { return kk::FindGraphicsPreset(get, [](const auto&) { return true; }); };
  kk::ApplyGraphicsPresetSettings(kk::kGraphicsPresets[2], set);
  Check(settings["anisotropic_override"] == "5", "High uses 16x");
  Check(match() == 2, "High selection survives identical Ultra values");
  kk::ApplyGraphicsPresetSettings(kk::kGraphicsPresets[3], set);
  Check(settings["anisotropic_override"] == "5", "Ultra uses 16x");
  Check(match() == 3, "Ultra selection is kept");
  settings.erase("kk_graphics_preset");
  Check(match() == 3, "unmarked old Ultra is still recognised");
  settings["kk_graphics_preset"] = "High";
  settings["anisotropic_override"] = "4";
  Check(match() == -1 && settings["anisotropic_override"] == "4", "existing 8x choice remains Custom and is not overwritten");
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
  for (int i = 4; i < sf::kOptionCount; ++i) sf::WriteWord(b.data() + sf::kFileFields[i], i == 4 ? subtitles : 0);
  sf::WriteWord(b.data() + sf::kInventoryCompanion, 0);
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
    option |= i >= sf::kInventoryCompanion && i < sf::kInventoryCompanion + 4;
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

  // Original modes / control indicators use the same atomic save writer, with unrelated bytes intact.
  auto before_modes = ReadFile(slot_file(0));
  values.fill(0); changed.fill(false);
  for (int i = 5; i < sf::kOptionCount; ++i) { values[i] = 1; changed[i] = true; }
  Check(sf::WriteOptions(slot_file(0), backups, 0, values, changed), "original video and control options save");
  auto after_modes = ReadFile(slot_file(0));
  for (int i = 5; i < sf::kOptionCount; ++i)
    Check(sf::ReadWord(after_modes.data() + sf::kFileFields[i]) == 1, "original option has its selected value");
  Check(sf::ReadWord(after_modes.data() + sf::kInventoryCompanion) == 1, "both original inventory words agree");
  Check(OnlyOptionsChanged(before_modes, after_modes), "changing bonus modes leaves progress, scores and other bytes untouched");
  auto progress_save = MadeUpSave(9, 1, 0);
  sf::WriteWord(progress_save.data() + 0x34, 41);
  for (int i = 0; i < 40; ++i) sf::WriteWord(progress_save.data() + 0x38 + i * 4, i == 0 ? 250000 : 0);
  for (int i = 0; i < 20; ++i) sf::WriteWord(progress_save.data() + 0x168 + 4 * i, i == 15 ? 1 : 0);
  const auto ep = kk::play::DecodeExtraProgress(progress_save);
  Check(ep.percent == 100 && ep.score == 250000, "private save progress and scores decode exactly");
  Check(kk::play::ExtraUnlocked(*kk::play::FindExtra(2), ep), "completed private save unlocks the alternate ending");
  Check(kk::play::ExtraUnlocked(*kk::play::FindExtra(15), ep) && !kk::play::ExtraUnlocked(*kk::play::FindExtra(3), ep),
        "each original saved online-code word unlocks only its own bonus");
  Check(kk::play::DecodeExtraProgress({}).score == 0, "missing save is safe");
  progress_save.resize(100);
  Check(kk::play::DecodeExtraProgress(progress_save).online_codes == 0, "truncated save has no unlocks");
  std::error_code ec;
  fs::remove_all(root, ec);
}
}  // namespace

int main() {
  // Extras playback does not set either story-movie flag. SKIP must still
  // send Start, and must stop sending it once playback ends.
  std::map<uint32_t, uint32_t> movie;
  auto read_movie = [&](uint32_t address) { return movie.at(address); };
  movie[0x82CCAE80] = 0;
  Check(!kk::DevMoviePlaying(read_movie), "no movie player is idle without dereferencing it");
  movie[0x82CCAE80] = 0x1000;
  for (uint32_t flags = 0; flags < 8; ++flags) {
    movie[0x110C] = flags & 1;
    movie[0x1110] = flags & 2;
    movie[0x1130] = flags & 4;
    Check(kk::DevMoviePlaying(read_movie) == (flags != 0), "story and Extras playback enable scripted skip");
  }
  movie[0x110C] = movie[0x1110] = movie[0x1130] = 0;
  Check(!kk::DevMoviePlaying(read_movie), "completed Extras playback cannot pause the next screen");
  // A loaded save's video modes wait for the game's profile pointer and flip target (a launcher start loads
  // the save on the title, where sub_825EE130 read guest 0x4DA4 and crashed), then apply exactly once.
  {
    using kk::play::NextVideoModeStep;
    using kk::play::VideoModeStep;
    Check(NextVideoModeStep(false, 0x1000, 0x2000, 1, 1) == VideoModeStep::kNothing, "no load, nothing to apply");
    for (uint32_t profile : {0u, 0x1000u})
      for (uint32_t target : {0u, 0x2000u})
        for (uint32_t modes = 1; modes < 4; ++modes) {
          const auto step = NextVideoModeStep(true, profile, target, modes & 1, modes >> 1);
          Check(step == (profile && target ? VideoModeStep::kApply : VideoModeStep::kWait),
                "video modes apply only once both game pointers are set");
        }
    Check(NextVideoModeStep(true, 0x1000, 0x2000, 0, 0) == VideoModeStep::kNothing,
          "a save with both modes off calls nothing");
    Check(NextVideoModeStep(true, 0, 0, 0, 0) == VideoModeStep::kWait, "pending modes keep waiting before the profile");
  }
  // Chained scripted Extras sessions: the launcher runs only the first, a started bonus hands on the rest.
  Check(kk::DevLauncherSession("a,b") == "a,b", "a single launcher session runs whole");
  Check(kk::DevLauncherSession("a,b|c|d") == "a,b", "only the first chained session runs");
  Check(kk::DevLauncherSession("") == "", "no launcher input stays empty");
  Check(kk::DevLauncherAfterStart("a,b") == "shot:returned-extras", "an unchained run only marks the return");
  Check(kk::DevLauncherAfterStart("a|c,d|e") == "shot:returned-extras,c,d|e", "the next session follows the return");
  Check(kk::DevLauncherAfterStart("a|") == "shot:returned-extras", "an empty trailing session is dropped");
  Check(kk::DevLauncherSession(kk::DevLauncherAfterStart("a|c|e")) == "shot:returned-extras,c",
        "each return runs exactly one further session");
  // Script buttons: existing names keep their bits; the left stick drives both axes and half pushes.
  Check(kk::DevScriptButton("A") == 0x1000 && kk::DevScriptButton("RIGHT") == 0x0008 &&
            kk::DevScriptButton("SKIP") == kk::kDevSkip && kk::DevScriptButton("RSUP") == kk::kDevRsUp,
        "existing script buttons are unchanged");
  Check(kk::DevScriptButton("NOPE") == 0 && kk::DevScriptButton("") == 0, "unknown script buttons press nothing");
  {
    uint32_t all = 0;
    for (const char* n : {"LSUP", "LSDOWN", "LSLEFT", "LSRIGHT", "LSHALF"}) {
      const uint32_t bit = kk::DevScriptButton(n);
      Check(bit && !(all & bit) && bit > kk::kDevSkip, "left stick names have their own bits");
      all |= bit;
    }
    const auto none = kk::DevScriptSticks(0x1000 | kk::kDevSkip);
    Check(!none.lx && !none.ly && !none.rx && !none.ry, "buttons alone leave both sticks centred");
    const auto up = kk::DevScriptSticks(kk::kDevLsUp);
    Check(up.ly == 32767 && !up.lx && !up.rx && !up.ry, "LSUP pushes the left stick fully forward only");
    const auto back = kk::DevScriptSticks(kk::kDevLsDown | kk::kDevLsLeft | kk::kDevLsHalf);
    Check(back.ly == -16384 && back.lx == -16384, "LSHALF halves both left axes");
    const auto look = kk::DevScriptSticks(kk::kDevRsRight | kk::kDevLsRight);
    Check(look.rx == 32767 && look.lx == 32767 && !look.ry && !look.ly, "both sticks can be held together");
    const auto right_half = kk::DevScriptSticks(kk::kDevRsUp | kk::kDevRsHalf);
    Check(right_half.ry == 16384 && !right_half.ly, "RSHALF still halves only the right stick");
  }
  // All launcher/pause slider ranges and F8's float/integer/opacity controls.
  for (auto range : std::vector<std::array<float, 3>>{{0,100,1}, {0,100,5}, {69,110,1}, {0,100,10},
       {0.05f,10,0.05f}, {0,50,1}, {0,1,0.05f}, {0,4,0.05f}, {0.25f,4,0.05f},
       {1,16,1}, {-2,1,0.05f}, {0.3f,1,0.05f}, {10,200,5}, {25,300,5}, {0,40,1}, {0.1f,5,0.05f}}) {
    const auto [lo, hi, step] = range;
    float v = lo;
    for (int i = 0; i < 2500 && v < hi; ++i) {
      const float next = kk::ui::StepSlider(v, lo, hi, step, 1);
      Check(next > v && next <= hi, "each right press advances and clamps"); v = next;
    }
    Check(v == hi, "slider reaches exact maximum");
    for (int i = 0; i < 2500 && v > lo; ++i) {
      const float next = kk::ui::StepSlider(v, lo, hi, step, -1);
      Check(next < v && next >= lo, "each left press advances and clamps"); v = next;
    }
    Check(v == lo, "slider reaches exact minimum");
    Check(kk::ui::StepSlider(lo,lo,hi,step,-1) == lo && kk::ui::StepSlider(hi,lo,hi,step,1) == hi,
          "slider endpoints stay clamped");
  }
  Check(kk::ui::StepSlider(1.000006f,0,100,1,-1) == 0, "save-file percent rounding reaches mute");
  Check(kk::ui::StepSlider(49.999996f,0,100,1,1) == 51, "rounded percentage advances one displayed step");
  kk::ui::DirectionRepeat repeat;
  Check(repeat.Update(true,0) && !repeat.Update(true,0.1) && repeat.Update(true,0.34), "press and initial repeat delay");
  Check(!repeat.Update(true,0.4) && repeat.Update(true,0.42), "held input repeats");
  Check(!repeat.Update(false,0.43) && repeat.Update(true,0.44), "quick second press is not lost");
  for (int fps : {30,60,120,144,240}) {
    kk::ui::DirectionRepeat clock;
    int presses = 0;
    for (int frame=0;frame<=fps*5;++frame) presses += clock.Update(true,double(frame)/fps);
    Check(presses>=63 && presses<=64, "held repeat rate stays the same across UI frame rates");
  }
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
  ExtrasChecks();
  ShaderProgressChecks();
  AllSavesChecks();
  std::cout << checks << " launcher checks passed\n";
  return 0;
}
