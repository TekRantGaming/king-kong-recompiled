// Chapter-start hints learned locally from real binds, replayed only while the
// loading screen is up. All cache publication stays on the render thread.
#include "backend/game_renderer.h"
#include "backend/upload_batch.h"
#include "backend/log.h"
#include "plugin/native_benchmark_stats.h"

namespace nr {

void GameRenderer::IdleTexturePreload() {
  if (!resource_loading_) return;
  const auto* screen = memory_.Virtual(0x82D64080);
  const auto* state = memory_.Virtual(0x82D6408C);
  ++chapter_texture_idle_;
  chapter_texture_screen_ = screen ? LoadBE32(screen) : UINT32_MAX;
  chapter_texture_state_ = state ? LoadBE32(state) : UINT32_MAX;
  // The original loader clears these guest flags before its wait for the clip
  // ends. resource_loading_ remains true until the first real game frame polls
  // the request again; use that lifetime for the idle clip preparation too.
  PrepareChapterTextures();
}

void GameRenderer::PrepareChapterTextures() {
  // Emergency comparison switch, read once. It never affects ordinary binds.
  static const bool enabled = [] {
    const char* value = std::getenv("KK341_CONTENT_PRELOAD");
    return !value || *value != '0';
  }();
  if (!enabled || !resource_loading_ || !options_.upload_submit || pack_pressure_) return;
  const MemoryBudget budget = options_.memory_budget ? options_.memory_budget() : MemoryBudget{};
  if (MemoryPressure(budget) || (budget.valid &&
      (budget.usage >= budget.budget || budget.budget - budget.usage <
       MemoryBudgetReserve(budget.budget) + ChapterTexturePrepare::kMaxBytes))) return;
  // Do not reuse staging storage while the previous upload is in flight, and
  // never wait for the GPU or an unfinished CPU job.
  if (chapter_texture_submitted_) {
    if (!device_->pollEventQuery(chapter_texture_query_)) return;
    chapter_texture_submitted_ = false;
  }
  if (chapter_textures_) if (auto job = chapter_textures_->Take()) {
    if (job->matched) ++chapter_texture_matches_;
    ProcessInvalidations();
    auto* entry = textures_.Find(job->fetch);
    bool stale = job->generation != chapter_texture_generation_ || !entry || entry->host ||
                 entry->gpu_written || !entry->watched;
    {
      std::lock_guard lock(invalidation_mutex_);
      for (const auto& [start, bytes] : invalidations_)
        for (const auto& range : job->snapshot.original) if (range.Overlaps(start, bytes)) stale = true;
    }
    const auto alias = resolved_by_base_.find(job->fetch.BaseAddress());
    if (alias != resolved_by_base_.end() && alias->second->gpu_written) stale = true;
    if (stale) ++chapter_texture_stale_;
    if (!stale && job->converted && job->allocation_bytes <= ChapterPreloadBytes() -
        std::min(preload_reserved_bytes_, ChapterPreloadBytes())) {
      if (!chapter_texture_list_) {
        nvrhi::CommandListParameters params;
        params.enableImmediateExecution = false;
        chapter_texture_list_ = device_->createCommandList(params);
        chapter_texture_query_ = device_->createEventQuery();
      }
      if (chapter_texture_list_ && chapter_texture_query_) {
        auto* host = CreateHostTexture(job->data.plan, false);
        if (host) {
          chapter_texture_list_->open();
          for (const auto& sub : job->data.subresources)
            chapter_texture_list_->writeTexture(host->texture, sub.layer, sub.level,
                job->data.bytes.data() + sub.offset, sub.row_pitch, sub.depth_pitch);
          ZeroUnwrittenSubresources(chapter_texture_list_, host->texture, job->data);
          chapter_texture_list_->setTextureState(host->texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
          chapter_texture_list_->close();
          options_.upload_submit(chapter_texture_list_);
          device_->resetEventQuery(chapter_texture_query_);
          device_->setEventQuery(chapter_texture_query_, nvrhi::CommandQueue::Graphics);
          chapter_texture_submitted_ = true;
          host->preloaded = true;
          host->pack_used_frame = frame_;
          host->pack_hash = kknr::TextureBaseHash(job->data);
          if (host->pack_hash) {
            pack_originals_.insert(host);
            pack_scan_ = true;
            if (texture_pack_stream_) texture_pack_stream_->Request(host->pack_hash, host->plan);
          }
          entry->host = host;
          textures_.OnUploaded(*entry, job->hash);
          ++stats_.texture_uploads;
          ++stats_.preloaded_resources;
          ++stats_.prepared_resources;
          stats_.upload_bytes += job->data.bytes.size();
          stats_.preload_bytes += job->data.bytes.size();
          preload_reserved_bytes_ += job->allocation_bytes;
          ++chapter_texture_uploads_;
          // The developer sampler must see uploads made while the game's frame
          // counter is parked under the clip, so they are excluded from play.
          if (benchmark_stats_enabled.load(std::memory_order_relaxed)) {
            std::lock_guard lock(benchmark_stats_mutex);
            benchmark_stats.texture_uploads = stats_.texture_uploads;
            benchmark_stats.upload_bytes = stats_.upload_bytes;
          }
          // The content identity selects the hint. Addresses only locate this
          // allocation; the ordinary cache and write watches remain authoritative.
          for (auto& record : preload_records_) if (record.kind == ResourcePreloadRecord::Texture &&
              record.hash == job->hash && ChapterTextureLayout(kknr::TextureFetch::FromWords(record.words.data())) ==
                                         ChapterTextureLayout(job->fetch)) {
            record = ResourcePreloadRecord::Tex(job->fetch);
            record.hash = job->hash;
          }
        }
      }
    }
  }
  auto incoming = TextureCreationInbox().Take();
  for (const auto object : incoming) {
    if (std::find(chapter_candidates_.begin(), chapter_candidates_.end(), object) != chapter_candidates_.end()) continue;
    if (chapter_candidates_.size() < 4096) chapter_candidates_.push_back(object);
    else {  // Recent creations must not be crowded out by old gameplay notifications.
      chapter_candidate_cursor_ %= chapter_candidates_.size();
      chapter_candidates_[chapter_candidate_cursor_++] = object;
    }
  }
  if (chapter_candidates_.empty() || preload_records_.empty() ||
      preload_reserved_bytes_ >= ChapterPreloadBytes() || (chapter_textures_ && chapter_textures_->Busy())) return;
  // CPU snapshots and decoded output have separate hard bounds. One candidate
  // per service call; unrelated headers are passed over without hashing.
  for (size_t i = 0; i < chapter_candidates_.size(); ++i) {
    if (chapter_candidate_cursor_ >= chapter_candidates_.size()) chapter_candidate_cursor_ = 0;
    const auto object = chapter_candidates_[chapter_candidate_cursor_++];
    uint8_t header[40];
    if (!memory_.CopyVirtual(object, header, sizeof(header)) || ((LoadBE32(header) >> 16) & 7) != 3) continue;
    auto fetch = kknr::TextureFetch::FromGuest(header + 16);
    fetch.words[1] = (fetch.words[1] & 0xFFF) | fetch.BaseAddress();
    fetch.words[5] = (fetch.words[5] & 0xFFF) | fetch.MipAddress();
    if (fetch.Type() != 2) continue;
    const auto layout = ChapterTextureLayout(fetch);
    auto job = std::make_unique<ChapterTexturePrepare::Job>();
    for (const auto& record : preload_records_) if (record.kind == ResourcePreloadRecord::Texture &&
        ChapterTextureLayout(kknr::TextureFetch::FromWords(record.words.data())) == layout)
      job->expected.push_back(record.hash);
    if (job->expected.empty()) continue;
    const auto* known = textures_.Find(fetch);
    if (known && (known->host || known->gpu_written)) continue;
    const auto alias = resolved_by_base_.find(fetch.BaseAddress());
    if (alias != resolved_by_base_.end() && alias->second->gpu_written) continue;
    kknr::HostTexturePlan plan;
    const auto options = TextureOptions();
    if (!kknr::PlanHostTexture(fetch, plan, nullptr, options)) continue;
    const auto& format = kknr::GetHostFormatInfo(plan.format);
    uint64_t output = 0;
    // Count every allocated mip, including levels the conversion leaves zero.
    for (uint32_t level = 0; level < plan.levels; ++level)
      output += ((uint64_t(std::max(plan.width >> level, 1u)) + format.block_size - 1) / format.block_size) *
                ((uint64_t(std::max(plan.height >> level, 1u)) + format.block_size - 1) / format.block_size) *
                format.bytes_per_block * std::max(plan.depth >> level, 1u) * plan.layers;
    if (output < UploadBatch::kMinBytes || output > ChapterTexturePrepare::kMaxBytes ||
        output > ChapterPreloadBytes() - preload_reserved_bytes_) continue;
    const auto ranges = kknr::GetTextureRanges(fetch, options);
    kknr::GuestRange source[2] = {{ranges.base, ranges.base_bytes}, {ranges.mip, ranges.mip_bytes}};
    if ((source[0].bytes && !physical_.At(source[0].start, source[0].bytes)) ||
        (source[1].bytes && !physical_.At(source[1].start, source[1].bytes))) continue;
    ProcessInvalidations();
    textures_.options = options;
    auto* entry = textures_.Bind(fetch, physical_, frame_).entry;
    if (!entry || entry->host || entry->gpu_written) continue;
    textures_.EnsureWatched(*entry);
    job->fetch = fetch;
    job->options = options;
    job->generation = chapter_texture_generation_;
    job->allocation_bytes = output;
    if (!job->snapshot.Capture(source, physical_, job->source, ChapterTexturePrepare::kMaxBytes)) continue;
    if (!chapter_textures_) chapter_textures_ = std::make_unique<ChapterTexturePrepare>();
    chapter_textures_->Start(std::move(job));
    ++chapter_texture_attempts_;
    break;
  }
}

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
    ++chapter_texture_generation_;
    chapter_texture_uploads_ = 0;
    chapter_texture_attempts_ = chapter_texture_matches_ = chapter_texture_stale_ = chapter_texture_idle_ = 0;
    chapter_candidates_.clear();
    chapter_candidate_cursor_ = 0;
    // A different chapter: the previous one's unused pack replacements are freed after this load.
    if (changed) TexturePackLoadStarted();
    PipelineLevelLoading(level);  // task 276: level tags, its pipelines first, all workers while loading
    SaveResourcePreload();
    preload_level_ = level; preload_start_ = start;
    preload_cursor_ = 0; preload_reserved_bytes_ = 0;
    resource_learning_ = false;
    resource_trickle_ = false;
    start_uploads_ = {};
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
    Logf(LogLevel::kInfo, "rexgpu-native: chapter content preparation: level %u, %llu textures uploaded before play, %zu creation headers",
         preload_level_, static_cast<unsigned long long>(chapter_texture_uploads_), chapter_candidates_.size());
    Logf(LogLevel::kInfo, "rexgpu-native: chapter content probe: attempts %llu matches %llu stale %llu idle %llu last screen %u state %u",
         (unsigned long long)chapter_texture_attempts_, (unsigned long long)chapter_texture_matches_,
         (unsigned long long)chapter_texture_stale_, (unsigned long long)chapter_texture_idle_, chapter_texture_screen_, chapter_texture_state_);
    TexturePackLoadEnded();
    PipelineLevelPlaying();  // task 276: fewer background workers while playing (native_pipeline_play_workers)
    // The universe can clear/change its request fields as loading ends. Keep
    // the map/start captured while that request's loading screen was up.
    resource_learning_ = !resource_cache_path_.empty();
    resource_learn_start_ = now;
    // Task 280: how much of the chapter's list is on the GPU when its first gameplay frame starts.
    size_t textures = 0, resident = 0;
    uint64_t missing_bytes = 0;
    for (const auto& r : preload_records_) {
      if (r.kind != ResourcePreloadRecord::Texture) continue;
      ++textures;
      const auto fetch = kknr::TextureFetch::FromWords(r.words.data());
      const auto* known = textures_.Find(fetch);
      if (known && known->host) { ++resident; continue; }
      const auto ranges = kknr::GetTextureRanges(fetch, TextureOptions());
      missing_bytes += uint64_t(ranges.base_bytes) + ranges.mip_bytes;
    }
    resource_trickle_ = options_.chapter_trickle_mib > 0 && !preload_records_.empty() &&
                        preload_reserved_bytes_ < ChapterPreloadBytes();
    resource_trickle_until_ = now + std::chrono::seconds(10);
    resource_sweep_ = resource_trickle_;
    start_uploads_ = {};
    start_uploads_.frames_left = 240;
    start_uploads_.texture_ms = stats_.texture_ms;
    start_uploads_.prepare_ms = stats_.resource_prepare_ms;
    start_uploads_.trickled = stats_.trickled_resources;
    start_uploads_.trickle_bytes = stats_.trickle_bytes;
    Logf(LogLevel::kInfo, "rexgpu-native: resource loading ended: level %u start %u, %llu resources, %llu bytes (run totals); "
         "this chapter: %.1f of %.0f MiB prepared, listed textures resident %zu of %zu (%.1f MiB guest bytes missing)%s",
         preload_level_, preload_start_, static_cast<unsigned long long>(stats_.preloaded_resources),
         static_cast<unsigned long long>(stats_.preload_bytes), double(preload_reserved_bytes_) / 1048576.0,
         double(ChapterPreloadBytes()) / 1048576.0, resident, textures, double(missing_bytes) / 1048576.0,
         resource_trickle_ ? "; trickle on" : "");
  }
  resource_loading_ = loading;
  UpdateEffects();
  ServiceTexturePacks(cl);
  if (loading) PrepareChapterTextures();
  if (resource_learning_ && now - resource_learn_start_ >= std::chrono::seconds(10)) {
    resource_learning_ = false;
    SaveResourcePreload();
  }
  if (resource_trickle_ && (loading || now >= resource_trickle_until_ ||
                            preload_reserved_bytes_ >= ChapterPreloadBytes())) resource_trickle_ = false;
  const bool trickle = !loading && resource_trickle_;
  if ((!loading && !trickle) || preload_records_.empty() || preload_reserved_bytes_ >= ChapterPreloadBytes()) return;

  ProcessInvalidations();
  FlushUploads();
  // Limit polling/copying/hashing as well as successful uploads. A mismatch is
  // retried on a later loading frame, so bytes still being loaded can become a
  // candidate without a write-watch callback doing work in a fault handler.
  // Task 280: resources already on the GPU are passed over without using the allowance (a lookup only).
  // Returns how many listed resources are still missing; requests gets the next ones that fit.
  auto collect = [&](uint64_t frame_bytes, size_t frame_requests, std::vector<ResourcePreloadRecord>& requests) {
    uint64_t scanned = 0;
    size_t missing = 0;
    for (size_t n = 0; n < preload_records_.size() && requests.size() < frame_requests; ++n) {
      const auto& r = preload_records_[preload_cursor_];
      uint64_t bytes = 0;
      bool resident = false;
      if (r.kind == ResourcePreloadRecord::Texture) {
        const auto fetch = kknr::TextureFetch::FromWords(r.words.data());
        const auto* known = textures_.Find(fetch);
        resident = known && (known->host || known->gpu_written);
        const auto ranges = kknr::GetTextureRanges(fetch, TextureOptions());
        bytes = ((uint64_t(ranges.base_bytes) + 4095) & ~uint64_t(4095)) +
                ((uint64_t(ranges.mip_bytes) + 4095) & ~uint64_t(4095));
      } else {
        const bool index = r.kind != ResourcePreloadRecord::Vertex;
        const auto* known = (index ? index_buffers_ : vertex_buffers_)
            .Find(r.words[0], r.words[1], index ? kknr::IndexConversionId(r.kind == ResourcePreloadRecord::Index32) : 0);
        resident = known && known->host;
        bytes = (uint64_t(r.words[1]) + 4095) & ~uint64_t(4095);
      }
      if (resident) {
        preload_cursor_ = (preload_cursor_ + 1) % preload_records_.size();
        continue;
      }
      ++missing;
      if (bytes <= frame_bytes && bytes > frame_bytes - scanned) break;
      preload_cursor_ = (preload_cursor_ + 1) % preload_records_.size();
      if (!bytes || bytes > frame_bytes) continue;
      scanned += bytes;
      requests.push_back(r);
    }
    return missing;
  };
  std::vector<ResourcePreloadRecord> requests;
  if (!trickle) {
    collect(UploadBatch::kFrameBytes, 32, requests);
    PrepareResourceRequests(cl, requests, true);
    return;
  }
  const uint64_t before = stats_.preload_bytes, resources = stats_.preloaded_resources;
  if (resource_sweep_) {
    // The first gameplay frame: the game has finished writing the chapter, so what the loading screen
    // could not match yet is prepared now in full batches (all workers at once) before this frame's
    // draws, instead of draw by draw. Bounded by time, the chapter cap and one pass over the list.
    resource_sweep_ = false;
    const auto sweep_start = std::chrono::steady_clock::now();
    for (size_t pass = 0; pass <= preload_records_.size() / 32 + 1; ++pass) {
      requests.clear();
      if (!collect(UploadBatch::kFrameBytes, 32, requests) || requests.empty()) break;
      const uint64_t done = stats_.preloaded_resources + stats_.prepared_discarded + stats_.preload_hash_misses;
      PrepareResourceRequests(cl, requests, true);
      FlushUploads();
      if (stats_.preloaded_resources + stats_.prepared_discarded + stats_.preload_hash_misses == done ||
          preload_reserved_bytes_ >= ChapterPreloadBytes() ||
          std::chrono::steady_clock::now() - sweep_start >= std::chrono::milliseconds(kChapterSweepMs)) break;
    }
  } else {
    const size_t missing = collect(std::min<uint64_t>(uint64_t(options_.chapter_trickle_mib) << 20,
                                                      UploadBatch::kFrameBytes), 8, requests);
    if (!missing) {
      resource_trickle_ = false;  // everything listed is on the GPU (or larger than the allowance)
      return;
    }
    PrepareResourceRequests(cl, requests, true);
  }
  stats_.trickled_resources += stats_.preloaded_resources - resources;
  stats_.trickle_bytes += stats_.preload_bytes - before;
}

uint64_t GameRenderer::ChapterPreloadBytes() const {
  if (pack_pressure_) return kChapterPreloadBytes;
  return std::max<uint64_t>(kChapterPreloadBytes, uint64_t(options_.chapter_preload_mib) << 20);
}

void GameRenderer::NoteStartUpload(const kknr::TextureFetch& fetch, uint64_t bytes, bool prepared) {
  if (!start_uploads_.frames_left || resource_loading_) return;
  ++start_uploads_.textures;
  start_uploads_.bytes += bytes;
  if (prepared) ++start_uploads_.prepared;
  const auto record = ResourcePreloadRecord::Tex(fetch);
  for (const auto& r : preload_records_) if (r.SameResource(record)) {
    ++start_uploads_.listed;
    start_uploads_.listed_bytes += bytes;
    break;
  }
}

void GameRenderer::EndStartUploadFrame() {
  StartUploads& s = start_uploads_;
  if (!s.frames_left || resource_loading_) return;
  --s.frames_left;
  ++s.frame;
  const double tex = stats_.texture_ms - s.texture_ms, prep = stats_.resource_prepare_ms - s.prepare_ms;
  const uint64_t trickled = stats_.trickled_resources - s.trickled, trickle_bytes = stats_.trickle_bytes - s.trickle_bytes;
  if (s.textures || trickled || tex + prep >= 2.0) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: chapter start frame %u: %u textures uploaded at their draw (%u batched), %.2f MiB, "
         "%u of them listed (%.2f MiB); %.1f ms textures, %.1f ms prepare; trickle %llu resources %.2f MiB",
         s.frame, s.textures, s.prepared, double(s.bytes) / 1048576.0, s.listed, double(s.listed_bytes) / 1048576.0,
         tex, prep, static_cast<unsigned long long>(trickled), double(trickle_bytes) / 1048576.0);
  }
  s.textures = s.listed = s.prepared = 0;
  s.bytes = s.listed_bytes = 0;
  s.texture_ms = stats_.texture_ms;
  s.prepare_ms = stats_.resource_prepare_ms;
  s.trickled = stats_.trickled_resources;
  s.trickle_bytes = stats_.trickle_bytes;
}

}  // namespace nr
