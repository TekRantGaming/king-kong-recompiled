// The temporal upscalers behind one interface (docs/upscalers.md): AMD FSR 3.1 and FSR 4 (FidelityFX
// API), NVIDIA DLSS Super Resolution and DLAA (NGX, the DLSS SDK), Intel XeSS Super Resolution (XeSS
// SDK). The game renderer (game_upscale.cpp) prepares the inputs at its upscale point and asks one of
// these to write the output; everything SDK-specific stays in upscaler_*.cpp.
//
// Every SDK is loaded at run time from the folder of the game's executable (the launcher's updater
// installs only files beside king_kong.exe): amd_fidelityfx_vk.dll / amd_fidelityfx_dx12.dll (FSR 3.1),
// amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll (FSR 4 and FSR 3.1.5 on D3D12),
// nvngx_dlss.dll (DLSS), libxess.dll (XeSS). A missing file or an unsupported GPU makes CreateUpscaler
// fall back (FSR 4 to FSR 3.1, anything to FSR 3.1, FSR 3.1 to the plain bilinear stretch) and say why.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nvrhi/nvrhi.h>

#include "backend/upscale_config.h"

namespace nr {

struct UpscalerSize {
  uint32_t width = 0, height = 0;
};

// What one upscale needs. Input textures are as big as the scene target; the picture is the top-left
// `render` rectangle of them (the scene target can be taller than the frame).
struct UpscalerInputs {
  nvrhi::ITexture* color = nullptr;     // the scene (input size), gamma-encoded LDR as the game draws it
  nvrhi::ITexture* depth = nullptr;     // R32_FLOAT: the scene's depth (0 near, 1 far)
  nvrhi::ITexture* motion = nullptr;    // R16G16_FLOAT: last frame's position minus this frame's, input pixels
  nvrhi::ITexture* reactive = nullptr;  // R8_UNORM 0..1 (null: none): favour this frame's colour
  nvrhi::ITexture* output = nullptr;    // the output size, the colour's format, usable as a UAV
  UpscalerSize render;                  // the picture's size in the inputs
  UpscalerSize output_size;
  float jitter_x = 0, jitter_y = 0;     // input pixels, the picture's shift (+x right, +y down)
  bool reset = false;                   // a cut: forget the history
  float frame_ms = 33.3f;
  float sharpness = -1.0f;              // 0..1, negative: none
  float camera_near = 0.05f, camera_far = 100000.0f, camera_fov_y = 1.0f;  // FSR 3.1's heuristics
};

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  // "FSR 3.1.4 (Vulkan)", "DLSS 310.9.1 preset K", "XeSS 3.0.2".
  virtual std::string Name() const = 0;
  // The input size the SDK wants for an output size in the configured quality mode.
  virtual UpscalerSize InputSize(UpscalerSize output) const = 0;
  // Records the upscale into `cl` (an NVRHI command list, recording). The SDK's context is made on the
  // first call and again when the sizes change. False when it failed (logged).
  virtual bool Evaluate(nvrhi::ICommandList* cl, const UpscalerInputs& in) = 0;
  // The SDK's GPU memory, when it says (bytes; 0 unknown).
  virtual uint64_t GpuMemory() const { return 0; }
};

// What this device can run (for the launcher and the log; docs/upscalers.md, "Who gets what").
struct UpscalerSupport {
  uint32_t vendor = 0;  // PCI vendor: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
  bool fsr3 = false, fsr4 = false, dlss = false, xess = false;
  std::string notes;    // why something is missing
};
UpscalerSupport QueryUpscalerSupport(nvrhi::IDevice* device);

// The upscaler for `config` on `device`. kAuto: DLSS on NVIDIA RTX, FSR 4 on supported AMD, XeSS on
// Intel, FSR 3.1 otherwise. An upscaler that cannot run here falls back (FSR 4 to FSR 3.1, DLSS and
// XeSS to FSR 3.1); null when nothing can (kBilinear, kOff, or FSR 3.1 missing too), with `chosen` set to
// what runs and `why` saying what happened.
std::unique_ptr<Upscaler> CreateUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, UpscalerKind* chosen,
                                         std::string* why);

// The folder of the game's executable (where the SDKs' DLLs are), with a trailing separator.
std::wstring UpscalerDllFolder();

// The individual SDKs (upscaler_ffx.cpp, upscaler_dlss.cpp, upscaler_xess.cpp). Null with `why` when
// the SDK is not built in, its DLL is missing, or the GPU or API cannot run it.
std::unique_ptr<Upscaler> CreateFsr3Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why);
std::unique_ptr<Upscaler> CreateFsr4Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why);
std::unique_ptr<Upscaler> CreateDlssUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why);
std::unique_ptr<Upscaler> CreateXessUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why);

}  // namespace nr
