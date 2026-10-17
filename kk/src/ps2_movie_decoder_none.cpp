// Builds without a movie decoder (a Linux build made without the bundled FFmpeg libraries, see
// tools/build_ffmpeg_linux.sh and KK_FFMPEG_DIR): the PS2 cutscenes do not play, the game loads normally.
#include "ps2_movie_decoder.h"

#include <stdexcept>

namespace kk::movies {
bool DecoderAvailable() { return false; }
const char* DecoderName() { return "none"; }
std::vector<uint8_t> DecodeAudio(const std::filesystem::path&, const std::atomic<bool>&) {
  throw std::runtime_error("this build has no movie decoder");
}
struct VideoReader::Impl {};
VideoReader::VideoReader(const std::filesystem::path&) { throw std::runtime_error("this build has no movie decoder"); }
VideoReader::~VideoReader() = default;
uint32_t VideoReader::width() const { return 0; }
uint32_t VideoReader::height() const { return 0; }
float VideoReader::pixel_aspect() const { return 1; }
double VideoReader::duration() const { return 0; }
bool VideoReader::Next(int64_t&) { return false; }
void VideoReader::Pixels(std::vector<uint8_t>&) {}
}  // namespace kk::movies
