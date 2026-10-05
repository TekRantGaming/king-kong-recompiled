// Xbox 360 disc image (XDVDFS) reader, used by the launcher's installer.
// Handles plain XISO and XGD2/XGD3 "redump" images. Port of tools/XisoExtract.cs.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kk::iso {

struct Progress {
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<bool> cancel{false};
};

// Title ID from the image's default.xex (execution info header), or 0 if the
// file is not an Xbox disc image or has no readable default.xex.
uint32_t ReadTitleId(const std::filesystem::path& image);

// Extracts every file on the disc into `out_dir`. Returns an empty string on
// success, otherwise an error message.
std::string Extract(const std::filesystem::path& image, const std::filesystem::path& out_dir,
                    Progress* progress = nullptr);

}  // namespace kk::iso
