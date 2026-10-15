#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace nr {
// A failed secondary device is remembered only for this preparation plan and
// driver. Selecting that API always retries it, rather than claiming it ready.
class SetupPrewarmAvailability {
 public:
  SetupPrewarmAvailability(std::filesystem::path pack, std::string stamp, bool selected)
      : marker_(std::move(pack)), stamp_(std::move(stamp)), selected_(selected) {
    marker_ += ".pipelines-unavailable";
    std::ifstream input(marker_, std::ios::binary);
    const std::string previous((std::istreambuf_iterator<char>(input)), {});
    unavailable_ = !stamp_.empty() && previous == stamp_;
  }
  bool Skip() const { return unavailable_ && !selected_; }
  bool Retry() const { return unavailable_ && selected_; }
  // False means setup must fail: the device belongs to the selected API.
  bool DeviceUnavailable() const {
    std::ofstream marker(marker_, std::ios::binary | std::ios::trunc);
    marker << stamp_;
    return !selected_;
  }
  void Prepared() const {
    // Invalidate any old unavailable stamp without deleting cache files.
    if (unavailable_) std::ofstream(marker_, std::ios::binary | std::ios::trunc);
  }
 private:
  std::filesystem::path marker_;
  std::string stamp_;
  bool selected_, unavailable_;
};
}  // namespace nr
