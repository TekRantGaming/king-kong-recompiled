// The game renderer's state for the temporal upscalers (game_upscale.cpp, docs/upscalers.md): the
// tracker that finds the upscale point, the jitter, the draws the motion pass replays, the inputs the
// upscaler reads, and the passes that make them. Null in GameRenderer when --native_upscaler is off.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_classifier.h"
#include "backend/ao_timer.h"
#include "backend/upscale_config.h"
#include "backend/upscale_frame.h"
#include "backend/upscale_jitter.h"
#include "backend/upscale_motion.h"
#include "backend/upscaler.h"
#include "kknr/guest_texture.h"
#include "kkshaders/abi.h"

namespace nr {

struct GameShader;

struct GameUpscale {
  UpscaleConfig config;
  UpscalerKind kind = UpscalerKind::kOff;  // what runs (after the fallbacks)
  std::unique_ptr<Upscaler> upscaler;      // null: the bilinear stretch
  std::string name;                        // for the log
  float in_x = 1, in_y = 1;                // the input generation's scale (host pixels per guest pixel)
  float out_x = 1, out_y = 1;              // the output's (the render scale)
  uint32_t jitter_phases = 8;
  float mip_bias = 0;

  UpscaleRules rules;
  AoRuleTable classifier;
  UpscaleFrameTracker tracker;
  MotionHistory history;
  CameraReprojectionVote vote;

  // This frame.
  uint64_t frame = UINT64_MAX;   // the game's frame number (DrawCall::frame)
  Jitter jitter;
  bool upscaled = false;          // the upscale point was taken
  bool snapshot_due = false, snapshot_taken = false;  // the opaque pass's picture for the reactive mask
  bool screen_valid = false;
  MotionScreen screen;
  bool camera_valid = false;      // a projection for FSR's heuristics
  float camera_near = 0.05f, camera_far = 100000.0f, camera_fov_y = 1.0f;
  uint32_t scene_draws_since_copy = 0;

  // A draw the motion pass replays at the upscale point.
  struct MotionDraw {
    MotionDrawKey key;
    uint32_t ordinal = 0;
    std::shared_ptr<const GameShader> vs;
    VsConstants constants{};
    int wvp_register = -1;
    kkshaders::DrawConstants dc{};
    uint32_t topology = 0, mode_control = 0, clip_control = 0, poly_scale = 0, poly_offset = 0;
    bool flip_front = false;
    bool indexed = false;
    // Its vertex buffer is one the game rewrites and moves (streamed geometry, the V-Rex dinosaur):
    // matched by shader and size alone (native_upscaler_debug 1024: camera motion only, not replayed).
    bool camera_only = false;
    nvrhi::IndexBufferBinding index;
    nvrhi::DrawArguments args;
    nvrhi::Viewport viewport;
    nvrhi::Rect scissor;
  };
  std::vector<MotionDraw> motion_draws;

  // A full-frame copy of the scene target made since the scene's last draw: done again from the
  // upscaled picture at the upscale point (the post effects read it).
  struct SceneCopy {
    kknr::TextureFetch fetch;
    int32_t sx = 0, sy = 0, w = 0, h = 0, dx = 0, dy = 0;
    uint32_t level = 0, slice = 0, format = 0;
  };
  std::vector<SceneCopy> scene_copies;

  // The previous upscaled frame (for the reset): the game frame it was, and the output size.
  uint64_t last_upscaled_frame = UINT64_MAX;
  UpscalerSize last_output;

  // GPU: the inputs (the scene target's size), the output, the passes.
  nvrhi::TextureHandle motion, depth, reactive, snapshot, output;
  nvrhi::ShaderHandle vs_fullscreen, camera_ps, object_ps, reactive_ps, depth_ps, stretch_ps, show_motion_ps;
  nvrhi::BindingLayoutHandle pass_layout;       // push constants b0, t0, t1, s0
  nvrhi::BindingLayoutHandle motion_layout;     // set 0 of the motion pass: b0, b2, b5
  nvrhi::BindingSetHandle motion_set;
  nvrhi::BufferHandle motion_vs_constants, motion_draw_constants, motion_constants;
  nvrhi::SamplerHandle linear_clamp;
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> pass_sets;
  std::unordered_map<uint64_t, nvrhi::FramebufferHandle> framebuffers;
  // --native_upscaler_gpu_time: everything at the upscale point, and the upscaler's own work.
  std::unique_ptr<AoGpuTimer> timer, sdk_timer;
  // Debugging (native_upscaler_debug 512): the motion texture read back now and then.
  nvrhi::StagingTextureHandle probe;
  uint64_t probe_frame = 0;
  bool running = false;  // RunUpscale is making the output generation's scene targets

  struct Stats {
    uint64_t frames = 0;          // frames upscaled
    uint64_t motion_draws = 0;    // draws recorded for the motion pass
    uint64_t motion_rewritten = 0;  // of them, on vertex buffers the game rewrites (camera vote only)
    uint64_t motion_matched = 0;  // with last frame's constants
    uint64_t motion_drawn = 0;    // replayed (variant and pipeline ready)
    uint64_t motion_waiting = 0;  // variant or pipeline not ready yet
    uint64_t camera_frames = 0;   // frames with a camera reprojection
    uint64_t no_depth = 0;        // frames whose scene depth was gone at the point
    uint64_t resets = 0;
    uint64_t cuts = 0;            // of them, camera cuts (the reprojection jumps)
    uint64_t scene_copies = 0;    // copies done again from the upscaled picture
    uint64_t migrations = 0;      // targets carried to the output size
    uint64_t failures = 0;        // the upscaler failed (the bilinear stretch instead)
  };
  Stats stats, logged;
};

}  // namespace nr
