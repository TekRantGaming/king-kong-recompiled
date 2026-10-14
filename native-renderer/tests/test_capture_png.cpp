// Synthetic WIC tests only. No game runtime or GPU is started.
#include "../../kk/src/capture_png.cpp"
int main(int argc, char** argv) {
  if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
  rex::ui::RawImage image;
  if (kk::art::detail::SaveCapturePng(image, argv[1])) return 2;
  image.width = 7; image.height = 3; image.stride = 36;
  image.data.resize(image.stride * (image.height - 1) + image.width * 4);
  for (unsigned y = 0; y < image.height; ++y) {
    for (unsigned x = 0; x < image.width; ++x) {
      auto* p = image.data.data() + image.stride * y + x * 4;
      p[0] = uint8_t(10 + x); p[1] = uint8_t(50 + y); p[2] = uint8_t(100 + x + y);
      p[3] = uint8_t(x * y);  // RGBX padding must not become transparency.
    }
  }
  if (!kk::art::detail::SaveCapturePng(image, argv[1])) return 3;
  image.data.pop_back();
  if (kk::art::detail::SaveCapturePng(image, argv[1])) return 4;
  CoUninitialize();
  return 0;
}
