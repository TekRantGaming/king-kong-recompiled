// Job J3: system notifications when a content download or install ends.
// Windows 11: a toast (WinRT ToastNotificationManager) under the app ID "KingKongRecompiled", registered
// for the current user only (HKCU\Software\Classes\AppUserModelId\KingKongRecompiled, DisplayName), so a
// portable copy needs no installer or Start menu shortcut; the process's own taskbar ID is not changed.
// If toasts cannot be used, a notification-area balloon (Shell_NotifyIcon), which Windows 11 also shows as
// a notification. Linux: org.freedesktop.Notifications over D-Bus (libdbus loaded at run time), else
// notify-send, else nothing (the launcher's own pop-up still shows).
// Everything runs on one worker thread: Show() returns at once and never blocks the UI thread.

#pragma once

#include <functional>
#include <string>

namespace kk::notify {

inline constexpr const char* kAppName = "King Kong Recompiled";
inline constexpr const wchar_t* kAppIdW = L"KingKongRecompiled";

using LogFn = std::function<void(bool warning, const std::string& line)>;

// Starts the worker (once). `on_click` runs on the worker when the player clicks a notification (where the
// system tells the app); it should post to the UI thread. Calling it again replaces both functions.
void Start(std::function<void()> on_click, LogFn log);

// Sends a notification (`tag` names it: a newer one with the same tag replaces it where supported). The
// outcome is logged: "KK notify: shown as <toast|balloon|freedesktop|notify-send> ..." or why not.
void Show(std::string title, std::string body, std::string tag);

// Windows: logs how many of this app's notifications the notification centre holds (a check that does not
// look at the screen). Elsewhere logs that it is not available.
void LogHistory();

// Stops the worker, removes a balloon icon if one is up. Notifications already shown stay.
void Stop();

}  // namespace kk::notify
