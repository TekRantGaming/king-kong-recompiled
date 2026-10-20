// Beta 7 Extras: the Weta Artbook page (Play > Extras > Artbook; owner, 17 Oct 2026, DECISIONS "Beta 7 Extras:
// Artbook + screensaver"). A grid of the six concept paintings from the PS2 bonus disc with the disc's readme as
// a caption, a full-screen viewer (zoom and pan with the controller's triggers and sticks, the mouse's wheel and
// drag, or the keyboard; previous / next) and Save to PC (the original JPEG into Pictures\King Kong Artbook).
// The pictures are never shipped: the content service downloads the disc in the background (content.h,
// Kind::kArtbook); until it is there the page says so and offers Download now.
//
// Pictures are decoded on the page's own worker thread; the UI thread only makes textures (at most two a
// frame), so the launcher never waits for the disk.
//
// Task 333 (owner, 20 Oct, Beta 13): the same page and viewer show the game's Xbox 360 user manual (Book::kManual,
// Extras > User manual): its PDF (content::Kind::kManual, downloaded in the background like the artbook) is drawn
// page by page by PDFium (pdf_pages.h) on the same worker. The manual has no readme and no Save to PC.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_ui.h"

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

class Artbook {
 public:
  enum class Book { kArtbook, kManual };
  Artbook(rex::ui::ImmediateDrawer* drawer, std::filesystem::path user_dir, Book book = Book::kArtbook);
  ~Artbook();
  Artbook(const Artbook&) = delete;
  Artbook& operator=(const Artbook&) = delete;

  // The page between p0 and p1 (below the header, above the prompts); the viewer covers v0..v1 (the window).
  // `active`: the page has the input (no pop-up open).
  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 v0, ImVec2 v1, const ui::Metrics& m, ui::Input& in,
            bool active);
  // From the viewer back to the grid (true); false on the grid (the launcher goes back to Extras).
  bool Back();
  std::vector<ui::Prompt> Prompts() const;
  // Developer steps (artbook:<command>): state, open:<n>, next, prev, zoom:<x>, pan:<dx>:<dy>, save, back,
  // download.
  void DevCommand(const std::string& command);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
