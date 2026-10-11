#include "backend/pipeline_cache.h"

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
  if (keep) {
    // Drop a torn last record (the run was killed mid-write) before appending.
    std::filesystem::resize_file(p, good_bytes, ec);
    file_ = std::fopen(path.c_str(), "ab");
  } else {
    file_ = std::fopen(path.c_str(), "wb");
    if (file_) {
      Header header;
      std::fwrite(&header, sizeof(header), 1, file_);
      std::fflush(file_);
    }
  }
  return file_ != nullptr;
}

void PipelineCacheFile::Close() {
  if (file_) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool PipelineCacheFile::Append(const PipelineRecord& record) {
  if (!keys_.insert(PipelineKey(record)).second) return false;
  if (file_ && std::fwrite(&record, sizeof(record), 1, file_) == 1) ++appended_;
  return true;
}

void PipelineCacheFile::Flush() {
  if (file_) std::fflush(file_);
}

}  // namespace nr
