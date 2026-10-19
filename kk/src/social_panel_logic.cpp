#include "social_panel_logic.h"
#include <algorithm>

namespace kk::coop {
bool SocialAllowed(bool enabled, SocialPage page) { return enabled && page >= SocialPage::Page && page <= SocialPage::Extras; }
bool SocialPanelState::Update(bool enabled, SocialPage page, bool toggle, bool back, float dt) {
  const bool was = open_;
  if (!SocialAllowed(enabled, page)) open_ = false;
  else if (back && open_) open_ = false;
  else if (toggle) open_ = !open_;
  if (!enabled) progress_ = 0;
  else progress_ = std::clamp(progress_ + (open_ ? 1 : -1) * std::max(0.0f, dt) / 0.2f, 0.0f, 1.0f);
  return !was && open_;
}
const char* SocialFriendState(FriendState state) {
  switch (state) {
    case FriendState::Offline: return "Offline";
    case FriendState::Online: return "Online";
    case FriendState::InGame: return "In game";
    case FriendState::Away: return "Away";
    case FriendState::Busy: return "Busy";
  }
  return "Offline";
}
const char* SocialStatusKey(const SocialModel& m) {
  if (m.last_error == "The host left the lobby" || m.last_error == "The partner left the session" ||
      m.last_error == "Lobby membership changed during the session") return "Your partner left";
  if (m.last_error == "Lobby unavailable or incompatible" || m.last_error == "Lobby compatibility changed")
    return "Versions don't match: {0}";
  if (!m.last_error.empty()) {
    if (m.last_error == "Steam is needed for co-op" || m.last_error == "Steam not available in this build" ||
        m.last_error == "Both players must be ready" || m.last_error == "Could not send invite" ||
        m.last_error == "Steam overlay is unavailable" || m.last_error == "Steam lobby request timed out" ||
        m.last_error == "Steam requests are still pending; restart to retry" ||
        m.last_error == "Gameplay settings changed; open a new lobby") return m.last_error.c_str();
    if (m.last_error == "Could not create lobby" || m.last_error == "Could not request lobby creation") return "Could not create lobby";
    if (m.last_error == "Could not join lobby" || m.last_error == "Could not request lobby join" ||
        m.last_error == "Could not inspect invited lobby") return "Could not join lobby";
    return "Could not complete Social action";
  }
  switch (m.status) {
    case SocialStatus::Disabled: return "Local dev link selected";
    case SocialStatus::Dormant: return "Connecting to Steam";
    case SocialStatus::Unavailable:
      return m.status_text == "Steam not available in this build" ? "Steam not available in this build" : "Steam is needed for co-op";
    case SocialStatus::Creating: return "Creating lobby";
    case SocialStatus::Joining: return "Checking invited lobby";
    case SocialStatus::Starting: case SocialStatus::Connected:
      return m.status_text == "Partner connection lost" ? "Partner connection lost" : "Starting co-op is not ready yet";
    case SocialStatus::Ended: return "Your partner left";
    case SocialStatus::Lobby: return m.self != m.owner ? "Waiting for host" : m.can_start ? "Lobby ready" : "Both players must be ready";
    case SocialStatus::Idle: return "Steam connected";
  }
  return "Steam is needed for co-op";
}
std::vector<SocialRow> SocialRows(const SocialModel& m) {
  std::vector<SocialRow> rows;
  auto add = [&](SocialRowKind kind, SocialId id, std::string label, std::string detail = "", bool enabled = false) {
    rows.push_back({{kind, id}, std::move(label), std::move(detail), enabled});
  };
  const bool idle = m.status == SocialStatus::Idle || m.status == SocialStatus::Ended;
  const bool lobby = m.status == SocialStatus::Lobby && m.lobby != 0;
  const bool free = idle && !m.lobby;
  add(SocialRowKind::Section, 1, "Invites");
  if (m.pending_invites.empty()) add(SocialRowKind::Text, 2, "No invites");
  for (const auto& invite : m.pending_invites) {
    auto f = std::find_if(m.friends.begin(), m.friends.end(), [&](const auto& f) { return f.id == invite.inviter; });
    const std::string name = f == m.friends.end() ? "" : f->name;
    add(SocialRowKind::Accept, invite.lobby, "Accept invite", name, free);
    add(SocialRowKind::Decline, invite.lobby, "Decline invite", name,
        m.status != SocialStatus::Unavailable && m.status != SocialStatus::Disabled && m.status != SocialStatus::Dormant);
  }
  add(SocialRowKind::Section, 3, "Lobby");
  if (!m.lobby) add(SocialRowKind::Create, 0, "Create lobby", "", free);
  else {
    bool ready = false, self_seen = false;
    for (const auto& member : m.members) {
      add(SocialRowKind::Member, member.id, member.name, member.ready ? "Ready" : "Not ready");
      if (member.id == m.self) { ready = member.ready; self_seen = true; }
    }
    add(SocialRowKind::Ready, 0, ready ? "Not ready" : "Ready", "", lobby && self_seen);
    if (m.owner == m.self) add(SocialRowKind::Start, 0, "Start", "", lobby && m.can_start && CanStartLobby(m));
    add(SocialRowKind::OverlayInvite, 0, "Invite friend", "", lobby && m.members.size() < 2 && m.overlay_available);
    add(SocialRowKind::Leave, 0, "Leave lobby", "", true);
  }
  add(SocialRowKind::Section, 4, "Friends");
  std::vector<SocialFriend> friends = m.friends;
  auto rank = [](FriendState s) { return s == FriendState::Offline ? 1 : 0; };
  std::stable_sort(friends.begin(), friends.end(), [&](const auto& a, const auto& b) {
    if (rank(a.state) != rank(b.state)) return rank(a.state) < rank(b.state);
    if (a.name != b.name) return a.name < b.name;
    return a.id < b.id;
  });
  for (const auto& f : friends) if (f.id != m.self) {
    const bool member = std::any_of(m.members.begin(), m.members.end(), [&](const auto& p) { return p.id == f.id; });
    add(SocialRowKind::Friend, f.id, f.name, SocialFriendState(f.state),
        lobby && m.members.size() < 2 && f.state != FriendState::Offline && !member);
  }
  if (friends.empty()) add(SocialRowKind::Section, 5, "No friends to show");
  return rows;
}
int SocialFocus(const std::vector<SocialRow>& rows, std::optional<SocialRowKey> key, int fallback) {
  if (key) for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].focusable() && rows[i].key == *key) return int(i);
  for (int i = std::clamp(fallback, 0, int(rows.size())); i < int(rows.size()); ++i)
    if (rows[size_t(i)].focusable()) return i;
  for (int i = std::min(fallback, int(rows.size()) - 1); i >= 0; --i)
    if (rows[size_t(i)].focusable()) return i;
  return -1;
}
std::optional<SocialCommand> SocialRowCommand(const SocialRow& r, const SocialModel& m) {
  // Revalidate from the latest snapshot, so stale enabled rows cannot send an action.
  const auto current = SocialRows(m);
  auto found = std::find_if(current.begin(), current.end(), [&](const auto& row) { return row.key == r.key; });
  if (found == current.end() || !found->enabled) return {};
  switch (r.key.kind) {
    case SocialRowKind::Accept: return SocialCommand{SocialAction::Accept, r.key.id};
    case SocialRowKind::Decline: return SocialCommand{SocialAction::Decline, r.key.id};
    case SocialRowKind::Create: return SocialCommand{SocialAction::CreateLobby};
    case SocialRowKind::Ready: {
      const auto self = std::find_if(m.members.begin(), m.members.end(), [&](auto p) { return p.id == m.self; });
      return SocialCommand{SocialAction::SetReady, 0, !self->ready};
    }
    case SocialRowKind::Start: return SocialCommand{SocialAction::Start};
    case SocialRowKind::OverlayInvite: return SocialCommand{SocialAction::OverlayInvite};
    case SocialRowKind::Leave: return SocialCommand{SocialAction::Leave};
    case SocialRowKind::Friend: return SocialCommand{SocialAction::Invite, r.key.id};
    default: return {};
  }
}
}  // namespace kk::coop
