// Chapter-start hints learned locally from real binds, replayed only while the
// loading screen is up. All cache publication stays on the render thread.
#include "backend/game_renderer.h"
#include "backend/upload_batch.h"
#include "backend/log.h"

namespace nr {

bool GameRenderer::LoadResourceList(const std::string& path) {
  std::vector<ResourcePreloadChapter> chapters;
  const bool loaded = ResourcePreloadBundle::Read(path, chapters);
  size_t count = 0;
  for (const auto& chapter : chapters) count += chapter.records.size();
  std::lock_guard lock(cache_mutex_);
  shipped_resources_ = std::move(chapters);
  Logf(LogLevel::kInfo, "rexgpu-native: shipped resource list %s: %zu chapters, %zu descriptions%s",
       path.c_str(), shipped_resources_.size(), count, loaded ? "" : " (absent or incompatible; using local learning)");
  return loaded;
}

void GameRenderer::PollResourcePreload(nvrhi::ICommandList* cl) {
  if (resource_poll_frame_ == frame_) return;
  resource_poll_frame_ = frame_;
  // loading_fix.cpp and play_session.cpp: the loading thread, its state, and
  // the universe's requested map/start. Never change these guest variables.
  const auto* screen = memory_.Virtual(0x82D64080);
  const auto* ai_ptr = memory_.Virtual(0x82CCBF64);
  if (!screen || !ai_ptr) return;
  const uint32_t ai = LoadBE32(ai_ptr);
  const auto* variables = ai ? memory_.Virtual(ai + 0x40) : nullptr;
  const uint32_t g = variables ? LoadBE32(variables) : 0;
  const auto* level = g ? memory_.Virtual(g + 8) : nullptr;
  const auto* start = g ? memory_.Virtual(g + 12) : nullptr;
  if (!level || !start) return;
  const auto* state = memory_.Virtual(0x82D6408C);
  if (!state) return;
  const uint32_t s = LoadBE32(state);
  PreloadResources(cl, LoadBE32(screen) != 0 && (s == 1 || s == 2), LoadBE32(level), LoadBE32(start));
}

void GameRenderer::SaveResourcePreload() {
  if (!resource_cache_dirty_ || resource_cache_path_.empty()) return;
  // Task 177: the render thread only encodes the list (a copy); the background writer replaces the file
  // (temp file + rename), so a busy disk never holds a frame. A reader of the same path before the write
  // is done gets these bytes from the writer (PreloadResources).
  const auto start = std::chrono::steady_clock::now();
  std::vector<uint8_t> bytes;
  if (!ResourcePreloadFile::Encode(learned_resources_, bytes)) return;
  resource_cache_dirty_ = false;
  const uint32_t level = preload_level_, chapter_start = preload_start_;
  const size_t count = learned_resources_.size();
  const std::string path = resource_cache_path_;
  file_writer_.Replace(path, std::move(bytes), [path, level, chapter_start, count, start](bool ok, double ms) {
    const double queued_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() - ms;
    if (ok) {
      Logf(LogLevel::kInfo,
           "rexgpu-native: resource list saved: level %u start %u, %zu descriptions (written in %.1f ms on the file "
           "writer, started %.1f ms after it was queued at steady %.3f s)",
           level, chapter_start, count, ms, queued_ms,
           std::chrono::duration<double>(start.time_since_epoch()).count());
    } else {
      Logf(LogLevel::kWarning, "rexgpu-native: resource list %s not saved (level %u start %u, %zu descriptions)",
           path.c_str(), level, chapter_start, count);
    }
  });
  const double render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  stats_.resource_save_render_ms += render_ms;
  if (render_ms > 2) {
    Logf(LogLevel::kInfo, "rexgpu-native: resource list save took %.1f ms on the render thread", render_ms);
  }
}

void GameRenderer::RememberResource(ResourcePreloadRecord record, const kknr::ResidentEntry& entry) {
  if (!resource_learning_ || entry.gpu_written || entry.dirty || !entry.host ||
      uint64_t(entry.ranges[0].bytes) + entry.ranges[1].bytes < UploadBatch::kMinBytes ||
      !resource_seen_.insert(&entry).second) return;
  record.hash = entry.content_hash;
  for (auto& known : learned_resources_) if (known.SameResource(record)) {
    if (known.hash != record.hash) { known.hash = record.hash; resource_cache_dirty_ = true; }
    return;
  }
  if (learned_resources_.size() < ResourcePreloadFile::kMaxRecords) {
    learned_resources_.push_back(record);
    resource_cache_dirty_ = true;
  }
}

void GameRenderer::PreloadResources(nvrhi::ICommandList* cl, bool loading, uint32_t level, uint32_t start) {
  const auto now = std::chrono::steady_clock::now();
  const bool changed = level != preload_level_ || start != preload_start_;
  if (loading && (!resource_loading_ || changed)) {
    SaveResourcePreload();
    preload_level_ = level; preload_start_ = start;
    preload_cursor_ = 0; preload_reserved_bytes_ = 0;
    resource_learning_ = false;
    resource_cache_dirty_ = false;
    learned_resources_.clear(); preload_records_.clear();
    resource_seen_.clear();
    {
      std::lock_guard lock(cache_mutex_);
      resource_cache_path_ = resource_cache_base_.empty() ? std::string{} : resource_cache_base_ +
          ".resources-" + std::to_string(level) + "-" + std::to_string(start);
    }
    // Keep earlier local learning when extending it. Local content takes polling
    // priority; shipped variants are hints and never overwrite the player's file.
    // A write of this list still queued (a quick return to the same chapter) is what the file will hold.
    std::vector<uint8_t> pending;
    if (!resource_cache_path_.empty()) {
      if (file_writer_.Pending(resource_cache_path_, &pending)) ResourcePreloadFile::Decode(pending, learned_resources_);
      else ResourcePreloadFile::Read(resource_cache_path_, learned_resources_);
    }
    preload_records_ = learned_resources_;
    const size_t local = preload_records_.size();
    {
      std::lock_guard lock(cache_mutex_);
      for (const auto& chapter : shipped_resources_) if (chapter.level == level && chapter.start == start) {
        for (const auto& r : chapter.records) {
          if (preload_records_.size() >= ResourcePreloadFile::kMaxRecords) break;
          const bool duplicate = std::any_of(preload_records_.begin(), preload_records_.end(), [&](const auto& known) {
            return known.SameResource(r) && known.hash == r.hash;
          });
          if (!duplicate) preload_records_.push_back(r);
        }
        break;
      }
    }
    Logf(LogLevel::kInfo, "rexgpu-native: resource loading: level %u start %u, %zu descriptions (%zu local, %zu shipped)",
         level, start, preload_records_.size(), local, preload_records_.size() - local);
  } else if (!loading && resource_loading_) {
    // The universe can clear/change its request fields as loading ends. Keep
    // the map/start captured while that request's loading screen was up.
    resource_learning_ = !resource_cache_path_.empty();
    resource_learn_start_ = now;
    Logf(LogLevel::kInfo, "rexgpu-native: resource loading ended: level %u start %u, %llu resources, %llu bytes (run totals)",
         preload_level_, preload_start_, static_cast<unsigned long long>(stats_.preloaded_resources),
         static_cast<unsigned long long>(stats_.preload_bytes));
  }
  resource_loading_ = loading;
  UpdateEffects();
  ServiceTexturePacks(cl);
  if (resource_learning_ && now - resource_learn_start_ >= std::chrono::seconds(10)) {
    resource_learning_ = false;
    SaveResourcePreload();
  }
  if (!loading || preload_records_.empty() || preload_reserved_bytes_ >= kChapterPreloadBytes) return;

  ProcessInvalidations();
  FlushUploads();
  // Limit polling/copying/hashing as well as successful uploads. A mismatch is
  // retried on a later loading frame, so bytes still being loaded can become a
  // candidate without a write-watch callback doing work in a fault handler.
  std::vector<ResourcePreloadRecord> requests;
  uint64_t scanned = 0;
  for (size_t n = 0; n < preload_records_.size() && requests.size() < 32; ++n) {
    const auto& r = preload_records_[preload_cursor_];
    uint64_t bytes = 0;
    if (r.kind == ResourcePreloadRecord::Texture) {
      const auto fetch = kknr::TextureFetch::FromWords(r.words.data());
      const auto ranges = kknr::GetTextureRanges(fetch, TextureOptions());
      bytes = ((uint64_t(ranges.base_bytes) + 4095) & ~uint64_t(4095)) +
              ((uint64_t(ranges.mip_bytes) + 4095) & ~uint64_t(4095));
    } else bytes = (uint64_t(r.words[1]) + 4095) & ~uint64_t(4095);
    if (bytes <= UploadBatch::kFrameBytes && bytes > UploadBatch::kFrameBytes - scanned) break;
    preload_cursor_ = (preload_cursor_ + 1) % preload_records_.size();
    if (!bytes || bytes > UploadBatch::kFrameBytes) continue;
    scanned += bytes;
    requests.push_back(r);
  }
  PrepareResourceRequests(cl, requests, true);
}

}  // namespace nr
