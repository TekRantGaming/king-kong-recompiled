#include "../../kk/src/startup_recovery_state.h"
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void Check(bool ok, const char* name) {
  if (!ok) throw std::runtime_error(name);
  ++checks;
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  using kk::startup::RecoveryState;
  const std::filesystem::path root(argv[1]);
  std::filesystem::remove_all(root);  // a fresh start every run (an earlier run leaves its state behind)
#if defined(__linux__)
  // Linux has one graphics API: recovery goes from Vulkan straight to "failed" (startup_recovery_state.h).
  for (const std::string preferred : {"vulkan"}) {
#else
  for (const std::string preferred : {"vulkan", "d3d12"}) {
#endif
    const auto dir = root / preferred;
    std::filesystem::create_directories(dir / "saves");
    { std::ofstream save(dir / "saves" / "sentinel"); save << "save must survive"; }
    RecoveryState s(dir);
    Check(!s.Load(), "fresh state");
    s.preferred = s.selected = preferred;
    Check(s.Save() && s.Marker(preferred), "arm before startup");
    RecoveryState first(dir);
    Check(first.Load() && first.RecoverCrash(), "detect first crash");
    Check(first.selected == preferred && first.crashes == 1, "retry once");
    Check(first.Marker(preferred), "arm second launch");
    RecoveryState second(dir);
    Check(second.Load() && second.RecoverCrash(), "detect second crash");
    Check(second.selected != preferred && second.selected != "classic" && second.crashes == 0,
          "switch to other native API");
    Check(!second.RecoverCrash(), "failure consumed only once");
#if defined(__linux__)
    Check(second.selected == "failed", "the only native API terminates the chain");
#else
    Check(second.Advance("lost its graphics device"), "known device loss advances immediately");
    Check(second.selected == "failed", "both native APIs terminate chain");
#endif
    Check(!second.Advance("failed") && !second.RecoverCrash(), "exhausted chain cannot loop");
    RecoveryState remembered(dir);
    Check(remembered.Load() && remembered.selected == "failed" && remembered.preferred == preferred,
          "remember fallback and preference");
    Check(remembered.Retry() && remembered.selected == preferred && remembered.reason.empty(), "try native again");
    RecoveryState retry(dir);
    Check(retry.Load() && retry.retry_requested && retry.selected == preferred, "retry survives immediate launcher close");
    Check(remembered.Marker(preferred) && remembered.Marker() && !remembered.RecoverCrash(),
          "chapter or clean close clears marker");
    Check(remembered.Advance("failed initialisation"), "known init failure advances immediately");
#if defined(__linux__)
    Check(remembered.selected == "failed", "init failure ends the one API chain");
#else
    Check(remembered.selected == (preferred == "vulkan" ? "d3d12" : "vulkan"), "symmetric init chain");
#endif
    Check(!remembered.notice_shown, "new fallback needs a notice");
    remembered.notice_shown = true;
    Check(remembered.Save(), "notice shown saved");
    RecoveryState noticed(dir);
    Check(noticed.Load() && noticed.notice_shown && !noticed.reason.empty(), "notice shown once survives");
    Check(noticed.Clear() && noticed.reason.empty() && !noticed.retry_requested && !noticed.notice_shown,
          "manual choice clears fallback");
    RecoveryState cleared(dir);
    Check(!cleared.Load(), "manual choice removes the state file");
    std::string save;
    { std::ifstream in(dir / "saves" / "sentinel"); std::getline(in, save); }
    Check(save == "save must survive", "save unchanged");
  }
  RecoveryState legacy(root / "legacy");
  std::filesystem::create_directories(legacy.directory);
  { std::ofstream out(legacy.directory / "fallback.state"); out << "d3d12\nclassic\n1\nold fallback\n1\n1\n"; }
  Check(legacy.Marker("d3d12"), "old marker created");
  Check(!legacy.Load() && legacy.selected.empty() && !legacy.RecoverCrash(), "old Classic fallback and marker reset");
  Check(legacy.preferred == "vulkan", "legacy migration uses Vulkan default");
  RecoveryState broken(root / "corrupt");
  std::filesystem::create_directories(broken.directory);
  { std::ofstream out(broken.directory / "fallback.state"); out << "vulkan\nclassic\n999\ninvalid\n"; }
  Check(!broken.Load(), "invalid state rejected");
  std::cout << checks << " startup recovery checks passed\n";
}
