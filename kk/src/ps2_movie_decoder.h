#pragma once

// The restored PS2 cutscenes' decoder, without the game or a window (ps2_movies.cpp plays them).
// Windows: Media Foundation (ps2_movie_decoder_mf.cpp). Linux: FFmpeg's LGPL H.264/AAC decoders, built
// as small shared libraries that ship beside the program (ps2_movie_decoder_ffmpeg.cpp,
// tools/build_ffmpeg_linux.sh). Builds with neither: ps2_movie_decoder_none.cpp (no playback).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace kk::movies {
constexpr int kAudioRate = 48000, kAudioChannels = 2, kAudioFrameBytes = 4;

// Whether this build can play the movies at all.
bool DecoderAvailable();
// Which decoder, for the log ("Media Foundation", "FFmpeg 4.4 (LGPL)", "none").
const char* DecoderName();

// The whole soundtrack as 48 kHz 16-bit stereo PCM. Gaps in the source timestamps are kept as silence and
// overlaps trimmed. Returns what it has if `cancel` becomes true. Throws std::runtime_error on errors and
// for soundtracks over 60 seconds.
std::vector<uint8_t> DecodeAudio(const std::filesystem::path& file, const std::atomic<bool>& cancel);

// The video stream, frame by frame, at its own size. Throws std::runtime_error on errors.
class VideoReader {
 public:
  explicit VideoReader(const std::filesystem::path& file);
  ~VideoReader();
  VideoReader(const VideoReader&) = delete;
  VideoReader& operator=(const VideoReader&) = delete;
  uint32_t width() const;
  uint32_t height() const;
  float pixel_aspect() const;
  double duration() const;  // seconds, from the file's header (0 if unknown)
  // The next decoded frame's presentation time in 100 ns units (the source's own, not yet made relative to
  // the first frame); false at the end of the stream.
  bool Next(int64_t& pts);
  // The frame Next() returned, as RGBA (width * height * 4 bytes, alpha 255). Not needed for a frame that is
  // skipped because it is late, which saves the conversion.
  void Pixels(std::vector<uint8_t>& rgba);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};
}  // namespace kk::movies
