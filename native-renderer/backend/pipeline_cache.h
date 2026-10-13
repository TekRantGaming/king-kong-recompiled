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
//
// The same format is the shipped pipeline list (docs/backend.md, "Pipeline lists"): the files of
// several runs merged into one by the nr_pipelines tool, read beside the game at start and never
// written there.
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
  uint32_t flags = 0;          // kRecordFlipFrontFace, kRecordScaled
  uint32_t reserved[2] = {};
  // The translations' input hashes (kkshaders::translationInputHash): the same microcode has
  // several translations in a pack (render scale aware or not, other interpolators), and the
  // prewarm must make the one the draw uses. Version 1 named the microcode only, and its
  // prewarm made whichever translation the pack gave first (V-Rex's fog lookups broke).
  uint64_t vs_input = 0;
  uint64_t ps_input = 0;
  bool operator==(const PipelineRecord&) const = default;
};
static_assert(sizeof(PipelineRecord) == 144, "PipelineRecord layout changed: bump kPipelineCacheVersion");

// PipelineRecord::flags.
constexpr uint32_t kRecordFlipFrontFace = 1u << 0;  // --native_flip_front_face (a debugging aid)
// Drawn by a renderer at a render scale: the pipeline layout has the scale constants (b3) and the
// pixel shader is the render scale aware translation. A run prewarms only the records of its own
// kind; the other kind's pipelines would never be drawn there (and the aware shader does not fit
// the 1:1 layout).
constexpr uint32_t kRecordScaled = 1u << 1;
// Object motion replay: the vertex shader is its motion variant, the pixel shader and set 0
// are the upscaler's built-ins. This layout does not use the game's scale constants.
constexpr uint32_t kRecordMotion = 1u << 2;
PipelineRecord MotionPipelineRecord(const PipelineRecord& draw, uint32_t motion_format);

constexpr uint32_t kPipelineCacheMagic = 0x4350474Bu;  // 'KGPC'
// 3: kRecordScaled, and the key includes the translations' input hashes. A version 2 file could
// mix the records of runs at different render scales that the key could not tell apart, so it is
// started afresh.
constexpr uint32_t kPipelineCacheVersion = 3;

// The pipeline key: a hash of the record's contents (stable between runs and machines). Every
// field takes part, the translations' input hashes too: the same microcode drawn with its 1:1
// and its render scale aware translation are two pipelines.
uint64_t PipelineKey(const PipelineRecord& record);

// Reads the records of a pipeline file of this version without changing it (a shipped list, or
// the user's file read by another process). Whole records only, in file order, duplicates kept.
// False when the file is absent, unreadable or of another version; `note` says why (empty when
// the file does not exist). `torn_bytes`: bytes of an incomplete last record that were left out.
bool ReadPipelineList(const std::string& path, std::vector<PipelineRecord>& records, std::string* note = nullptr,
                      size_t* torn_bytes = nullptr);
// Writes a whole pipeline file (the header and the records) through a temporary file renamed over
// `path`, so a reader never sees half of it. False on an I/O error (`error` says what).
bool WritePipelineList(const std::string& path, const std::vector<PipelineRecord>& records,
                       std::string* error = nullptr);

// Merges pipeline files into one list (the nr_pipelines tool): every record once, in the order first
// seen, so the first input's pipelines are made first at start.
class PipelineListMerger {
 public:
  struct Input {
    std::string path;
    bool read = false;       // a pipeline file of this version
    std::string note;        // why it was not read ("absent" when it does not exist)
    size_t records = 0;      // whole records in it
    size_t added = 0;        // new to the list
    size_t duplicates = 0;   // already in the list
    size_t debug = 0;        // drawn with --native_flip_front_face (a debugging aid): left out
    size_t torn_bytes = 0;   // an incomplete last record, left out
  };
  // keep_debug: also keep the records of runs with flipped faces.
  explicit PipelineListMerger(bool keep_debug = false) : keep_debug_(keep_debug) {}
  Input Add(const std::string& path);
  // Adds records that are not files (tests).
  size_t AddRecords(const std::vector<PipelineRecord>& records);
  const std::vector<PipelineRecord>& records() const { return records_; }

 private:
  bool keep_debug_;
  std::vector<PipelineRecord> records_;
  std::unordered_set<uint64_t> keys_;
};

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
