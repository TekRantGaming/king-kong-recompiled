// SDK-free source tests. Claude builds/runs these; no Steam, UI, sockets or game.
#include "steam_lobby.h"
#include "steam_runtime.h"
#include "steam_transport.h"
#include "steam_loopback_test.h"
#include "steam_pad.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <limits>

using namespace kk::coop;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* text) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
Compatibility Compatible() { return {kProtocolVersion, "test-version", "test-build", "test-mods"}; }
void DelayedLobbyReadiness() {
  using R = LobbyStartReadiness::Result;
  LobbyStartReadiness gate;
  Check(gate.Observe(false, false, 50) == R::Waiting, "waiting lobby does not start");
  Check(gate.Observe(true, false, 51) == R::Waiting, "starting metadata can precede member Ready data");
  Check(gate.Observe(true, false, 55) == R::Waiting, "allow delayed Steam member callback");
  Check(gate.Observe(true, true, 56) == R::Ready, "start after both Ready flags arrive");
  Check(gate.Observe(true, true, 70) == R::Ready, "Ready before starting metadata starts immediately");
  Check(gate.Observe(true, false, 80) == R::Waiting, "new readiness wait has its own deadline");
  Check(gate.Observe(true, false, 90) == R::TimedOut, "missing Ready data has a bounded failure");
  gate.Observe(false, false, 91);
  Check(gate.Observe(true, false, 100) == R::Waiting, "returning to waiting clears the old deadline");
  Check(gate.Observe(true, true, 101) == R::Ready, "retry can start after readiness arrives");
}
void Keys() {
  const auto local = Compatible(); const auto keys = EncodeLobbyKeys(local);
  Compatibility out;
  Check(keys.size() == 5 && DecodeLobbyKeys(keys, out), "keys round trip");
  Check(CheckCompatibility(local, out) == RejectReason::None, "decoded matches Hello compatibility");
  Check(CompatibleLobby(keys, local), "matching lobby");
  for (const auto* field : {"kkrc_product", "kkrc_protocol", "kkrc_version", "kkrc_build", "kkrc_mods"}) {
    auto broken = keys; broken.erase(field);
    Check(!DecodeLobbyKeys(broken, out), "required key missing");
    broken = keys; broken[field] = "wrong";
    Check(!CompatibleLobby(broken, local), "mismatched field rejects lobby");
  }
  for (const char* value : {"", "0", "-1", "65536", "1suffix", "18446744073709551616"}) {
    auto broken = keys; broken["kkrc_protocol"] = value;
    Check(!DecodeLobbyKeys(broken, out), "invalid protocol text");
  }
  auto broken = keys; broken["kkrc_mods"] = std::string(129, 'x');
  Check(!DecodeLobbyKeys(broken, out), "oversized metadata");
  broken["kkrc_mods"] = std::string("x\0y", 3);
  Check(!DecodeLobbyKeys(broken, out), "embedded NUL metadata");
  auto invalid = local; invalid.build.clear();
  Check(EncodeLobbyKeys(invalid).empty(), "invalid local values not advertised");
  broken = keys; broken["other_app_key"] = "ignored";
  Check(CompatibleLobby(broken, local), "additional keys allowed");
}
void Invites() {
  SocialId id = 42;
  Check(ParseLobbyId("18446744073709551615", id) && id == std::numeric_limits<uint64_t>::max(), "u64 lobby bound");
  for (const char* text : {"", "0", "01", "+1", "-1", " 1", "1 ", "18446744073709551616", "12;cmd"}) {
    id = 42; Check(!ParseLobbyId(text, id) && id == 42, "bad ID rejected without changing output");
  }
  Check(ParseLobbyConnect("+connect_lobby 1234", id) && id == 1234, "rich presence grammar");
  Check(!ParseLobbyConnect("+connect_lobby 1234 --other=1", id), "rich presence trailing command rejected");
  std::vector<std::string> args{"--kk_coop_enabled=1", "+connect_lobby", "1234", "--other=1"};
  Check(ConsumeLobbyArgument(args) == 1234 && args == std::vector<std::string>{"--kk_coop_enabled=1", "--other=1"}, "consume startup intent");
  args = {"+connect_lobby", "1", "+connect_lobby", "2"};
  Check(ConsumeLobbyArgument(args) == 0 && args.empty(), "duplicate startup intents rejected");
  args = {"+connect_lobby", "--other=1"};
  Check(ConsumeLobbyArgument(args) == 0 && args.size() == 1, "missing startup argument keeps unrelated switch");
  Check(ShortSocialHash(1234).size() == 8 && ShortSocialHash(1234) != ShortSocialHash(1235), "short diagnostic hash");
}
void Social() {
  auto world = MakeFakeSocialWorld();
  FakeSocialService host(world, 1, "Host", Compatible());
  FakeSocialService guest(world, 2, "Guest", Compatible());
  FakeSocialService third(world, 3, "Other", Compatible());
  Check(host.Snapshot().status == SocialStatus::Dormant, "fake starts dormant");
  Check(host.Act({SocialAction::Open}) && host.Snapshot().status == SocialStatus::Idle, "open fake");
  Check(host.Act({SocialAction::CreateLobby}), "create lobby");
  Check(host.Act({SocialAction::SelectChapter,0,false,{0,1,false}}),"select chapter before Ready");
  const auto lobby = host.Snapshot().lobby;
  Check(lobby && host.Snapshot().owner == 1 && host.Snapshot().members.size() == 1, "host is owner");
  Check(!host.Act({SocialAction::Start,0,false,host.Snapshot().choice}), "one player cannot start");
  Check(!guest.Act({SocialAction::Accept, lobby + 999}), "cannot accept unknown invite");
  // C81: a friend's open lobby is offered to join (Steam connect presence), like an invite.
  { const auto offered = guest.Snapshot().friends;
    Check(std::any_of(offered.begin(), offered.end(), [&](const auto& f) { return f.id == 1 && f.in_our_game && f.lobby == lobby; }),
          "a friend hosting an open lobby offers it"); }
  Check(host.Act({SocialAction::Invite, 2}) && host.Act({SocialAction::Invite, 2}), "invite deduplicated");
  Check(guest.Snapshot().pending_invites.size() == 1, "one pending invite");
  Check(guest.Act({SocialAction::Decline, lobby}) && guest.Snapshot().pending_invites.empty(), "decline locally");
  Check(host.Act({SocialAction::Invite, 2}) && guest.Act({SocialAction::Accept, lobby}), "accept invite");
  Check(guest.Snapshot().members.size() == 2 && guest.Snapshot().owner == 1, "two member lobby");
  Check(host.Act({SocialAction::Invite, 3}) && !third.Act({SocialAction::Accept, lobby}), "third member rejected");
  Check(host.Act({SocialAction::SetReady, 0, true,host.Snapshot().choice}) && !host.Snapshot().can_start, "one ready cannot start");
  Check(guest.Act({SocialAction::SetReady, 0, true,guest.Snapshot().choice}) && host.Snapshot().can_start, "both ready");
  Check(!guest.Act({SocialAction::Start}), "only owner starts");
  Check(host.Act({SocialAction::Start,0,false,host.Snapshot().choice}) && guest.Snapshot().start_requested, "start visible to both");
  Check(!third.Act({SocialAction::Accept, lobby}), "cannot join started lobby");
  Check(host.Act({SocialAction::Leave}), "host leaves");
  Check(guest.Snapshot().status == SocialStatus::Ended && guest.Snapshot().lobby == 0, "host departure ends guest lobby");
  Check(!guest.Snapshot().last_error.empty() && !guest.Snapshot().can_start, "departure has clear error");
  Check(host.Act({SocialAction::CreateLobby}), "fresh lobby after leave");
  Check(host.Act({SocialAction::SelectChapter,0,false,{0,2,false}}),"select fresh lobby chapter");
  const auto fresh = host.Snapshot().lobby;
  Check(host.Act({SocialAction::Invite, 2}) && guest.Act({SocialAction::Accept, fresh}), "join fresh lobby");
  Check(host.Act({SocialAction::SetReady, 0, true,host.Snapshot().choice}) && guest.Act({SocialAction::Leave}), "guest leaves");
  Check(host.Snapshot().members.size() == 1 && !host.Snapshot().members.front().ready, "membership invalidates ready");
  auto mismatch = Compatible(); mismatch.gameplay_digest = "different";
  FakeSocialService wrong(world, 4, "Different mods", mismatch);
  Check(host.Act({SocialAction::Invite, 4}) && !wrong.Act({SocialAction::Accept, fresh}), "compatibility gates invite acceptance");
  Check(host.Act({SocialAction::Invite, 2}) && guest.Act({SocialAction::Accept, fresh}), "partner can return to waiting lobby");
  Check(host.Act({SocialAction::SetReady, 0, true,host.Snapshot().choice}) && guest.Act({SocialAction::SetReady, 0, true,guest.Snapshot().choice}) && host.Act({SocialAction::Start,0,false,host.Snapshot().choice}), "fresh readiness required for another start");
  Check(guest.Act({SocialAction::Leave}) && host.Snapshot().status == SocialStatus::Ended, "partner leaving started session ends host session");
  FakeSocialService disabled(world, 5, "Disabled", Compatible(), false);
  Check(!disabled.Act({SocialAction::Open}) && disabled.Snapshot().status == SocialStatus::Disabled, "disabled fake stays disabled");
  FakeSocialService unavailable(world, 6, "Unavailable", Compatible(), true, false);
  Check(!unavailable.Act({SocialAction::CreateLobby}) && unavailable.Snapshot().status_text == "Steam not available in this build", "unavailable fake");
}
std::vector<uint8_t> Packet(MessageType type) {
  Message m; m.type = type; m.hello = Compatible();
  EnemyState enemy{{123, 0, EnemyFamily::Raptor}, 1, 2, 3, 0, 50};
  m.enemies = {{303, 3, 1}, 1, {enemy}};
  m.enemy_event = {{303, 3, 1}, EnemyEventKind::Spawned, enemy};
  m.cheats = {0x125}; m.inventory = {{303, 3, 1}, 1, 2, {0, 999, 0, 0}};
  m.damage = {{303, 3, 1}, 11, 1, enemy.id, 16388, 6, {1, 0, 0}};
  m.player_status = {{303, 3, 1}, 11, 2, 1, PlayerCondition::Hurt};
  m.ownership = {{303, 3, 1}, OwnershipPhase::Prepare, 1, EnemyOwner::Guest, enemy};
  m.life = {{1,303,3,0},11,2,3};
  m.camera.generation = 1; m.camera.level = 303; m.camera.cut = 1;
  m.story = {1,303,1,123,456,1,kStoryRecords,StoryPhase::Armed};
    m.shot.world = {303, 3, 1}; m.shot.number = 1; m.shot.weapon = 1; m.shot.direction = {1, 0, 0};
  std::vector<uint8_t> bytes; Check(Encode(m, bytes), "encode backend fixture"); return bytes;
}
struct Backend final : ISteamMessagesBackend {
  Backend* remote = nullptr;
  uint64_t self = 0, accepted = 0, closed = 0;
  bool send_ok = true;
  unsigned receives = 0;
  std::deque<SteamBackendMessage> incoming;
  bool Send(uint64_t, int channel, std::span<const uint8_t> bytes, bool reliable) override {
    if (!send_ok) return false;
    if (remote) remote->incoming.push_back({self, channel, reliable, false, {bytes.begin(), bytes.end()}});
    return true;
  }
  bool Receive(int channel, SteamBackendMessage& out) override {
    ++receives;
    auto it = std::find_if(incoming.begin(), incoming.end(), [&](const auto& m) { return m.channel == channel; });
    if (it == incoming.end()) return false;
    out = std::move(*it); incoming.erase(it); return true;
  }
  bool Accept(uint64_t id) override { accepted = id; return true; }
  void Close(uint64_t id) override { closed = id; }
  TransportStats Stats(uint64_t) const override { TransportStats s; s.ping_ms = 25; return s; }
};
void Messages() {
  Backend a, b; a.self = 1; b.self = 2; a.remote = &b; b.remote = &a;
  SteamMessagesTransport host(a, 2), guest(b, 1);
  Check(!host.SessionRequest(3) && a.closed == 3 && a.accepted == 0, "reject stranger session");
  Check(host.SessionRequest(2) && a.accepted == 2, "accept only partner");
  const auto hello = Packet(MessageType::Hello), pose = Packet(MessageType::PlayerPose);
  Check(host.Send(hello, true) && host.Send(pose, false), "reliable events and unreliable poses");
  Check(!host.Send(hello, false) && !host.Send(pose, true), "channel misuse rejected");
  guest.Poll(0);
  std::vector<uint8_t> out;
  Check(guest.Receive(out) && out == hello && guest.Receive(out) && out == pose && !guest.Receive(out), "framing preserved");
  const auto enemies = Packet(MessageType::EnemySnapshot), event = Packet(MessageType::EnemyEvent);
  Check(host.Send(enemies, false) && host.Send(event, true), "enemy snapshot/event channels accepted");
  Check(!host.Send(enemies, true) && !host.Send(event, false), "enemy channel misuse rejected");
  guest.Poll(.1);
  Check(guest.Receive(out) && out == event && guest.Receive(out) && out == enemies && !guest.Receive(out), "enemy transport framing preserved");
  const auto camera = Packet(MessageType::CameraPose);
  Check(host.Send(camera, false) && !host.Send(camera, true), "camera poses only use unreliable channel");
  guest.Poll(.15);
  Check(guest.Receive(out) && out == camera && !guest.Receive(out), "camera pose framing preserved");
    const auto shot = Packet(MessageType::ShotIntent);
    Check(host.Send(shot, true) && !host.Send(shot, false), "shot intent only uses reliable channel");
    guest.Poll(.2);
    Check(guest.Receive(out) && out == shot && !guest.Receive(out), "shot framing preserved");
  for (auto type : {MessageType::Cheats, MessageType::InventoryState, MessageType::DamageEvent, MessageType::PlayerStatus,
                    MessageType::OwnershipChange,
      MessageType::PlayerDowned, MessageType::PlayerRevived, MessageType::CameraCut}) {
    const auto packet = Packet(type);
    Check(host.Send(packet, true) && !host.Send(packet, false), "cheats, inventory and combat require reliable channel");
    guest.Poll(.3);
    Check(guest.Receive(out) && out == packet && !guest.Receive(out), "cheat/inventory/combat framing preserved");
  }
  b.incoming.push_back({3, 0, true, false, hello});
  b.incoming.push_back({1, 0, true, true, {}});
  b.incoming.push_back({1, 0, true, false, {1, 2, 3}});
  b.incoming.push_back({1, 0, false, false, hello});
  guest.Poll(1);
  Check(!guest.Receive(out) && guest.Stats().malformed == 4, "identity size framing and reliability checked");
  Check(guest.Stats().ping_ms == 25, "backend stats exposed");
  for (unsigned i = 0; i < 100; ++i) b.incoming.push_back({1, 0, true, false, hello});
  b.receives = 0; guest.Poll(2);
  Check(b.receives <= 64 && b.incoming.size() == 68, "bounded poll work");
  guest.Poll(3); guest.Poll(4);
  Check(guest.State() == TransportState::Failed, "reliable queue overflow fails closed");
  host.SessionFailed(3); Check(host.State() == TransportState::Ready, "foreign failure ignored");
  host.SessionFailed(2); Check(host.State() == TransportState::Failed, "partner failure surfaced");
  host.Close(); Check(a.closed == 2 && host.State() == TransportState::Closed, "close partner");
  Backend failure; failure.send_ok = false; SteamMessagesTransport fail(failure, 2);
  Check(!fail.Send(hello, true) && fail.State() == TransportState::Failed, "send failure surfaced");
  SteamMessagesTransport invalid(failure, 0);
  Check(invalid.State() == TransportState::Failed, "zero partner invalid");
}
void EveryProtocolMessage() {
  // Explicit expectations, independent of the adapter's Valid() fallback.
  const MessageType reliable[] = {
    MessageType::Hello, MessageType::HelloAck, MessageType::Reject, MessageType::Bye,
    MessageType::EnemyEvent, MessageType::ShotIntent, MessageType::Cheats, MessageType::InventoryState,
    MessageType::SessionStart, MessageType::SaveChunk, MessageType::LevelChange, MessageType::Loaded,
    MessageType::LoadRelease, MessageType::Checkpoint, MessageType::ChapterFinished, MessageType::ReloadRequest,
    MessageType::FlowAbort, MessageType::DamageEvent, MessageType::PlayerStatus, MessageType::OwnershipChange,
    MessageType::PlayerDowned, MessageType::PlayerRevived, MessageType::CameraCut,
    MessageType::StoryEvent, MessageType::TriggerRequest, MessageType::WorldDigest, MessageType::ShotReceipt,
    MessageType::StartPlan, MessageType::StartPlanAck,
    MessageType::ObjectState, MessageType::ObjectRequest
  };
  const MessageType unreliable[] = {MessageType::Ping, MessageType::Pong, MessageType::PlayerPose,
      MessageType::EnemySnapshot, MessageType::CameraPose};
  static_assert(kProtocolVersion == 24 && std::size(reliable)+std::size(unreliable) == 36);
  Backend a,b; a.self=1; b.self=2; a.remote=&b; b.remote=&a;
  SteamMessagesTransport at(a,2),bt(b,1);
  unsigned seen[35]{};
  for (bool r : {true,false}) {
    const auto types = r ? std::span<const MessageType>(reliable) : std::span<const MessageType>(unreliable);
    for (auto type : types) {
      ++seen[unsigned(type)];
      std::vector<uint8_t> packet,out;
      Check(Encode(SteamProbeMessage(type),packet) && packet.size()<=kMaxPacketBytes,"maximum probe fixture encodes within packet cap");
      Check(at.Send(packet,r) && b.incoming.size()==1,"every protocol message reaches Steam backend");
      Check(b.incoming.front().channel==(r ? 0 : 1) && b.incoming.front().reliable==r,"every type uses expected Steam channel and flag");
      bt.Poll(0);
      Check(bt.Receive(out) && out==packet && !bt.Receive(out),"every type preserves framing through Steam adapter");
      if (type!=MessageType::Ping && type!=MessageType::Pong) {
        Check(!at.Send(packet,!r),"every non-heartbeat rejects the wrong channel");
        const auto malformed=bt.Stats().malformed;
        b.incoming.push_back({a.self,r ? 1 : 0,!r,false,packet});
        bt.Poll(0);
        Check(!bt.Receive(out) && bt.Stats().malformed==malformed+1,
            "every non-heartbeat rejects incoming traffic on the wrong channel");
      }
      if (type==MessageType::EnemySnapshot) Check(packet.size()==976,"full enemy batch fits unreliable message limit");
      if (type==MessageType::SaveChunk) Check(packet.size()==798,"768-byte save chunk includes bounded framing");
      if (type==MessageType::ShotIntent) Check(packet.size()==558,"maximum shotgun hits fit packet cap");
    }
  }
  for (unsigned i=1;i<=34;++i) Check(seen[i]==1,"all 34 distinct protocol IDs covered");
}
void StoryOverMessages() {
  Backend a,b; a.self=1; b.self=2; a.remote=&b; b.remote=&a;
  SteamMessagesTransport at(a,2),bt(b,1);
  CoopSession host({true,Role::Host,12345,Compatible()},at),guest({true,Role::Guest,0,Compatible()},bt);
  for (unsigned i=0;i<20;++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected,
      "protocol 22 story session handshakes over Steam adapter");
  StoryMailbox h,g;
  uint32_t number=0;
  double now=.2;
  for (auto phase : {StoryPhase::Armed,StoryPhase::Started,StoryPhase::Advance,
      StoryPhase::Finished,StoryPhase::Recovery,StoryPhase::Checksum}) {
    auto event=SteamProbeMessage(MessageType::StoryEvent);
    event.story.number=++number; event.story.phase=phase;
    if (phase!=StoryPhase::Armed && phase!=StoryPhase::Started) event.story.record=7;
    Check(h.Submit(event),"host submits each story phase including checksum");
    h.Exchange(host); host.Tick(now);
    const auto pending=std::find_if(b.incoming.begin(),b.incoming.end(),[](const auto& packet) {
      Message m; return Decode(packet.bytes,m) && m.type==MessageType::StoryEvent;
    });
    Check(pending!=b.incoming.end() && pending->channel==0 && pending->reliable,
        "all story phases including checksum use reliable Steam channel 0");
    guest.Tick(now); g.Exchange(guest);
    const auto received=g.Take();
    Check(received.size()==1 && received[0].type==event.type && received[0].story==event.story,
        "each host story phase reaches guest game mailbox unchanged");
    if (ValidStoryRequest(phase)) {
      auto request=event; request.type=MessageType::TriggerRequest;
      if (phase==StoryPhase::Checksum) ++request.story.record;
      Check(g.Submit(request),"guest submits activation or differing checksum reply");
      g.Exchange(guest); guest.Tick(now+.001);
      const auto reply=std::find_if(a.incoming.begin(),a.incoming.end(),[](const auto& packet) {
        Message m; return Decode(packet.bytes,m) && m.type==MessageType::TriggerRequest;
      });
      Check(reply!=a.incoming.end() && reply->channel==0 && reply->reliable,
          "trigger requests and checksum replies use reliable Steam channel 0");
      host.Tick(now+.002); h.Exchange(host);
      const auto returned=h.Take();
      Check(returned.size()==1 && returned[0].type==request.type && returned[0].story==request.story,
          "guest activation and differing checksum reach host game mailbox unchanged");
    }
    now+=.01;
  }
  auto stale=SteamProbeMessage(MessageType::StoryEvent); ++stale.epoch;
  Check(h.Submit(stale),"stale story work can enter game mailbox before reconnect check");
  h.Exchange(host); host.Tick(now); guest.Tick(now); g.Exchange(guest);
  Check(g.Take().empty(),"stale story epoch never crosses the Steam session");
  host.End(now+.01); guest.Tick(now+.01); h.Exchange(host); g.Exchange(guest);
  Check(h.Take().empty() && g.Take().empty(),"Steam disconnect clears both story mailboxes");
}
void FullSaveOverMessages() {
  Backend a,b; a.self=1; b.self=2; a.remote=&b; b.remote=&a;
  SteamMessagesTransport at(a,2),bt(b,1);
  CoopSession host({true,Role::Host,12345,Compatible()},at),guest({true,Role::Guest,0,Compatible()},bt);
  FlowMailbox h,g;
  std::vector<uint8_t> save(kSessionSaveBytes);
  for (size_t i=0;i<save.size();++i) save[i]=uint8_t(i*37);
  auto start=SteamProbeMessage(MessageType::SessionStart).start;
  start.save_bytes=uint32_t(save.size()); start.save_hash=SessionSaveHash(save);
  Check(h.Begin(start,save),"full host save queued before Steam handshake");
  // Different PC steady-clock epochs must not be compared to each other.
  auto tick=[&](unsigned i) {
    const double t=i*.016;
    host.Tick(1000+t); guest.Tick(20000+t); h.Exchange(host,1000+t); g.Exchange(guest,20000+t);
  };
  for (unsigned i=0;i<80;++i) tick(i);
  Check(h.Snapshot().host && !g.Snapshot().host && g.Snapshot().start_ready && g.Snapshot().save==save,
      "entire 0x8040 save and SessionStart cross real Steam adapter with skewed clocks");
  Check(h.Submit(Message{.type=MessageType::Loaded,.flow=h.Snapshot().world}) &&
      g.Submit(Message{.type=MessageType::Loaded,.flow=g.Snapshot().world}),"both loads queued");
  for (unsigned i=80;i<100;++i) tick(i);
  Check(h.Snapshot().phase==FlowPhase::Playing && g.Snapshot().phase==FlowPhase::Playing,"Steam reliable load barrier releases both peers");
  h.Disconnect(); g.Disconnect();
  Check(h.Snapshot().phase==FlowPhase::Failed && g.Snapshot().phase==FlowPhase::Failed,"leave keeps failure visible for game return");
  Check(h.Begin(start,save),"fresh host can start after previous disconnect");
  h.Disconnect();
  Check(h.Begin(start,save),"cancelled pending save does not block subsequent lobby");
  h.Disconnect();
  // Reuse both mailboxes with swapped roles, as a newly created/joined lobby can.
  Backend c,d; c.self=3; d.self=4; c.remote=&d; d.remote=&c;
  SteamMessagesTransport ct(c,4),dt(d,3);
  CoopSession new_guest({true,Role::Guest,0,Compatible()},ct),new_host({true,Role::Host,54321,Compatible()},dt);
  Check(g.Begin(start,save),"former guest queues next host start");
  for (unsigned i=0;i<80;++i) {
    const double t=30000+i*.016;
    new_host.Tick(t); new_guest.Tick(t); g.Exchange(new_host,t); h.Exchange(new_guest,t);
  }
  Check(!h.Snapshot().host && g.Snapshot().host && h.Snapshot().start_ready && h.Snapshot().save==save,
      "new Steam session resets failed flow, role and save assembly");
}
void Stub() {
  unsigned paints = 0;
  SteamRuntimeOptions options; options.enabled = true; options.compatibility = Compatible();
  options.startup_lobby = 1234; options.request_ui_paint = [&] { ++paints; };
  SteamRuntime runtime(options);
  Check(runtime.Snapshot().status == SocialStatus::Dormant && paints == 0, "stub lazy construction");
  Check(runtime.Snapshot().pending_invites.size() == 1, "cold invite retained until UI ready");
  Check(!runtime.Act({SocialAction::Open}) && runtime.Snapshot().status == SocialStatus::Unavailable, "SDK missing stub");
  Check(runtime.Snapshot().status_text == "Steam not available in this build" && paints == 1, "clear stub status and repaint");
  Check(runtime.SessionSnapshot().state == SessionState::Idle, "stub cannot start session");
  Check(runtime.SelfTest([](const std::string&) {}) == 1, "stub self-test fails cleanly");
  Check(runtime.SelfTest([](const std::string&) {},true) == 1, "SDK-missing loopback probe fails cleanly");
  runtime.Shutdown(); Check(!runtime.Act({SocialAction::CreateLobby}), "shutdown rejects new work");
  SteamRuntime disabled(SteamRuntimeOptions{});
  Check(!disabled.Act({SocialAction::Open}) && disabled.Snapshot().status == SocialStatus::Disabled, "disabled runtime never opens Steam");
}
void SessionOverMessages() {
  Backend a, b; a.self = 1; b.self = 2; a.remote = &b; b.remote = &a;
  SteamMessagesTransport at(a, 2), bt(b, 1);
  CoopSession host({true, Role::Host, 12345, Compatible()}, at);
  CoopSession guest({true, Role::Guest, 0, Compatible()}, bt);
  for (unsigned i = 0; i < 700; ++i) { host.Tick(i * .01); guest.Tick(i * .01); }
  Check(host.State() == SessionState::Connected && guest.State() == SessionState::Connected, "C03 session handshakes over messages adapter");
  Check(at.Stats().sent > 8 && bt.Stats().sent > 8, "idle lobby heartbeats traverse messages transport");
  PlayerPose pose; pose.time = 7; pose.level = 1402; pose.x = 42;
  for (unsigned i = 700; i < 740; ++i) { pose.time = i * .01; host.Tick(i * .01, pose); guest.Tick(i * .01); }
  Check(guest.Partner(7.39).has_value() && guest.Partner(7.39)->x == 42, "poses cross unreliable messages channel");
  host.End(7.4); guest.Tick(7.4);
  Check(guest.State() == SessionState::Ended, "reliable Bye ends partner session");
}
void PreviousSessionMessages() {
  Backend a, b; a.self=1; b.self=2; a.remote=&b; b.remote=&a;
  SteamMessagesTransport at(a,2), bt(b,1);
  SessionConfig config{true,Role::Guest,0,Compatible()};
  config.expected_host_epoch=12345;
  CoopSession guest(config,bt);
  Message stale; stale.type=MessageType::Hello; stale.epoch=987; stale.hello=Compatible();
  auto inject=[&](const Message& m) {
    std::vector<uint8_t> bytes; Check(Encode(m,bytes),"encode previous session message");
    b.incoming.push_back({1,0,true,false,std::move(bytes)});
  };
  inject(stale); stale.type=MessageType::HelloAck; inject(stale); stale.type=MessageType::Bye; inject(stale);
  guest.Tick(0);
  Check(guest.State()==SessionState::Connecting && guest.Epoch()==0,
        "old Hello Ack and Bye cannot select the new lobby epoch or end its guest");
  CoopSession host({true,Role::Host,12345,Compatible()},at);
  for(unsigned i=1;i<300;++i) { host.Tick(i*.01); guest.Tick(i*.01); }
  Check(host.State()==SessionState::Connected && guest.State()==SessionState::Connected && guest.Epoch()==12345,
        "guest accepts the advertised host after delayed messages from the last session");
}
}
// C82: Steam Input actions become one Xbox pad; the manifest names every action the reader asks for.
void SteamPad() {
  namespace sp = kk::steam_pad;
  bool down[sp::kDigitalCount] = {};
  float axes[sp::kAnalogCount][2] = {};
  Check(sp::FromActions(down, axes) == sp::State{}, "idle actions give an idle pad");
  uint16_t all = 0;
  for (int i = 0; i < sp::kDigitalCount; ++i) {
    bool one[sp::kDigitalCount] = {}; one[i] = true;
    const auto s = sp::FromActions(one, axes);
    Check(s.buttons == sp::kDigitalBits[i] && !(all & s.buttons), "each digital action is its own XInput button");
    all |= s.buttons;
  }
  Check(all == 0xF3FF, "every XInput button except guide and the unused bits");
  axes[0][0] = 1; axes[0][1] = -1; axes[1][0] = -0.5f; axes[1][1] = 2; axes[2][0] = 1; axes[3][0] = 0.5f;
  auto s = sp::FromActions(down, axes);
  Check(s.lx == 32767 && s.ly == -32767 && s.rx == -16384 && s.ry == 32767, "sticks scale and clamp to XInput range");
  Check(s.lt == 255 && s.rt == 128, "triggers scale to 0..255");
  axes[0][0] = std::numeric_limits<float>::quiet_NaN();
  Check(sp::FromActions(down, axes).lx == 0, "a broken value reads as centred");
  sp::State a, b; a.buttons = 0x1000; a.lx = 100; a.lt = 10; b.buttons = 0x0001; b.lx = -2000; b.lt = 5;
  sp::Merge(a, b);
  Check(a.buttons == 0x1001 && a.lx == -2000 && a.lt == 10, "two managed pads merge, stronger axis wins");
  sp::Merge(a, a);
  Check(a.buttons == 0x1001 && a.lx == -2000, "merging the same pad twice changes nothing");
  // Snapshot: only fresh samples with a managed controller count.
  sp::State out;
  Check(!sp::Read(out, 1) && sp::Controllers() == 0, "nothing before Steam Input");
  sp::State held; held.buttons = 0x1000;
  sp::Publish(1, held, 10);
  Check(sp::Read(out, 10.1) && out == held && sp::Controllers() == 1, "fresh managed pad reads");
  Check(!sp::Read(out, 10.5), "stale sample never holds a button");
  sp::Publish(0, held, 11);
  Check(!sp::Read(out, 11), "no managed controller gives no pad (SDL keeps the physical one)");
  sp::Publish(1, held, 12); sp::Stopped();
  Check(!sp::Read(out, 12) && sp::Controllers() == 0, "Steam shutdown clears the pad");
  uint16_t l = 0, r = 0;
  sp::SetVibration(100, 200);
  Check(sp::TakeVibration(l, r) && l == 100 && r == 200 && !sp::TakeVibration(l, r), "rumble forwarded once");
  // Manifest and bindings.
  const auto manifest = sp::ManifestText();
  Check(manifest.find(sp::kActionSet) != std::string::npos, "manifest has our action set");
  for (int i = 0; i < sp::kDigitalCount; ++i)
    Check(manifest.find(std::string("\"") + sp::kDigitalNames[i] + "\"") != std::string::npos, "manifest has every button action");
  for (int i = 0; i < sp::kAnalogCount; ++i)
    Check(manifest.find(std::string("\"") + sp::kAnalogNames[i] + "\"") != std::string::npos, "manifest has every analog action");
  for (const char* type : {"controller_xboxone", "controller_xbox360", "controller_ps5", "controller_switch_pro"})
    Check(manifest.find(std::string("kk_input_") + type + ".vdf") != std::string::npos, "manifest names each configuration");
  const auto binding = sp::BindingText("controller_xboxone");
  for (int i = 0; i < sp::kDigitalCount; ++i)
    Check(binding.find(std::string("game_action ") + sp::kActionSet + " " + sp::kDigitalNames[i] + "\"") != std::string::npos,
          "configuration binds every button action");
  for (int i = 0; i < sp::kAnalogCount; ++i)
    Check(binding.find(std::string("\"") + sp::kActionSet + "\"\t\t\"" + sp::kAnalogNames[i] + "\"") != std::string::npos,
          "configuration binds every analog action");
  int depth = 0; bool balanced = true;
  for (const auto* text : {&manifest, &binding})
    for (char c : *text) { depth += c == '{' ? 1 : c == '}' ? -1 : 0; balanced &= depth >= 0; }
  Check(balanced && depth == 0, "manifest and configuration braces balance");
  for (const auto* text : {&manifest, &binding})
    Check(text->find('-') == std::string::npos, "no dashes in text Steam shows");
}
int main() {
  DelayedLobbyReadiness();
  PreviousSessionMessages();
  Keys(); Invites(); Social(); Messages(); EveryProtocolMessage(); StoryOverMessages(); FullSaveOverMessages(); SessionOverMessages(); Stub();
  SteamPad();
  std::printf("Steam service tests: %u checks OK\n", checks);
}
