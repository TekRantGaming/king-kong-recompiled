#include "steam_lobby.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace kk::coop {
namespace {
bool BoundedValue(const std::string& s) {
  return !s.empty() && s.size() <= 128 &&
      std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c < 127; });
}
}
LobbyKeys EncodeLobbyKeys(const Compatibility& c) {
  if (!c.protocol || !BoundedValue(c.version) || !BoundedValue(c.build) || !BoundedValue(c.gameplay_digest)) return {};
  return {{kProductKey, kProductValue}, {"kkrc_protocol", std::to_string(c.protocol)},
      {"kkrc_version", c.version}, {"kkrc_build", c.build}, {"kkrc_mods", c.gameplay_digest}};
}
bool DecodeLobbyKeys(const LobbyKeys& keys, Compatibility& out) {
  auto get = [&](const char* key) -> std::string { auto i = keys.find(key); return i == keys.end() ? "" : i->second; };
  const auto protocol = get("kkrc_protocol");
  unsigned p = 0;
  const auto result = std::from_chars(protocol.data(), protocol.data() + protocol.size(), p);
  Compatibility c{uint16_t(p), get("kkrc_version"), get("kkrc_build"), get("kkrc_mods")};
  if (get(kProductKey) != kProductValue || protocol.empty() || protocol.size() > 5 || !p || p > 65535 ||
      result.ec != std::errc{} || result.ptr != protocol.data() + protocol.size() ||
      !BoundedValue(c.version) || !BoundedValue(c.build) || !BoundedValue(c.gameplay_digest)) return false;
  out = std::move(c); return true;
}
bool CompatibleLobby(const LobbyKeys& keys, const Compatibility& local) {
  Compatibility remote;
  return DecodeLobbyKeys(keys, remote) && CheckCompatibility(local, remote) == RejectReason::None;
}
bool ParseLobbyId(std::string_view s, SocialId& out) {
  if (s.empty() || s.size() > 20 || s.front() == '0') return false;
  SocialId value = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !value) return false;
  out = value; return true;
}
bool ParseLobbyConnect(std::string_view s, SocialId& out) {
  constexpr std::string_view prefix = "+connect_lobby ";
  return s.starts_with(prefix) && ParseLobbyId(s.substr(prefix.size()), out);
}
SocialId ConsumeLobbyArgument(std::vector<std::string>& args) {
  SocialId id = 0; unsigned count = 0; bool valid = true;
  for (size_t i = 0; i < args.size();) {
    if (args[i] != "+connect_lobby") { ++i; continue; }
    ++count;
    args.erase(args.begin() + i);
    if (i == args.size() || args[i].starts_with("--") || args[i] == "+connect_lobby") { valid = false; continue; }
    valid = ParseLobbyId(args[i], id) && valid;
    args.erase(args.begin() + i);
  }
  return valid && count == 1 ? id : 0;
}
std::string ShortSocialHash(SocialId id) {
  uint32_t hash = 2166136261u;
  for (unsigned i = 0; i < 8; ++i) { hash ^= uint8_t(id >> (i * 8)); hash *= 16777619u; }
  char text[9]; std::snprintf(text, sizeof(text), "%08x", hash); return text;
}
std::string EncodeLobbyChoice(LobbyChoice c) {
  return c.valid() ? std::to_string(c.revision)+":"+std::to_string(c.chapter+1+(c.continue_game ? 42 : 0)) : "0";
}
LobbyChoice DecodeLobbyChoice(std::string_view value) {
  const auto split=value.find(':'); uint64_t revision=0, entry=0;
  if (split==std::string_view::npos || !ParseLobbyId(value.substr(0,split),revision) ||
      !ParseLobbyId(value.substr(split+1),entry) || entry>84) return {};
  return {revision,int((entry-1)%42),entry>42};
}
bool CanStartLobby(const SocialModel& m) {
  return m.choice.valid() && m.status == SocialStatus::Lobby && m.owner == m.self && m.members.size() == 2 &&
      m.members[0].id && m.members[1].id && m.members[0].id != m.members[1].id &&
      (m.members[0].id == m.self || m.members[1].id == m.self) && m.members[0].ready && m.members[1].ready;
}

struct FakeSocialWorld {
  struct User { SocialModel model; Compatibility compatibility; bool enabled, available; };
  struct Lobby { SocialId owner; LobbyKeys keys; std::vector<SocialId> members; bool started = false; LobbyChoice choice; };
  std::map<SocialId, User> users;
  std::map<SocialId, Lobby> lobbies;
  SocialId next_lobby = 1000;
};
std::shared_ptr<FakeSocialWorld> MakeFakeSocialWorld() { return std::make_shared<FakeSocialWorld>(); }
FakeSocialService::FakeSocialService(std::shared_ptr<FakeSocialWorld> world, SocialId self, std::string name,
    Compatibility c, bool enabled, bool available) : world_(std::move(world)), self_(self) {
  SocialModel m; m.self = self; m.status = enabled ? SocialStatus::Dormant : SocialStatus::Disabled;
  m.friends.push_back({self, std::move(name), FriendState::Online});
  world_->users.emplace(self, FakeSocialWorld::User{m, std::move(c), enabled, available});
}
FakeSocialService::~FakeSocialService() { Act({SocialAction::Leave}); world_->users.erase(self_); }
SocialModel FakeSocialService::Snapshot() const {
  auto m = world_->users.at(self_).model;
  m.friends.clear();
  for (const auto& [id, u] : world_->users) if (id != self_) {
    m.friends.push_back(u.model.friends.front());
    // C81: a friend hosting an open lobby offers it to join, like Steam's connect presence.
    auto l = world_->lobbies.find(u.model.lobby);
    if (l != world_->lobbies.end() && l->second.owner == id && !l->second.started && l->second.members.size() == 1) {
      m.friends.back().in_our_game = true; m.friends.back().lobby = l->first;
    }
  }
  if (m.lobby && world_->lobbies.contains(m.lobby)) {
    const auto& l = world_->lobbies.at(m.lobby); m.owner = l.owner; m.members.clear();
    m.choice=l.choice;
    for (auto id : l.members) {
      const auto& u = world_->users.at(id).model;
      m.members.push_back({id, u.friends.front().name, !u.members.empty() && u.members.front().ready});
    }
    m.status = l.started ? SocialStatus::Starting : SocialStatus::Lobby;
    m.start_requested = l.started;
  }
  m.can_start = CanStartLobby(m); return m;
}
bool FakeSocialService::Act(SocialCommand c) {
  auto& user = world_->users.at(self_); auto& m = user.model;
  if (!user.enabled) { m.status = SocialStatus::Disabled; m.status_text = "Co-op is disabled"; return false; }
  if (!user.available) { m.status = SocialStatus::Unavailable; m.status_text = "Steam not available in this build"; return false; }
  auto leave = [&] {
    auto it = world_->lobbies.find(m.lobby);
    if (it != world_->lobbies.end()) {
      if (it->second.owner == self_) {
        for (auto id : it->second.members) if (id != self_) {
          auto& guest = world_->users.at(id).model; guest.lobby = guest.owner = 0;
          guest.members.clear(); guest.status = SocialStatus::Ended; guest.last_error = "The host left the lobby";
        }
        world_->lobbies.erase(it);
      } else if (it->second.started) {
        auto& host = world_->users.at(it->second.owner).model;
        host.lobby = host.owner = 0; host.members.clear(); host.status = SocialStatus::Ended;
        host.start_requested = false; host.last_error = "The partner left the session";
        world_->lobbies.erase(it);
      } else { std::erase(it->second.members, self_); for (auto id : it->second.members) world_->users.at(id).model.members.clear(); }
    }
    m.lobby = m.owner = 0; m.members.clear(); m.start_requested = false; m.status = SocialStatus::Idle;
  };
  switch (c.action) {
    case SocialAction::Open: if (m.status == SocialStatus::Dormant) m.status = SocialStatus::Idle; return true;
    case SocialAction::CreateLobby:
      if (m.lobby || EncodeLobbyKeys(user.compatibility).empty()) return false;
      m.last_error.clear();
      m.lobby = ++world_->next_lobby; m.owner = self_; m.status = SocialStatus::Lobby;
      world_->lobbies.emplace(m.lobby, FakeSocialWorld::Lobby{self_, EncodeLobbyKeys(user.compatibility), {self_}}); return true;
    case SocialAction::Invite:
      if (!m.lobby || !world_->users.contains(c.id) || c.id == self_ || Snapshot().status != SocialStatus::Lobby) return false;
      { auto& invites = world_->users.at(c.id).model.pending_invites;
        if (std::none_of(invites.begin(), invites.end(), [&](auto i) { return i.lobby == m.lobby; }) && invites.size() < 16)
          invites.push_back({m.lobby, self_}); }
      return true;
    case SocialAction::Accept: {
      const auto offered = Snapshot().friends;
      if (m.lobby || (std::none_of(m.pending_invites.begin(), m.pending_invites.end(), [&](auto i) { return i.lobby == c.id; }) &&
                      std::none_of(offered.begin(), offered.end(), [&](const auto& f) { return f.lobby == c.id; }))) return false;
      auto it = world_->lobbies.find(c.id);
      if (it == world_->lobbies.end() || it->second.started || it->second.members.size() != 1 ||
          !CompatibleLobby(it->second.keys, user.compatibility)) { m.last_error = "Lobby unavailable or incompatible"; return false; }
      it->second.members.push_back(self_); m.lobby = c.id; m.status = SocialStatus::Lobby; m.last_error.clear();
      for (auto id : it->second.members) world_->users.at(id).model.members.clear();
      std::erase_if(m.pending_invites, [&](auto i) { return i.lobby == c.id; }); return true;
    }
    case SocialAction::Decline: std::erase_if(m.pending_invites, [&](auto i) { return i.lobby == c.id; }); return true;
    case SocialAction::SelectChapter: {
      if (!m.lobby || Snapshot().status!=SocialStatus::Lobby || Snapshot().owner!=self_ || c.choice.chapter<0 || c.choice.chapter>=42) return false;
      auto& l=world_->lobbies.at(m.lobby);
      if (l.choice.valid() && l.choice.chapter==c.choice.chapter && l.choice.continue_game==c.choice.continue_game) return true;
      l.choice={l.choice.revision+1,c.choice.chapter,c.choice.continue_game};
      for (auto id : l.members) world_->users.at(id).model.members.clear();
      return true;
    }
    case SocialAction::SetReady:
      if (!m.lobby || Snapshot().status != SocialStatus::Lobby || !Snapshot().choice.valid() || c.choice!=Snapshot().choice) return false;
      m.members = {{self_, "", c.ready}}; return true;
    case SocialAction::Start:
      if (!Snapshot().can_start || c.choice!=Snapshot().choice) return false;
      world_->lobbies.at(m.lobby).started = true; return true;
    case SocialAction::Leave: leave(); return true;
    case SocialAction::OverlayInvite: return false;
    case SocialAction::Resume: return false;  // C80: real Steam only
  }
  return false;
}
}  // namespace kk::coop
