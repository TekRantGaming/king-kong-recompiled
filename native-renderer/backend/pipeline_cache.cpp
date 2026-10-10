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
  return h;
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

  bool keep = false;
  size_t good_bytes = sizeof(Header);
  if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
    Header header;
    Header want;
    if (std::fread(&header, sizeof(header), 1, in) != 1) {
      note_ = "empty or truncated header";
    } else if (std::memcmp(&header, &want, sizeof(header)) != 0) {
      note_ = "another version, record size or byte order";
    } else {
      keep = true;
      PipelineRecord record;
      while (std::fread(&record, sizeof(record), 1, in) == 1) {
        good_bytes += sizeof(record);
        if (keys_.insert(PipelineKey(record)).second) loaded_.push_back(record);
      }
    }
    std::fclose(in);
  }
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
