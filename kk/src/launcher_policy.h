// Launcher decisions that need no window, kept here so tests can check every case
// (tests/launcher_logic_tests.cpp).
#pragma once

namespace kk {

// What's new (the notes after an update) and when it may open (owner, 16 Oct; job J6, 17 Oct): never during the
// first-time setup (any of its pages, up to Finish setup), never over the one-time New in this update screen,
// only once the game files are there and the shaders are prepared. It then opens once on the main menu: after
// Finish setup on a new install, after the New screen's Continue for a player who updated.
struct WhatsNewGate {
  bool pending = false;           // an update was seen, or the setup has just finished: What's new is waiting
  bool modal_open = false;        // another pop-up is up
  bool files_ok = false;          // the game files are installed
  bool in_setup = false;          // any setup page: disc, install, artwork, shaders, downloads, graphics, prompts,
                                  // all set, the New screen, port update
  bool new_screen = false;        // the New in this update screen is up (or waits to come back from its graphics pages)
  bool shader_screen = false;     // the shader screen is up (setup, after an update, rebuild)
  bool shaders_busy = false;      // a shader job is running (also one continued in the background)
  bool shaders_wanted = false;    // the native renderer uses the prepared shaders
  bool shaders_finished = false;  // this launcher's shader job has finished
};

inline bool ShowWhatsNewNow(const WhatsNewGate& g) {
  if (!g.pending || g.modal_open || !g.files_ok || g.in_setup || g.new_screen || g.shader_screen || g.shaders_busy)
    return false;
  // The shaders were checked (and prepared if needed) since the launcher opened.
  return !g.shaders_wanted || g.shaders_finished;
}

}  // namespace kk
