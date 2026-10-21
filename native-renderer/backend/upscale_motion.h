// Motion vectors for the temporal upscalers, the CPU side (docs/upscalers.md, "Motion vectors").
//
// The game never computes motion vectors: the renderer makes them from what it sees.
//
//  - Per-object motion. Every draw that writes the scene's depth (the depth pre-pass, the opaque draws
//    with depth writes, Kong's fur shells, the water) has an identity: its vertex shader, its vertex and
//    index buffers and the range it draws (MotionDrawKey). The renderer keeps each such draw's vertex
//    shader constants (world, view, projection, bone matrices: all 256 registers) for one frame. When
//    the same draw comes again, MotionHistory hands back last frame's constants; the motion pass then
//    draws it once more with a vertex shader translated to compute the position twice, with this
//    frame's and last frame's constants, and writes the difference where its depth is the scene's.
//  - Camera motion, everywhere else. From any draw whose world did not move, last frame's
//    world-view-projection times the inverse of this frame's maps this frame's clip space to last
//    frame's (the world cancels out). The scene's draws vote; the matrix most of them agree on is the
//    camera's (CameraReprojectionVote). A full-screen pass applies it to every pixel's depth.
//
// No GPU here; GameRenderer (game_upscale.cpp) records and draws.
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nr {

// 4x4 matrices, row-major: m[r * 4 + c]. clip = M * position with M's rows the guest's c0..c3 (dp4).
using Mat4 = std::array<double, 16>;
Mat4 Mat4Identity();
Mat4 Mat4Multiply(const Mat4& a, const Mat4& b);
// False when singular (|det| tiny against the entries' scale).
bool Mat4Invert(const Mat4& m, Mat4& out);
// out = m * v.
void Mat4Transform(const Mat4& m, const double v[4], double out[4]);
Mat4 Mat4FromRows(const float rows[16]);

// A draw's identity across frames.
struct MotionDrawKey {
  uint64_t vs_hash = 0;          // the vertex shader's microcode hash
  uint32_t vertex_buffer = 0;    // stream 0's buffer (physical address)
  uint32_t vertex_offset = 0;    // stream 0's offset
  uint32_t vertex_buffer2 = 0;   // stream 1's buffer (0: none)
  uint32_t index_buffer = 0;     // physical address (0: not indexed)
  uint32_t start = 0, count = 0;
  int32_t base_vertex = 0;
  uint32_t primitive = 0;
  uint64_t Hash() const;
  bool operator==(const MotionDrawKey& o) const;
};

// The vertex constants of one draw, as the shaders read them (host floats, 256 float4).
using VsConstants = std::array<float, 1024>;

// Last frame's constants of each draw. Not thread-safe (the render thread).
class MotionHistory {
 public:
  // A new frame: this frame's records become the previous frame's.
  void BeginFrame(uint64_t frame);
  // Records a draw of the current frame; returns its index among this frame's draws with the same
  // key (its ordinal).
  uint32_t Record(const MotionDrawKey& key, const VsConstants& constants);
  // Last frame's constants for a draw of this frame recorded with `ordinal`: when last frame had as
  // many draws with this key as this frame has so far, the one with the same ordinal (draw order is
  // stable frame to frame); else the one whose constants are closest. Null when last frame had none.
  // `final_count`: how many draws of this key this frame has in all (when known), else 0.
  const VsConstants* FindPrevious(const MotionDrawKey& key, uint32_t ordinal, const VsConstants& current,
                                  uint32_t final_count = 0) const;
  // How many draws this frame recorded with a key.
  uint32_t CountThisFrame(const MotionDrawKey& key) const;
  uint64_t frame() const { return frame_; }
  size_t previous_draws() const { return previous_count_; }
  size_t current_draws() const { return current_count_; }

 private:
  struct Slot {
    MotionDrawKey key;
    std::vector<VsConstants> constants;  // in draw order
    // The closest-constants search's last answer + 1: draws keep their order when some are culled,
    // so the next draw's match is usually there (only where the search starts; task 319).
    mutable size_t next_guess = 0;
    // How many leading floats all its draws share (bit for bit; SIZE_MAX: not measured yet).
    mutable size_t common_prefix = SIZE_MAX;
    mutable size_t common_suffix = 1024;  // first float of the shared suffix
    mutable bool prefix_finite = false, suffix_finite = false;
    // Task 351, the closest-constants search of a key with many draws: its draws sorted by one
    // float that tells them apart (a translation, typically), so a search measures only the draws
    // whose difference in that float alone is not already more than the best sum so far.
    mutable bool index_built = false;
    mutable int index_column = -1;  // -1: no usable float (the plain search)
    mutable std::vector<std::pair<float, uint32_t>> index;  // (that float, draw), ascending
  };
  // Draws per key from which the closest-constants search uses the index.
  static constexpr size_t kIndexedSearchMin = 24;
  static void BuildIndex(const Slot& s);
  using Map = std::unordered_map<uint64_t, std::vector<Slot>>;  // hash -> keys with that hash
  static const Slot* Find(const Map& m, const MotionDrawKey& key);
  static Slot& FindOrAdd(Map& m, const MotionDrawKey& key);
  // Empties every slot, keeping its storage; with drop_unused, removes slots already empty.
  static void Recycle(Map& m, bool drop_unused);
  Map current_, previous_;
  size_t current_count_ = 0, previous_count_ = 0;
  uint64_t frame_ = UINT64_MAX;
};

// The camera's reprojection (this frame's clip space to last frame's) by a vote of the scene's draws.
class CameraReprojectionVote {
 public:
  void BeginFrame(uint64_t frame);
  // A draw's world-view-projection rows this frame and last frame (the same register of the same draw).
  // Ignored when the matrices are not invertible.
  void Offer(const float current_rows[16], const float previous_rows[16]);
  // The matrix most offers agreed on (entries within `tolerance` of the candidate's scale), with how
  // many agreed and how many offered. False when nothing was offered this frame.
  bool Result(Mat4& reprojection, uint32_t* votes = nullptr, uint32_t* offers = nullptr) const;
  uint32_t offers() const { return offers_; }

  static constexpr double kTolerance = 2e-3;

 private:
  struct Candidate {
    Mat4 m;    // the first member (what others are compared with)
    Mat4 sum;  // all members, added
    uint32_t votes = 0;
  };
  static bool Close(const Mat4& a, const Mat4& b);
  std::vector<Candidate> candidates_;
  uint32_t offers_ = 0;
  uint64_t frame_ = UINT64_MAX;
  // The last offer (task 351): draws in a row with the same rows (a field of grass under one camera,
  // a thousand draws a frame in Ann's chapter) give the same matrix and join the same candidate, so
  // the inversion and the search are done once. The same arithmetic on the same bits: identical sums.
  struct LastOffer {
    bool valid = false;
    float current[16], previous[16];
    bool counted = false;     // invertible: it was counted as an offer
    int candidate = -1;       // the candidate it joined or started (-1: none, the list was full)
    Mat4 r;
  } last_;
};

// How the guest's viewport puts clip space on the scene target, with the host's render scale and the
// frame's jitter: what the camera motion pass needs to turn a host pixel and its stored depth back into
// clip space (the inverse of GameRenderer::ComputeViewport's transform).
struct MotionScreen {
  float x_scale = 640, x_offset = 640;   // guest pixel (D3D10 convention: centres at .5) = ndc.x * x_scale + x_offset
  float y_scale = -360, y_offset = 360;  // the same for y (y_scale negative: NDC y up, pixels down)
  float z_scale = 1, z_offset = 0;       // stored depth = ndc.z * z_scale + z_offset
  float host_scale_x = 1, host_scale_y = 1;  // host pixels per guest pixel of the scene target
};

// A host pixel (its centre) of a picture rendered with `jitter` (input pixels) and its stored depth, to
// the guest's NDC of the unjittered frame.
void HostPixelToNdc(const MotionScreen& s, float jitter_x, float jitter_y, double host_x, double host_y,
                    double depth, double ndc[3]);
// The motion (host input pixels, previous minus current, +y down) of a pixel under a reprojection.
// False when the point is behind last frame's camera.
bool CameraMotion(const MotionScreen& s, const Mat4& reprojection, float jitter_x, float jitter_y, double host_x,
                  double host_y, double depth, double motion[2]);

}  // namespace nr
