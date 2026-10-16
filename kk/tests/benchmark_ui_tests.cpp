// The benchmark's launcher pages (start pop-up and results page) driven with
// the real launcher pop-up and input code in an in-memory ImGui frame: the
// controller, the keyboard and the mouse. Also the results files, the pass
// median/spread rules and the in-run cancel press. No window, game or saves.
#include "launcher_benchmark.h"
#include "benchmark_other_gpu.h"
#include "launcher_ui.h"
#include "art.h"
#include "platform.h"
#include <rex/ui/immediate_drawer.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace kk {
const UiFonts& GetUiFonts() { static UiFonts fonts{}; return fonts; }
void KeepTextureAlive(std::unique_ptr<rex::ui::ImmediateTexture>) {}
std::string PadPromptsFor(int, const char*) { return "xbox360"; }
namespace art { Image LoadImage(const std::filesystem::path&) { return {}; } }
}  // namespace kk

namespace {
using namespace kk;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kA = 0x1000, kB = 0x2000;

void Check(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

ui::Modal ModalFor(const bench_ui::Page& page, int* chosen) {
  ui::Modal m;
  m.open = true;
  m.title = page.title; m.body = page.body; m.reader = page.reader;
  m.buttons = page.buttons; m.cancel = page.cancel; m.width = page.width;
  m.on_button = [chosen](int i) { *chosen = i; };
  return m;
}

struct Driver {
  ui::Input input{false};
  ui::Modal modal;
  // One frame: a pad state, a key held, the mouse.
  void Frame(uint16_t pad = 0, ImGuiKey key = ImGuiKey_None, ImVec2 mouse = {-1, -1}, bool click = false) {
    auto& io = ImGui::GetIO();
    if (key != ImGuiKey_None) io.AddKeyEvent(key, true);
    if (mouse.x >= 0) io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, click);
    ImGui::NewFrame();
    ImGui::Begin("benchmark test");
    input.FeedPad(pad, 0, 0, 0, 0);
    input.Update();
    ui::DrawModal(ImGui::GetWindowDrawList(), modal, {0, 0}, {1280, 720}, ui::Metrics{}, input);
    ImGui::End();
    ImGui::Render();
    if (key != ImGuiKey_None) io.AddKeyEvent(key, false);
  }
  // Press and release a pad button.
  void Press(uint16_t pad) { Frame(pad); Frame(0); }
};

void StartPageNavigation() {
  // Controller: D-pad moves a visible focus, A chooses, B goes back.
  for (bool have : {false, true}) {
    const auto page = bench_ui::StartPage(have);
    Check(page.buttons.size() == (have ? 4u : 3u), "start page buttons");
    Check(page.buttons.back() == "Back" && page.cancel == int(page.buttons.size()) - 1, "B is Back");
    int chosen = -1;
    Driver d; d.modal = ModalFor(page, &chosen);
    d.Frame(); d.Frame();
    Check(d.modal.focus == 0, "focus starts on Standard run");
    d.Press(kRight);
    Check(d.modal.focus == 1, "D-pad right moves the focus");
    d.Press(kDown);  // no reader: down moves too
    Check(d.modal.focus == 2, "D-pad down moves the focus");
    d.Press(kUp); d.Press(kLeft);
    Check(d.modal.focus == 0, "D-pad up/left move back");
    d.Press(kRight);
    d.Press(kA);
    Check(chosen == 1 && !d.modal.open, "A chooses the focused button");
    Check(bench_ui::StartChoiceFor(chosen, have) == bench_ui::StartChoice::kQuick, "Quick run");
    chosen = -1; d.modal = ModalFor(page, &chosen); d.Frame(); d.Frame();
    d.Press(kB);
    Check(chosen == page.cancel && bench_ui::StartChoiceFor(chosen, have) == bench_ui::StartChoice::kBack, "B is Back");
    if (have) Check(bench_ui::StartChoiceFor(2, true) == bench_ui::StartChoice::kResults, "Last results");
    Check(bench_ui::StartChoiceFor(0, have) == bench_ui::StartChoice::kStandard, "Standard run");
  }
  // Keyboard: arrows, Enter, Esc.
  {
    const auto page = bench_ui::StartPage(true);
    int chosen = -1;
    Driver d; d.modal = ModalFor(page, &chosen); d.Frame(); d.Frame();
    d.Frame(0, ImGuiKey_RightArrow); d.Frame();
    d.Frame(0, ImGuiKey_RightArrow); d.Frame();
    Check(d.modal.focus == 2, "arrow keys move the focus");
    d.Frame(0, ImGuiKey_Enter); d.Frame();
    Check(chosen == 2, "Enter chooses");
    chosen = -1; d.modal = ModalFor(page, &chosen); d.Frame(); d.Frame();
    d.Frame(0, ImGuiKey_Escape); d.Frame();
    Check(chosen == page.cancel, "Esc goes back");
  }
  // Mouse: hover focuses, click chooses.
  {
    const auto page = bench_ui::StartPage(false);
    int chosen = -1;
    Driver d; d.modal = ModalFor(page, &chosen); d.Frame(); d.Frame();
    Check(d.modal.button_rects.size() == page.buttons.size(), "buttons drawn");
    const auto [a, b] = d.modal.button_rects[1];
    const ImVec2 mid((a.x + b.x) / 2, (a.y + b.y) / 2);
    d.Frame(0, ImGuiKey_None, {mid.x - 3, mid.y});
    d.Frame(0, ImGuiKey_None, mid);
    Check(d.modal.focus == 1, "hover moves the focus");
    d.Frame(0, ImGuiKey_None, mid, true); d.Frame(0, ImGuiKey_None, mid, false);
    Check(chosen == 1, "click chooses");
  }
}

void WriteFile(const std::filesystem::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary) << text;
}

std::string Passes(const std::vector<std::array<double, 5>>& fps) {
  std::vector<benchmark::PassScope> rows;
  for (size_t p = 0; p < fps.size(); ++p)
    for (int c = 0; c <= 4; ++c) {
      benchmark::PassScope r; r.pass = int(p + 1); r.chapter = c;
      r.frames.count = 1000; r.frames.seconds = 1000 / fps[p][size_t(c)]; r.frames.average_fps = fps[p][size_t(c)];
      r.frames.low_fps = fps[p][size_t(c)] / 2; r.frames.worst_ms = 50; r.other_cpu = 2; r.other_gpu = 1;
      rows.push_back(r);
    }
  return benchmark::PassesCsv(rows);
}

void ResultsPage(const std::filesystem::path& root) {
  std::filesystem::remove_all(root);
  const auto older = root / "benchmarks" / "run-1000", newer = root / "benchmarks" / "run-2000",
             cancelled = root / "benchmarks" / "run-999";
  for (const auto& dir : {older, newer, cancelled}) std::filesystem::create_directories(dir);
  WriteFile(older / "status.txt", "status=complete\nsettle=5\n");
  WriteFile(older / "passes.csv", Passes({{100, 100, 100, 100, 100}}));
  WriteFile(cancelled / "status.txt", "status=cancelled\n");
  WriteFile(newer / "status.txt", "status=complete\npasses=3\nsettle=5\napi=Vulkan\nrender=1280x720 > 1280x720\n"
                                  "upscaler=off\npreset=native\ncap=0\n");
  WriteFile(newer / "passes.csv", Passes({{110, 120, 100, 90, 150}, {100, 121, 101, 60, 151}, {105, 122, 102, 92, 149}}));
  WriteFile(newer / "results.txt", "report");
  Check(bench_ui::HasResults(root), "results found");
  const auto r = bench_ui::LoadNewest(root);
  Check(r.found && r.dir == newer && r.report == "report", "newest run");
  Check(r.rows.size() == 15, "passes read");
  Check(std::abs(r.previous_fps - 100) < 1e-9, "previous finished run");
  const auto kong = benchmark::SummarizePasses(r.rows, 3);
  Check(kong.passes == 3 && kong.median.average_fps == 90, "median ignores one slow pass");
  Check(std::abs(kong.spread - 32.0 / 90 * 100) < 1e-9 && kong.confidence == benchmark::Confidence::kLow, "spread");
  Check(benchmark::SummarizePasses(r.rows, 2).confidence == benchmark::Confidence::kHigh, "high confidence");
  const auto page = bench_ui::ResultsPage(r);
  Check(page.buttons.size() == 4 && page.cancel == 0, "results buttons");
  Check(page.reader.find("Kong") != std::string::npos && page.reader.find("Previous run") != std::string::npos,
        "results text");
  // Controller on the results page: the stick/D-pad up and down scroll the text,
  // left and right move between buttons, A chooses, B closes.
  int chosen = -1;
  Driver d; d.modal = ModalFor(page, &chosen);
  d.modal.reader += std::string(200, '\n') + "end\n";  // long enough to scroll
  d.Frame(); d.Frame();
  d.Press(kDown);
  Check(d.modal.focus == 0 && d.modal.reader_scroll > 0, "down scrolls the results");
  d.Press(kRight); d.Press(kRight);
  Check(d.modal.focus == 2, "right moves between buttons");
  d.Press(kA);
  Check(bench_ui::ResultChoiceFor(chosen) == bench_ui::ResultChoice::kOpenFolder, "A opens the folder");
  chosen = -1; d.modal = ModalFor(page, &chosen); d.Frame(); d.Frame();
  d.Press(kRight); d.Press(kB);
  Check(bench_ui::ResultChoiceFor(chosen) == bench_ui::ResultChoice::kClose, "B closes");
  // A missing or cancelled run.
  std::filesystem::remove(newer / "status.txt");
  const auto c = bench_ui::LoadNewest(root);
  Check(c.dir == older, "unfinished run skipped");
  Check(!bench_ui::LoadNewest(root / "nothing").found, "no results");
  Check(bench_ui::ResultsPage(bench_ui::LoadNewest(root / "nothing")).buttons.size() == 2, "no results page");
  std::filesystem::remove_all(root);
}

void Rules() {
  using namespace benchmark;
  Check(Median({3, 1, 2}) == 2 && Median({4, 1, 2, 3}) == 2.5 && Median({}) == 0, "median");
  Check(SpreadPercent({100}) == 0 && std::abs(SpreadPercent({97, 100, 103}) - 6) < 1e-9, "spread");
  Check(ConfidenceFor(1, 0) == Confidence::kUnknown && ConfidenceFor(3, 3) == Confidence::kHigh &&
        ConfidenceFor(3, 5) == Confidence::kMedium && ConfidenceFor(3, 6.5) == Confidence::kLow, "confidence");
  PassScope busy; busy.other_cpu = 20; Check(!busy.Busy(), "system time alone is not busy");
  busy.other_cpu = 35; Check(busy.Busy(), "busy CPU");
  busy.other_cpu = 1; busy.other_gpu = 40; Check(busy.Busy(), "busy GPU");
  busy.other_gpu = -1; Check(!busy.Busy(), "unavailable is not busy");
  const auto rows = ReadPassesCsv(Passes({{1, 2, 3, 4, 5}}));
  Check(rows.size() == 5 && rows[3].frames.average_fps == 4 && rows[3].other_cpu == 2, "csv round trip");
  Check(ReadPassesCsv("pass,scope,count,seconds,average_fps,x\n1,0,abc\n").empty(), "malformed csv");
  Check(ReadPassesCsv("bad header\n").empty(), "bad header");
  // Cancel: a button held as the run starts does not cancel; a new press does.
  CancelPress c;
  Check(!c.Update(0x2000) && !c.Update(0x2000), "held B ignored");
  Check(!c.Update(0) && c.Update(0x2000), "new B cancels");
  CancelPress s; s.Update(0);
  Check(s.Update(0x0010) && !CancelPress{}.Update(0x1000), "Start cancels, A does not");
  CancelPress a; a.Update(0);
  Check(!a.Update(0x1000) && a.Update(0x0020), "Back cancels");
}
}  // namespace

// Other programs' GPU use (task 172): the game's own processes, other adapters and video/copy engines are
// not counted; other programs' 3D/compute work on the game's adapter is.
void OtherGpu() {
  using kk::benchmark::GpuInstance;
  const std::wstring dgpu = L"_luid_0x00000000_0x0001_phys_0_eng_", igpu = L"_luid_0x00000000_0x0002_phys_0_eng_";
  std::vector<std::wstring> names;
  std::vector<double> values;
  auto add = [&](int pid, const std::wstring& adapter, const wchar_t* engine, double v) {
    names.push_back(L"pid_" + std::to_wstring(pid) + adapter + engine);
    values.push_back(v);
  };
  add(100, dgpu, L"0_engtype_3D", 78.9);           // the game
  add(100, igpu, L"0_engtype_3D", 0.5);            // the game's frames shown by a laptop's integrated GPU
  add(90, dgpu, L"0_engtype_3D", 2.0);             // the game's startup parent / closing launcher
  add(500, dgpu, L"2_engtype_VideoEncode", 81.7);  // recording or streaming the game
  add(600, dgpu, L"3_engtype_VideoDecode", 30);    // a video playing
  add(700, igpu, L"0_engtype_3D", 60);             // the integrated GPU composing (dwm.exe)
  add(800, dgpu, L"1_engtype_Copy", 20);
  add(4, dgpu, L"1_engtype_Copy", 5);
  auto run = [&](std::set<uint32_t> own) {
    std::vector<GpuInstance> in;
    for (size_t i = 0; i < names.size(); ++i) in.push_back({names[i], values[i]});
    return kk::benchmark::OtherGpuUse(in, 100, own);
  };
  auto r = run({100, 90});
  Check(r.shader == 0, "video, copy, other-adapter and own-process GPU use is not other programs' use");
  Check(r.all_engines > 81 && r.all_engines < 82, "the old all-engines value keeps the encoder's 81.7%");
  Check(run({100}).shader == 2.0, "a process of another exe on the game's 3D engine counts");
  add(900, dgpu, L"0_engtype_3D", 12);             // a real background app (another game, an upscaler)
  add(901, dgpu, L"5_engtype_Compute_0", 40);      // CUDA/compute work
  add(902, dgpu, L"0_engtype_3D", 3);
  r = run({100, 90});
  Check(r.shader == 40, "background compute counts (busiest engine)");
  values[values.size() - 2] = 1;
  Check(run({100, 90}).shader == 15, "background 3D use is summed over programs");
  // An idle game: every adapter it has a context on is the game's.
  for (size_t i = 0; i < names.size(); ++i) if (names[i].starts_with(L"pid_100_")) values[i] = 0;
  Check(run({100, 90}).shader == 60, "idle game: other programs on any of its adapters count");
  Check(kk::benchmark::InstancePid(L"pid_12345_luid_x") == 12345 && kk::benchmark::InstancePid(L"x") == 0, "pid parse");
}

int main(int argc, char** argv) {
  try {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.DisplaySize = {1280, 720}; io.DeltaTime = 0.01f; io.IniFilename = nullptr;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    StartPageNavigation();
    ResultsPage(std::filesystem::path(argc > 1 ? argv[1] : "benchmark-ui-test"));
    Rules();
    OtherGpu();
    ImGui::DestroyContext();
    std::cout << "benchmark launcher pages (controller, keyboard, mouse), results, cancel rules and other-GPU rules passed\n";
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
