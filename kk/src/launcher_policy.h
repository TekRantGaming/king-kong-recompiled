// Launcher decisions that need no window, kept here so tests can check every case
// (tests/launcher_logic_tests.cpp).
#pragma once

namespace kk {

// What's new (the notes after an update) and when it may open (owner, 16 Oct): never during the first-time
// setup, only once the game files are there, the shaders are prepared and Original or Remastered is chosen.
struct WhatsNewGate {
  bool pending = false;           // an update was seen: What's new is waiting to open
  bool modal_open = false;        // another pop-up is up
  bool files_ok = false;          // the game files are installed
  bool in_setup = false;          // any setup step: disc, install, artwork, shaders, look, port update
  bool shader_screen = false;     // the shader screen is up (setup, after an update, rebuild)
  bool shaders_busy = false;      // a shader job is running (also one continued in the background)
  bool shaders_wanted = false;    // the native renderer uses the prepared shaders
  bool shaders_finished = false;  // this launcher's shader job has finished
};

inline bool ShowWhatsNewNow(const WhatsNewGate& g) {
  if (!g.pending || g.modal_open || !g.files_ok || g.in_setup || g.shader_screen || g.shaders_busy) return false;
  // The shaders were checked (and prepared if needed) since the launcher opened.
  return !g.shaders_wanted || g.shaders_finished;
}

}  // namespace kk
