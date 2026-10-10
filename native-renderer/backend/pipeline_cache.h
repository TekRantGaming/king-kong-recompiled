// The game renderer's pipeline descriptions on disk.
//
// A pipeline is made the first time a draw needs it, and the driver's compile takes
// milliseconds to seconds, so the first visit to a chapter stalls (or, with asynchronous
// creation, loses draws). Every description is therefore recorded to a file under the cache
// root the first time it is seen; the next run reads the file at start and creates them all on
// the pipeline worker threads before the game asks for them.
//
// A record is the inputs the renderer builds a pipeline from, not the NVRHI description itself:
// the shaders by the microcode hash the shader library keys them by (the pack, or the
// container, give the shader again), the host attachment formats, and the register values the
// blend, depth, stencil and raster state come from. The same record builds the same
// description in Draw and in the prewarm, and its hash is the pipeline key.
//
// The file is the header below, then records of sizeof(PipelineRecord) bytes, appended as
// they are first seen (nothing is rewritten). A file of another version, record size or
// byte order is ignored and started afresh.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace nr {

struct PipelineRecord {
  uint64_t vs_hash = 0;        // microcode hash of the vertex shader (the shader library's key)
  uint64_t ps_hash = 0;        // of the pixel shader; 0 = none (depth-only)
  uint32_t topology = 0;       // nvrhi::PrimitiveType
  uint32_t color_count = 0;    // attachments, 0-4
  uint32_t color_format[4] = {};   // nvrhi::Format of each attachment, in attachment order
  uint32_t color_control[4] = {};  // RB_BLENDCONTROLn of the attachment's guest target
  uint32_t color_mask[4] = {};     // its write mask nibble, 0 when the pixel shader does not write it
  uint32_t color_index[4] = {};    // the guest target index
  uint32_t depth_format = 0;   // nvrhi::Format, 0 (UNKNOWN) = no depth attachment
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL
  uint32_t ref_mask = 0;       // RB_STENCILREFMASK (the read and write masks)
  uint32_t mode_control = 0;   // PA_SU_SC_MODE_CNTL
  uint32_t clip_control = 0;   // PA_CL_CLIP_CNTL
  uint32_t poly_scale = 0;     // PA_SU_POLY_OFFSET_FRONT_SCALE, float bits (0 unless poly offset is on)
  uint32_t poly_offset = 0;    // PA_SU_POLY_OFFSET_FRONT_OFFSET, float bits
  uint32_t flags = 0;          // bit 0: front face flipped (--native_flip_front_face)
  uint32_t reserved[2] = {};
  bool operator==(const PipelineRecord&) const = default;
};
static_assert(sizeof(PipelineRecord) == 128, "PipelineRecord layout changed: bump kPipelineCacheVersion");

constexpr uint32_t kPipelineCacheMagic = 0x4350474Bu;  // 'KGPC'
constexpr uint32_t kPipelineCacheVersion = 1;

// The pipeline key: a hash of the record's contents (stable between runs and machines).
uint64_t PipelineKey(const PipelineRecord& record);

class PipelineCacheFile {
 public:
  PipelineCacheFile() = default;
  PipelineCacheFile(const PipelineCacheFile&) = delete;
  PipelineCacheFile& operator=(const PipelineCacheFile&) = delete;
  ~PipelineCacheFile() { Close(); }

  // Reads `path` if it is a cache file of this version (records are available through loaded())
  // and opens it for appending; creates the directory and file otherwise. False when it cannot
  // be opened for writing (loaded() still holds what could be read).
  bool Open(const std::string& path);
  void Close();
  bool is_open() const { return file_ != nullptr; }

  const std::vector<PipelineRecord>& loaded() const { return loaded_; }
  // Records seen so far (loaded or appended): Append ignores one that is already known.
  bool Known(const PipelineRecord& record) const { return keys_.count(PipelineKey(record)) != 0; }
  // Appends a record the first time it is seen. True when it was new.
  bool Append(const PipelineRecord& record);
  void Flush();

  // What was wrong with an existing file, for the log ("" = fine or absent).
  const std::string& note() const { return note_; }
  size_t appended() const { return appended_; }

 private:
  std::FILE* file_ = nullptr;
  std::vector<PipelineRecord> loaded_;
  std::unordered_set<uint64_t> keys_;
  std::string note_;
  size_t appended_ = 0;
};

}  // namespace nr
