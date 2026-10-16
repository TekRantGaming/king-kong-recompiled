// The window handoff between two runs of the program (owner, 16 Oct: never a desktop gap). The launcher and
// the game go to each other by starting the program again (play_session.cpp, RelaunchWith): Play with
// settings that need a restart, Return to main menu, the end of a chapter-select run, an Extras bonus. The
// old process used to close its window at once, and the desktop showed until the new one had drawn its
// first picture. Now the old window stays up (the launcher's last picture, or the game's black cover) until
// the new process has presented its first frame, and only then does the old process quit.
//
//   parent: BeforeChild() just before CreateProcess (the child inherits an event's name in its
//           environment), ChildStarted(process) after it, then QuitWhenChildShown(quit).
//   child:  ArmChild() at startup; the first presented frame (the SDK's present observer) signals the event.
// The wait ends when the child signals, exits, or after kTimeoutSeconds, whichever comes first.
#pragma once

#include <functional>

namespace kk::handoff {

constexpr double kTimeoutSeconds = 20.0;

// Parent side.
void BeforeChild();
void ChildStarted(void* process_handle);  // takes the handle (closed when the wait ends)
void ChildFailed();                       // CreateProcess failed: nothing to wait for
// Calls `quit` (on a thread of its own) once the child is shown, or at once when no child was started. While
// it waits, the old window is kept above the new one when it is the foreground window (never otherwise:
// test windows behind other programs stay behind them).
void QuitWhenChildShown(std::function<void()> quit);

// Child side: called once, early. Does nothing unless this process was started by a handoff.
void ArmChild();
// A frame was presented (the present observer; also called from the benchmark's own observer).
void OnPresent();

// Log lines with a shared clock (milliseconds since the Unix epoch), for gap measurements.
long long NowMs();

}  // namespace kk::handoff
