// Beta 13 Extras, the user manual (owner, 20 Oct 2026, DECISIONS "Beta 13 additions" D): the game's Xbox 360 manual,
// a PDF the launcher downloads at run time, shown page by page in the artbook's viewer (artbook.h, Book::kManual).
//
// The pages are drawn by PDFium (BSD 3-Clause / Apache 2.0; the bblanchon/pdfium-binaries build, no JavaScript, no
// XFA), the library pdfium.dll (Windows) or libpdfium.so (Linux) beside the program. It is loaded when the manual is
// first shown, so a build or package without it still runs: the manual page then says it cannot be shown.
// PDFium is not thread safe: every call here holds one lock.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kk::pdf {

// The library beside the program loads (once; false and `why` when it is missing or not PDFium).
bool Available(std::string* why = nullptr);

struct PageSize {
  float width = 0, height = 0;  // points, after the page's own rotation
};

// RGBA pixels, rows top to bottom.
struct Bitmap {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  explicit operator bool() const { return width > 0 && height > 0; }
};

class Document {
 public:
  // The whole file is read into memory (a manual is a few MB). False and `why` when it does not open.
  static std::unique_ptr<Document> Open(const std::filesystem::path& file, std::string& why);
  ~Document();
  Document(const Document&) = delete;
  Document& operator=(const Document&) = delete;

  int PageCount() const { return int(sizes_.size()); }
  PageSize Size(int page) const { return page >= 0 && page < PageCount() ? sizes_[size_t(page)] : PageSize{}; }
  // The page drawn on white to fit inside max_width x max_height pixels (its shape kept).
  Bitmap Render(int page, int max_width, int max_height) const;

 private:
  Document() = default;
  std::vector<uint8_t> bytes_;  // PDFium reads from it while the document is open
  void* doc_ = nullptr;
  std::vector<PageSize> sizes_;
};

}  // namespace kk::pdf
