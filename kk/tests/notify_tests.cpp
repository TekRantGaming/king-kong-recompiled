// Job J3: when a download end plays the ding and sends a system notification (notify_logic.h), and the
// text that goes into the Windows toast's XML and the freedesktop body. No window, sound or system calls.
#include "notify_logic.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

int main() {
  using kk::notify::Decide;
  using kk::notify::Situation;

  // The launcher in front, nothing asked: its own pop-up and the ding only.
  Situation s;
  auto p = Decide(s);
  Check(p.ding && !p.system, "in front: ding, no system notification");

  // Minimised or behind other windows: a system notification (switch on by default).
  s = {};
  s.minimised = true;
  s.focused = false;
  p = Decide(s);
  Check(p.ding && p.system, "minimised: ding and notification");
  s = {};
  s.focused = false;
  p = Decide(s);
  Check(p.ding && p.system, "not the active window: ding and notification");

  // "Minimise and notify me": always a notification, even with the launcher back in front, and even with
  // the Settings switch off (the button promised it).
  s = {};
  s.asked = true;
  p = Decide(s);
  Check(p.system, "asked, launcher in front: notification");
  s.setting_on = false;
  p = Decide(s);
  Check(p.system, "asked wins over the switch");

  // The switch off: no notification when minimised or in the background; the ding still plays.
  s = {};
  s.setting_on = false;
  s.minimised = true;
  s.focused = false;
  p = Decide(s);
  Check(p.ding && !p.system, "switch off, minimised: ding only");
  s.minimised = false;
  p = Decide(s);
  Check(p.ding && !p.system, "switch off, background: ding only");

  // Muted sounds (switch off or 0 %): no ding in any case; the notification decision is unchanged.
  s = {};
  s.sounds_on = false;
  s.minimised = true;
  p = Decide(s);
  Check(!p.ding && p.system, "sounds off: no ding, notification still sent");
  s = {};
  s.sounds_volume = 0;
  s.asked = true;
  p = Decide(s);
  Check(!p.ding && p.system, "volume 0: no ding");
  s = {};
  s.sounds_volume = 5;
  p = Decide(s);
  Check(p.ding && !p.system, "volume 5 %: ding");

  // Every case has a reason for the log.
  for (int bits = 0; bits < 32; ++bits) {
    Situation t;
    t.asked = bits & 1;
    t.minimised = bits & 2;
    t.focused = bits & 4;
    t.setting_on = bits & 8;
    t.sounds_on = bits & 16;
    const auto q = Decide(t);
    Check(q.why && *q.why, "every decision has a reason");
    // The full table: a notification exactly when asked, or switch on and (minimised or not active).
    Check(q.system == (t.asked || (t.setting_on && (t.minimised || !t.focused))), "decision table");
    Check(q.ding == t.sounds_on, "ding follows the sounds switch");
  }

  // Toast XML: escaped, silent, with the bold title first.
  using kk::notify::ToastXml;
  using kk::notify::XmlEscape;
  Check(XmlEscape("a&b<c>\"d'") == "a&amp;b&lt;c&gt;&quot;d&apos;", "XML escapes");
  Check(XmlEscape(std::string("x\x01y\tz\n")) == "xy\tz\n", "XML drops control characters, keeps tab and newline");
  Check(XmlEscape("Pack de textures HD installé") == "Pack de textures HD installé", "UTF-8 kept");
  const std::string xml = ToastXml("HD texture pack installed", "Settings > Graphics & more");
  Check(xml.find("<text>HD texture pack installed</text><text>Settings &gt; Graphics &amp; more</text>") !=
            std::string::npos,
        "toast texts in order and escaped");
  Check(xml.find("<audio silent=\"true\"/>") != std::string::npos, "toast is silent (the launcher dings)");
  Check(xml.find("activationType=\"foreground\"") != std::string::npos, "toast click activates the launcher");
  Check(ToastXml("T", "").find("<text>T</text></binding>") != std::string::npos, "no empty body line");
  Check(kk::notify::FreedesktopEscape("a<b>&c") == "a&lt;b&gt;&amp;c", "freedesktop body escaped");

  if (failures) return 1;
  std::printf("download notification decisions, toast XML and freedesktop text passed\n");
  return 0;
}
