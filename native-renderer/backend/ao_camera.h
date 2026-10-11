// The camera for the ambient occlusion (docs/ao.md): the projection read back from the
// world-view-projection matrix the game's vertex shaders get, the way the renderer
// already finds that matrix (LooksLikePerspectiveRows), and the mapping from the
// scene's host pixels and stored depth to view space that XeGTAO works in.
//
// Why this works without the game's camera code: with clip = P * V * W * position,
// the w row is view z as a function of the model's position, the z row is a times it
// plus b, and the x and y rows are px and py times view x and y (plus the off-centre
// terms times view z). For a world and view made of rotations, translations and one
// uniform scale, the ratios of the rows' lengths give px, py and the off-centre terms
// exactly, and a and b come out whatever the world and view are. Draws whose world
// matrix stretches one axis give other values; a vote over the frame's scene draws keeps
// the camera most of them agree on.
#pragma once

#include <array>
#include <cstdint>

namespace nr {

struct AoProjection {
  float px = 0, py = 0;  // clip x = px * view x + cx * view z (1 / tan of half the field of view)
  float cx = 0, cy = 0;
  float a = 0, b = 0;    // clip z = a * view z + b, clip w = view z
  float near_plane() const { return a != 0.0f ? -b / a : 0.0f; }
  float far_plane() const { return a != 1.0f ? b / (1.0f - a) : 0.0f; }
  bool valid() const { return px > 0.0f && py > 0.0f && near_plane() > 0.0f; }
};

// From four registers used as rows (clip = dot(row, position) per component, which is what the
// game's dp4 against c0..c3 computes). False when they are not a perspective projection.
bool ExtractAoProjection(const float rows[16], AoProjection& out);

// The first four consecutive vertex constants (of 256, big-endian as in the D3D device's shadow)
// that look like a perspective matrix, as the tracker's FindPerspectiveRows does; -1 when none.
// rows receives them.
int FindAoPerspectiveRegister(const uint8_t* vs_constants_be, float rows[16]);

// How the guest's viewport puts clip space on the target (PA_CL_VPORT_* with the VTE enables
// applied, the window offset and D3D9's half-pixel offset) and the target's render scale.
struct AoScreen {
  float x_scale = 1, x_offset = 0, y_scale = 1, y_offset = 0, z_scale = 1, z_offset = 0;
  float window_x = 0, window_y = 0;  // the window offset (guest pixels), when enabled
  float half_pixel = 0;              // 0.5 with D3D9's half-pixel offset (PA_SU_VTX_CNTL pix_center 0)
  float host_scale_x = 1, host_scale_y = 1;
  // What the viewport transform adds to a pixel's position besides its offset.
  float offset_x() const { return window_x + half_pixel; }
  float offset_y() const { return window_y + half_pixel; }
  // The picture the viewport covers, in guest pixels (the half-pixel offset moves pixel centres,
  // not the picture).
  float left() const { return x_offset - (x_scale < 0 ? -x_scale : x_scale) + window_x; }
  float top() const { return y_offset - (y_scale < 0 ? -y_scale : y_scale) + window_y; }
  float width() const { return 2.0f * (x_scale < 0 ? -x_scale : x_scale); }
  float height() const { return 2.0f * (y_scale < 0 ? -y_scale : y_scale); }
};

// The constants XeGTAO needs for a working image of padded_w x padded_h pixels whose pixel i
// is the host pixel origin + step * i (step 1: full resolution, 2: half): XeGTAO's
// DepthUnpackConsts (view z = depth_mul / (depth_add - stored depth)) and NDCToViewMul / Add
// (view xy = (mul * uv + add) * view z, uv = (i + 0.5) / padded size).
struct AoViewMapping {
  float depth_mul = 0, depth_add = 0;
  float mul[2] = {}, add[2] = {};
};
AoViewMapping ComputeAoViewMapping(const AoProjection& p, const AoScreen& s, int32_t origin_x, int32_t origin_y,
                                   uint32_t padded_w, uint32_t padded_h, uint32_t step);

// The frame's camera: candidates from the scene's draws, the one offered most often wins
// (values within 0.5 %). Without a candidate this frame, the last frame's.
class AoCameraFinder {
 public:
  void Offer(uint64_t frame, const AoProjection& p, const AoScreen& s);
  bool Get(uint64_t frame, AoProjection& p, AoScreen& s) const;
  uint32_t offers(uint64_t frame) const { return frame == frame_ ? offers_ : 0; }

 private:
  struct Candidate {
    AoProjection p;
    AoScreen s;
    uint32_t count = 0;
  };
  static bool Same(const Candidate& c, const AoProjection& p, const AoScreen& s);
  uint64_t frame_ = UINT64_MAX;
  std::array<Candidate, 8> candidates_{};
  uint32_t used_ = 0, offers_ = 0;
  bool have_previous_ = false;
  Candidate previous_;
};

}  // namespace nr
