#pragma once

#include "steam_lobby.h"
#include <optional>

namespace kk::coop {
// Value-only input policy, shared by the launcher and SDK/UI-free tests.
enum class SocialPage { Page, Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced, About,
                        Achievements, Workshop, PlayMenu, Load, NewGame, Chapters, Extras,
                        Artbook, Setup, Shaders, Modal, Capture };
bool SocialAllowed(bool enabled, SocialPage page);
class SocialTrigger {
 public:
  bool Update(bool down, bool active = true) {
    const bool pressed = down && !down_ && !stale_ && active;
    if (!down) stale_ = false;
    else if (!active) stale_ = true;
    down_ = down;
    return pressed;
  }
 private:
  bool down_ = false, stale_ = false;
};
class SocialPanelState {
 public:
  // Returns true when opening. Ineligible contexts close the drawer.
  bool Update(bool enabled, SocialPage page, bool toggle, bool back, float dt);
  bool open() const { return open_; }
  bool blocking() const { return open_ || progress_ > 0; }
  float slide() const { return progress_ * progress_ * (3 - 2 * progress_); }
 private:
  bool open_ = false;
  float progress_ = 0;
};
enum class SocialRowKind { Section, Member, Accept, Decline, Create, Ready, Start, OverlayInvite, Leave, Friend, Text,
                           AddSteam, DirectAddress, DirectPort, DirectCode, DirectHost, DirectJoin };
struct SocialRowKey {
  SocialRowKind kind = SocialRowKind::Section;
  SocialId id = 0;
  bool operator==(const SocialRowKey&) const = default;
};
struct SocialRow {
  SocialRowKey key;
  std::string label, detail;
  bool enabled = false;
  bool focusable() const { return key.kind != SocialRowKind::Section && key.kind != SocialRowKind::Text; }
};
std::vector<SocialRow> SocialRows(const SocialModel& model);
// Retain identity across friend reordering, duplicate names, invite removal and translation changes.
int SocialFocus(const std::vector<SocialRow>& rows, std::optional<SocialRowKey> key, int fallback = 0);
std::optional<SocialCommand> SocialRowCommand(const SocialRow& row, const SocialModel& model);
const char* SocialFriendState(FriendState state);
// Only stable text keys leave this layer. Persona names are kept out of status/errors.
const char* SocialStatusKey(const SocialModel& model);
}  // namespace kk::coop
