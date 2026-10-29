// SDK/UI-free source tests. Claude builds and runs them, never starts Steam or a game.
#include "social_panel_logic.h"
#include "social_fake.h"
#include "coop_flow.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace kk::coop;
namespace {
unsigned checks = 0;
void Check(bool yes, const char* message) {
  ++checks;
  if (!yes) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
const SocialRow& Find(const std::vector<SocialRow>& rows, SocialRowKind kind, SocialId id = 0) {
  auto i = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.key == SocialRowKey{kind, id}; });
  Check(i != rows.end(), "expected panel row exists"); return *i;
}
void FocusAndActions() {
  SocialModel dormant;
  auto dormant_rows=SocialRows(dormant);
  Check(std::string_view(SocialStatusKey(dormant))=="Choose Steam or Direct IP","dormant status does not pretend to connect");
  const auto connect=Find(dormant_rows,SocialRowKind::ConnectSteam);
  Check(SocialRowCommand(connect,dormant)->action==SocialAction::Open,"explicit Steam opt-in command");
  const auto ip_rows=SocialRows(dormant,0,true);
  Check(Find(ip_rows,SocialRowKind::DirectChooseHost).enabled &&
        Find(ip_rows,SocialRowKind::DirectChooseJoin).enabled,"both IP roles available before Steam init");
  Check(dormant_rows[0].label=="Play with a Steam friend" && dormant_rows[1].label=="Play by IP",
      "two clear paths are at the top");
  Check(!Find(dormant_rows,SocialRowKind::Create).enabled,"hosting waits for explicit Steam connection");
  Check(std::none_of(dormant_rows.begin(),dormant_rows.end(),[](const auto& r) {
    return r.key.kind==SocialRowKind::DirectHost || r.key.kind==SocialRowKind::DirectChooseHost;
  }),"Steam path cannot look like IP hosting");
  Check(std::none_of(ip_rows.begin(),ip_rows.end(),[](const auto& r) {
    return r.key.kind==SocialRowKind::Create || r.key.kind==SocialRowKind::ConnectSteam;
  }),"IP path has no Steam hosting or connect rows");
  dormant.pending_invites={{1234,0}}; dormant_rows=SocialRows(dormant);
  Check(SocialRowCommand(Find(dormant_rows,SocialRowKind::Accept,1234),dormant)->action==SocialAction::Accept,
        "cold Steam invite can explicitly connect and accept");
  Check(SocialRowCommand(Find(dormant_rows,SocialRowKind::Decline,1234),dormant)->action==SocialAction::Decline,
        "cold Steam invite can be declined without connecting");
  dormant.status_text="Connecting to Steam";
  Check(!SocialRowCommand(connect,dormant) && std::string_view(SocialStatusKey(dormant))=="Connecting to Steam",
        "pending connection revokes stale opt-in row");
  auto fake = MakeSocialPreview();
  auto m = fake->Snapshot(); auto rows = SocialRows(m);
  Check(Find(rows, SocialRowKind::Accept, m.pending_invites.front().lobby).enabled, "idle may accept invite");
  Check(Find(rows, SocialRowKind::Create).enabled, "idle may create lobby");
  std::vector<SocialRowKind> order;
  for (const auto& r : rows) if (r.focusable()) order.push_back(r.key.kind);
  Check(order[0]==SocialRowKind::PlaySteam && order[1]==SocialRowKind::PlayIP && order[2]==SocialRowKind::Create &&
      order[3]==SocialRowKind::Accept && order[4]==SocialRowKind::Decline,"paths and Steam hosting precede invitations");
  Check(Find(rows,SocialRowKind::Create).label=="Host a Steam game","Steam host action is explicit");
  Check(SocialFocus(rows, {}) == 0, "first focus selects the Steam path");
  const SocialRowKey friend_key{SocialRowKind::Friend, 2};
  m.friends.front().name = "Duplicate"; m.friends.back().name = "Duplicate";
  std::reverse(m.friends.begin(), m.friends.end());
  rows = SocialRows(m);
  const int focus = SocialFocus(rows, friend_key);
  Check(focus >= 0 && rows[size_t(focus)].key == friend_key, "focus follows ID through reorder and duplicate names");
  auto off = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.key.kind == SocialRowKind::Friend && r.detail == "Offline"; });
  Check(off != rows.end() && std::none_of(off+1,rows.end(),[](const auto& r) { return r.key.kind==SocialRowKind::Friend; }), "offline friends are last in the Steam section");
  Check(SocialFocus({}, friend_key) == -1, "empty model has no focus");
  const auto invite = Find(rows, SocialRowKind::Accept, m.pending_invites.front().lobby);
  auto cmd = SocialRowCommand(invite, m); Check(cmd && fake->Act(*cmd), "panel accepts through service action");
  m = fake->Snapshot(); rows = SocialRows(m);
  Check(SocialFocus(rows, invite.key, 1) >= 0, "removed invite focus falls back inside drawer");
  Check(std::none_of(rows.begin(), rows.end(), [](const auto& r) { return r.key.kind == SocialRowKind::Start; }), "guest cannot Start");
  Check(std::string_view(SocialStatusKey(m)) == "Connected. Waiting for the host to start", "guest waits for host");
  Check(!SocialRowCommand(invite, m), "removed/stale invite cannot act");
  Check(fake->Scenario("host"), "host preview");
  m = fake->Snapshot(); rows = SocialRows(m);
  Check(std::string_view(SocialStatusKey(m))=="Lobby open. Invite a friend.","new host knows to invite first");
  Check(SocialFocus(rows,SocialRowKey{SocialRowKind::OverlayInvite,0})<SocialFocus(rows,SocialRowKey{SocialRowKind::ChapterSelect,0}),
      "Steam invitations precede chapter selection");
  Check(Find(rows,SocialRowKind::OverlayInvite).label=="Open Steam invite","overlay action has a clear label");
  Check(!Find(rows, SocialRowKind::Start).enabled, "one member cannot Start");
  const auto& empty_invites = Find(rows, SocialRowKind::Text, 2);
  Check(empty_invites.label == "No invites" && !empty_invites.focusable() &&
      !SocialRowCommand(empty_invites, m), "empty invites are plain text and cannot take focus or act");
  Check(Find(rows, SocialRowKind::Ready).enabled, "host may ready");
  Check(Find(rows, SocialRowKind::OverlayInvite).enabled, "available overlay may invite");
  m.overlay_available = false;
  Check(!Find(SocialRows(m), SocialRowKind::OverlayInvite).enabled, "missing overlay disables its action");
  m = fake->Snapshot(); rows = SocialRows(m);
  auto ready = SocialRowCommand(Find(rows, SocialRowKind::Ready), m);
  Check(ready && ready->ready && fake->Act(*ready), "Ready toggles through fake");
  ready = SocialRowCommand(Find(SocialRows(fake->Snapshot()), SocialRowKind::Ready), fake->Snapshot());
  Check(ready && !ready->ready, "Ready toggles back");
  auto invite_friend = SocialRowCommand(Find(rows, SocialRowKind::Friend, 3), m);
  Check(invite_friend && invite_friend->action == SocialAction::Invite, "A and X share validated friend invitation");
  Check(!Find(rows, SocialRowKind::Friend, 6).enabled, "offline friend cannot be invited");
  Check(fake->Scenario("ready"), "two ready members preview");
  m = fake->Snapshot(); rows = SocialRows(m);
  const auto start_row = Find(rows, SocialRowKind::Start);
  Check(start_row.enabled && SocialRowCommand(start_row, m)->action == SocialAction::Start, "both-ready host can select Start");
  Check(!Find(rows, SocialRowKind::OverlayInvite).enabled && !Find(rows, SocialRowKind::Friend, 2).enabled, "full lobby cannot invite");
  m.members[1].ready = false;
  Check(!SocialRowCommand(start_row, m), "latest snapshot revokes stale Start enablement");
  m = fake->Snapshot(); m.members[1].id = m.self;
  Check(!Find(SocialRows(m), SocialRowKind::Start).enabled, "duplicate member cannot Start");
  for (auto status : {SocialStatus::Creating, SocialStatus::Joining, SocialStatus::Starting, SocialStatus::Connected}) {
    m = fake->Snapshot(); m.status = status;
    rows = SocialRows(m);
    Check(!Find(rows, SocialRowKind::Start).enabled && !Find(rows, SocialRowKind::Ready).enabled &&
          Find(rows, SocialRowKind::Leave).enabled, "busy/starting lobby only permits leaving");
  }
  for (const char* scene : {"offline", "needed", "unavailable", "mismatch", "left", "busy", "starting", "guest"}) {
    Check(fake->Scenario(scene), "preview scene selectable without Steam");
    m = fake->Snapshot(); Check(!std::string_view(SocialStatusKey(m)).empty(), "every scene has status");
  }
  fake->Scenario("needed");
  Check(std::string_view(SocialStatusKey(fake->Snapshot())) == "Steam is unavailable. You can connect by IP.", "missing client points to Direct IP");
  Check(Find(SocialRows(fake->Snapshot(),0,true), SocialRowKind::DirectChooseHost).enabled, "missing Steam leaves Direct IP available");
  fake->Scenario("unavailable");
  Check(std::string_view(SocialStatusKey(fake->Snapshot())) == "Steam is unavailable. You can connect by IP.", "SDK-missing state points to Direct IP");
  fake->Scenario("mismatch");
  Check(std::string_view(SocialStatusKey(fake->Snapshot())) == "Versions don't match: {0}", "mismatch reason template");
  fake->Scenario("left");
  Check(std::string_view(SocialStatusKey(fake->Snapshot())) == "Your partner left", "partner departure state");
  Check(!fake->Scenario("not-a-scene"), "unknown preview leaves current scene intact");
}
void PreviewScenes() {
  auto fake = MakeSocialPreview();
  const char* scenes[] = {"invites", "host", "guest", "ready", "offline", "needed", "unavailable",
      "mismatch", "left", "busy", "starting", "long"};
  // Exercise every transition, including returning from a started/full/error lobby.
  for (const char* from : scenes) for (const char* scene : scenes) {
    Check(fake->Scenario(from) && fake->Scenario(scene), "scene transition succeeds");
    Check(fake->Act({SocialAction::Open}) == (std::string_view(scene) != "needed" &&
          std::string_view(scene) != "unavailable"), "opening drawer preserves scene availability");
    const auto m = fake->Snapshot();
    const auto rows = SocialRows(m);
    const std::string_view name = scene;
    const bool host = name == "host" || name == "ready" || name == "starting";
    const bool joined = host || name == "guest";
    Check((m.lobby != 0) == joined, "scene has the requested lobby membership after Open");
    Check((m.owner == m.self) == host, "scene has the requested owner");
    Check(m.members.size() == (name == "host" ? 1u : joined ? 2u : 0u), "scene has correct member count");
    Check(m.can_start == (name == "ready"), "only ready scene permits Start");
    Check(m.start_requested == (name == "starting"), "starting scene performs fake Start");
    Check(std::any_of(rows.begin(), rows.end(), [](const auto& r) { return r.key.kind == SocialRowKind::Create && r.enabled; })
          == (!joined && name!="needed" && name!="unavailable" && name!="busy"), "Host a Steam game requires connected idle Steam and no existing lobby");
    if (joined) {
      Check(m.status == (name == "starting" ? SocialStatus::Starting : SocialStatus::Lobby), "actual joined scene status");
      Check(m.pending_invites.empty(), "lobby capture is not obscured by stale incoming invites");
    }
    if (name == "ready" || name == "starting")
      Check(std::all_of(m.members.begin(), m.members.end(), [](const auto& p) { return p.ready; }), "both fake members ready");
    if (name == "left") Check(m.status == SocialStatus::Ended && !m.last_error.empty(), "host departure actually ends guest lobby");
    if (name == "mismatch") Check(m.last_error == "Lobby unavailable or incompatible", "incompatible fake invite is rejected");
    if (name == "busy") Check(m.status == SocialStatus::Creating && !Find(rows, SocialRowKind::Create).enabled, "pending-create display");
    if (name == "needed" || name == "unavailable") Check(m.status == SocialStatus::Unavailable, "unavailable fake service");
    if (name == "offline")
      Check(std::all_of(m.friends.begin(), m.friends.end(), [](const auto& f) { return f.state == FriendState::Offline; }), "offline friend scene");
    if (name == "long") {
      Check(!m.pending_invites.empty() && m.friends.front().name.size() >= 24, "long inviter fixture");
      Check(fake->Act({SocialAction::CreateLobby}), "long-name host can be created for capture");
      Check(fake->Snapshot().members.front().name.size() >= 24, "long member fixture");
    }
    bool offline_seen = false;
    for (const auto& row : rows) if (row.key.kind == SocialRowKind::Friend) {
      if (row.detail == "Offline") offline_seen = true;
      else Check(!offline_seen, "every online/away/busy/in-game friend precedes offline friends");
    }
    const auto before = fake->Snapshot();
    Check(!fake->Scenario("not-a-scene"), "bad scene fails visibly");
    const auto after = fake->Snapshot();
    Check(after.status == before.status && after.lobby == before.lobby && after.owner == before.owner &&
          after.members.size() == before.members.size() && after.last_error == before.last_error,
          "bad scene preserves the previous fake world");
  }
}
void DirectRows() {
  SocialModel m; m.status=SocialStatus::Unavailable;
  auto rows=SocialRows(m,0,true);
  Check(Find(rows,SocialRowKind::DirectChooseHost).enabled && Find(rows,SocialRowKind::DirectChooseJoin).enabled,"Direct IP role choice works without Steam");
  Check(Find(SocialRows(m,1,true),SocialRowKind::DirectHost).enabled && Find(SocialRows(m,2,true),SocialRowKind::DirectJoin).enabled,"Direct IP confirmations work without Steam");
  Check(Find(SocialRows(m),SocialRowKind::AddSteam).enabled,"Steam path links to existing Add to Steam");
  m.status=SocialStatus::Lobby; m.direct=true; m.self=m.owner=1; m.lobby=99;
  m.members={{1,"Host",false},{2,"Guest",false}};
  Check(!Find(SocialRows(m),SocialRowKind::Ready).enabled && !Find(SocialRows(m),SocialRowKind::Start).enabled,
      "no default chapter: Ready and Start wait for host selection");
  m.choice={1,1,false};
  rows=SocialRows(m);
  Check(Find(rows,SocialRowKind::Ready).enabled && !Find(rows,SocialRowKind::Start).enabled,"Direct IP readiness is explicit");
  Check(std::none_of(rows.begin(),rows.end(),[](const auto& r) { return r.key.kind==SocialRowKind::Friend || r.key.kind==SocialRowKind::OverlayInvite || r.key.kind==SocialRowKind::DirectHost; }),"active Direct session has no Steam invites or second host");
  m.members[0].ready=m.members[1].ready=true; m.can_start=true;
  Check(SocialRowCommand(Find(SocialRows(m),SocialRowKind::Start),m).has_value(),"Direct host uses shared Start command");
  m.self=2;
  rows=SocialRows(m);
  Check(std::none_of(rows.begin(),rows.end(),[](const auto& r) { return r.key.kind==SocialRowKind::Start; }),"Direct guest cannot Start");
  Check(!Find(rows,SocialRowKind::ChapterSelect).enabled,"guest cannot change chapter");
  for (const auto& row : rows) if (row.key.kind==SocialRowKind::Text && !row.detail.empty())
    Check(row.focusable() && SocialFocus(rows,row.key)==int(&row-rows.data()),"Direct detail focus survives restoration");
}
void ChapterChoice() {
  for (auto choice : {LobbyChoice{1,0,false},LobbyChoice{42,41,false},LobbyChoice{123,10,true}})
    Check(DecodeLobbyChoice(EncodeLobbyChoice(choice))==choice,"Steam lobby choice round trip");
  for (const char* bad : {"", "0", "1:0", "1:85", "0:1", "1:1:x", "-1:1", "18446744073709551616:1"})
    Check(!DecodeLobbyChoice(bad).valid(),"malformed lobby choice rejected");
  auto world=MakeFakeSocialWorld();
  const Compatibility c{kProtocolVersion,"test","test","test"};
  FakeSocialService host(world,1,"Host",c),guest(world,2,"Guest",c);
  Check(host.Act({SocialAction::CreateLobby}) && host.Act({SocialAction::Invite,2}) &&
      guest.Act({SocialAction::Accept,host.Snapshot().lobby}),"choice test joins two players");
  Check(!Find(SocialRows(host.Snapshot()),SocialRowKind::Ready).enabled,"new lobby has no default choice");
  Check(!guest.Act({SocialAction::SelectChapter,0,false,{0,1,false}}),"guest cannot publish choice");
  Check(host.Act({SocialAction::SelectChapter,0,false,{0,10,false}}),"host chooses separate chapter");
  const auto chosen=host.Snapshot().choice;
  Check(chosen.valid() && guest.Snapshot().choice==chosen,"guest sees host choice");
  Check(host.Act({SocialAction::SetReady,0,true,chosen}) && guest.Act({SocialAction::SetReady,0,true,chosen}),"both ready for choice");
  Check(host.Act({SocialAction::SelectChapter,0,false,{0,40,false}}),"host changes to Watch chapter");
  auto m=host.Snapshot();
  Check(m.choice.revision==chosen.revision+1 && !m.members[0].ready && !m.members[1].ready,
      "changed chapter resets both Ready values");
  Check(!guest.Act({SocialAction::SetReady,0,true,chosen}),"stale Ready rejected after chapter change");
  const auto policy=Find(SocialRows(m),SocialRowKind::Text,13);
  Check(policy.detail=="Watch","per-chapter policy visible in lobby");
}
// C81: the Play Online Co-op pages use the drawer's model and rows.
bool Has(const std::vector<SocialRow>& rows, SocialRowKind kind) {
  return std::any_of(rows.begin(), rows.end(), [&](const auto& r) { return r.key.kind == kind; });
}
void CoopPages() {
  SocialModel m;  // Steam not started yet
  auto rows = CoopPageRows(m, CoopPage::Hub);
  Check(Find(rows, SocialRowKind::CoopHost).enabled && Find(rows, SocialRowKind::CoopJoin).enabled, "hub offers Host and Join");
  Check(Find(rows, SocialRowKind::ConnectSteam).enabled, "Connect to Steam is one clear button, never automatic");
  rows = CoopPageRows(m, CoopPage::Host, {false, false});
  Check(Find(rows, SocialRowKind::HostNewGame).enabled && !Find(rows, SocialRowKind::HostLoadGame).enabled,
        "Load game needs a save");
  rows = CoopPageRows(m, CoopPage::HostHow, {true, true});
  Check(!Find(rows, SocialRowKind::Create).enabled && Find(rows, SocialRowKind::ConnectSteam).enabled,
        "Steam hosting waits for Connect to Steam");
  Check(Find(rows, SocialRowKind::DirectHost).enabled, "by IP hosting works without Steam");
  Check(!Find(CoopPageRows(m, CoopPage::HostHow, {true, false}), SocialRowKind::DirectHost).enabled,
        "no lobby before a chapter or save is chosen");
  m.status = SocialStatus::Idle;
  Check(Find(CoopPageRows(m, CoopPage::HostHow, {true, true}), SocialRowKind::Create).enabled, "Steam host after connecting");
  Check(CoopPageFor(m, CoopPage::Lobby) == CoopPage::Hub, "a lobby page without a lobby goes back");
  m.status = SocialStatus::Dormant; m.status_text = "Connecting to Steam";
  Check(CoopPageFor(m, CoopPage::Lobby) == CoopPage::Lobby && CoopPageFor(m, CoopPage::Join) == CoopPage::Join,
        "a Steam invite start keeps the lobby page while Steam starts; Connect to Steam stays put");
  m.status_text.clear();
  m.status = SocialStatus::Creating;
  Check(CoopPageFor(m, CoopPage::HostHow) == CoopPage::Lobby, "creating a lobby shows the lobby page");

  auto world = MakeFakeSocialWorld();
  const Compatibility c{kProtocolVersion, "test", "test", "test"};
  FakeSocialService host(world, 1, "Sam", c), guest(world, 2, "Alex", c);
  Check(host.Act({SocialAction::Open}) && guest.Act({SocialAction::Open}), "both connect");
  Check(host.Act({SocialAction::CreateLobby}), "host lobby");
  auto hm = host.Snapshot();
  Check(CoopPageFor(hm, CoopPage::HostHow) == CoopPage::Lobby && CoopPageFor(hm, CoopPage::HostNew) == CoopPage::HostNew,
        "host goes to the lobby, and may still change the chapter");
  Check(std::string_view(CoopLobbyStatus(hm).key) == "Lobby open. Invite a friend.", "host alone status");
  const auto st = CoopLobbyStatus(hm, 2);
  Check(std::string_view(st.key) == "Waiting for {0} to join" && st.name == "Alex", "invited friend named");
  auto gm = guest.Snapshot();
  rows = CoopPageRows(gm, CoopPage::Join);
  const auto& join = Find(rows, SocialRowKind::JoinFriend, 1);
  Check(join.enabled && join.label == "Sam", "friends playing now lists the host with Join");
  const auto command = CoopRowCommand(join, gm, CoopPage::Join);
  Check(command && command->action == SocialAction::Accept && command->id == hm.lobby, "Join uses the friend's lobby");
  Check(guest.Act(*command) && guest.Snapshot().lobby == hm.lobby, "guest joined from friends playing now");
  gm = guest.Snapshot(); hm = host.Snapshot();
  Check(CoopPageFor(gm, CoopPage::Join) == CoopPage::Lobby && CoopPageFor(gm, CoopPage::HostNew) == CoopPage::Lobby,
        "guest shows the lobby and cannot change the chapter");
  Check(std::string_view(CoopLobbyStatus(gm).key) == "Waiting for {0} to choose a chapter" && CoopLobbyStatus(gm).name == "Sam",
        "guest waits for the host's choice");
  Check(std::string_view(CoopLobbyStatus(hm).key) == "Choose a chapter or save", "host asked to choose");
  rows = CoopPageRows(hm, CoopPage::Lobby);
  Check(Find(rows, SocialRowKind::ChangeChoice).enabled && Has(rows, SocialRowKind::Start) && Has(rows, SocialRowKind::Leave),
        "host lobby: Change, Start and Leave");
  Check(!Find(CoopPageRows(gm, CoopPage::Lobby), SocialRowKind::ChangeChoice).enabled &&
        !Has(CoopPageRows(gm, CoopPage::Lobby), SocialRowKind::Start), "guest has no Change or Start");
  Check(Find(CoopPageRows(hm, CoopPage::Host, {true, false}), SocialRowKind::HostNewGame).enabled, "host may change in the lobby");
  Check(host.Act({SocialAction::SelectChapter, 0, false, {0, 2, false}}), "host picks chapter 3");
  const auto choice = host.Snapshot().choice;
  Check(guest.Act({SocialAction::SetReady, 0, true, choice}), "guest ready");
  hm = host.Snapshot();
  Check(std::string_view(CoopLobbyStatus(hm).key) == "{0} is ready" && CoopLobbyStatus(hm).name == "Alex", "host sees Alex is ready");
  Check(std::string_view(CoopLobbyStatus(guest.Snapshot()).key) == "Waiting for {0} to start", "guest waits for Start");
  Check(host.Act({SocialAction::SetReady, 0, true, choice}), "host ready");
  hm = host.Snapshot();
  Check(std::string_view(CoopLobbyStatus(hm).key) == "Both ready. Select Start", "both ready");
  Check(CoopRowCommand(Find(CoopPageRows(hm, CoopPage::Lobby), SocialRowKind::Start), hm, CoopPage::Lobby).has_value(),
        "Start through the shared command");
  Check(!CoopRowCommand({{SocialRowKind::JoinFriend, 1}, "Sam", "", true}, gm, CoopPage::Join), "stale Join is refused in a lobby");

  SocialModel d; d.direct = true; d.lobby = 7; d.self = d.owner = 1; d.status = SocialStatus::Joining; d.members = {{1, "Host", false}}; d.direct_code = "ABC123";
  Check(std::string_view(CoopLobbyStatus(d).key) == "Waiting for your partner to join", "IP host waits");
  rows = CoopPageRows(d, CoopPage::Lobby);
  Check(Has(rows, SocialRowKind::Text) && Find(rows, SocialRowKind::Text, 11).focusable(), "IP lobby shares the code");
  d.self = 2; d.members = {{2, "Guest", false}};
  Check(std::string_view(CoopLobbyStatus(d).key) == "Connecting to the host", "IP guest connecting");
  rows = CoopPageRows(d, CoopPage::Lobby);
  Check(std::none_of(rows.begin(), rows.end(), [](const auto& r) { return r.key == SocialRowKey{SocialRowKind::Text, 11}; }),
        "the IP guest is not asked to share the host's details");

  SocialModel s; s.pending_invites = {{55, 9}, {77, 0}};
  Check(StartupInvite(s) == 77, "the Steam start lobby is the invite without an inviter");
  s.pending_invites = {{55, 9}};
  Check(!StartupInvite(s), "ordinary invites are not a Steam start");
  SocialModel reconnect;
  reconnect.status=SocialStatus::Connected; reconnect.lobby=77; reconnect.self=reconnect.owner=1;
  reconnect.rejoin_partner=2; reconnect.overlay_available=true;
  reconnect.members={{1,"Host",false}};
  reconnect.friends={{2,"Partner",FriendState::Online},{3,"Other friend",FriendState::Online}};
  const auto rejoin_rows=SocialRows(reconnect);
  Check(Find(rejoin_rows,SocialRowKind::OverlayInvite).enabled,"active host can open Steam rejoin invite");
  Check(Find(rejoin_rows,SocialRowKind::Friend,2).enabled && !Find(rejoin_rows,SocialRowKind::Friend,3).enabled,
      "active campaign invites only the previous partner");
  Check(CoopRowCommand(Find(rejoin_rows,SocialRowKind::Friend,2),reconnect,CoopPage::Lobby).has_value(),
      "controller rejoin invite uses the existing command path");
}
// UI1 (29 Oct, Design 1): the co-op pages' step bar, Start's lock reason, the modes in plain words, and the early
// access warning that nothing closes for 5 seconds.
bool NoDash(const char* text) {
  const std::string_view t(text);
  return t.find('-') == std::string_view::npos && t.find("\xE2\x80\x93") == std::string_view::npos &&
         t.find("\xE2\x80\x94") == std::string_view::npos;
}
void DesignOne() {
  EarlyAccessGate gate;
  Check(!gate.open() && !gate.Ready(100), "the warning starts closed");
  gate.Open(10.0);
  Check(gate.open() && !gate.Ready(10.0) && gate.SecondsLeft(10.0) == 5, "the warning opens with 5 seconds to go");
  Check(!gate.Ready(14.99) && gate.SecondsLeft(14.2) == 1 && gate.SecondsLeft(11.5) == 4, "the countdown counts down");
  Check(gate.Ready(15.0) && gate.SecondsLeft(15.0) == 0 && gate.Ready(60.0), "after 5 seconds it can be closed");
  gate.Open(70.0);
  Check(!gate.Ready(72.0), "every opening waits its own 5 seconds");
  gate.Close();
  Check(!gate.open() && !gate.Ready(100), "closed");

  SocialModel m;
  Check(CoopLobbyStep(m) == 1, "no lobby: choose what to play");
  m.status = SocialStatus::Lobby; m.lobby = 5; m.self = m.owner = 1; m.members = {{1, "Host", false}};
  Check(CoopLobbyStep(m) == 1, "a lobby without a choice is still step 1");
  m.choice = {1, 3, false};
  Check(CoopLobbyStep(m) == 2, "a choice and no partner: invite a friend");
  Check(std::string_view(CoopStartLock(m).key) == "Waiting for a friend to join", "Start waits for a friend");
  m.members.push_back({2, "Dazza", false});
  Check(CoopLobbyStep(m) == 3, "two players: both ready, then start");
  Check(std::string_view(CoopStartLock(m).key) == "Press Ready first", "Start asks the host to press Ready first");
  m.members[0].ready = true;
  const auto lock = CoopStartLock(m);
  Check(std::string_view(lock.key) == "Waiting for {0} to press Ready" && lock.name == "Dazza",
        "Start names the partner who is not ready");
  m.members[1].ready = true;
  Check(std::string_view(CoopStartLock(m).key) == "Waiting for both games to be ready", "can_start comes from the lobby");
  m.can_start = true;
  Check(!*CoopStartLock(m).key && CanStartLobby(m), "both ready: Start is not locked");
  m.start_requested = true;
  Check(std::string_view(CoopStartLock(m).key) == "Starting co-op...", "starting");
  for (int chapter = 0; chapter < 42; ++chapter) {
    const auto policy = kChapterRules[size_t(chapter)].policy;
    const std::string_view name = CoopModeName(chapter);
    Check(policy == ChapterPolicy::Together ? name == "Play together"
          : policy == ChapterPolicy::SoloRegroup ? name == "Apart, then regroup" : name == "Watch",
          "every chapter shows its co-op mode in plain words");
    Check(NoDash(CoopModeName(chapter)) && NoDash(CoopModeText(chapter)) && *CoopModeText(chapter),
          "mode words and their meaning have no dashes");
  }
  Check(std::string_view(CoopModeName(-1)) == "Play together" && std::string_view(CoopModeName(99)) == "Play together",
        "out of range chapters fall back safely");
}
}
int main() { FocusAndActions(); PreviewScenes(); DirectRows(); ChapterChoice(); CoopPages(); DesignOne(); std::printf("Social panel: %u checks passed\n", checks); }
