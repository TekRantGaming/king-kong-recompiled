// Screen-space ambient occlusion for the native renderer (cloud session F).
//
// The technique of the Xenos plugin (tools/rexglue-patches 0004-0007, described
// in that folder's README), with depth and colour as ordinary host textures:
//
//  - the scene depth is noticed when the game resolves it out of EDRAM before
//    lighting (a full-screen D24S8 depth resolve): it is copied at once (the
//    resolve may clear the depth right after);
//  - at the next full-screen 8:8:8:8 colour resolve of that frame, and at any
//    later one to the same destination, the occlusion is multiplied into the
//    scene colour under the game's fog. The first of those resolves also works
//    the occlusion out (once a frame): 10 samples around a normal rebuilt from
//    the depth, a depth-aware 9-tap blur each way, an edge-aware upscale with
//    the distance fade and the fog amount per pixel.
//
// Unlike the Xenos plugin, which changes the EDRAM copy it dumps for the
// resolve, the multiplication goes into a scratch copy of the colour target and
// the resolve reads that: the game's own colour target is not touched, so a
// later resolve of the same target (a second scene copy) starts from the
// unchanged scene, exactly as with the dump.
//
// Passes are full-screen pixel shaders (shaders/ambient_occlusion.hlsl,
// compiled to SPIR-V and DXIL like the other renderer shaders): no compute,
// no storage images, so every colour format the targets use works. Everything
// is sized from the textures it is given, in host pixels: with a render scale
// the half-size working images are half the scaled size, and the blur taps keep
// their distance in the picture (docs/backend.md, "Ambient occlusion").
//
// Independent of the game renderer (it needs NVRHI only), so the tests drive it
// with synthetic depth and colour. Recorded on the render thread.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>

#include <nvrhi/nvrhi.h>

namespace nr {

struct AoSettings {
  int mode = 0;      // ao_mode: 0 off, 1 on, 2 show the AO only
  int strength = 1;  // ao_strength: 1 to 3 (1 is recommended)
  // tan(half the horizontal field of view); the Xenos plugin's default (it
  // takes the same value from REX_DEV_AO_PARAMS when set).
  float tan_half_fov_x = 0.684f;
  // Distance (in near-plane units) at which the occlusion has faded out.
  float fade_distance = 2000.0f;
};

// The game's fog pass this frame: its two AfterEffects fog pixel shaders'
// constants c0 (fog colour), c1 (g_vFogParams) and c2 (g_vFogNormalizing).
struct AoFog {
  bool valid = false;
  float normalizing[3] = {};
  float near_over_distance = 0, density = 0, inverse_distance = 0;
  float color[3] = {};
};

// What a resolve is, as the game renderer sees it.
struct AoResolve {
  uint64_t frame = 0;
  bool from_depth = false;
  // The resolve covers the whole picture from its top-left corner: at least
  // 1280 x 720 guest pixels, no offset.
  bool full_screen = false;
  // Depth: D24S8 (not D24FS8). Colour: 8:8:8:8 or 8:8:8:8 gamma. Both 1x
  // multisampling, which the game always uses.
  bool format_ok = false;
  // Where the resolve goes (any value that tells destinations apart: the
  // destination texture's base address).
  uint32_t dest_key = 0;
  // The source target's texture and the host size of the picture in it.
  nvrhi::ITexture* source = nullptr;
  uint32_t width = 0, height = 0;
};

class AmbientOcclusion {
 public:
  explicit AmbientOcclusion(nvrhi::IDevice* device);
  ~AmbientOcclusion();

  // Shaders, layout, sampler. False (and the object unusable) when the API has
  // no build of the shaders or the device refused them.
  bool Initialize();
  bool ready() const { return ready_; }

  // The fog constants c0..c3 of one of the game's fog draws (pixel shader
  // constants as floats, 16 of them). Cheap; call for each such draw.
  void NoteFog(const float constants[16], uint64_t frame);

  // A resolve is about to be recorded. Returns the texture to read the colour
  // from instead of resolve.source (a copy with the occlusion applied), or
  // null when the resolve is not a scene copy or AO is off. For a depth
  // resolve, records the depth preparation and returns null.
  nvrhi::ITexture* OnResolve(nvrhi::ICommandList* cl, const AoResolve& resolve, const AoSettings& settings);

  // Statistics (tests and the log).
  struct Stats {
    uint64_t depth_passes = 0;     // depth preparations recorded
    uint64_t computed_frames = 0;  // frames the occlusion was worked out
    uint64_t applied = 0;          // scene copies with the occlusion applied
  };
  const Stats& stats() const { return stats_; }

  // The set of tuning values for ao_strength 1..3: radius (near-plane units),
  // bias, intensity (the Xenos plugin's "sets").
  static std::array<float, 3> TuningFor(int strength);

 private:
  struct Pass {
    nvrhi::ShaderHandle ps;
    nvrhi::GraphicsPipelineHandle pipeline;
  };
  struct Image {
    nvrhi::TextureHandle texture;
    nvrhi::FramebufferHandle framebuffer;
  };
  struct Constants;

  bool EnsureImages(uint32_t width, uint32_t height);
  Image MakeImage(uint32_t width, uint32_t height, nvrhi::Format format, const char* name);
  bool EnsureScratch(nvrhi::ITexture* color);
  nvrhi::GraphicsPipelineHandle MakePipeline(const nvrhi::ShaderHandle& ps, nvrhi::IFramebuffer* fb);
  void Run(nvrhi::ICommandList* cl, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* fb,
           nvrhi::ITexture* a, nvrhi::ITexture* b, nvrhi::ITexture* c, const Constants& constants);
  void Compute(nvrhi::ICommandList* cl, const AoSettings& settings);

  nvrhi::IDevice* device_;
  bool ready_ = false;
  Stats stats_;

  nvrhi::ShaderHandle vs_;
  Pass copy_pass_, depth_pass_, ao_pass_, blur_pass_, upsample_pass_, apply_pass_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::SamplerHandle sampler_;
  nvrhi::TextureHandle dummy_;
  std::map<std::tuple<nvrhi::ITexture*, nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> sets_;

  // Working images for a picture of picture_w_ x picture_h_ host pixels.
  uint32_t picture_w_ = 0, picture_h_ = 0;
  // depth_full_: the scene depth as the game stored it (copied when it is resolved); depth_half_:
  // its linear distance at half size.
  Image depth_full_, depth_half_, ao_a_, ao_b_, ao_full_;
  // The scratch copy of the colour target (same size and format as it).
  Image scratch_;
  nvrhi::Format scratch_format_ = nvrhi::Format::UNKNOWN;
  uint32_t scratch_w_ = 0, scratch_h_ = 0;

  // The detection state (the Xenos plugin's ao_depth_copied_ and friends).
  bool depth_ready_ = false;
  uint64_t depth_frame_ = 0;
  uint32_t scene_dest_ = 0;
  bool scene_dest_set_ = false;
  uint64_t computed_frame_ = UINT64_MAX;
  AoFog fog_;
  uint64_t fog_frame_ = UINT64_MAX;
};

}  // namespace nr
