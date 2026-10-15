#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
namespace rex::ui { class ImGuiDrawer; }
namespace kk::benchmark {
bool Requested();
bool Active();
// Finish an owned save before either normal teardown or the SDK hard exit.
void Shutdown();
// Call before runtime setup. Returns an empty directory for the benchmark's
// private saves. Results go under the caller's user root / benchmarks.
std::filesystem::path Prepare(const std::filesystem::path& user_root);
void InstallPresentObserver();
void LevelLoaded();
void Frame(double unscaled_dt);
void GuestSwap();
bool Pad(uint32_t user, uint8_t* state);
void Interrupted();
void CreateBenchmarkDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> quit);
}  // namespace kk::benchmark
