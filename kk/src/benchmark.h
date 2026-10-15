#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
namespace rex::ui { class ImGuiDrawer; }
namespace kk::benchmark {
bool Requested();
bool Active();
bool RecordingRoute();
std::filesystem::path RouteFile();
bool RouteAvailable();
// Final state/status returned to the game, after remapping and developer input.
void PadRead(uint32_t user, const uint8_t* state, uint32_t status, const uint8_t* base);
// Game thread only, before uni_go services a load. -1 means no route jump.
int NextChapter();
// Finish an owned save before either normal teardown or the SDK hard exit.
void Shutdown();
// Call before runtime setup. Returns an empty directory for the benchmark's
// private saves. Results go under the caller's user root / benchmarks.
std::filesystem::path Prepare(const std::filesystem::path& user_root);
void InstallPresentObserver();
void LevelLoaded();
void Frame(double unscaled_dt, double scaled_dt, const uint8_t* base);
void GuestSwap();
bool Pad(uint32_t user, uint8_t* state, uint32_t& status);
void Interrupted();
void CreateBenchmarkStartDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> start);
void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit);
}  // namespace kk::benchmark
