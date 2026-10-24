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
                           AddSteam, DirectAddress, DirectPort, DirectCode, DirectHost, DirectJoin, ChapterSelect,
                           DirectChooseHost, DirectChooseJoin, ConnectSteam, PlaySteam, PlayIP,
                           // C81 Play Online Co-op pages
                           CoopHost, CoopJoin, HostNewGame, HostLoadGame, JoinFriend, ChangeChoice };
struct SocialRowKey {
  SocialRowKind kind = SocialRowKind::Section;
  SocialId id = 0;
  bool operator==(const SocialRowKey&) const = default;
};
struct SocialRow {
  SocialRowKey key;
  std::string label, detail;
  bool enabled = false;
  bool focusable() const { return key.kind != SocialRowKind::Section && (key.kind != SocialRowKind::Text || !detail.empty()); }
};
// 0 chooses a role, 1 prepares a host, 2 enters a host's connection details.
std::vector<SocialRow> SocialRows(const SocialModel& model, int direct_mode = 0, bool ip_path = false);
// Retain identity across friend reordering, duplicate names, invite removal and translation changes.
int SocialFocus(const std::vector<SocialRow>& rows, std::optional<SocialRowKey> key, int fallback = 0);
std::optional<SocialCommand> SocialRowCommand(const SocialRow& row, const SocialModel& model);
const char* SocialFriendState(FriendState state);
// Only stable text keys leave this layer. Persona names are kept out of status/errors.
const char* SocialStatusKey(const SocialModel& model);

// C81: the Play Online Co-op pages under the main menu. Their rows come from the same model and row kinds as
// the Social drawer (SocialRows), so the drawer and the pages never disagree about what can be done.
enum class CoopPage { Hub, Host, HostNew, HostLoad, HostHow, Join, Lobby };
struct CoopPageOptions {
  bool has_saves = false;     // Host > Load game
  bool choice_ready = false;  // Host > How: a chapter or save was picked first
};
std::vector<SocialRow> CoopPageRows(const SocialModel& model, CoopPage page, CoopPageOptions options = {});
// Which page to show next frame: a lobby (created or joined) always shows the lobby page; a lobby page whose
// lobby has gone (and is not being created or joined) goes back to the first page.
CoopPage CoopPageFor(const SocialModel& model, CoopPage current);
// The command for a page row, revalidated against the latest model (Accept, Join friend, Ready, Start...).
// Rows the launcher handles itself (page moves, editors, Direct IP host/join) return nothing.
std::optional<SocialCommand> CoopRowCommand(const SocialRow& row, const SocialModel& model, CoopPage page);
// The lobby's plain status line: an English text key and the name that fills its {0} (empty when none).
struct CoopStatus { const char* key = ""; std::string name; };
CoopStatus CoopLobbyStatus(const SocialModel& model, SocialId invited = 0);
// The Steam startup lobby (+connect_lobby): Steam lists it as an invite without an inviter.
SocialId StartupInvite(const SocialModel& model);
}  // namespace kk::coop
