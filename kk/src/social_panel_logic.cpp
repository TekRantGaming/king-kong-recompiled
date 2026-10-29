#include "social_panel_logic.h"
#include "coop_flow.h"
#include <algorithm>
#include <cmath>

namespace kk::coop {
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
  if (m.direct) return !m.last_error.empty() ? m.last_error.c_str() : m.status_text.c_str();
  if (m.status==SocialStatus::Unavailable || m.last_error=="Steam is needed for co-op")
    return "Steam is unavailable. You can connect by IP.";
  if (m.last_error == "The host left the lobby" || m.last_error == "The partner left the session" ||
      m.last_error == "Lobby membership changed during the session") return "Your partner left";
  if (m.last_error == "Lobby unavailable or incompatible" || m.last_error == "Lobby compatibility changed")
    return "Versions don't match: {0}";
  if (!m.last_error.empty()) {
    if (m.last_error == "Steam is needed for co-op" || m.last_error == "Steam not available in this build" ||
        m.last_error == "Both players must be ready" || m.last_error == "Could not send invite" ||
        m.last_error == "Steam overlay is unavailable" || m.last_error == "Could not enable Steam invites" || m.last_error == "Steam lobby request timed out" ||
        m.last_error == "Steam requests are still pending; restart to retry" ||
        m.last_error == "Gameplay settings changed; open a new lobby") return m.last_error.c_str();
    if (m.last_error == "Could not create lobby" || m.last_error == "Could not request lobby creation") return "Could not create lobby";
    if (m.last_error == "Could not join lobby" || m.last_error == "Could not request lobby join" ||
        m.last_error == "Could not inspect invited lobby") return "Could not join lobby";
    return "Could not complete Social action";
  }
  switch (m.status) {
    case SocialStatus::Disabled: return "Local dev link selected";
    case SocialStatus::Dormant: return m.status_text == "Connecting to Steam" ? "Connecting to Steam" : "Choose Steam or Direct IP";
    case SocialStatus::Unavailable:
      return m.status_text == "Steam not available in this build" ? "Steam not available in this build" : "Steam is needed for co-op";
    case SocialStatus::Creating: return "Creating lobby";
    case SocialStatus::Joining: return "Checking invited lobby";
    case SocialStatus::Starting: case SocialStatus::Connected:
      return m.status_text == "Partner connection lost" ? "Partner connection lost" : "Starting co-op is not ready yet";
    case SocialStatus::Ended: return "Your partner left";
    case SocialStatus::Lobby:
      if (m.self != m.owner) return "Connected. Waiting for the host to start";
      if (m.members.size()<2) return "Lobby open. Invite a friend.";
      return !m.choice.valid() ? "Connected. Choose a chapter" :
          m.can_start ? "Both ready. Select Start" : "Connected. Both players must choose Ready";
    case SocialStatus::Idle: return "Steam connected. Select Host a Steam game.";
  }
  return "Steam is needed for co-op";
}
std::vector<SocialRow> SocialRows(const SocialModel& m, int direct_mode, bool ip_path) {
  std::vector<SocialRow> rows;
  auto add = [&](SocialRowKind kind, SocialId id, std::string label, std::string detail = "", bool enabled = false) {
    rows.push_back({{kind, id}, std::move(label), std::move(detail), enabled});
  };
  const bool idle = m.status == SocialStatus::Idle || m.status == SocialStatus::Ended;
  const bool lobby = m.status == SocialStatus::Lobby && m.lobby != 0;
  const bool free = idle && !m.lobby;
  const bool dormant=m.status==SocialStatus::Dormant && m.status_text!="Connecting to Steam";
  if (m.lobby) ip_path=m.direct;
  const bool show_steam=!ip_path && !m.direct && m.status!=SocialStatus::Unavailable && m.status!=SocialStatus::Disabled &&
      m.status!=SocialStatus::Dormant;
  if (!m.lobby) {
    const bool available=m.status!=SocialStatus::Creating && m.status!=SocialStatus::Joining &&
        m.status!=SocialStatus::Starting && m.status!=SocialStatus::Connected;
    add(SocialRowKind::PlaySteam,0,"Play with a Steam friend","",available);
    add(SocialRowKind::PlayIP,0,"Play by IP","",available);
  } else add(SocialRowKind::Section,20,ip_path ? "Play by IP" : "Play with a Steam friend");
  if (!ip_path) {
    if (!m.lobby) {
      add(SocialRowKind::Section,21,"1. Connect to Steam");
      if (dormant && !m.direct) add(SocialRowKind::ConnectSteam,0,"Connect to Steam","",true);
      else add(SocialRowKind::Text,21,show_steam ? "Steam connected" : "Connect to Steam");
      add(SocialRowKind::Section,22,"2. Host, or accept an invite");
      add(SocialRowKind::Create,0,"Host a Steam game","",free && show_steam);
    }
  }
  if (show_steam || (!ip_path && dormant && !m.pending_invites.empty())) {
  add(SocialRowKind::Section, 1, "Invites");
  if (m.pending_invites.empty()) add(SocialRowKind::Text, 2, "No invites");
  for (const auto& invite : m.pending_invites) {
    auto f = std::find_if(m.friends.begin(), m.friends.end(), [&](const auto& f) { return f.id == invite.inviter; });
    const std::string name = f == m.friends.end() ? "" : f->name;
    add(SocialRowKind::Accept, invite.lobby, "Accept invite", name, free || dormant);
    add(SocialRowKind::Decline, invite.lobby, "Decline invite", name,
        m.status != SocialStatus::Unavailable && m.status != SocialStatus::Disabled);
  }
  }
  auto steam_invites = [&] {
    const bool invite_lobby=lobby || (m.rejoin_partner && m.lobby && m.owner==m.self);
    add(SocialRowKind::Section,23,"3. Invite a friend");
    add(SocialRowKind::OverlayInvite,0,"Open Steam invite","",invite_lobby && m.members.size()<2 && m.overlay_available);
    if (!m.overlay_available) add(SocialRowKind::Text,23,"Overlay unavailable. Use an Invite button below.");
    add(SocialRowKind::Section,4,"Friends");
    std::vector<SocialFriend> friends=m.friends;
    auto rank=[](FriendState s) { return s==FriendState::Offline ? 1 : 0; };
    std::stable_sort(friends.begin(),friends.end(),[&](const auto& a,const auto& b) {
      if (rank(a.state)!=rank(b.state)) return rank(a.state)<rank(b.state);
      if (a.name!=b.name) return a.name<b.name;
      return a.id<b.id;
    });
    for (const auto& f : friends) if (f.id!=m.self) {
      const bool member=std::any_of(m.members.begin(),m.members.end(),[&](const auto& p) { return p.id==f.id; });
      add(SocialRowKind::Friend,f.id,f.name,SocialFriendState(f.state),
          invite_lobby && m.members.size()<2 && f.state!=FriendState::Offline && !member &&
          (!m.rejoin_partner || f.id==m.rejoin_partner));
    }
    if (friends.empty()) add(SocialRowKind::Text,5,"No friends to show");
  };
  if (show_steam && m.lobby) steam_invites();
  if (m.direct && m.lobby) {
    add(SocialRowKind::Section, 6, "2. Share these details");
    add(SocialRowKind::Text, 10, "UDP port", std::to_string(m.direct_port));
    add(SocialRowKind::Text, 11, "Join code", m.direct_code);
    add(SocialRowKind::Text, 12, "Local IP", m.direct_addresses);
  }
  if (show_steam || m.lobby) add(SocialRowKind::Section, 3, "Lobby");
  if (m.lobby) {
    add(SocialRowKind::Section, 7, m.direct ? "3. Choose a chapter" : "4. Choose a chapter");
    add(SocialRowKind::ChapterSelect,0,"Chapter select","",m.owner==m.self && !m.start_requested &&
        (lobby || (m.direct && m.status==SocialStatus::Joining)));
    if (m.choice.valid()) {
      // The policy gets its own row so long chapter names cannot hide it.
      const auto policy=kChapterRules[size_t(m.choice.chapter)].policy;
      add(SocialRowKind::Text,13,"Co-op",policy==ChapterPolicy::Together ? "Together" :
          policy==ChapterPolicy::SoloRegroup ? "Separate then regroup" : "Watch");
    }
    bool ready = false, self_seen = false;
    for (const auto& member : m.members) {
      add(SocialRowKind::Member, member.id, member.name, member.ready ? "Ready" : "Not ready");
      if (member.id == m.self) { ready = member.ready; self_seen = true; }
    }
    add(SocialRowKind::Section, 8, m.direct ? "4. Both players choose Ready" : "5. Both players choose Ready");
    add(SocialRowKind::Ready, 0, ready ? "Not ready" : "Ready", "", lobby && self_seen && m.choice.valid());
    add(SocialRowKind::Section, 9, m.direct ? "5. The host starts the game" : "6. The host starts the game");
    if (m.owner == m.self) add(SocialRowKind::Start, 0, "Start", "", lobby && m.choice.valid() && m.can_start && CanStartLobby(m));
    add(SocialRowKind::Leave, 0, "Leave lobby", "", true);
  }
  if (show_steam && !m.lobby) steam_invites();
  if (!m.lobby) {
    const bool available = m.status != SocialStatus::Creating && m.status != SocialStatus::Joining &&
        m.status != SocialStatus::Starting && m.status != SocialStatus::Connected;
    if (!ip_path) add(SocialRowKind::AddSteam, 0, "Add to Steam", "", !m.direct);
    if (ip_path) {
    add(SocialRowKind::Section, 6, "1. Host or Join by IP");
    add(SocialRowKind::DirectChooseHost, 0, "Host a game", "", available);
    add(SocialRowKind::DirectChooseJoin, 0, "Join a game", "", available);
    if (direct_mode) {
    add(SocialRowKind::Section, 10, direct_mode==1 ? "2. Create and share a code" : "2. Enter the host's details");
    if (direct_mode==2) add(SocialRowKind::DirectAddress, 0, "IP address", "", available);
    add(SocialRowKind::DirectPort, 0, "UDP port", "", available);
    add(SocialRowKind::DirectCode, 0, "Join code", "", available);
    if (direct_mode==1) add(SocialRowKind::DirectHost, 0, "Host by IP", "", available);
    else add(SocialRowKind::DirectJoin, 0, "Join", "", available);
    }
    }
  }
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
    case SocialRowKind::ConnectSteam: return SocialCommand{SocialAction::Open};
    case SocialRowKind::Accept: return SocialCommand{SocialAction::Accept, r.key.id};
    case SocialRowKind::Decline: return SocialCommand{SocialAction::Decline, r.key.id};
    case SocialRowKind::Create: return SocialCommand{SocialAction::CreateLobby};
    case SocialRowKind::Ready: {
      const auto self = std::find_if(m.members.begin(), m.members.end(), [&](auto p) { return p.id == m.self; });
      return SocialCommand{SocialAction::SetReady, 0, !self->ready, m.choice};
    }
    case SocialRowKind::Start: return SocialCommand{SocialAction::Start,0,false,m.choice};
    case SocialRowKind::OverlayInvite: return SocialCommand{SocialAction::OverlayInvite};
    case SocialRowKind::Leave: return SocialCommand{SocialAction::Leave};
    case SocialRowKind::Friend: return SocialCommand{SocialAction::Invite, r.key.id};
    default: return {};
  }
}
namespace {
bool Busy(const SocialModel& m) {
  return m.status==SocialStatus::Creating || m.status==SocialStatus::Joining ||
      m.status==SocialStatus::Starting || m.status==SocialStatus::Connected;
}
bool SteamDormant(const SocialModel& m) {
  return !m.direct && m.status==SocialStatus::Dormant && m.status_text!="Connecting to Steam";
}
bool SteamShown(const SocialModel& m) {
  return !m.direct && m.status!=SocialStatus::Unavailable && m.status!=SocialStatus::Disabled && m.status!=SocialStatus::Dormant;
}
const char* SteamLine(const SocialModel& m) {
  if (m.status==SocialStatus::Unavailable || m.status==SocialStatus::Disabled) return "Steam is unavailable. You can connect by IP.";
  if (m.status==SocialStatus::Dormant) return "Connecting to Steam";
  return "Steam connected";
}
}  // namespace
std::vector<SocialRow> CoopPageRows(const SocialModel& m, CoopPage page, CoopPageOptions o) {
  std::vector<SocialRow> rows;
  auto add=[&](SocialRowKind kind, SocialId id, std::string label, std::string detail="", bool enabled=false) {
    rows.push_back({{kind,id},std::move(label),std::move(detail),enabled});
  };
  const bool available=!Busy(m) && !m.lobby;
  const bool free=(m.status==SocialStatus::Idle || m.status==SocialStatus::Ended) && !m.lobby;
  auto steam_row=[&](SocialId section) {
    if (SteamDormant(m)) add(SocialRowKind::ConnectSteam,0,"Connect to Steam","",true);
    else add(SocialRowKind::Text,section,SteamLine(m));
  };
  // The drawer's own rows, so both views offer exactly the same lobby actions.
  const auto drawer=SocialRows(m,0,m.lobby ? m.direct : false);
  auto copy=[&](SocialRowKind kind, std::optional<SocialId> id={}) {
    for (const auto& r : drawer) if (r.key.kind==kind && (!id || r.key.id==*id)) rows.push_back(r);
  };
  switch (page) {
    case CoopPage::Hub:
      add(SocialRowKind::Section,30,"Host or join");
      add(SocialRowKind::CoopHost,0,"Host a game","",available);
      add(SocialRowKind::CoopJoin,0,"Join a game","",available);
      add(SocialRowKind::Section,31,"Steam");
      steam_row(31);
      add(SocialRowKind::AddSteam,0,"Add to Steam","",!m.direct);
      break;
    case CoopPage::Host:
      add(SocialRowKind::Section,32,"What to play");
    {
      // Also the lobby's Change: the host picks another chapter or save before Start.
      const bool can=available || (m.lobby && m.owner==m.self && !m.start_requested);
      add(SocialRowKind::HostNewGame,0,"New game","",can);
      add(SocialRowKind::HostLoadGame,0,"Load game","",can && o.has_saves);
    }
      break;
    case CoopPage::HostNew: case CoopPage::HostLoad: break;  // the launcher's chapter and save lists
    case CoopPage::HostHow:
      add(SocialRowKind::Section,33,"Steam friends");
      steam_row(33);
      add(SocialRowKind::Create,0,"Host with Steam friends","",free && SteamShown(m) && o.choice_ready);
      add(SocialRowKind::Section,34,"By IP");
      add(SocialRowKind::DirectPort,0,"UDP port","",available);
      add(SocialRowKind::DirectCode,0,"Join code","",available);
      add(SocialRowKind::DirectHost,0,"Host by IP","",available && o.choice_ready);
      break;
    case CoopPage::Join: {
      add(SocialRowKind::Section,35,"Steam invites");
      if (SteamDormant(m)) add(SocialRowKind::ConnectSteam,0,"Connect to Steam","",true);
      size_t invites=0;
      for (const auto& r : drawer) if (r.key.kind==SocialRowKind::Accept || r.key.kind==SocialRowKind::Decline) {
        rows.push_back(r); ++invites;
      }
      if (!invites) add(SocialRowKind::Text,36,"No invites");
      if (SteamShown(m)) {
        add(SocialRowKind::Section,37,"Friends playing now");
        size_t playing=0;
        for (const auto& f : m.friends) if (f.id!=m.self && f.in_our_game && f.lobby && f.state!=FriendState::Offline) {
          add(SocialRowKind::JoinFriend,f.id,f.name,"Playing King Kong",free); ++playing;
        }
        if (!playing) add(SocialRowKind::Text,38,"No friends are playing now");
      }
      add(SocialRowKind::Section,39,"By IP");
      add(SocialRowKind::DirectAddress,0,"IP address","",available);
      add(SocialRowKind::DirectPort,0,"UDP port","",available);
      add(SocialRowKind::DirectCode,0,"Join code","",available);
      add(SocialRowKind::DirectJoin,0,"Join","",available);
      break;
    }
    case CoopPage::Lobby:
      if (!m.lobby) { add(SocialRowKind::Leave,0,"Leave lobby","",Busy(m)); break; }
      add(SocialRowKind::Section,41,"Players");
      copy(SocialRowKind::Member);
      add(SocialRowKind::Section,42,"Chapter or save");
      for (const auto& r : drawer) if (r.key.kind==SocialRowKind::ChapterSelect)
        add(SocialRowKind::ChangeChoice,0,"Chapter or save","",r.enabled);
      copy(SocialRowKind::Text,13);
      if (m.direct && m.owner==m.self) {  // the host's own details; the guest already has them
        add(SocialRowKind::Section,43,"Share these details");
        copy(SocialRowKind::Text,10); copy(SocialRowKind::Text,11); copy(SocialRowKind::Text,12);
      } else if (SteamShown(m)) {
        add(SocialRowKind::Section,43,"Invite a friend");
        copy(SocialRowKind::OverlayInvite); copy(SocialRowKind::Text,23);
        copy(SocialRowKind::Friend); copy(SocialRowKind::Text,5);
      }
      add(SocialRowKind::Section,44,"Ready");
      copy(SocialRowKind::Ready); copy(SocialRowKind::Start); copy(SocialRowKind::Leave);
      break;
  }
  return rows;
}
CoopPage CoopPageFor(const SocialModel& m, CoopPage current) {
  const bool waiting=m.status==SocialStatus::Creating || m.status==SocialStatus::Joining;
  // The lobby page also stays while Steam starts for an accepted invite (it then checks the lobby).
  const bool connecting=m.status==SocialStatus::Dormant && m.status_text=="Connecting to Steam";
  if ((m.lobby || waiting) && (current==CoopPage::Hub || current==CoopPage::HostHow || current==CoopPage::Join))
    return CoopPage::Lobby;
  if (current==CoopPage::Lobby && !m.lobby && !waiting && !connecting) return CoopPage::Hub;
  // In a lobby, Host, New game and Load game are the host's Change (before Start) only.
  const bool host_pages=current==CoopPage::Host || current==CoopPage::HostNew || current==CoopPage::HostLoad;
  if (host_pages && (waiting || (m.lobby && (m.owner!=m.self || m.start_requested)))) return CoopPage::Lobby;
  return current;
}
std::optional<SocialCommand> CoopRowCommand(const SocialRow& r, const SocialModel& m, CoopPage page) {
  const auto rows=CoopPageRows(m,page,{true,true});
  auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row) { return row.key==r.key; });
  if (found==rows.end() || !found->enabled) return {};
  switch (r.key.kind) {
    case SocialRowKind::JoinFriend: {
      auto f=std::find_if(m.friends.begin(),m.friends.end(),[&](const auto& f) { return f.id==r.key.id; });
      if (f==m.friends.end() || !f->lobby) return {};
      return SocialCommand{SocialAction::Accept,f->lobby};
    }
    case SocialRowKind::Create: return SocialCommand{SocialAction::CreateLobby};
    case SocialRowKind::ConnectSteam: return SocialCommand{SocialAction::Open};
    case SocialRowKind::Accept: case SocialRowKind::Decline: case SocialRowKind::Ready: case SocialRowKind::Start:
    case SocialRowKind::OverlayInvite: case SocialRowKind::Leave: case SocialRowKind::Friend:
      return SocialRowCommand(r,m);
    default: return {};
  }
}
CoopStatus CoopLobbyStatus(const SocialModel& m, SocialId invited) {
  if (!m.lobby) {
    if (m.status==SocialStatus::Creating) return {"Creating lobby"};
    if (m.status==SocialStatus::Joining) return {"Joining the lobby"};
    return {SocialStatusKey(m)};
  }
  if (!m.last_error.empty()) return {SocialStatusKey(m)};
  const SocialMember* self=nullptr; const SocialMember* partner=nullptr;
  for (const auto& p : m.members) (p.id==m.self ? self : partner)=&p;
  const bool host=m.owner==m.self;
  if (!partner) {
    if (!host) return {"Connecting to the host"};
    if (m.direct) return {"Waiting for your partner to join"};
    auto f=std::find_if(m.friends.begin(),m.friends.end(),[&](const auto& f) { return f.id==invited; });
    if (invited && f!=m.friends.end()) return {"Waiting for {0} to join",f->name};
    return {"Lobby open. Invite a friend."};
  }
  const std::string name=partner->name;
  if (m.start_requested || m.status==SocialStatus::Starting) return {"Starting co-op..."};
  if (!m.choice.valid()) return host ? CoopStatus{"Choose a chapter or save"} : CoopStatus{"Waiting for {0} to choose a chapter",name};
  const bool me=self && self->ready;
  if (host) {
    if (me && partner->ready) return {"Both ready. Select Start"};
    if (partner->ready) return {"{0} is ready",name};
    return {"Waiting for {0} to choose Ready",name};
  }
  if (!me) return partner->ready ? CoopStatus{"{0} is ready",name} : CoopStatus{"Choose Ready when you are set"};
  return {"Waiting for {0} to start",name};
}
SocialId StartupInvite(const SocialModel& m) {
  for (const auto& i : m.pending_invites) if (!i.inviter && i.lobby) return i.lobby;
  return 0;
}
namespace {
ChapterPolicy PolicyOf(int chapter) {
  return chapter >= 0 && size_t(chapter) < kChapterRules.size() ? kChapterRules[size_t(chapter)].policy
                                                                : ChapterPolicy::Together;
}
}  // namespace
const char* CoopModeName(int chapter) {
  switch (PolicyOf(chapter)) {
    case ChapterPolicy::Together: return "Play together";
    case ChapterPolicy::SoloRegroup: return "Apart, then regroup";
    case ChapterPolicy::Observer: return "Watch";
  }
  return "Play together";
}
const char* CoopModeText(int chapter) {
  switch (PolicyOf(chapter)) {
    case ChapterPolicy::Together:
      return "You both play the whole chapter in the same world. If one of you goes down, the other can keep going "
             "and you come back next to them.";
    case ChapterPolicy::SoloRegroup:
      return "Each of you plays this part on your own, then you meet up again.";
    case ChapterPolicy::Observer:
      return "The host plays this chapter and the partner watches it.";
  }
  return "";
}
int CoopLobbyStep(const SocialModel& m) {
  if (!m.lobby || !m.choice.valid()) return 1;
  return m.members.size() < 2 ? 2 : 3;
}
CoopStatus CoopStartLock(const SocialModel& m) {
  if (m.start_requested || m.status == SocialStatus::Starting) return {"Starting co-op..."};
  const SocialMember* self = nullptr;
  const SocialMember* partner = nullptr;
  for (const auto& p : m.members) (p.id == m.self ? self : partner) = &p;
  if (!m.lobby || !partner) return {"Waiting for a friend to join"};
  if (!m.choice.valid()) return {"Choose a chapter or save first"};
  if (!self || !self->ready) return {"Press Ready first"};
  if (!partner->ready) return {"Waiting for {0} to press Ready", partner->name};
  if (!m.can_start || !CanStartLobby(m)) return {"Waiting for both games to be ready"};
  return {};
}
int EarlyAccessGate::SecondsLeft(double now) const {
  if (!open_) return 0;
  const double left = kSeconds - (now - at_);
  return left <= 0 ? 0 : int(std::ceil(left));
}
}  // namespace kk::coop

