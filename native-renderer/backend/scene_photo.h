// Photo mode's depth of field (task 208, docs/photo-mode.md): the scene's view-space depth, prepared at the
// end of its opaque pass (the moment the cel shading and the AO use, found the same way: the AO's rule table
// and frame tracker, the camera voted from the scene draws' constants), for the photo grade at the end of
// the frame. Only while photo mode's depth of field is on; nothing else in the frame changes.
//
// NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/gtao.h"
#include "backend/photo_grade.h"
#include "backend/scene_ao.h"

namespace nr {

class ScenePhotoDepth {
 public:
  ScenePhotoDepth(nvrhi::IDevice* device, std::vector<AoRule> rules);
  ~ScenePhotoDepth();
  bool Initialize();
  void ForgetTargets();

  // A draw as the ambient occlusion describes it, before it is recorded.
  void OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d);
  void OnDepthClear(uint64_t frame, uint64_t depth_key);

  // The depth prepared in `frame` (or the frame before: the picture is still), for a picture of
  // `width` x `height`; an empty Depth when there is none.
  PhotoGrade::Depth DepthFor(uint64_t frame, uint32_t width, uint32_t height) const;

 private:
  void NewFrame(uint64_t frame);

  nvrhi::IDevice* device_;
  AoRuleTable rules_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  std::unique_ptr<Gtao> gtao_;
  uint64_t frame_ = UINT64_MAX, prepared_frame_ = UINT64_MAX;
  bool applied_ = false;
  uint32_t prepared_w_ = 0, prepared_h_ = 0, step_ = 1;  // the picture prepared, host pixels, and its step
  uint64_t prepared_ = 0, warned_ = 0;
  SceneAo::Target scene_depth_;
};

}  // namespace nr
