// Task 357 (owner, 22 Oct): the owner's Easter egg game (launcher\easter-egg\ beside the program), started from
// the launcher's Home screen with a famous controller code. The launcher keeps reading the controller while it
// runs: Start brings the launcher to the front with a pause panel (Resume, Quit to main menu).
#pragma once

#include <filesystem>
#include <memory>

namespace kk::egg {

// launcher\easter-egg\LegallyDistinctGorilla.exe beside the program (Windows only; empty elsewhere).
std::filesystem::path AppPath();

// The game's process (and the processes it starts, in one job: none outlive the launcher). Windows only;
// elsewhere Start fails.
class Process {
 public:
  Process();
  ~Process();
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  // Starts it with its own folder as the working folder. False (logged) when it could not start.
  bool Start(const std::filesystem::path& exe);
  // Started and not yet ended.
  bool Running();
  // Its window is up (shown on screen).
  bool WindowShown();
  // Its window in front (the launcher is in front then, so Windows allows it).
  void ToFront();
  // Asks its windows to close (the game quits as when its window is closed).
  void RequestClose();
  // Ends it and everything it started.
  void Terminate();

 private:
  struct State;
  std::unique_ptr<State> s_;
};

// Brings a window (an HWND) in front of everything else, minimised or not, even while another program (the
// game) has the foreground. True when it is the foreground window afterwards.
bool ForceToFront(void* hwnd);

}  // namespace kk::egg
