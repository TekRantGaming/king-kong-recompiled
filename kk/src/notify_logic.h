// Job J3: when a content download or install ends, what the launcher does besides its own pop-up (which
// always shows): a short ding and a system notification (Windows toast, freedesktop notification).
// Pure logic, no platform calls, unit tested (tests/notify_tests.cpp).

#pragma once

#include <string>
#include <string_view>

namespace kk::notify {

struct Situation {
  bool minimised = false;       // the launcher's window is minimised
  bool focused = true;          // the launcher's window is the active window
  bool setting_on = true;       // Settings: "Notify me when downloads finish" (kk_notify_downloads)
  bool sounds_on = true;        // Settings: "Menu sounds" (kk_launcher_sounds)
  int sounds_volume = 50;       // Settings: "Menu sound volume" 0..100
};

struct Plan {
  bool ding = false;            // play the ding (the game's menu sound, as a two-note chime)
  bool system = false;          // send a system notification
  const char* why = "";         // for the log
};

// The ding plays in every case unless the launcher's sounds are muted (off or at 0%). A system
// notification goes out only with the Settings switch on and the launcher minimised or not the active
// window (e.g. the player chose Run in background and went elsewhere): when it is in front, its own pop-up
// is enough. ("Minimise and notify me" was removed, owner 19 Oct.)
inline Plan Decide(const Situation& s) {
  Plan p;
  p.ding = s.sounds_on && s.sounds_volume > 0;
  if (!s.setting_on) {
    p.why = "notifications are switched off in Settings";
  } else if (s.minimised) {
    p.system = true;
    p.why = "the launcher is minimised";
  } else if (!s.focused) {
    p.system = true;
    p.why = "the launcher is not the active window";
  } else {
    p.why = "the launcher is in front (its popup is enough)";
  }
  return p;
}

// Text for XML (the Windows toast's template): & < > " ' escaped; control characters other than tab and
// line breaks dropped (they are not allowed in XML 1.0).
inline std::string XmlEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 16);
  for (const char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
        out += c;
    }
  }
  return out;
}

// The Windows toast: the app's name ("King Kong Recompiled") is its header (the registered display name),
// `title` the bold line, `body` below it. Silent: the launcher plays its own ding. Clicking it activates the
// running launcher (the in-process Activated event), which restores its window.
inline std::string ToastXml(std::string_view title, std::string_view body) {
  std::string xml = "<toast launch=\"kk-open\" activationType=\"foreground\"><visual><binding template=\"ToastGeneric\">";
  xml += "<text>" + XmlEscape(title) + "</text>";
  if (!body.empty()) xml += "<text>" + XmlEscape(body) + "</text>";
  xml += "</binding></visual><audio silent=\"true\"/></toast>";
  return xml;
}

// Freedesktop notification bodies may hold a little markup on servers that support it: escape & < >.
inline std::string FreedesktopEscape(std::string_view in) {
  std::string out;
  for (const char c : in) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
  return out;
}

}  // namespace kk::notify
