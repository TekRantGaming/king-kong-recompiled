#include "backend/pipeline_cache.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace nr {

namespace {

struct Header {
  uint32_t magic = kPipelineCacheMagic;
  uint32_t version = kPipelineCacheVersion;
  uint32_t record_size = sizeof(PipelineRecord);
  uint32_t byte_order = 0x01020304u;
};

uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 33);
}

}  // namespace

uint64_t PipelineKey(const PipelineRecord& r) {
  uint64_t h = Mix(0x6B6B706Cull, r.vs_hash);
  h = Mix(h, r.ps_hash);
  h = Mix(h, (uint64_t(r.topology) << 32) | r.color_count);
  for (uint32_t i = 0; i < 4; ++i) {
    h = Mix(h, (uint64_t(r.color_format[i]) << 32) | r.color_control[i]);
    h = Mix(h, (uint64_t(r.color_mask[i]) << 32) | r.color_index[i]);
  }
  h = Mix(h, (uint64_t(r.depth_format) << 32) | r.depth_control);
  h = Mix(h, (uint64_t(r.ref_mask) << 32) | r.mode_control);
  h = Mix(h, (uint64_t(r.clip_control) << 32) | r.flags);
  h = Mix(h, (uint64_t(r.poly_scale) << 32) | r.poly_offset);
  h = Mix(h, r.vs_input);
  h = Mix(h, r.ps_input);
  return h;
}

uint64_t SimilarPipelineKey(const PipelineRecord& record) {
  PipelineRecord r = record;
  r.flags &= ~kRecordScaled;
  r.vs_input = 0;
  r.ps_input = 0;
  return PipelineKey(r);
}

PipelineRecord MotionPipelineRecord(const PipelineRecord& draw, uint32_t motion_format) {
  PipelineRecord r;
  r.vs_hash = draw.vs_hash;
  r.vs_input = draw.vs_input;
  r.topology = draw.topology;
  r.color_count = 1;
  r.color_format[0] = motion_format;
  r.color_control[0] = 0x00010001;  // ONE, ZERO
  r.color_mask[0] = 3;
  r.depth_format = draw.depth_format;
  r.depth_control = 2u | (2u << 4);  // EQUAL, read only
  r.mode_control = draw.mode_control;
  r.clip_control = draw.clip_control;
  r.poly_scale = draw.poly_scale;
  r.poly_offset = draw.poly_offset;
  r.flags = (draw.flags & (kRecordFlipFrontFace | kRecordTessellation)) | kRecordMotion;
  r.reserved[0] = draw.reserved[0];  // task 276: drawn in the same level
  return r;
}

bool ReadPipelineList(const std::string& path, std::vector<PipelineRecord>& records, std::string* note,
                      size_t* torn_bytes) {
  if (note) note->clear();
  if (torn_bytes) *torn_bytes = 0;
  std::FILE* in = std::fopen(path.c_str(), "rb");
  if (!in) return false;
  Header header;
  const Header want;
  bool ok = false;
  if (std::fread(&header, sizeof(header), 1, in) != 1) {
    if (note) *note = "empty or truncated header";
  } else if (std::memcmp(&header, &want, sizeof(header)) != 0) {
    if (note) *note = "another version, record size or byte order";
  } else {
    ok = true;
    PipelineRecord record;
    size_t got = 0;
    while ((got = std::fread(&record, 1, sizeof(record), in)) == sizeof(record)) records.push_back(record);
    if (torn_bytes) *torn_bytes = got;
  }
  std::fclose(in);
  return ok;
}

bool WritePipelineList(const std::string& path, const std::vector<PipelineRecord>& records, std::string* error) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  const std::string temp = path + ".tmp";
  std::FILE* out = std::fopen(temp.c_str(), "wb");
  if (!out) {
    if (error) *error = "cannot create " + temp;
    return false;
  }
  const Header header;
  bool ok = std::fwrite(&header, sizeof(header), 1, out) == 1;
  if (ok && !records.empty()) ok = std::fwrite(records.data(), sizeof(PipelineRecord), records.size(), out) == records.size();
  ok = std::fflush(out) == 0 && ok;
  ok = std::fclose(out) == 0 && ok;
  if (ok) {
    std::filesystem::rename(temp, p, ec);
    ok = !ec;
  }
  if (!ok) {
    if (error) *error = "cannot write " + path;
    std::filesystem::remove(temp, ec);
  }
  return ok;
}

namespace {

// Task 276: one more vote for `tag`; returns the most voted tag (a tie: the first voted for).
uint32_t Vote(std::vector<std::pair<uint32_t, uint32_t>>& votes, uint32_t tag) {
  auto vote = std::find_if(votes.begin(), votes.end(), [&](const auto& v) { return v.first == tag; });
  if (vote == votes.end()) votes.push_back({tag, 1});
  else ++vote->second;
  auto best = votes.begin();
  for (auto v = votes.begin(); v != votes.end(); ++v)
    if (v->second > best->second) best = v;
  return best->first;
}

}  // namespace

PipelineListMerger::Added PipelineListMerger::AddOne(const PipelineRecord& r) {
  if ((r.flags & kRecordFlipFrontFace) && !keep_debug_) return Added::kDebug;
  if (RecordLevel(r)) Vote(similar_votes_[SimilarPipelineKey(r)], r.reserved[0]);
  const uint64_t key = PipelineKey(r);
  auto known = keys_.find(key);
  if (known == keys_.end()) {
    if (tags_only_ && first_input_done_) return Added::kLeftOut;
    keys_.emplace(key, records_.size());
    records_.push_back(r);
    votes_.emplace_back();
    if (RecordLevel(r)) votes_.back().push_back({r.reserved[0], 1});
    return Added::kNew;
  }
  // Task 276: a duplicate is one more vote for its level; the record carries the most voted (a tie: the
  // first voted for, i.e. the earliest input naming it with a level).
  if (RecordLevel(r)) records_[known->second].reserved[0] = Vote(votes_[known->second], r.reserved[0]);
  return Added::kDuplicate;
}

size_t PipelineListMerger::TagFromSimilar() {
  size_t tagged = 0;
  for (PipelineRecord& r : records_) {
    if (RecordLevel(r)) continue;
    const auto similar = similar_votes_.find(SimilarPipelineKey(r));
    if (similar == similar_votes_.end() || similar->second.empty()) continue;
    auto best = similar->second.begin();
    for (auto v = similar->second.begin(); v != similar->second.end(); ++v)
      if (v->second > best->second) best = v;
    r.reserved[0] = best->first;
    ++tagged;
  }
  return tagged;
}

PipelineListMerger::Input PipelineListMerger::Add(const std::string& path) {
  Input in;
  in.path = path;
  std::vector<PipelineRecord> records;
  in.read = ReadPipelineList(path, records, &in.note, &in.torn_bytes);
  if (!in.read) {
    std::error_code ec;
    if (in.note.empty()) in.note = std::filesystem::exists(path, ec) ? "cannot be read" : "absent";
    return in;
  }
  in.records = records.size();
  for (const PipelineRecord& r : records) {
    if (RecordLevel(r)) ++in.tagged;
    switch (AddOne(r)) {
      case Added::kNew: ++in.added; break;
      case Added::kDuplicate: ++in.duplicates; break;
      case Added::kDebug: ++in.debug; break;
      case Added::kLeftOut: break;
    }
  }
  first_input_done_ = true;
  return in;
}

size_t PipelineListMerger::AddRecords(const std::vector<PipelineRecord>& records) {
  size_t added = 0;
  for (const PipelineRecord& r : records)
    if (AddOne(r) == Added::kNew) ++added;
  first_input_done_ = true;
  return added;
}

bool PipelineCacheFile::Open(const std::string& path) {
  Close();
  loaded_.clear();
  keys_.clear();
  note_.clear();
  appended_ = 0;
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

  std::vector<PipelineRecord> records;
  const bool keep = ReadPipelineList(path, records, &note_);
  for (const PipelineRecord& record : records) {
    if (keys_.insert(PipelineKey(record)).second) loaded_.push_back(record);
  }
  const size_t good_bytes = sizeof(Header) + records.size() * sizeof(PipelineRecord);
  std::FILE* file = nullptr;
  if (keep) {
    // Drop a torn last record (the run was killed mid-write) before appending.
    std::filesystem::resize_file(p, good_bytes, ec);
    file = std::fopen(path.c_str(), "ab");
  } else {
    file = std::fopen(path.c_str(), "wb");
    if (file) {
      Header header;
      std::fwrite(&header, sizeof(header), 1, file);
      std::fflush(file);
    }
  }
  {
    std::lock_guard lock(file_mutex_);
    file_ = file;
  }
  open_.store(file != nullptr);
  return file != nullptr;
}

void PipelineCacheFile::Close() {
  Flush();
  std::lock_guard lock(file_mutex_);
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
  open_.store(false);
}

bool PipelineCacheFile::Append(const PipelineRecord& record) {
  if (!keys_.insert(PipelineKey(record)).second) return false;
  if (open_.load(std::memory_order_relaxed)) {
    std::lock_guard lock(pending_mutex_);
    pending_.push_back(record);
    pending_count_.store(pending_.size(), std::memory_order_relaxed);
    ++appended_;
  }
  return true;
}

void PipelineCacheFile::Flush() {
  std::lock_guard file_lock(file_mutex_);  // one writer at a time keeps the records in order
  std::vector<PipelineRecord> records;
  {
    std::lock_guard lock(pending_mutex_);
    records.swap(pending_);
    pending_count_.store(0, std::memory_order_relaxed);
  }
  if (!file_ || records.empty()) return;
  std::fwrite(records.data(), sizeof(PipelineRecord), records.size(), file_);
  std::fflush(file_);
}

}  // namespace nr
