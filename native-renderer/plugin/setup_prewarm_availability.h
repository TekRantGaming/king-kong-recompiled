#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace nr {
// The setup's pipeline step is complete when every pipeline it could queue was made and none failed. A listed
// pipeline whose shaders are not in this player's pack ("waiting for shaders": a shipped or recorded record
// from other game files or an older translator) cannot be made at setup at all; the game makes it when it meets
// those shaders. Counting those as incomplete made every setup report "not prepared" (task 210: the Beta 9
// list has 47 such records), so Home said the shaders were not prepared and Rebuild could never fix it. The
// ambient-only AO's own shaders are the same case.
inline bool SetupPipelinesComplete(bool done, uint64_t queued, uint64_t made, uint64_t failed) {
  return done && made == queued && failed == 0;
}

// A failed secondary device is remembered only for this preparation plan and
// driver. Selecting that API always retries it, rather than claiming it ready.
//
// Task 368 (crash reports G1): a driver can end the whole program while the device is made (Intel's D3D12
// driver in CreateCommandSignature), and then nothing was written, so every launcher start crashed again.
// BeforeDevice writes a crash mark first and DeviceMade clears it. A mark found at the next start means that
// device ended the program: the other API skips it (as a clean failure), and the selected API does not make it
// again either (no crash loop; the game makes its own device at Play, where startup recovery switches the API).
// Both last until the plan or the driver changes (the stamp).
class SetupPrewarmAvailability {
 public:
  SetupPrewarmAvailability(std::filesystem::path pack, std::string stamp, bool selected)
      : marker_(std::move(pack)), stamp_(std::move(stamp)), selected_(selected) {
    marker_ += ".pipelines-unavailable";
    std::ifstream input(marker_, std::ios::binary);
    const std::string previous((std::istreambuf_iterator<char>(input)), {});
    crashed_ = !stamp_.empty() && previous == CrashMark();
    unavailable_ = !stamp_.empty() && (previous == stamp_ || crashed_);
  }
  bool Skip() const { return unavailable_ && !selected_; }
  bool Retry() const { return unavailable_ && selected_ && !crashed_; }
  // The device ended the program at an earlier setup with this plan and driver: do not make it again.
  bool CrashedBefore() const { return crashed_; }
  // Right before the device is made, and right after it was made.
  void BeforeDevice() const {
    std::ofstream marker(marker_, std::ios::binary | std::ios::trunc);
    marker << CrashMark();
  }
  void DeviceMade() const { std::ofstream(marker_, std::ios::binary | std::ios::trunc); }
  // False means setup must fail: the device belongs to the selected API.
  bool DeviceUnavailable() const {
    if (!crashed_) {  // a crash mark stays (a clean stamp would make the selected API retry and crash again)
      std::ofstream marker(marker_, std::ios::binary | std::ios::trunc);
      marker << stamp_;
    }
    return !selected_;
  }
  void Prepared() const {
    // Invalidate any old unavailable stamp without deleting cache files.
    if (unavailable_) std::ofstream(marker_, std::ios::binary | std::ios::trunc);
  }
 private:
  std::string CrashMark() const { return "device crashed " + stamp_; }
  std::filesystem::path marker_;
  std::string stamp_;
  bool selected_, unavailable_, crashed_;
};
}  // namespace nr
