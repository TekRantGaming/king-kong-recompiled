#pragma once

#include "steam_lobby.h"
#include <optional>

namespace kk::coop {
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

// UI1 (29 Oct, Design 1 "Clear steps"): what the new co-op pages show, as English text keys.
// A chapter's co-op mode in plain words (Play together; Apart, then regroup; Watch) and what it means.
const char* CoopModeName(int chapter);
const char* CoopModeText(int chapter);
// The lobby's step bar: 1 choose what to play, 2 invite a friend, 3 both ready then start.
int CoopLobbyStep(const SocialModel& model);
// Why the host's Start game is locked (key and the name for its {0}); an empty key when Start can be pressed.
CoopStatus CoopStartLock(const SocialModel& model);
// The early access warning before the co-op pages (Craig, 29 Oct): nothing closes it for its first 5 seconds.
class EarlyAccessGate {
 public:
  static constexpr double kSeconds = 5.0;
  void Open(double now) { open_ = true; at_ = now; }
  void Close() { open_ = false; }
  bool open() const { return open_; }
  double opened_at() const { return at_; }
  bool Ready(double now) const { return open_ && now - at_ >= kSeconds; }
  // The countdown shown on Continue: 5, 4, 3, 2, 1, then 0 once it can be closed.
  int SecondsLeft(double now) const;
 private:
  bool open_ = false;
  double at_ = 0;
};
}  // namespace kk::coop
