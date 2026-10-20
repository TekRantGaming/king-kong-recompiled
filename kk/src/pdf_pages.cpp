// The user manual's pages through PDFium: see pdf_pages.h.
#include "pdf_pages.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace kk::pdf {
namespace {

// PDFium's C interface (public/fpdfview.h), only what the manual needs. Every handle is a pointer.
struct Api {
  void (*InitLibrary)() = nullptr;
  void* (*LoadMemDocument64)(const void* data, size_t size, const char* password) = nullptr;
  unsigned long (*GetLastError)() = nullptr;
  int (*GetPageCount)(void* doc) = nullptr;
  void* (*LoadPage)(void* doc, int index) = nullptr;
  float (*GetPageWidthF)(void* page) = nullptr;
  float (*GetPageHeightF)(void* page) = nullptr;
  void* (*BitmapCreateEx)(int width, int height, int format, void* first_scan, int stride) = nullptr;
  int (*BitmapFillRect)(void* bitmap, int left, int top, int width, int height, uint32_t color) = nullptr;
  void (*RenderPageBitmap)(void* bitmap, void* page, int x, int y, int w, int h, int rotate, int flags) = nullptr;
  void* (*BitmapGetBuffer)(void* bitmap) = nullptr;
  int (*BitmapGetStride)(void* bitmap) = nullptr;
  void (*BitmapDestroy)(void* bitmap) = nullptr;
  void (*ClosePage)(void* page) = nullptr;
  void (*CloseDocument)(void* doc) = nullptr;
};

constexpr int kBitmapBgra = 4;     // FPDFBitmap_BGRA
constexpr int kRenderAnnot = 0x01; // FPDF_ANNOT: the page's annotations too, as a reader shows them

std::mutex g_mutex;  // PDFium is not thread safe
Api g_api;
bool g_tried = false, g_ok = false;
std::string g_why;

std::filesystem::path ProgramDir() {
#if defined(_WIN32)
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return n ? std::filesystem::path(std::wstring(exe, n)).parent_path() : std::filesystem::path();
#else
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : exe.parent_path();
#endif
}

template <typename F>
bool Find(void* lib, const char* name, F& out) {
#if defined(_WIN32)
  out = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)));
#else
  out = reinterpret_cast<F>(dlsym(lib, name));
#endif
  return out != nullptr;
}

// Under g_mutex.
bool LoadLocked() {
  if (g_tried) return g_ok;
  g_tried = true;
  void* lib = nullptr;
#if defined(_WIN32)
  const auto file = ProgramDir() / L"pdfium.dll";
  lib = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
  const auto file = ProgramDir() / "libpdfium.so";
  lib = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
  if (!lib) {
    g_why = "no " + file.filename().string() + " beside the program";
    return false;
  }
  Api& a = g_api;
  const bool all = Find(lib, "FPDF_InitLibrary", a.InitLibrary) && Find(lib, "FPDF_LoadMemDocument64", a.LoadMemDocument64) &&
                   Find(lib, "FPDF_GetLastError", a.GetLastError) && Find(lib, "FPDF_GetPageCount", a.GetPageCount) &&
                   Find(lib, "FPDF_LoadPage", a.LoadPage) && Find(lib, "FPDF_GetPageWidthF", a.GetPageWidthF) &&
                   Find(lib, "FPDF_GetPageHeightF", a.GetPageHeightF) && Find(lib, "FPDFBitmap_CreateEx", a.BitmapCreateEx) &&
                   Find(lib, "FPDFBitmap_FillRect", a.BitmapFillRect) && Find(lib, "FPDF_RenderPageBitmap", a.RenderPageBitmap) &&
                   Find(lib, "FPDFBitmap_GetBuffer", a.BitmapGetBuffer) && Find(lib, "FPDFBitmap_GetStride", a.BitmapGetStride) &&
                   Find(lib, "FPDFBitmap_Destroy", a.BitmapDestroy) && Find(lib, "FPDF_ClosePage", a.ClosePage) &&
                   Find(lib, "FPDF_CloseDocument", a.CloseDocument);
  if (!all) {
    g_why = file.filename().string() + " is not a PDFium library this program can use";
    return false;  // the library stays loaded (harmless)
  }
  a.InitLibrary();  // kept for the program's life
  g_ok = true;
  return true;
}

}  // namespace

bool Available(std::string* why) {
  std::lock_guard lock(g_mutex);
  const bool ok = LoadLocked();
  if (!ok && why) *why = g_why;
  return ok;
}

std::unique_ptr<Document> Document::Open(const std::filesystem::path& file, std::string& why) {
  std::unique_ptr<Document> d(new Document());
  {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
      why = "cannot read the file";
      return nullptr;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > (256ll << 20)) {
      why = "the file's size is not usable";
      return nullptr;
    }
    d->bytes_.resize(size_t(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(d->bytes_.data()), size);
    if (!in) {
      why = "cannot read the file";
      return nullptr;
    }
  }
  std::lock_guard lock(g_mutex);
  if (!LoadLocked()) {
    why = g_why;
    return nullptr;
  }
  const Api& a = g_api;
  d->doc_ = a.LoadMemDocument64(d->bytes_.data(), d->bytes_.size(), nullptr);
  if (!d->doc_) {
    why = "PDFium could not open it (error " + std::to_string(a.GetLastError()) + ")";
    return nullptr;
  }
  const int count = std::clamp(a.GetPageCount(d->doc_), 0, 1000);
  for (int i = 0; i < count; ++i) {
    PageSize size;
    if (void* page = a.LoadPage(d->doc_, i)) {
      size.width = a.GetPageWidthF(page);
      size.height = a.GetPageHeightF(page);
      a.ClosePage(page);
    }
    if (!(size.width > 1 && size.height > 1)) size = {612, 792};  // a page that does not load: a letter page
    d->sizes_.push_back(size);
  }
  if (d->sizes_.empty()) {
    why = "the document has no pages";
    return nullptr;
  }
  return d;
}

Document::~Document() {
  if (!doc_) return;
  std::lock_guard lock(g_mutex);
  g_api.CloseDocument(doc_);
}

Bitmap Document::Render(int page, int max_width, int max_height) const {
  Bitmap out;
  if (page < 0 || page >= PageCount() || max_width <= 0 || max_height <= 0) return out;
  const PageSize size = sizes_[size_t(page)];
  const double scale = std::min(double(max_width) / size.width, double(max_height) / size.height);
  const int w = std::max(1, int(std::lround(size.width * scale)));
  const int h = std::max(1, int(std::lround(size.height * scale)));
  std::lock_guard lock(g_mutex);
  const Api& a = g_api;
  void* p = a.LoadPage(doc_, page);
  if (!p) return out;
  void* bitmap = a.BitmapCreateEx(w, h, kBitmapBgra, nullptr, 0);
  if (!bitmap) {
    a.ClosePage(p);
    return out;
  }
  a.BitmapFillRect(bitmap, 0, 0, w, h, 0xFFFFFFFFu);  // paper white
  a.RenderPageBitmap(bitmap, p, 0, 0, w, h, 0, kRenderAnnot);
  const auto* src = static_cast<const uint8_t*>(a.BitmapGetBuffer(bitmap));
  const int stride = a.BitmapGetStride(bitmap);
  if (src && stride >= w * 4) {
    out.width = w;
    out.height = h;
    out.rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
      const uint8_t* s = src + size_t(y) * size_t(stride);
      uint8_t* d = out.rgba.data() + size_t(y) * w * 4;
      for (int x = 0; x < w; ++x, s += 4, d += 4) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
      }
    }
  }
  a.BitmapDestroy(bitmap);
  a.ClosePage(p);
  return out;
}

}  // namespace kk::pdf
