#pragma once
#include <filesystem>
namespace rex::ui { struct RawImage; }
namespace kk::art::detail {
bool SaveCapturePng(const rex::ui::RawImage& image, const std::filesystem::path& path);
}
