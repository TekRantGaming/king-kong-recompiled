// SDK/UI-free source tests. Claude builds and runs them, never starts Steam or a game.
#include "social_panel_logic.h"
#include "social_fake.h"
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
void InputPolicy() {
  for (auto page : {SocialPage::Page, SocialPage::Display, SocialPage::Graphics, SocialPage::Audio,
                    SocialPage::Controls, SocialPage::Gameplay, SocialPage::Cheats, SocialPage::Advanced,
                    SocialPage::About, SocialPage::Achievements, SocialPage::Workshop, SocialPage::PlayMenu,
                    SocialPage::Load, SocialPage::NewGame, SocialPage::Chapters, SocialPage::Extras,
                    SocialPage::Artbook, SocialPage::Setup, SocialPage::Shaders, SocialPage::Modal, SocialPage::Capture}) {
    SocialPanelState state;
    Check(!state.Update(false, page, true, false, 0.1f) && !state.blocking(), "disabled never opens on any page");
    const bool allowed = page <= SocialPage::Extras;
    Check(state.Update(true, page, true, false, 0.1f) == allowed && state.open() == allowed,
          "only normal launcher pages open Social; Artbook and input-priority contexts do not");
  }
  SocialPanelState state;
  Check(state.Update(true, SocialPage::Page, true, false, 0.1f), "RT/F4/click toggle opens");
  Check(state.slide() > 0 && state.slide() < 1 && state.blocking(), "partial slide blocks background");
  state.Update(true, SocialPage::Page, false, false, 0.1f);
  Check(state.slide() == 1, "slide completes in 0.2 seconds");
  state.Update(true, SocialPage::Page, false, true, 0.1f);
  Check(!state.open() && state.blocking(), "B/Esc closes and traps input through closing animation");
  state.Update(true, SocialPage::Page, false, false, 0.1f);
  Check(!state.blocking(), "background focus is available after close");
  state.Update(true, SocialPage::Page, true, false, 0.2f);
  state.Update(true, SocialPage::Page, true, false, 0.2f);
  Check(!state.open(), "RT again closes");
  state.Update(true, SocialPage::Page, true, false, 0.2f);
  state.Update(true, SocialPage::Modal, true, false, 0.2f);
  Check(!state.blocking(), "modal takes priority and prevents reopening");
  state.Update(true, SocialPage::Page, true, false, 0.2f);
  state.Update(false, SocialPage::Page, true, false, 0.01f);
  Check(!state.blocking(), "disabling co-op clears the drawer immediately");
  SocialTrigger trigger;
  Check(trigger.Update(true), "first RT press is an edge");
  for (int i = 0; i < 120; ++i) Check(!trigger.Update(true), "held RT never repeats");
  Check(!trigger.Update(false) && trigger.Update(true), "release rearms RT");
  trigger.Update(false);
  Check(!trigger.Update(true, false) && !trigger.Update(true, true), "inactive-window press stays stale");
  trigger.Update(false); Check(trigger.Update(true), "release after focus return rearms");
  // Context/off-state still tracks physical edges, before routing or suppressing page actions.
  trigger.Update(false); trigger.Update(true);
  Check(!trigger.Update(true), "holding RT while leaving Artbook cannot open Social");
}
void FocusAndActions() {
  auto fake = MakeSocialPreview();
  auto m = fake->Snapshot(); auto rows = SocialRows(m);
  Check(Find(rows, SocialRowKind::Accept, m.pending_invites.front().lobby).enabled, "idle may accept invite");
  Check(Find(rows, SocialRowKind::Create).enabled, "idle may create lobby");
  std::vector<SocialRowKind> order;
  for (const auto& r : rows) if (r.focusable()) order.push_back(r.key.kind);
  Check(order.size() == 11 && order[0] == SocialRowKind::Accept && order[1] == SocialRowKind::Decline &&
        order[2] == SocialRowKind::Create && order[3] == SocialRowKind::Friend, "focus order follows invites, lobby, friends");
  Check(SocialFocus(rows, {}) == 1, "first focus skips section header");
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
  Check(std::string_view(SocialStatusKey(m)) == "Waiting for host", "guest waits for host");
  Check(!SocialRowCommand(invite, m), "removed/stale invite cannot act");
  Check(fake->Scenario("host"), "host preview");
  m = fake->Snapshot(); rows = SocialRows(m);
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
  Check(Find(SocialRows(fake->Snapshot()), SocialRowKind::DirectChooseHost).enabled, "missing Steam leaves Direct IP available");
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
    Check(std::any_of(rows.begin(), rows.end(), [](const auto& r) { return r.key.kind == SocialRowKind::Create; })
          == (!joined && name!="needed" && name!="unavailable"), "Create lobby requires Steam and no existing lobby");
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
  auto rows=SocialRows(m);
  Check(Find(rows,SocialRowKind::DirectChooseHost).enabled && Find(rows,SocialRowKind::DirectChooseJoin).enabled,"Direct IP role choice works without Steam");
  Check(Find(SocialRows(m,1),SocialRowKind::DirectHost).enabled && Find(SocialRows(m,2),SocialRowKind::DirectJoin).enabled,"Direct IP confirmations work without Steam");
  Check(Find(rows,SocialRowKind::AddSteam).enabled,"recommendation links to existing Add to Steam");
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
}
int main() { InputPolicy(); FocusAndActions(); PreviewScenes(); DirectRows(); ChapterChoice(); std::printf("Social panel: %u checks passed\n", checks); }
