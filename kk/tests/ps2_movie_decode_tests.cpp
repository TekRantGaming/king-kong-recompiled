// Decodes one restored PS2 cutscene clip with this platform's movie decoder (Media Foundation on Windows,
// the bundled FFmpeg on Linux) to frames and audio, and checks it against the clip's known numbers.
// The clips are private (downloaded, never in git), so without one the test is skipped (exit 77).
//
//   kk-ps2-movie-decode-tests [clip.mp4 [frames [seconds]]]
//   or KK_PS2_TEST_CLIP / KK_PS2_TEST_FRAMES / KK_PS2_TEST_SECONDS in the environment.
//
// It prints one RESULT line (frame count, timestamps, audio length, colour samples, decode speed) so that
// the Windows and Linux decoders can be compared with each other.
#include "ps2_movie_decoder.h"
#include "ps2_movies_logic.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Expect(bool ok, const char* what, double got, double want) {
  std::printf("%s %s (got %.4f, want %.4f)\n", ok ? "ok  " : "FAIL", what, got, want);
  if (!ok) ++failures;
}
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
const char* Arg(int argc, char** argv, int i, const char* env) {
  if (argc > i) return argv[i];
  const char* v = std::getenv(env);
  return v && *v ? v : nullptr;
}
// Mean colour of the whole frame and of a 3x3 grid of patches: compares decoders without saving frames.
std::string Colours(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::string out;
  char text[64];
  auto mean = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    double sum[3] = {};
    uint64_t n = 0;
    for (uint32_t y = y0; y < y1; y += 4)
      for (uint32_t x = x0; x < x1; x += 4, ++n)
        for (int c = 0; c < 3; ++c) sum[c] += rgba[(size_t(y) * w + x) * 4 + c];
    std::snprintf(text, sizeof(text), "%.0f/%.0f/%.0f", sum[0] / n, sum[1] / n, sum[2] / n);
    return std::string(text);
  };
  out = mean(0, 0, w, h);
  for (uint32_t gy = 0; gy < 3; ++gy)
    for (uint32_t gx = 0; gx < 3; ++gx)
      out += "," + mean(w * gx / 3, h * gy / 3, w * gx / 3 + w / 12, h * gy / 3 + h / 12);
  return out;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace kk::movies;
  const char* clip = Arg(argc, argv, 1, "KK_PS2_TEST_CLIP");
  const char* frames_arg = Arg(argc, argv, 2, "KK_PS2_TEST_FRAMES");
  const char* seconds_arg = Arg(argc, argv, 3, "KK_PS2_TEST_SECONDS");
  std::printf("decoder: %s\n", DecoderName());
  if (!DecoderAvailable()) { std::printf("skipped: this build has no movie decoder\n"); return 77; }
  if (!clip) { std::printf("skipped: no clip (KK_PS2_TEST_CLIP); the clips are private test data\n"); return 77; }
  try {
    const std::filesystem::path file = std::filesystem::u8path(clip);
    std::atomic<bool> cancel{false};
    double t = Now();
    const auto pcm = DecodeAudio(file, cancel);
    const double audio_ms = (Now() - t) * 1000;
    const double audio_seconds = double(pcm.size()) / (kAudioRate * kAudioFrameBytes);
    double peak = 0, energy = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
      const double s = double(int16_t(uint16_t(pcm[i] | pcm[i + 1] << 8))) / 32768.0;
      peak = std::max(peak, std::fabs(s));
      energy += s * s;
    }
    const double rms = pcm.empty() ? 0 : std::sqrt(energy / double(pcm.size() / 2));

    t = Now();
    VideoReader video(file);
    const double open_ms = (Now() - t) * 1000;
    const uint32_t w = video.width(), h = video.height();
    int frames = 0, backwards = 0;
    int64_t origin = 0, first_step = 0, last = 0, max_step = 0, min_step = INT64_MAX;
    double next_ms = 0, pixels_ms = 0, worst_frame_ms = 0;
    std::vector<uint8_t> rgba;
    std::string colours_first, colours_mid, colours_last;
    const int mid_wanted = frames_arg ? std::atoi(frames_arg) / 2 : 100;
    for (;;) {
      int64_t pts = 0;
      double a = Now();
      if (!video.Next(pts)) break;
      double b = Now();
      video.Pixels(rgba);
      double c = Now();
      next_ms += (b - a) * 1000;
      pixels_ms += (c - b) * 1000;
      worst_frame_ms = std::max(worst_frame_ms, (c - a) * 1000);
      if (frames == 0) origin = pts;
      const int64_t rel = RelativePts(pts, origin);
      if (frames > 0) {
        const int64_t step = rel - last;
        if (step <= 0) ++backwards;
        max_step = std::max(max_step, step);
        min_step = std::min(min_step, step);
        if (frames == 1) first_step = step;
      }
      if (frames == 0) colours_first = Colours(rgba, w, h);
      if (frames == mid_wanted) colours_mid = Colours(rgba, w, h);
      colours_last = Colours(rgba, w, h);
      last = rel;
      ++frames;
    }
    const double frame_seconds = double(first_step) / 1e7;
    const double end_seconds = double(last) / 1e7 + frame_seconds;
    std::printf("RESULT decoder=\"%s\" size=%ux%u aspect=%.4f duration=%.6f frames=%d origin_ms=%.3f "
                "step_ms=%.3f..%.3f last_pts=%.6f end=%.6f backwards=%d audio_bytes=%zu audio_seconds=%.6f "
                "audio_peak=%.4f audio_rms=%.5f audio_ms=%.0f open_ms=%.0f decode_ms_per_frame=%.2f "
                "convert_ms_per_frame=%.2f worst_frame_ms=%.1f\n",
                DecoderName(), w, h, video.pixel_aspect(), video.duration(), frames, double(origin) / 1e4,
                double(min_step) / 1e4, double(max_step) / 1e4, double(last) / 1e7, end_seconds, backwards,
                pcm.size(), audio_seconds, peak, rms, audio_ms, open_ms, frames ? next_ms / frames : 0,
                frames ? pixels_ms / frames : 0, worst_frame_ms);
    std::printf("COLOURS first=%s\nCOLOURS mid=%s\nCOLOURS last=%s\n", colours_first.c_str(), colours_mid.c_str(),
                colours_last.c_str());

    // The player's own limits (ps2_movies.cpp) and the clip's known numbers.
    Expect(w > 0 && h > 0 && w <= 4096 && h <= 4096, "size within the player's limit (4096)", w, 4096);
    Expect(video.duration() > 0 && video.duration() <= 60, "duration within the player's limit (60 s)", video.duration(), 60);
    Expect(backwards == 0, "presentation times only go forward", backwards, 0);
    Expect(frames > 0, "frames decoded", frames, 1);
    if (frames_arg) Expect(frames == std::atoi(frames_arg), "frame count matches the clip", frames, std::atoi(frames_arg));
    if (seconds_arg) Expect(std::fabs(video.duration() - std::atof(seconds_arg)) < 0.05, "duration matches the clip",
                            video.duration(), std::atof(seconds_arg));
    Expect(std::fabs(end_seconds - video.duration()) < 0.1, "last frame ends with the movie", end_seconds, video.duration());
    Expect(std::fabs(audio_seconds - video.duration()) < 0.15, "soundtrack as long as the movie", audio_seconds,
           video.duration());
    Expect(rms > 0.001, "soundtrack is not silent", rms, 0.001);
    Expect(max_step - min_step < 20000, "steady frame cadence (within 2 ms)", double(max_step - min_step) / 1e4, 2);
  } catch (const std::exception& e) {
    std::printf("FAIL decoder error: %s\n", e.what());
    return 1;
  }
  std::printf(failures ? "%d check(s) FAILED\n" : "all checks passed\n", failures);
  return failures ? 1 : 0;
}
