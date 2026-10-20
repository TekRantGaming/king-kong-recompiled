// Task 332 (owner, 20 Oct, DECISIONS row B): the rules of the startup movies the launcher plays (startup_movies.h),
// without a window. After the first-time setup (Finish setup or Skip all) they ALWAYS play once, with a pop-up first
// when kk_skip_intros is on (its choice counts from the next start); later the launcher opened from the desktop plays
// them unless kk_skip_intros is on. The game's three logo movies (the ones intros.cpp skips at the game's own boot),
// from the player's game files, then the owner's TRG Studios movie last (shipped in launcher/video). The save notice
// loop always follows them (loading_loop_logic.h).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kk::startup_movies {

// Whether the movies play: always right after the setup, else unless the player skips them.
inline bool Play(bool after_setup, bool skip_intros) { return after_setup || !skip_intros; }

// The pop-up before them ("The startup movies play this first time..."): after the setup with skip on.
inline bool AskFirst(bool after_setup, bool skip_intros) { return after_setup && skip_intros; }

// A launcher that opens plays them itself when it was opened from the desktop (`open_on` empty: not one the game
// started on its way back, nor one restarted for a shader rebuild), with the game files there and no first-time setup
// waiting (that one plays them when it is done).
inline bool AtLauncherStart(const std::string& open_on, bool files_ok, bool setup_pending, bool skip_intros) {
  return open_on.empty() && files_ok && !setup_pending && Play(false, skip_intros);
}

// In order: the game's logos (from the game files: <game>/Video), then the TRG Studios movie (<program>/launcher/video).
inline std::vector<std::filesystem::path> Files(const std::filesystem::path& game_dir,
                                                const std::filesystem::path& program_dir) {
  return {game_dir / "Video" / "Ubisoft.wmv", game_dir / "Video" / "Universal.wmv", game_dir / "Video" / "Wingnut.wmv",
          program_dir / "launcher" / "video" / "trg-startup.mp4"};
}

// Where the picture is, in seconds since the movie's first frame: the sound's position while it plays (so the picture
// keeps with it), else the launcher's own frame clock carried on from there (a movie without sound, or after it).
inline double Clock(double own, double sound, bool sound_playing) { return sound_playing ? sound : own; }

}  // namespace kk::startup_movies
