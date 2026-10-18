// Beta 7 uploads (owner, 17 Oct 2026; codex/state/telemetry-b7.md): the pure parts, unit tested without the
// game (tests/telemetry_tests.cpp). gzip (a small deflate encoder: LZ77 with fixed Huffman codes), removing
// user names and personal folders from report text, per-chapter frame statistics and the performance summary's
// JSON with the server's size limits. No platform calls here.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::telemetry {

// ------------------------------------------------------------------------------------------------- gzip ---

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace detail {
struct BitWriter {
  std::vector<uint8_t>& out;
  uint32_t buf = 0;
  int count = 0;
  void Put(uint32_t bits, int n) {  // least significant bit first
    buf |= bits << count;
    count += n;
    while (count >= 8) {
      out.push_back(uint8_t(buf));
      buf >>= 8;
      count -= 8;
    }
  }
  void PutCode(uint32_t code, int n) {  // Huffman codes go most significant bit first
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
    Put(r, n);
  }
  void Flush() {
    if (count) out.push_back(uint8_t(buf));
    buf = 0;
    count = 0;
  }
};
inline void Literal(BitWriter& w, unsigned v) {  // the fixed literal/length code (RFC 1951 3.2.6)
  if (v < 144) w.PutCode(0x30 + v, 8);
  else if (v < 256) w.PutCode(0x190 + (v - 144), 9);
  else if (v < 280) w.PutCode(v - 256, 7);
  else w.PutCode(0xC0 + (v - 280), 8);
}
inline void Match(BitWriter& w, unsigned length, unsigned distance) {
  static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                             33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  int l = 28;
  while (kLenBase[l] > length) --l;
  Literal(w, 257 + unsigned(l));
  if (kLenExtra[l]) w.Put(length - kLenBase[l], kLenExtra[l]);
  int d = 29;
  while (kDistBase[d] > distance) --d;
  w.PutCode(unsigned(d), 5);
  if (kDistExtra[d]) w.Put(distance - kDistBase[d], kDistExtra[d]);
}
}  // namespace detail

// Raw deflate (one fixed-Huffman block): LZ77 over a 32 KB window, hash chains of at most 32 candidates.
inline std::vector<uint8_t> Deflate(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out;
  out.reserve(size / 3 + 64);
  detail::BitWriter w{out};
  w.Put(1, 1);  // the last block
  w.Put(1, 2);  // fixed Huffman codes
  constexpr size_t kWindow = 32768, kMaxLen = 258;
  constexpr int kHashBits = 15, kChain = 32;
  std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(kWindow, -1);
  const auto hash = [&](size_t i) {
    return (uint32_t(data[i]) << 10 ^ uint32_t(data[i + 1]) << 5 ^ uint32_t(data[i + 2])) & ((1u << kHashBits) - 1);
  };
  const auto insert = [&](size_t i) {
    if (i + 3 > size) return;
    const uint32_t h = hash(i);
    prev[i % kWindow] = head[h];
    head[h] = int32_t(i);
  };
  for (size_t i = 0; i < size;) {
    size_t best = 0, best_dist = 0;
    if (i + 3 <= size) {
      int32_t cand = head[hash(i)];
      const size_t limit = std::min(kMaxLen, size - i);
      for (int n = 0; cand >= 0 && n < kChain; ++n) {
        const size_t c = size_t(cand);
        if (i - c > kWindow - 1 || c >= i) break;
        if (data[c + best] == data[i + best]) {
          size_t len = 0;
          while (len < limit && data[c + len] == data[i + len]) ++len;
          if (len > best) {
            best = len;
            best_dist = i - c;
            if (len == limit) break;
          }
        }
        const int32_t next = prev[c % kWindow];
        if (next >= cand) break;  // the slot was reused by a newer position
        cand = next;
      }
    }
    if (best >= 3) {
      detail::Match(w, unsigned(best), unsigned(best_dist));
      for (size_t k = 0; k < best; ++k) insert(i + k);
      i += best;
    } else {
      detail::Literal(w, data[i]);
      insert(i);
      ++i;
    }
  }
  detail::Literal(w, 256);  // end of block
  w.Flush();
  if (out.size() <= size + size / 64 + 8) return out;
  // Data that does not compress (already packed): stored blocks, 5 bytes per 64 KB more than the data.
  out.clear();
  for (size_t at = 0;;) {
    const size_t n = std::min<size_t>(65535, size - at);
    const bool last = at + n == size;
    out.push_back(last ? 1 : 0);  // BFINAL, BTYPE 00, then to the byte boundary
    out.push_back(uint8_t(n));
    out.push_back(uint8_t(n >> 8));
    out.push_back(uint8_t(~n));
    out.push_back(uint8_t(~n >> 8));
    out.insert(out.end(), data + at, data + at + n);
    at += n;
    if (last) return out;
  }
}

// A gzip member (RFC 1952): no name, no time, so equal input gives equal bytes.
inline std::vector<uint8_t> Gzip(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0xFF};
  const std::vector<uint8_t> body = Deflate(data, size);
  out.insert(out.end(), body.begin(), body.end());
  const uint32_t crc = Crc32(data, size), isize = uint32_t(size);
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(crc >> (8 * i)));
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(isize >> (8 * i)));
  return out;
}
inline std::vector<uint8_t> Gzip(std::string_view text) {
  return Gzip(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

// ------------------------------------------------------------------------------------------- redaction ---

// What counts as personal on this PC: the user's names (USERNAME, USER, the computer's name) and home folders
// (USERPROFILE, HOME).
struct Private {
  std::vector<std::string> names;
  std::vector<std::string> homes;
};

namespace detail {
inline char Lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
inline bool EqualNoCase(std::string_view a, size_t at, std::string_view b) {
  if (at + b.size() > a.size()) return false;
  for (size_t i = 0; i < b.size(); ++i)
    if (Lower(a[at + i]) != Lower(b[i])) return false;
  return true;
}
inline bool Slash(char c) { return c == '\\' || c == '/'; }
inline bool NameEnd(char c) { return Slash(c) || c == '"' || c == '\'' || c == '\r' || c == '\n' || c == '\0'; }
inline bool Alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
}  // namespace detail

// Before a report leaves the PC:
//   the home folder itself (USERPROFILE / HOME, wherever it is)  -> %USERPROFILE% (Windows) or ~
//   X:\Users\<name>\ (either slash, any drive)                   -> %USERPROFILE%\ (with that slash)
//   /home/<name>/, /var/home/<name>/, /Users/<name>/, \home\<name>\ -> ~/ (or ~\)
//   the user and computer names anywhere (any case)              -> <user>; names under 3 characters
//                                                                   only as a whole word
inline std::string RedactForUpload(std::string text, const Private& p) {
  using namespace detail;
  // 1. The home folders as they are (longest first, so a parent never cuts a longer one short).
  std::vector<std::string> homes;
  for (std::string h : p.homes) {
    while (h.size() > 1 && Slash(h.back())) h.pop_back();
    if (h.size() >= 3) homes.push_back(h);
  }
  std::sort(homes.begin(), homes.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
  for (const auto& h : homes) {
    const bool windows = h.size() >= 2 && h[1] == ':';
    std::string out;
    for (size_t i = 0; i < text.size();) {
      // Windows paths match either slash and any case; POSIX ones exactly.
      bool hit = windows ? EqualNoCase(text, i, h) : text.compare(i, h.size(), h) == 0;
      if (hit && windows) {
        for (size_t k = 0; k < h.size(); ++k)
          if (Slash(h[k]) != Slash(text[i + k])) hit = false;
      }
      if (hit && (i + h.size() == text.size() || NameEnd(text[i + h.size()]))) {
        out += windows ? "%USERPROFILE%" : "~";
        i += h.size();
      } else {
        out += text[i++];
      }
    }
    text = std::move(out);
  }
  // 2. Any profile folder, whoever's it is.
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    // X:\Users\<name>
    if (i + 9 < text.size() && std::isalpha(static_cast<unsigned char>(text[i])) && text[i + 1] == ':' &&
        Slash(text[i + 2]) && EqualNoCase(text, i + 3, "users") && Slash(text[i + 8]) && !NameEnd(text[i + 9])) {
      size_t j = i + 9;
      while (j < text.size() && !NameEnd(text[j])) ++j;
      out += "%USERPROFILE%";
      i = j;
      continue;
    }
    // /var/home/<name>, /home/<name>, /Users/<name>, and the same with backslashes (Wine's Z:\home\<name>)
    size_t skip = 0;
    for (const char* lead : {"/var/home/", "\\var\\home\\", "/home/", "\\home\\", "/Users/", "\\Users\\"})
      if (text.compare(i, std::char_traits<char>::length(lead), lead) == 0) {
        skip = std::char_traits<char>::length(lead);
        break;
      }
    if (skip && i + skip < text.size() && !NameEnd(text[i + skip])) {
      const bool windows_users = text.compare(i, 7, "\\Users\\") == 0;  // a profile without its drive
      // Wine: "Z:\home\name" keeps no drive: "~".
      if (!windows_users && out.size() >= 2 && out.back() == ':' &&
          std::isalpha(static_cast<unsigned char>(out[out.size() - 2])))
        out.resize(out.size() - 2);
      size_t j = i + skip;
      while (j < text.size() && !NameEnd(text[j])) ++j;
      out += windows_users ? "%USERPROFILE%" : "~";
      i = j;
      continue;
    }
    out += text[i++];
  }
  text = std::move(out);
  // 3. The names anywhere.
  for (const auto& name : p.names) {
    if (name.empty()) continue;
    const bool word = name.size() < 3;
    std::string replaced;
    replaced.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      if (EqualNoCase(text, i, name) &&
          (!word || ((i == 0 || !Alnum(text[i - 1])) &&
                     (i + name.size() == text.size() || !Alnum(text[i + name.size()]))))) {
        replaced += "<user>";
        i += name.size();
      } else {
        replaced += text[i++];
      }
    }
    text = std::move(replaced);
  }
  return text;
}

// ------------------------------------------------------------------------------------------------ JSON ---

// A JSON string, at most `max` bytes of text (cut at a character boundary).
inline std::string JsonString(std::string_view s, size_t max = 128) {
  if (s.size() > max) {
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 character
    s = s.substr(0, n);
  }
  std::string out = "\"";
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20) {
      char esc[8];
      std::snprintf(esc, sizeof(esc), "\\u%04x", u);
      out += esc;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

// A number with `decimals` places and a dot whatever the C locale says; "null" when it isn't finite.
inline std::string JsonNumber(double v, int decimals = 1) {
  if (!std::isfinite(v)) return "null";
  char text[64];
  std::snprintf(text, sizeof(text), "%.*f", decimals, v);
  std::string s = text;
  std::replace(s.begin(), s.end(), ',', '.');
  return s;
}

// ------------------------------------------------------------------------------- worst-frame detail ---

// Task 211 (owner, 18 Oct 2026): what the slowest frames of a chapter were doing, so a report with a
// multi-second worst frame says why. Only frames of kWorstFrameMs or more are looked at (nothing is
// collected for a normal frame beyond the running totals the frame already reads); each chapter keeps the
// kMaxWorstFrames slowest, at most kMaxWorstLoading of them loading frames (so a long level load cannot
// push every gameplay frame out). Numbers only and a few fixed words: no names, no paths, no free text.
inline constexpr double kWorstFrameMs = 250.0;
inline constexpr size_t kMaxWorstFrames = 3;
inline constexpr size_t kMaxWorstLoading = 1;

struct WorstFrame {
  double ms = 0;         // the frame (guest swap to guest swap)
  double at_s = -1;      // seconds since this chapter visit started
  bool loading = false;  // a level change's frames, or the game's loading screen was up
  // The swap thread's side of the frame (ms; negative: not known). game = ms - swap - cap wait.
  double cpu_ms = -1;       // that thread's CPU time in the frame
  double swap_ms = -1;      // inside the game's swap call (hands the frame to the renderer; waits when it is behind)
  double cap_wait_ms = -1;  // the frame rate cap's wait
  // The native renderer's running totals over the frame and the 2 after it (it runs up to 2 frames behind
  // the game). Only with `native`.
  bool native = false;
  uint32_t render_frames = 0;
  double render_cpu_ms = 0, gpu_ms = -1, gpu_wait_ms = 0, submit_ms = 0;
  uint32_t pipe_waits = 0, pipes_made = 0, draws_skipped = 0;
  double pipe_wait_ms = 0, compile_ms = 0;
  uint32_t tex_uploads = 0, pack_textures = 0;
  double upload_mb = 0, tex_ms = 0, prepare_ms = 0, pack_mb = 0;
  bool renderer_loading = false;
  int worker_jobs = -1;  // pipeline jobs queued or being made in the background (shader preparation)
  // The game's file reads in the frame (every thread).
  uint32_t reads = 0, video_reads = 0;
  double read_mb = 0, read_ms = 0, read_max_ms = 0;
  // What else was going on.
  int focus = -1;  // 0 focused, 1 in the background, 2 minimised; -1 not known
  bool focus_changed = false, paused = false, download = false, texture_pack = false;
  double sleep_ms = 0;  // the PC was asleep this long during the frame
  // The settings at the time.
  int frame_cap = -1, vsync = -1, hfr = -1;
  std::string upscaler;  // a fixed word from the renderer ("dlss", "fsr3", ...); cut to 16

  static std::string Word(std::string_view s) {  // letters, digits and '-' only, at most 16
    std::string out;
    for (const char c : s)
      if (out.size() < 16 && (std::isalnum(static_cast<unsigned char>(c)) || c == '-')) out += c;
    return out;
  }
  std::string Json() const {
    const auto n = [](const char* key, double v, int decimals = 1) {
      return std::string(",\"") + key + "\":" + JsonNumber(v, decimals);
    };
    const auto u = [](const char* key, uint64_t v) { return std::string(",\"") + key + "\":" + std::to_string(v); };
    const auto b = [](const char* key, bool v) { return std::string(",\"") + key + "\":" + (v ? "true" : "false"); };
    std::string s = "{\"ms\":" + JsonNumber(ms, 1);
    if (at_s >= 0) s += n("at", at_s);
    s += std::string(",\"phase\":\"") + (loading ? "load" : "play") + "\"";
    if (cpu_ms >= 0) s += n("cpuMs", cpu_ms);
    if (swap_ms >= 0) s += n("swapMs", swap_ms);
    if (cap_wait_ms >= 0) s += n("capMs", cap_wait_ms);
    if (swap_ms >= 0 && cap_wait_ms >= 0) s += n("gameMs", std::max(0.0, ms - swap_ms - cap_wait_ms));
    if (native) {
      s += u("rFrames", render_frames) + n("rCpuMs", render_cpu_ms);
      if (gpu_ms >= 0) s += n("gpuMs", gpu_ms, 2);
      s += n("gpuWaitMs", gpu_wait_ms) + n("submitMs", submit_ms) + b("rLoading", renderer_loading);
      if (worker_jobs >= 0) s += u("jobs", uint64_t(worker_jobs));
      s += u("pipeWaits", pipe_waits) + n("pipeWaitMs", pipe_wait_ms) + u("pipesMade", pipes_made) +
           u("drawsSkipped", draws_skipped) + n("compileMs", compile_ms);
      s += u("texUploads", tex_uploads) + n("uploadMB", upload_mb, 2) + n("texMs", tex_ms) + n("prepMs", prepare_ms) +
           u("packTex", pack_textures) + n("packMB", pack_mb, 2);
    }
    s += b("pack", texture_pack);
    s += u("reads", reads) + n("readMB", read_mb, 2) + n("readMs", read_ms) + n("readMaxMs", read_max_ms) +
         u("videoReads", video_reads);
    if (focus >= 0) {
      s += std::string(",\"focus\":\"") + (focus == 0 ? "focused" : focus == 1 ? "background" : "minimised") + "\"";
      s += b("focusChanged", focus_changed);
    }
    s += b("paused", paused) + n("sleepMs", sleep_ms, 0) + b("download", download);
    if (frame_cap >= 0) s += u("cap", uint64_t(frame_cap));
    if (vsync >= 0) s += b("vsync", vsync != 0);
    if (hfr >= 0) s += b("hfr", hfr != 0);
    if (const std::string w = Word(upscaler); !w.empty()) s += ",\"upscaler\":\"" + w + "\"";
    return s + "}";
  }
};

// Task 217: a restored PS2 cutscene plays between story chapters while the level loader waits for it
// (ps2_movies.cpp BeforeLoad), so the game swaps no frame for the clip's length (8-14 s). The player is
// watching the cutscene, not a frozen game: the frame that spans it stays out of the chapter's statistics.
class CutsceneFrames {
 public:
  // At each frame: `waits` = cutscene waits ended so far, `waiting` = one is in progress now.
  bool Spans(uint32_t waits, bool waiting) {
    const bool spans = waiting || waits != seen_;
    seen_ = waits;
    return spans;
  }

 private:
  uint32_t seen_ = 0;
};

// Whether a frame gets its detail recorded: in a chapter, and at least kWorstFrameMs.
inline bool WorstFrameWanted(int entry, double ms) { return entry >= 0 && std::isfinite(ms) && ms >= kWorstFrameMs; }

// Keeps `list` the slowest kMaxWorstFrames frames, slowest first, with at most kMaxWorstLoading loading ones.
inline void AddWorstFrame(std::vector<WorstFrame>& list, const WorstFrame& f) {
  if (!std::isfinite(f.ms) || f.ms < kWorstFrameMs) return;
  if (f.loading) {
    size_t loading = 0, smallest = list.size();
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].loading) {
        ++loading;
        if (smallest == list.size() || list[i].ms < list[smallest].ms) smallest = i;
      }
    if (loading >= kMaxWorstLoading) {
      if (f.ms <= list[smallest].ms) return;
      list.erase(list.begin() + std::ptrdiff_t(smallest));
    }
  }
  list.push_back(f);
  std::stable_sort(list.begin(), list.end(), [](const WorstFrame& a, const WorstFrame& b) { return a.ms > b.ms; });
  if (list.size() > kMaxWorstFrames) list.resize(kMaxWorstFrames);
}

// ------------------------------------------------------------------------------------------- frame stats ---

// One chapter's frames in a session. Updated on the game thread per frame (a few additions; a 0.5 ms
// histogram gives the 1% low without keeping the frames).
struct ChapterStats {
  static constexpr int kBuckets = 400;  // 0.5 ms each, to 200 ms
  static constexpr double kBucketMs = 0.5;
  int entry = -1;
  uint64_t frames = 0;
  double total_ms = 0, worst_ms = 0;
  uint32_t over50 = 0;
  std::array<uint32_t, kBuckets> histogram{};
  uint64_t long_frames = 0;  // 200 ms and over: counted exactly
  double long_ms = 0;
  double gpu_ms_sum = 0;
  uint32_t gpu_samples = 0;
  double game_cpu_ms = 0;   // the game thread's CPU time over...
  uint64_t game_cpu_frames = 0;  // ...this many frames
  double pipeline_wait_ms = 0;
  std::vector<WorstFrame> worst;  // task 211: the slowest frames' detail (AddWorstFrame)

  void AddFrame(double ms) {
    if (!(ms > 0) || !std::isfinite(ms)) return;
    ++frames;
    total_ms += ms;
    worst_ms = std::max(worst_ms, ms);
    if (ms > 50.0) ++over50;
    const int b = int(ms / kBucketMs);
    if (b < kBuckets) ++histogram[size_t(b)];
    else {
      ++long_frames;
      long_ms += ms;
    }
  }
  double Seconds() const { return total_ms / 1000.0; }
  double AverageFps() const { return total_ms > 0 ? 1000.0 * double(frames) / total_ms : 0; }
  // The average frame rate of the slowest 1% of frames (as the log's "1% low").
  double OnePercentLowFps() const {
    if (!frames) return 0;
    const uint64_t want = std::max<uint64_t>(1, frames / 100);
    uint64_t got = 0;
    double ms = 0;
    const uint64_t from_long = std::min(want, long_frames);
    if (from_long) {
      ms += long_ms * double(from_long) / double(long_frames);
      got += from_long;
    }
    for (int b = kBuckets - 1; b >= 0 && got < want; --b) {
      const uint64_t take = std::min<uint64_t>(histogram[size_t(b)], want - got);
      ms += double(take) * (b + 0.5) * kBucketMs;
      got += take;
    }
    return ms > 0 ? 1000.0 * double(got) / ms : 0;
  }
  void Merge(const ChapterStats& o) {
    frames += o.frames;
    total_ms += o.total_ms;
    worst_ms = std::max(worst_ms, o.worst_ms);
    over50 += o.over50;
    for (int b = 0; b < kBuckets; ++b) histogram[size_t(b)] += o.histogram[size_t(b)];
    long_frames += o.long_frames;
    long_ms += o.long_ms;
    gpu_ms_sum += o.gpu_ms_sum;
    gpu_samples += o.gpu_samples;
    game_cpu_ms += o.game_cpu_ms;
    game_cpu_frames += o.game_cpu_frames;
    pipeline_wait_ms += o.pipeline_wait_ms;
    for (const auto& f : o.worst) AddWorstFrame(worst, f);
  }
  // `details`: with the worst frames' detail (PerfJson leaves it out of the shortest chapters to fit).
  std::string Json(bool details = true) const {
    std::string s = "{\"entry\":" + std::to_string(entry) + ",\"seconds\":" + JsonNumber(Seconds()) +
                    ",\"avgFps\":" + JsonNumber(AverageFps()) + ",\"p1LowFps\":" + JsonNumber(OnePercentLowFps()) +
                    ",\"worstMs\":" + JsonNumber(worst_ms) + ",\"stutters50ms\":" + std::to_string(over50);
    if (gpu_samples) s += ",\"gpuMs\":" + JsonNumber(gpu_ms_sum / gpu_samples, 2);
    if (game_cpu_frames) s += ",\"cpuGameMs\":" + JsonNumber(game_cpu_ms / double(game_cpu_frames), 2);
    s += ",\"pipelineWaitMs\":" + JsonNumber(pipeline_wait_ms, 1);
    if (details && !worst.empty()) {
      s += ",\"worst\":[";
      for (size_t i = 0; i < worst.size(); ++i) s += (i ? "," : "") + worst[i].Json();
      s += "]";
    }
    return s + "}";
  }
};

// --------------------------------------------------------------------------------------- the summary ---

inline constexpr size_t kMaxPerfBytes = 16 * 1024;       // POST /v1/perf
inline constexpr size_t kMaxChapters = 64;
inline constexpr size_t kMaxCrashGzip = 256 * 1024;      // POST /v1/crash, compressed
inline constexpr size_t kMaxCrashText = 1024 * 1024;     // ... and its text
inline constexpr size_t kMaxPipelinesGzip = 1024 * 1024; // POST /v1/pipelines, compressed
inline constexpr double kMinChapterSeconds = 5.0;        // shorter visits are left out

struct PerfSummary {
  std::string install_id, version, api, gpu, gpu_vendor, driver, cpu, os, display;
  int64_t vram_mib = -1, ram_mib = -1;
  int cpu_threads = -1;
  // "settings": each value already JSON (a string, number or true/false).
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<ChapterStats> chapters;
  bool pipeline_library = false, startup_fallback = false;
};

// The summary's JSON, within kMaxPerfBytes: strings cut to 128 bytes, at most 64 chapters (the longest
// played ones are kept when there are more, or when the text would be too long). Empty when there is no
// chapter worth sending.
inline std::string PerfJson(const PerfSummary& in) {
  std::vector<ChapterStats> chapters;
  for (const auto& c : in.chapters)
    if (c.entry >= 0 && c.Seconds() >= kMinChapterSeconds) chapters.push_back(c);
  if (chapters.empty()) return {};
  std::stable_sort(chapters.begin(), chapters.end(),
                   [](const ChapterStats& a, const ChapterStats& b) { return a.total_ms > b.total_ms; });
  if (chapters.size() > kMaxChapters) chapters.resize(kMaxChapters);
  const auto field = [](const char* key, const std::string& value) {
    return std::string(",\"") + key + "\":" + JsonString(value);
  };
  std::string head = "{\"installId\":" + JsonString(in.install_id, 36) + field("version", in.version) +
                     field("api", in.api);
  if (!in.gpu.empty()) head += field("gpu", in.gpu);
  if (!in.gpu_vendor.empty()) head += field("gpuVendor", in.gpu_vendor);
  if (!in.driver.empty()) head += field("driver", in.driver);
  if (in.vram_mib >= 0) head += ",\"vramMiB\":" + std::to_string(in.vram_mib);
  if (!in.cpu.empty()) head += field("cpu", in.cpu);
  if (in.cpu_threads > 0) head += ",\"cpuThreads\":" + std::to_string(in.cpu_threads);
  if (in.ram_mib >= 0) head += ",\"ramMiB\":" + std::to_string(in.ram_mib);
  if (!in.os.empty()) head += field("os", in.os);
  if (!in.display.empty()) head += field("display", in.display);
  head += ",\"settings\":{";
  size_t n = 0;
  for (const auto& [key, value] : in.settings) {
    if (n == 32) break;
    head += (n++ ? "," : "") + JsonString(key, 32) + ":" + (value.size() > 130 ? std::string("null") : value);
  }
  head += "}";
  const std::string tail = std::string(",\"pipelineLibrary\":") + (in.pipeline_library ? "true" : "false") +
                           ",\"startupFallback\":" + (in.startup_fallback ? "true" : "false") + "}";
  // Too long: the worst-frame detail goes first (from the shortest chapter up), then the shortest chapters.
  size_t detailed = chapters.size();
  for (;;) {
    std::string json = head + ",\"chapters\":[";
    for (size_t i = 0; i < chapters.size(); ++i) json += (i ? "," : "") + chapters[i].Json(i < detailed);
    json += "]" + tail;
    if (json.size() <= kMaxPerfBytes) return json;
    if (detailed > 0) {
      --detailed;
      continue;
    }
    if (chapters.size() <= 1) return "";
    chapters.pop_back();
  }
}

// The crash report's gzip within the server's limits: the text cut to 1 MB, then (if its gzip is still
// over 256 KB) to its start and end halves until it fits. Empty for an empty text.
inline std::vector<uint8_t> CrashGzip(std::string text) {
  if (text.empty()) return {};
  const auto shorten = [&text](size_t keep) {
    if (text.size() <= keep) return;
    const std::string mark = "\n[... cut to fit the upload limit ...]\n";
    const size_t half = (keep - std::min(keep, mark.size())) / 2;
    text = text.substr(0, half) + mark + text.substr(text.size() - half);
  };
  shorten(kMaxCrashText);
  for (;;) {
    std::vector<uint8_t> gz = Gzip(text);
    if (gz.size() <= kMaxCrashGzip) return gz;
    shorten(text.size() / 2);
    if (text.size() < 64) return {};
  }
}

}  // namespace kk::telemetry
