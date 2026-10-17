// XeGTAO for the native renderer (docs/ao.md): ground-truth based ambient occlusion from
// the scene's depth, with NVRHI compute passes (shaders/gtao.hlsl over the vendored
// thirdparty/XeGTAO): the depth into view space with four smaller mips, normals rebuilt
// from it, the occlusion (3 slices of 3 steps per pixel) at full or half resolution, two
// edge-aware denoise passes, and the result as one R32F image over the picture (1 = no
// occlusion). Then either multiplied into the scene's colour target, or into a copy of it
// under the game's fog (the resolve moment), or read by the lighting shaders (ambient-only).
//
// Everything is in host pixels and sized from the textures it is given: at any
// --native_render_scale the effect radius stays in world units and the screen mapping
// comes from the viewport and the render scale (ao_camera.h).
//
// Independent of the game renderer (NVRHI only), recorded on the render thread.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ambient_occlusion.h"
#include "backend/ao_camera.h"
#include "backend/upscale_motion.h"

namespace nr {

struct GtaoSettings {
  int mode = 1;          // ao_mode: 1 on, 2 the occlusion only
  int strength = 1;      // ao_strength 1 to 3
  bool half = false;     // --native_ao_quality=half
  int sample_quality = 2;
  bool temporal = false;  // local accumulation, when there is no temporal upscaler
  bool temporal_upscaler = false;
  float radius = 0.0f;   // world units; 0: the legacy radius for the strength times the near plane
  float radius_scale = 1.0f;  // times the radius from the camera
  float falloff = 0.0f;       // XeGTAO's falloff range (0: its default)
  float power = 0.0f;         // XeGTAO's final power (0: FinalPower(strength))
  float fade = 0.0f;          // near-plane distances where the occlusion has faded out linearly (0: none)
  int normal_smoothing = 0;   // native_ao_normal_smoothing: 0 off, 1 low, 2 high (Gtao::SmoothingFor)
};

struct GtaoInput {
  nvrhi::ITexture* depth = nullptr;  // the stored depth (a D32S8 target or an R32F copy of one)
  // The picture in host pixels of that texture (the viewport of the scene's draws).
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
  AoProjection projection;
  AoScreen screen;
  uint64_t frame = UINT64_MAX;
  Mat4 reprojection = Mat4Identity();
  bool reprojection_valid = false;
};

class Gtao {
 public:
  explicit Gtao(nvrhi::IDevice* device);
  ~Gtao();

  // Shaders, layouts, pipelines. False when the API has no build of the shaders.
  bool Initialize();
  bool ready() const { return ready_; }

  // Works the occlusion out. Returns the R32F image of in.width x in.height (1 = none; left in the
  // ShaderResource state, so the bindless tables can sample it), or null when the input cannot be used.
  nvrhi::ITexture* Compute(nvrhi::ICommandList* cl, const GtaoInput& in, const GtaoSettings& s);
  // Only the first three passes: the stored depth into the working image, view-space depth with its
  // four smaller mips, and the normals rebuilt from it (what the screen-space GI works from, ssgi.h).
  // False when the input cannot be used. Compute runs this first.
  bool Prepare(nvrhi::ICommandList* cl, const GtaoInput& in, const GtaoSettings& s);
  // After a Prepare with the same settings: the normal smoothing passes alone (as Compute runs them),
  // for an effect that reads the normals without the occlusion (the cel shading's creases). False
  // when nothing was prepared or s.normal_smoothing is off.
  bool SmoothNormals(nvrhi::ICommandList* cl, const GtaoSettings& s);
  // The last Prepare's normals were smoothed since (by Compute or SmoothNormals).
  bool normals_smoothed() const { return normals_smoothed_; }
  // The working images of the last Prepare (padded_width x padded_height; pixel i is host pixel
  // origin + step * i): the stored depth (R32F), view-space depth (R32F, XE_GTAO_DEPTH_MIP_LEVELS
  // mips) and the normals (R32_UINT, XeGTAO's R11G11B10 packing). After a Compute with normal
  // smoothing on, the normals are the smoothed ones (smoothed in place; Prepare alone never smooths).
  nvrhi::ITexture* raw_depth() const { return raw_.Get(); }
  nvrhi::ITexture* view_depth() const { return depth_mips_.Get(); }
  nvrhi::ITexture* normals() const { return normal_map_.Get(); }
  uint32_t padded_width() const { return padded_w_; }
  uint32_t padded_height() const { return padded_h_; }
  uint32_t step() const { return step_; }
  // The constants of the last Prepare (shaders/gtao.hlsl's GTAOConstantBuffer: XeGTAO's
  // GTAOConstants first, then the picture and the working size).
  const std::vector<uint8_t>& constants_bytes() const { return constants_bytes_; }
  // The last result and where it lies in its target (host pixels).
  nvrhi::ITexture* occlusion() const { return output_.Get(); }
  int32_t origin_x() const { return origin_x_; }
  int32_t origin_y() const { return origin_y_; }
  uint32_t width() const { return out_w_; }
  uint32_t height() const { return out_h_; }

  // Over the picture of the last Compute: the colour target times the occlusion (mode 1, blended:
  // nothing is read back) or the occlusion itself (mode 2). The alpha channel is left alone.
  bool ApplyToTarget(nvrhi::ICommandList* cl, nvrhi::ITexture* color, int mode);
  // The resolve moment: a copy of `color` with the occlusion multiplied in under the game's fog
  // (`depth` gives the fog's distance). Returns the copy, or null.
  nvrhi::ITexture* ApplyToCopy(nvrhi::ICommandList* cl, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                               const AoFog& fog, int mode);
  // A depth target copied into an R32F image of its size (kept at the depth resolve: the game may
  // clear the target right after). The same image every time.
  nvrhi::ITexture* CopyDepth(nvrhi::ICommandList* cl, nvrhi::ITexture* depth);

  // The values Compute uses for a strength: the effect radius in near-plane units (the legacy
  // method's) and XeGTAO's final power.
  static float RadiusInNearPlanes(int strength);
  static float FinalPower(int strength);
  // Normal smoothing (native_ao_normal_smoothing 1 low, 2 high): the largest radius in host pixels of a
  // 1080-line picture (scaled with the picture), the radius in view units as a fraction of the effect
  // radius (so it shrinks with distance), and the angles between normals smoothed fully and not at all
  // (low-poly creases are smoothed; the right angles of boxes and walls never). A tap's angle off the
  // pixel's plane uses the same ramp, capped at kPlaneFull/MaxDegrees (silhouettes stay sharp).
  struct Smoothing {
    float max_pixels_1080 = 0.0f;
    float radius_fraction = 0.0f;
    float full_angle_degrees = 0.0f;
    float max_angle_degrees = 0.0f;
  };
  static constexpr float kPlaneFullDegrees = 20.0f, kPlaneMaxDegrees = 40.0f;
  static Smoothing SmoothingFor(int level);

  struct Stats {
    uint64_t computed = 0;
    uint64_t applied = 0;       // ApplyToTarget
    uint64_t copies = 0;        // ApplyToCopy
    uint64_t depth_copies = 0;  // CopyDepth
    uint64_t history_frames = 0;  // frames eligible for local depth-validated history
  };
  const Stats& stats() const { return stats_; }
  // These views can retain guest colour/depth targets across a scale change.
  // Submitted command lists keep their own binding/framebuffer references.
  void ForgetTargetViews() { sets_.clear(); framebuffers_.clear(); }

 private:
  struct Constants;
  struct Pass {
    nvrhi::ShaderHandle shader;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::ComputePipelineHandle pipeline;
  };
  bool MakePass(Pass& pass, int id, const char* entry, const std::vector<nvrhi::BindingLayoutItem>& items);
  nvrhi::TextureHandle MakeImage(uint32_t width, uint32_t height, nvrhi::Format format, uint32_t mips, bool uav,
                                 bool render_target, const char* name);
  bool EnsureImages(uint32_t padded_w, uint32_t padded_h, uint32_t out_w, uint32_t out_h, bool temporal);
  void Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t x, uint32_t y);
  nvrhi::BindingSetHandle Set(Pass& pass, const std::vector<nvrhi::BindingSetItem>& items);
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* GraphicsPipeline(bool resolve, int mode, const nvrhi::FramebufferInfo& info);
  void FillConstants(Constants& k, const GtaoInput& in, const GtaoSettings& s, uint32_t padded_w,
                     uint32_t padded_h, uint32_t step) const;

  nvrhi::IDevice* device_;
  bool ready_ = false;
  bool prepared_ = false, normals_smoothed_ = false;
  Stats stats_;

  Pass prepare_, prefilter_, normals_, smooth_h_, smooth_v_, main_, main_low_, main_high_, denoise_, denoise_last_, output_pass_, temporal_;
  nvrhi::ShaderHandle vs_, apply_ps_, apply_resolve_ps_;
  nvrhi::BindingLayoutHandle apply_layout_, apply_resolve_layout_;
  nvrhi::SamplerHandle point_clamp_;
  nvrhi::BufferHandle constants_;
  std::vector<uint8_t> constants_bytes_;  // the last Compute's constants (the apply passes reuse them)

  // Working images (padded_w_ x padded_h_) and the result (out_w_ x out_h_).
  uint32_t padded_w_ = 0, padded_h_ = 0, out_w_ = 0, out_h_ = 0, step_ = 1;
  int32_t origin_x_ = 0, origin_y_ = 0;
  nvrhi::TextureHandle raw_, depth_mips_, normal_map_, term_a_, term_b_, edges_, output_;
  nvrhi::TextureHandle spatial_, temporal_depth_, history_ao_, history_depth_;
  uint64_t history_frame_ = UINT64_MAX;
  GtaoSettings history_settings_;
  AoScreen history_screen_;
  AoProjection history_projection_;
  int32_t history_x_ = 0, history_y_ = 0;
  nvrhi::TextureHandle depth_copy_, scratch_;
  // Binding sets of the passes over the working images (remade with them), and sets keyed by
  // the textures they bind for the passes whose inputs vary.
  std::vector<nvrhi::BindingSetHandle> fixed_sets_;
  std::map<std::tuple<int, nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> sets_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<std::tuple<bool, int, uint32_t>, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
