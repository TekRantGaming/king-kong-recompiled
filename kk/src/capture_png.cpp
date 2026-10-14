#include "capture_png.h"
#include <cstring>
#include <rex/ui/presenter.h>
#if defined(_WIN32)
#include <wincodec.h>
#include <wrl/client.h>
namespace kk::art::detail {
bool SaveCapturePng(const rex::ui::RawImage& image, const std::filesystem::path& path) {
  using Microsoft::WRL::ComPtr;
  if (!image.width || !image.height || image.stride < image.width * 4 ||
      image.data.size() < size_t(image.stride) * (image.height - 1) + size_t(image.width) * 4) return false;
  std::error_code ec;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
  auto pending = path;
  pending += ".writing";
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(pending.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->SetSize(image.width, image.height))) return false;
  // RawImage is RGBX, not RGBA. Drop the undefined X byte on the encoder worker.
  std::vector<uint8_t> rgb(size_t(image.width) * image.height * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    uint8_t* dst = rgb.data() + size_t(y) * image.width * 3;
    for (uint32_t x = 0; x < image.width; ++x, src += 4, dst += 3)
      dst[0] = src[2], dst[1] = src[1], dst[2] = src[0];
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR ||
      FAILED(frame->WritePixels(image.height, image.width * 3, UINT(rgb.size()), rgb.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
  frame.Reset();
  encoder.Reset();
  stream.Reset();
  std::filesystem::rename(pending, path, ec);
  return !ec;
}

}  // namespace kk::art::detail
#endif
