#include "steam_runtime.h"
#include "steam_transport.h"
#include "coop_pose.h"
#if KK_HAS_STEAM
#include <rex/logging.h>
#if defined(KK_DEV_TOOLS)
#include "steam_loopback_test.h"
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#else
#include <fstream>
#endif
#if KK_HAS_STEAM
#include <steam/steam_api.h>
#include <steam/steam_api_flat.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#endif

namespace kk::coop {
SocialId CaptureStartupLobby() {
  std::vector<std::string> args;
#ifdef _WIN32
  int count = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!argv) return 0;
  for (int i = 1; i < count; ++i) {
    std::string s;
    for (const wchar_t* p = argv[i]; *p && s.size() < 128; ++p) s += *p < 128 ? char(*p) : '?';
    args.push_back(std::move(s));
  }
  LocalFree(argv);
#elif defined(__APPLE__)
  for (int i = 1; i < *_NSGetArgc(); ++i) args.emplace_back((*_NSGetArgv())[i]);
#else
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string s; std::getline(in, s, '\0');
  while (args.size() < 4096 && std::getline(in, s, '\0')) args.push_back(s);
#endif
  return ConsumeLobbyArgument(args);
}
namespace {
constexpr size_t kMaxCommands = 64;
#if KK_HAS_STEAM
double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr size_t kMaxInvites = 16, kMaxFriends = 256;
// Declarations/types come from SDK 1.65. Every invocation goes through a resolved pointer.
#define KK_STEAM_EXPORTS(X) \
 X(SteamAPI_InitFlat) X(SteamAPI_Shutdown) X(SteamAPI_GetHSteamPipe) \
 X(SteamAPI_ManualDispatch_Init) X(SteamAPI_ManualDispatch_RunFrame) \
 X(SteamAPI_ManualDispatch_GetNextCallback) X(SteamAPI_ManualDispatch_FreeLastCallback) \
 X(SteamAPI_ManualDispatch_GetAPICallResult) \
 X(SteamAPI_SteamUser_v023) X(SteamAPI_SteamFriends_v018) X(SteamAPI_SteamUtils_v011) \
 X(SteamAPI_SteamMatchmaking_v009) X(SteamAPI_SteamNetworkingMessages_SteamAPI_v002) \
 X(SteamAPI_SteamNetworkingUtils_SteamAPI_v004) \
 X(SteamAPI_ISteamUser_GetSteamID) X(SteamAPI_ISteamUser_BLoggedOn) \
 X(SteamAPI_ISteamUtils_GetAppID) X(SteamAPI_ISteamUtils_IsOverlayEnabled) \
 X(SteamAPI_ISteamUtils_GetImageSize) X(SteamAPI_ISteamUtils_GetImageRGBA) \
 X(SteamAPI_ISteamFriends_GetFriendCount) X(SteamAPI_ISteamFriends_GetFriendByIndex) \
 X(SteamAPI_ISteamFriends_GetPersonaState) X(SteamAPI_ISteamFriends_GetPersonaName) \
 X(SteamAPI_ISteamFriends_GetFriendPersonaName) \
 X(SteamAPI_ISteamFriends_GetFriendPersonaState) X(SteamAPI_ISteamFriends_GetFriendRelationship) \
 X(SteamAPI_ISteamFriends_GetFriendGamePlayed) X(SteamAPI_ISteamFriends_GetSmallFriendAvatar) \
 X(SteamAPI_ISteamFriends_GetFriendRichPresence) X(SteamAPI_ISteamFriends_RequestFriendRichPresence) \
 X(SteamAPI_ISteamFriends_SetRichPresence) X(SteamAPI_ISteamFriends_ClearRichPresence) \
 X(SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog) \
 X(SteamAPI_ISteamMatchmaking_CreateLobby) X(SteamAPI_ISteamMatchmaking_JoinLobby) \
 X(SteamAPI_ISteamMatchmaking_LeaveLobby) X(SteamAPI_ISteamMatchmaking_RequestLobbyData) \
 X(SteamAPI_ISteamMatchmaking_GetLobbyData) X(SteamAPI_ISteamMatchmaking_SetLobbyData) \
 X(SteamAPI_ISteamMatchmaking_GetNumLobbyMembers) X(SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex) \
 X(SteamAPI_ISteamMatchmaking_GetLobbyMemberData) X(SteamAPI_ISteamMatchmaking_SetLobbyMemberData) \
 X(SteamAPI_ISteamMatchmaking_GetLobbyOwner) X(SteamAPI_ISteamMatchmaking_SetLobbyJoinable) \
 X(SteamAPI_ISteamMatchmaking_InviteUserToLobby) \
 X(SteamAPI_ISteamNetworkingMessages_SendMessageToUser) \
 X(SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel) \
 X(SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser) \
 X(SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser) \
 X(SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo) \
 X(SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess) X(SteamAPI_ISteamNetworkingUtils_SetConfigValue)
struct Api {
#define DECLARE(name) decltype(&::name) name = nullptr;
  KK_STEAM_EXPORTS(DECLARE)
#undef DECLARE
  // Deliberately retained until process exit: Steam shutdown does not unhook the overlay.
  void* module = nullptr;
  bool Load(const std::filesystem::path& folder) {
    if (folder.empty() || !folder.is_absolute()) return false;
#ifdef _WIN32
    module = LoadLibraryExW((folder / "steam_api64.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto symbol = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name)); };
#else
#ifdef __APPLE__
    const auto file = folder / "libsteam_api.dylib";
#else
    const auto file = folder / "libsteam_api.so";
#endif
    module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto symbol = [&](const char* name) { return dlsym(module, name); };
#endif
    if (!module) return false;
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(symbol(#name)); if (!name) return false;
    KK_STEAM_EXPORTS(RESOLVE)
#undef RESOLVE
    return true;
  }
};
std::string Text(const char* s, size_t limit = 128) {
  if (!s) return {};
  size_t n = 0; while (n <= limit && s[n]) ++n;
  return n <= limit ? std::string(s, n) : std::string{};
}
template<class T> bool Callback(const CallbackMsg_t& m, T& out) {
  if (m.m_iCallback != T::k_iCallback || m.m_cubParam != sizeof(T) || !m.m_pubParam) return false;
  std::memcpy(&out, m.m_pubParam, sizeof(T)); return true;
}
#endif
}

struct SteamRuntime::Impl
#if KK_HAS_STEAM
    final : ISteamMessagesBackend
#endif
{
  SteamRuntimeOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  SocialModel published;
  SteamSessionView session_view;
  std::optional<PlayerPose> local_pose;
  enemies::Frame local_enemies;
  std::optional<CheatSet> local_cheats;
  ShotMailbox shots;
  CombatMailbox combat;
  OwnershipMailbox ownership;
  StoryMailbox story;
  CameraMailbox camera;
  FlowMailbox flow;
  const char* local_failure = "not-published";
#if defined(KK_DEV_TOOLS)
  double next_pose_log = 0;
#endif
  struct PendingCommand { SocialCommand command; std::optional<Compatibility> compatibility; };
  std::deque<PendingCommand> commands;
  std::thread worker;
  bool used = false, stopped = false;
  std::atomic<bool> stop{false};
  bool self_test = false;
#if defined(KK_DEV_TOOLS)
  bool loopback_test = false;
#endif
  int test_result = 1;
  std::function<void(const std::string&)> test_log;
  explicit Impl(SteamRuntimeOptions o) : options(std::move(o)) {
    published.status = options.enabled ? SocialStatus::Dormant : SocialStatus::Disabled;
    published.status_text = options.enabled ? "Choose Steam or Direct IP" : "Co-op is disabled";
    if (options.startup_lobby) published.pending_invites.push_back({options.startup_lobby, 0});
  }
  ~Impl() { Shutdown(); }
  void Shutdown() {
    { std::lock_guard lock(mutex); stopped = true; stop = true; commands.clear(); }
    wake.notify_all();
    if (worker.joinable()) worker.join();
  }
  void Paint() { if (options.request_ui_paint) options.request_ui_paint(); }
  bool Enqueue(SocialCommand c, std::optional<Compatibility> compatibility) {
    std::unique_lock lock(mutex);
    if ((!used || published.status == SocialStatus::Unavailable) && !stopped && c.action == SocialAction::Decline) {
      std::erase_if(published.pending_invites, [&](auto i) { return i.lobby == c.id; });
      lock.unlock(); Paint(); return true;
    }
    if (!options.enabled || stopped || commands.size() >= kMaxCommands) return false;
#if !KK_HAS_STEAM
    (void)compatibility;
    published.status = SocialStatus::Unavailable; published.status_text = "Steam not available in this build";
    published.last_error = published.status_text;
    lock.unlock(); Paint(); return false;
#else
    if (used && published.status == SocialStatus::Unavailable) return false;
    if (!used && c.action != SocialAction::Open && c.action != SocialAction::CreateLobby && c.action != SocialAction::Accept &&
        c.action != SocialAction::Resume) return false;
    commands.push_back({c, std::move(compatibility)});
    if (!used) {
      used = true;
      published.status_text = "Connecting to Steam";
      worker = std::thread([this] { Run(); });
    }
    lock.unlock(); wake.notify_all(); return true;
#endif
  }
#if KK_HAS_STEAM
  Api api;
  ISteamUser* user = nullptr;
  ISteamFriends* friends = nullptr;
  ISteamUtils* utils = nullptr;
  ISteamMatchmaking* matchmaking = nullptr;
  ISteamNetworkingMessages* messages = nullptr;
  ISteamNetworkingUtils* networking = nullptr;
  HSteamPipe pipe = 0;
  bool initialized = false;
  SocialModel model;
  uint64_t epoch = 0, generation = 1, recorded_owner = 0, joining = 0, joining_owner = 0, validating = 0, partner = 0;
  SteamAPICall_t create_call = 0, join_call = 0;
  std::map<SteamAPICall_t, int> abandoned;
  double operation_deadline = 0, next_friends = 0, next_lobby = 0, next_invite = 0;
  int logged_friend_count = -1;
  std::string ready_token;
  std::unique_ptr<SteamMessagesTransport> transport;
  std::unique_ptr<CoopSession> session;
  SessionState last_session_state = SessionState::Idle;
  // C80 resumed session: the partner and token from the hand over file (set before the Resume command).
  uint64_t resume_partner = 0, resume_token = 0;
  bool resume_host = false, presence_lobby = false, presence_wanted = false;
  void Publish() {
    model.can_start = CanStartLobby(model);
    { std::lock_guard lock(mutex); published = model; }
    Paint();
  }
  void Error(const char* text) { model.last_error = text; Publish(); }
  void Status(SocialStatus status, const char* text) { model.status = status; model.status_text = text; Publish(); }
  bool Friend(uint64_t id) { return id && api.SteamAPI_ISteamFriends_GetFriendRelationship(friends, id) == k_EFriendRelationshipFriend; }
  bool Init() {
    REXLOG_INFO("KK co-op: initializing Steam after an explicit Steam request (app 480)");
#ifdef _WIN32
    if (!SetEnvironmentVariableW(L"SteamAppId", L"480") || !SetEnvironmentVariableW(L"SteamGameId", L"480")) return false;
#else
    if (setenv("SteamAppId", "480", 1) || setenv("SteamGameId", "480", 1)) return false;
#endif
    if (!api.Load(options.executable_folder)) return false;
    SteamErrMsg error{};
    if (api.SteamAPI_InitFlat(&error) != k_ESteamAPIInitResult_OK) return false;
    initialized = true;
    api.SteamAPI_ManualDispatch_Init(); pipe = api.SteamAPI_GetHSteamPipe();
    user = api.SteamAPI_SteamUser_v023(); friends = api.SteamAPI_SteamFriends_v018();
    utils = api.SteamAPI_SteamUtils_v011(); matchmaking = api.SteamAPI_SteamMatchmaking_v009();
    messages = api.SteamAPI_SteamNetworkingMessages_SteamAPI_v002();
    networking = api.SteamAPI_SteamNetworkingUtils_SteamAPI_v004();
    if (!pipe || !user || !friends || !utils || !matchmaking || !messages || !networking ||
        api.SteamAPI_ISteamUtils_GetAppID(utils) != 480 || !api.SteamAPI_ISteamUser_BLoggedOn(user)) return false;
    model.self = api.SteamAPI_ISteamUser_GetSteamID(user);
    if (!model.self) return false;
    const int32_t no_ice = 0, buffer = 64 * int32_t(kMaxPacketBytes), max_message = int32_t(kMaxPacketBytes), max_messages = 64;
    if (!api.SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking, k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &no_ice) ||
        !api.SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking, k_ESteamNetworkingConfig_SendBufferSize,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &buffer) ||
        !api.SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking, k_ESteamNetworkingConfig_RecvMaxMessageSize,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &max_message) ||
        !api.SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking, k_ESteamNetworkingConfig_RecvBufferSize,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &buffer) ||
        !api.SteamAPI_ISteamNetworkingUtils_SetConfigValue(networking, k_ESteamNetworkingConfig_RecvBufferMessages,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &max_messages)) return false;
    api.SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(networking);
    model.overlay_available = api.SteamAPI_ISteamUtils_IsOverlayEnabled(utils);
    if (!self_test) api.SteamAPI_ISteamFriends_SetRichPresence(friends, kPresenceKey, kProductValue);
    return true;
  }
  void Friends() {
    // Refresh at most every five seconds; no per-frame avatar allocation or rich-presence request.
    std::vector<SocialFriend> list;
    const int count = std::clamp(api.SteamAPI_ISteamFriends_GetFriendCount(friends, k_EFriendFlagImmediate), 0, int(kMaxFriends));
    if (count!=logged_friend_count) {
      REXLOG_INFO("KK co-op Steam: Social friend rows={}",count);
      logged_friend_count=count;
    }
    for (int i = 0; i < count; ++i) {
      SocialFriend f; f.id = api.SteamAPI_ISteamFriends_GetFriendByIndex(friends, i, k_EFriendFlagImmediate);
      f.name = Text(api.SteamAPI_ISteamFriends_GetFriendPersonaName(friends, f.id));
      const auto state = api.SteamAPI_ISteamFriends_GetFriendPersonaState(friends, f.id);
      f.state = state == k_EPersonaStateOffline || state == k_EPersonaStateInvisible ? FriendState::Offline :
          state == k_EPersonaStateAway || state == k_EPersonaStateSnooze ? FriendState::Away :
          state == k_EPersonaStateBusy ? FriendState::Busy : FriendState::Online;
      FriendGameInfo_t game{};
      if (f.state == FriendState::Online && api.SteamAPI_ISteamFriends_GetFriendGamePlayed(friends, f.id, &game)) f.state = FriendState::InGame;
      f.in_our_game = Text(api.SteamAPI_ISteamFriends_GetFriendRichPresence(friends, f.id, kPresenceKey)) == kProductValue;
      // C81: a friend whose game offers a lobby can be joined from Join a game (the same check as an invite).
      if (f.in_our_game && !ParseLobbyConnect(Text(api.SteamAPI_ISteamFriends_GetFriendRichPresence(friends, f.id, "connect")), f.lobby)) f.lobby = 0;
      auto old = std::find_if(model.friends.begin(), model.friends.end(), [&](const auto& p) { return p.id == f.id; });
      if (old == model.friends.end()) api.SteamAPI_ISteamFriends_RequestFriendRichPresence(friends, f.id);
      const int avatar = api.SteamAPI_ISteamFriends_GetSmallFriendAvatar(friends, f.id);
      uint32 w = 0, h = 0;
      if (avatar > 0 && api.SteamAPI_ISteamUtils_GetImageSize(utils, avatar, &w, &h) && w && h && w <= 64 && h <= 64) {
        f.avatar_rgba.resize(size_t(w) * h * 4);
        if (api.SteamAPI_ISteamUtils_GetImageRGBA(utils, avatar, f.avatar_rgba.data(), int(f.avatar_rgba.size()))) {
          f.avatar_width = w; f.avatar_height = h;
        } else f.avatar_rgba.clear();
      }
      list.push_back(std::move(f));
    }
    model.friends = std::move(list); Publish();
  }
  LobbyKeys ReadKeys(uint64_t lobby) {
    LobbyKeys keys;
    for (const char* key : {kProductKey, "kkrc_protocol", "kkrc_version", "kkrc_build", "kkrc_mods"})
      keys[key] = Text(api.SteamAPI_ISteamMatchmaking_GetLobbyData(matchmaking, lobby, key));
    return keys;
  }
  std::string Data(uint64_t lobby, const char* key) { return Text(api.SteamAPI_ISteamMatchmaking_GetLobbyData(matchmaking, lobby, key)); }
  bool SetData(const char* key, const std::string& value) {
    return api.SteamAPI_ISteamMatchmaking_SetLobbyData(matchmaking, model.lobby, key, value.c_str());
  }
  void StopSession() {
    flow.Disconnect();
    if (session) {
      session->End(Now());
      // Give reliable Bye a bounded best-effort flush before closing the underlying session.
      const double until = Now() + .2;
      while (transport && transport->State() == TransportState::Ready && Now() < until) {
        transport->Poll(Now()); std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    session.reset();
    if (transport) transport->Close();
    transport.reset(); partner = 0; last_session_state = SessionState::Idle; resume_partner = 0;
    { std::lock_guard lock(mutex); session_view = {}; }
    shots.Clear();
    combat.Clear();
    ownership.Clear();
    story.Clear();
    camera.Clear();
  }
  void RetireCalls() {
    if (create_call) abandoned.emplace(create_call, LobbyCreated_t::k_iCallback);
    if (join_call) abandoned.emplace(join_call, LobbyEnter_t::k_iCallback);
    create_call = join_call = 0;
  }
  void Leave(const char* reason = nullptr) {
    StopSession();
    if (model.lobby) api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, model.lobby);
    api.SteamAPI_ISteamFriends_ClearRichPresence(friends);
    if (!self_test && !stop) api.SteamAPI_ISteamFriends_SetRichPresence(friends, kPresenceKey, kProductValue);
    model.lobby = model.owner = recorded_owner = epoch = validating = joining = joining_owner = 0;
    model.members.clear(); model.start_requested = false; model.choice={}; ready_token.clear();
    presence_lobby = presence_wanted = false;
    RetireCalls();
    model.status = reason ? SocialStatus::Ended : SocialStatus::Idle;
    model.status_text = reason ? reason : "Steam connected";
    if (reason) model.last_error = reason;
    Publish();
  }
  bool RefreshLobby() {
    if (!model.lobby) return false;
    if (presence_lobby) {
      // The resumed host's lobby is for Steam's Join game and invites only: the session runs by SteamID.
      if (api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(matchmaking, model.lobby) != model.self) {
        api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, model.lobby); model.lobby = 0; presence_lobby = false;
      }
      return session != nullptr;
    }
    const auto owner = api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(matchmaking, model.lobby);
    if (owner != recorded_owner) { Leave("The host left the lobby"); return false; }
    if (!CompatibleLobby(ReadKeys(model.lobby), options.compatibility) || Data(model.lobby, "kkrc_epoch") != std::to_string(epoch)) {
      Leave("Lobby compatibility changed"); return false;
    }
    const int count = api.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(matchmaking, model.lobby);
    if (count < 1 || count > 2) { Leave("Invalid lobby membership"); return false; }
    std::vector<SocialMember> members;
    bool self_seen = false, owner_seen = false;
    for (int i = 0; i < count; ++i) {
      const auto id = api.SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(matchmaking, model.lobby, i);
      self_seen |= id == model.self; owner_seen |= id == recorded_owner;
      if (!id || (id != model.self && !Friend(id)) || (!members.empty() && members.front().id == id)) {
        Leave("Lobby contains an unapproved member"); return false;
      }
      members.push_back({id, Text(api.SteamAPI_ISteamFriends_GetFriendPersonaName(friends, id)), false});
    }
    if (!self_seen || !owner_seen) { Leave("The host left the lobby"); return false; }
    std::sort(members.begin(), members.end(), [](auto& a, auto& b) { return a.id < b.id; });
    if (!ParseLobbyId(Data(model.lobby, "kkrc_generation"), generation)) { Leave("Invalid ready generation"); return false; }
    std::string token = std::to_string(epoch) + ":" + std::to_string(generation);
    const auto choice_value=Data(model.lobby,"kkrc_choice");
    model.choice=DecodeLobbyChoice(choice_value);
    token += ":" + choice_value;
    for (const auto& m : members) token += ":" + std::to_string(m.id);
    if (!ready_token.empty() && token != ready_token && model.start_requested) { Leave("The partner left the session"); return false; }
    if (token != ready_token)
      api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(matchmaking, model.lobby, kReadyKey, "0");
    ready_token = token;
    for (auto& m : members) m.ready = Text(api.SteamAPI_ISteamMatchmaking_GetLobbyMemberData(matchmaking, model.lobby, m.id, kReadyKey)) == ready_token;
    model.members = std::move(members); model.owner = owner;
    const auto phase = Data(model.lobby, "kkrc_phase");
    if (phase == "starting" && !model.start_requested) {
      if (model.members.size() != 2 || !model.members[0].ready || !model.members[1].ready) {
        Leave("The lobby was not ready to start"); return false;
      }
      StartSession();
    } else if (phase != "waiting" && phase != "starting") { Leave("Invalid lobby phase"); return false; }
    Publish(); return true;
  }
  void StartSession() {
    if (session || model.members.size() != 2) return;
    for (const auto& m : model.members) if (m.id != model.self) partner = m.id;
    if (!partner || !Friend(partner)) return;
    transport = std::make_unique<SteamMessagesTransport>(*this, partner);
    SessionConfig config{true, model.self == recorded_owner ? Role::Host : Role::Guest,
                         model.self == recorded_owner ? epoch : 0, options.compatibility};
    // Flat SDK call owned by this worker. Persona stays out of logs and lobby metadata.
    config.display_name = SanitizeDisplayName(Text(api.SteamAPI_ISteamFriends_GetPersonaName(friends)));
    session = std::make_unique<CoopSession>(config, *transport);
    model.start_requested = true;
    api.SteamAPI_ISteamFriends_SetRichPresence(friends, "connect", nullptr);
    Status(SocialStatus::Starting, "Connecting to partner");
  }
  void ResumeSession(uint64_t peer) {
    if (model.lobby) Leave();
    StopSession();
    uint64_t token = 0; bool host = false;
    { std::lock_guard lock(mutex); token = resume_token; host = resume_host; }
#if defined(KK_DEV_TOOLS)
    // One-account check (C80): the lobby a resumed host recreates, without a partner to reconnect to.
    if (host && std::getenv("KK_DEV_RESUME_LOBBY_NOW") && !create_call) {
      presence_lobby = true;
      create_call = api.SteamAPI_ISteamMatchmaking_CreateLobby(matchmaking, k_ELobbyTypeFriendsOnly, 2);
      operation_deadline = Now() + 20;
      REXLOG_INFO("KK dev co-op Steam: KK_DEV_RESUME_LOBBY_NOW: recreating the friends-only lobby now (request {})", create_call ? "sent" : "failed");
    }
#endif
    if (!peer || !token || peer == model.self || !Friend(peer)) {
      REXLOG_WARN("KK co-op Steam: resumed partner is not a Steam friend of this account; cannot reconnect");
      Status(SocialStatus::Ended, "Partner unavailable"); return;
    }
    partner = resume_partner = peer;
    transport = std::make_unique<SteamMessagesTransport>(*this, partner);
    SessionConfig config{true, host ? Role::Host : Role::Guest, host ? token : 0, options.compatibility};
    config.display_name = SanitizeDisplayName(Text(api.SteamAPI_ISteamFriends_GetPersonaName(friends)));
    config.resume_token = token; config.wait_for_peer = host;
    session = std::make_unique<CoopSession>(config, *transport);
    model.start_requested = true; presence_wanted = host;
    REXLOG_INFO("KK co-op Steam: resumed session {} reconnecting to the partner by SteamID", host ? "host" : "guest");
    Status(SocialStatus::Starting, "Connecting to partner");
  }
  void Created(const LobbyCreated_t& c, bool failed) {
    create_call = 0;
    if (failed || c.m_eResult != k_EResultOK || !c.m_ulSteamIDLobby) {
      Status(SocialStatus::Idle, "Steam connected"); Error("Could not create lobby"); if (self_test) stop = true; return;
    }
    if (stop) { api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, c.m_ulSteamIDLobby); return; }
    model.lobby = c.m_ulSteamIDLobby; model.owner = recorded_owner = model.self;
    std::random_device random; epoch = (uint64_t(random()) << 32) ^ random(); if (!epoch) epoch = 1;
    bool ok = api.SteamAPI_ISteamMatchmaking_SetLobbyJoinable(matchmaking, model.lobby, false);
    const auto keys = EncodeLobbyKeys(options.compatibility);
    ok = ok && !keys.empty();
    for (const auto& [key, value] : keys) ok = SetData(key.c_str(), value) && ok;
    ok = SetData("kkrc_epoch", std::to_string(epoch)) && ok;
    generation = 1;
    ok = SetData("kkrc_generation", "1") && ok;
    ok = SetData("kkrc_phase", "waiting") && ok;
    ok = SetData("kkrc_choice", "0") && ok;
    api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(matchmaking, model.lobby, kReadyKey, "0");
    if (!ok) { Leave("Could not publish lobby compatibility"); if (self_test) stop = true; return; }
    if (!self_test && !api.SteamAPI_ISteamMatchmaking_SetLobbyJoinable(matchmaking, model.lobby, true)) { Leave("Could not open lobby"); return; }
    const auto connect = "+connect_lobby " + std::to_string(model.lobby);
    const bool presence_set=api.SteamAPI_ISteamFriends_SetRichPresence(friends,kPresenceKey,kProductValue) &&
        api.SteamAPI_ISteamFriends_SetRichPresence(friends,"connect",connect.c_str());
    const bool presence_ok=presence_set && Text(api.SteamAPI_ISteamFriends_GetFriendRichPresence(friends,model.self,"connect"))==connect;
    REXLOG_INFO("KK co-op Steam: lobby created friends-only={} connect rich presence set={} readback={}",!self_test,presence_set,presence_ok);
    if (self_test) {
      const bool same = ReadKeys(model.lobby) == keys && Data(model.lobby, "kkrc_epoch") == std::to_string(epoch) &&
          Data(model.lobby, "kkrc_generation") == "1" && Data(model.lobby, "kkrc_phase") == "waiting";
      test_log("Steam self-test: lobby created hash=" + ShortSocialHash(model.lobby));
      test_log(same ? "Steam self-test: lobby keys OK" : "Steam self-test: lobby keys failed");
      test_log(presence_ok ? "Steam self-test: connect rich presence OK" : "Steam self-test: connect rich presence failed");
      test_result = same && presence_ok ? 0 : 1; Leave();
      const bool cleared=Text(api.SteamAPI_ISteamFriends_GetFriendRichPresence(friends,model.self,"connect")).empty();
      test_log(cleared ? "Steam self-test: connect cleared on Leave OK" : "Steam self-test: connect clear failed");
      if (!cleared) test_result=1;
      stop = true; return;
    }
    if (presence_lobby) {
      REXLOG_INFO("KK co-op Steam: resumed host lobby recreated friends-only connect presence={}", presence_ok);
      model.owner = model.self; Publish(); return;
    }
    if (!presence_ok) { Leave("Could not enable Steam invites"); return; }
    Status(SocialStatus::Lobby, "Lobby open"); RefreshLobby();
  }
  void Joined(const LobbyEnter_t& c, bool failed) {
    join_call = 0;
    if (failed || c.m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess || c.m_ulSteamIDLobby != joining) {
      if (c.m_ulSteamIDLobby && c.m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
        api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, c.m_ulSteamIDLobby);
      joining = 0; Status(SocialStatus::Idle, "Steam connected"); Error("Could not join lobby"); return;
    }
    if (stop) { api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, c.m_ulSteamIDLobby); return; }
    model.lobby = joining; joining = 0;
    recorded_owner = api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(matchmaking, model.lobby);
    if (recorded_owner != joining_owner || !Friend(recorded_owner) ||
        !ParseLobbyId(Data(model.lobby, "kkrc_epoch"), epoch) || Data(model.lobby, "kkrc_phase") != "waiting") {
      Leave("Lobby unavailable or incompatible"); return;
    }
    api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(matchmaking, model.lobby, kReadyKey, "0");
    std::erase_if(model.pending_invites, [&](auto i) { return i.lobby == model.lobby; });
    api.SteamAPI_ISteamFriends_SetRichPresence(friends, kPresenceKey, kProductValue);
    Status(SocialStatus::Lobby, "Joined lobby"); RefreshLobby();
  }
  void Invite(uint64_t lobby, uint64_t inviter) {
    if (!CSteamID(lobby).IsLobby() || !CSteamID(lobby).IsValid() || (inviter && !Friend(inviter))) return;
    if (std::none_of(model.pending_invites.begin(), model.pending_invites.end(), [&](auto i) { return i.lobby == lobby; })) {
      if (model.pending_invites.size() == kMaxInvites) model.pending_invites.erase(model.pending_invites.begin());
      model.pending_invites.push_back({lobby, inviter}); Publish();
    }
  }
  void Validated(const LobbyDataUpdate_t& d) {
    if (d.m_ulSteamIDLobby != validating || d.m_ulSteamIDMember != validating) return;
    const auto lobby = validating; validating = 0;
    const auto owner = api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(matchmaking, lobby);
    if (!d.m_bSuccess || !Friend(owner) || !CompatibleLobby(ReadKeys(lobby), options.compatibility) ||
        Data(lobby, "kkrc_phase") != "waiting" || api.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(matchmaking, lobby) != 1) {
      Status(SocialStatus::Idle, "Steam connected"); Error("Lobby unavailable or incompatible"); return;
    }
    joining = lobby; joining_owner = owner; join_call = api.SteamAPI_ISteamMatchmaking_JoinLobby(matchmaking, lobby);
    if (!join_call) { joining = 0; Status(SocialStatus::Idle, "Steam connected"); Error("Could not request lobby join"); }
  }
  void Command(SocialCommand c) {
    if (c.action == SocialAction::Decline) { std::erase_if(model.pending_invites, [&](auto i) { return i.lobby == c.id; }); Publish(); return; }
    if (c.action == SocialAction::Leave) { Leave(); return; }
    if (c.action == SocialAction::Open) { Publish(); return; }
    if (c.action == SocialAction::Resume) { ResumeSession(c.id); return; }
    if (create_call || join_call || validating) return;
    if (abandoned.size() >= 8) { Error("Steam requests are still pending; restart to retry"); return; }
    switch (c.action) {
      case SocialAction::CreateLobby:
        if (model.lobby) return;
        model.last_error.clear(); create_call = api.SteamAPI_ISteamMatchmaking_CreateLobby(matchmaking,
            self_test ? k_ELobbyTypeInvisible : k_ELobbyTypeFriendsOnly, 2);
        operation_deadline = Now() + 20;
        if (create_call) Status(SocialStatus::Creating, "Creating lobby");
        else { Error("Could not request lobby creation"); if (self_test) stop = true; }
        return;
      case SocialAction::Accept:
        if (model.lobby || (std::none_of(model.pending_invites.begin(), model.pending_invites.end(), [&](auto i) { return i.lobby == c.id; }) &&
                            std::none_of(model.friends.begin(), model.friends.end(), [&](const auto& f) { return f.lobby == c.id; }))) return;
        if (!CSteamID(c.id).IsLobby() || !CSteamID(c.id).IsValid()) { Error("Invalid lobby invite"); return; }
        validating = c.id; operation_deadline = Now() + 20; model.last_error.clear();
        if (!api.SteamAPI_ISteamMatchmaking_RequestLobbyData(matchmaking, validating)) { validating = 0; Error("Could not inspect invited lobby"); }
        else Status(SocialStatus::Joining, "Checking invited lobby");
        return;
      case SocialAction::Invite:
        if (!model.lobby || model.status != SocialStatus::Lobby || !Friend(c.id) || Now() < next_invite) return;
        next_invite = Now() + 2;
        if (!api.SteamAPI_ISteamMatchmaking_InviteUserToLobby(matchmaking, model.lobby, c.id)) Error("Could not send invite");
        return;
      case SocialAction::OverlayInvite:
        if (model.lobby && model.status == SocialStatus::Lobby && model.overlay_available)
          api.SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(friends, model.lobby);
        else Error("Steam overlay is unavailable");
        return;
      case SocialAction::SelectChapter: {
        if (model.status!=SocialStatus::Lobby || !RefreshLobby() || model.self!=model.owner ||
            c.choice.chapter<0 || c.choice.chapter>=42) return;
        if (model.choice.valid() && model.choice.chapter==c.choice.chapter && model.choice.continue_game==c.choice.continue_game) return;
        const LobbyChoice choice{model.choice.revision+1,c.choice.chapter,c.choice.continue_game};
        if (!SetData("kkrc_choice",EncodeLobbyChoice(choice))) { Error("Could not complete Social action"); return; }
        RefreshLobby(); return;
      }
      case SocialAction::SetReady:
        if (model.status != SocialStatus::Lobby || !RefreshLobby()) return;
        if (!model.choice.valid() || c.choice!=model.choice) return;
        api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(matchmaking, model.lobby, kReadyKey, c.ready ? ready_token.c_str() : "0");
        RefreshLobby(); return;
      case SocialAction::Start:
        if (!RefreshLobby() || !CanStartLobby(model) || !model.choice.valid() || c.choice!=model.choice) { Error("Both players must be ready"); return; }
        if (!api.SteamAPI_ISteamMatchmaking_SetLobbyJoinable(matchmaking, model.lobby, false) || !SetData("kkrc_phase", "starting")) {
          Leave("Could not start lobby"); return;
        }
        StartSession(); return;
      default: return;
    }
  }
  void Dispatch(const CallbackMsg_t& m) {
    SteamAPICallCompleted_t completed{};
    if (Callback(m, completed)) {
      bool failed = true;
      const auto old = abandoned.find(completed.m_hAsyncCall);
      if (old != abandoned.end()) {
        uint64_t lobby = 0;
        if (old->second == LobbyCreated_t::k_iCallback) {
          LobbyCreated_t value{};
          if (api.SteamAPI_ManualDispatch_GetAPICallResult(pipe, old->first, &value, sizeof(value), LobbyCreated_t::k_iCallback, &failed) &&
              !failed && value.m_eResult == k_EResultOK) lobby = value.m_ulSteamIDLobby;
        } else {
          LobbyEnter_t value{};
          if (api.SteamAPI_ManualDispatch_GetAPICallResult(pipe, old->first, &value, sizeof(value), LobbyEnter_t::k_iCallback, &failed) &&
              !failed && value.m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess) lobby = value.m_ulSteamIDLobby;
        }
        if (lobby && lobby != model.lobby && lobby != joining) api.SteamAPI_ISteamMatchmaking_LeaveLobby(matchmaking, lobby);
        abandoned.erase(old); return;
      }
      if (create_call && completed.m_hAsyncCall == create_call) {
        LobbyCreated_t value{};
        const bool ok = completed.m_iCallback == LobbyCreated_t::k_iCallback && completed.m_cubParam == sizeof(value) &&
            api.SteamAPI_ManualDispatch_GetAPICallResult(pipe, create_call, &value, sizeof(value), LobbyCreated_t::k_iCallback, &failed);
        Created(value, !ok || failed);
      } else if (join_call && completed.m_hAsyncCall == join_call) {
        LobbyEnter_t value{};
        const bool ok = completed.m_iCallback == LobbyEnter_t::k_iCallback && completed.m_cubParam == sizeof(value) &&
            api.SteamAPI_ManualDispatch_GetAPICallResult(pipe, join_call, &value, sizeof(value), LobbyEnter_t::k_iCallback, &failed);
        Joined(value, !ok || failed);
      }
      return;
    }
    LobbyDataUpdate_t data{};
    if (Callback(m, data)) { Validated(data); if (data.m_ulSteamIDLobby == model.lobby) RefreshLobby(); return; }
    LobbyChatUpdate_t chat{};
    if (Callback(m, chat)) {
      if (chat.m_ulSteamIDLobby == model.lobby && presence_lobby) return;
      if (chat.m_ulSteamIDLobby == model.lobby) {
        // Even leave/rejoin between polls must invalidate the previous ready acknowledgements.
        if (model.start_requested) { Leave("Lobby membership changed during the session"); return; }
        if (recorded_owner == model.self) {
          if (++generation == 0 || !SetData("kkrc_generation", std::to_string(generation))) { Leave("Could not reset lobby readiness"); return; }
        }
        api.SteamAPI_ISteamMatchmaking_SetLobbyMemberData(matchmaking, model.lobby, kReadyKey, "0");
        RefreshLobby();
      }
      return;
    }
    GameLobbyJoinRequested_t join{};
    if (Callback(m, join)) { Invite(join.m_steamIDLobby.ConvertToUint64(), join.m_steamIDFriend.ConvertToUint64()); return; }
    LobbyInvite_t invite{};
    if (Callback(m, invite)) { if (invite.m_ulGameID == 480) Invite(invite.m_ulSteamIDLobby, invite.m_ulSteamIDUser); return; }
    GameRichPresenceJoinRequested_t rich{};
    if (Callback(m, rich)) {
      SocialId id = 0;
      if (ParseLobbyConnect(Text(rich.m_rgchConnect, sizeof(rich.m_rgchConnect) - 1), id)) Invite(id, rich.m_steamIDFriend.ConvertToUint64()); return;
    }
    SteamNetworkingMessagesSessionRequest_t request{};
    if (Callback(m, request)) {
      const auto id = request.m_identityRemote.GetSteamID64();
#if defined(KK_DEV_TOOLS)
      if (self_test && loopback_test && id == model.self && transport) {
        transport->SessionRequest(id); return;
      }
#endif
      if (transport && resume_partner && id == resume_partner && Friend(id)) transport->SessionRequest(id);
      else if (Friend(id) && RefreshLobby() && transport) transport->SessionRequest(id);
      else api.SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(messages, request.m_identityRemote);
      return;
    }
    SteamNetworkingMessagesSessionFailed_t failure{};
    if (Callback(m, failure)) { if (transport) transport->SessionFailed(failure.m_info.m_identityRemote.GetSteamID64()); return; }
    if (m.m_iCallback == SteamServersDisconnected_t::k_iCallback || m.m_iCallback == SteamShutdown_t::k_iCallback) {
      stop = true; Leave("Steam is needed for co-op");
      Status(SocialStatus::Unavailable, "Steam is needed for co-op");
    }
  }
  void Pump() {
    api.SteamAPI_ManualDispatch_RunFrame(pipe);
    CallbackMsg_t callback{};
    for (unsigned i = 0; i < 256 && api.SteamAPI_ManualDispatch_GetNextCallback(pipe, &callback); ++i) {
      Dispatch(callback);
      api.SteamAPI_ManualDispatch_FreeLastCallback(pipe);
    }
  }
  void TickSession() {
    if (!session) return;
    std::optional<PlayerPose> pose;
    enemies::Frame enemy_frame;
    CheatSet cheats;
    const char* failure;
    { std::lock_guard lock(mutex); pose = local_pose; failure = local_failure; enemy_frame = local_enemies;
      cheats = local_cheats.value_or(options.cheats); }
    // Snapshot copies can wait for the game thread. Keep heartbeat/receive
    // timestamps current when polling resumes after a native frame stall.
    const double now = Now();
    if (pose && now - pose->time > .25) { pose.reset(); failure = "publish-expired"; }
    session->Tick(now, pose, &enemy_frame, &cheats);
    shots.Exchange(*session);
    combat.Exchange(*session, now);
    ownership.Exchange(*session);
    story.Exchange(*session);
    camera.Exchange(*session, now);
    flow.Exchange(*session, now);
    SteamSessionView view{session->SessionRole(), session->State(), session->Rejection(), session->Stats(), pose, session->Partner(now), now};
    view.local_failure = failure; view.poses = session->Poses();
    view.enemies = session->Enemies(now); view.enemies_sent = session->EnemiesSent();
    view.shots_sent = session->ShotsSent();
    view.host_cheats = session->HostCheats();
    view.partner_name = session->PartnerName();
    view.epoch = session->Epoch();
#if defined(KK_DEV_TOOLS)
    if (view.state != SessionState::Connected) next_pose_log = now + 10;
    else if (now >= next_pose_log) {
      next_pose_log = now + 10;
      const float distance = view.local && view.partner && view.local->level == view.partner->level ?
          PartnerDistance(*view.local, *view.partner) : -1;
      REXLOG_INFO("KK co-op poses: local={} check={} sent={} received={} remote-level={} distance={:.1f}",
          view.local ? "yes" : "no", failure, view.poses.sent, view.poses.received,
          view.poses.remote_level ? std::to_string(*view.poses.remote_level) : "none", distance);
    }
#endif
    { std::lock_guard lock(mutex); session_view = view; }
    if (session->State() == SessionState::Ended) { Leave("Co-op session ended"); return; }
    if (session->State() != last_session_state) {
      last_session_state = session->State();
      if (last_session_state == SessionState::Connected && presence_wanted && !model.lobby && !create_call) {
        presence_wanted = false; presence_lobby = true;
        create_call = api.SteamAPI_ISteamMatchmaking_CreateLobby(matchmaking, k_ELobbyTypeFriendsOnly, 2);
        operation_deadline = Now() + 20;
        REXLOG_INFO("KK co-op Steam: resumed host reconnected; recreating a friends-only lobby (request {})", create_call ? "sent" : "failed");
      }
      if (last_session_state == SessionState::Connected) Status(SocialStatus::Connected, "Partner connected");
      else if (last_session_state == SessionState::Lost) Status(SocialStatus::Connected, "Partner connection lost");
      else Publish();
    }
  }
#if defined(KK_DEV_TOOLS)
  int LoopbackCheck() {
    // Steam self-send was unsupported in the C47 run. Keep the real DLL probe
    // for diagnostics; a failed round trip requires testing with two accounts.
    transport = std::make_unique<SteamMessagesTransport>(*this, model.self);
    for (unsigned i=1;i<=unsigned(MessageType::StartPlanAck);++i) {
      const auto type=MessageType(i);
      const bool reliable=type!=MessageType::Ping && type!=MessageType::Pong &&
          type!=MessageType::PlayerPose && type!=MessageType::EnemySnapshot && type!=MessageType::CameraPose;
      std::vector<uint8_t> expected;
      if (!Encode(SteamProbeMessage(type),expected) || !transport->Send(expected,reliable)) {
        test_log("Steam loopback: send failed type="+std::to_string(i)+
                 "; Steam self-send is unsupported; two Steam accounts are needed for message-path testing"); return 1;
      }
      const double until=Now()+(i==1 ? 10 : 3);
      double retry=Now()+.1;
      bool received=false;
      while (!stop && Now()<until && transport->State()==TransportState::Ready) {
        Pump(); transport->Poll(Now());
        std::vector<uint8_t> bytes;
        while (transport->Receive(bytes)) if (bytes==expected) received=true;
        if (received) break;
        // Unreliable loss is allowed; repeat only the current synthetic packet.
        if (!reliable && Now()>=retry) {
          if (!transport->Send(expected,false)) break;
          retry=Now()+.1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!received) {
        test_log("Steam loopback: no self-send round trip type="+std::to_string(i)+
                 "; Steam self-send is unsupported; two Steam accounts are needed for message-path testing"); return 1;
      }
      test_log("Steam loopback: OK type="+std::to_string(i)+" channel="+
               std::to_string(reliable ? 0 : 1)+" bytes="+std::to_string(expected.size()));
    }
    test_log("Steam loopback: all message types OK; two-account lobby and relay testing still required");
    return 0;
  }
#endif
  void Run() {
    { std::lock_guard lock(mutex); model = published; }
    if (!Init()) {
      model.last_error = "Steam is needed for co-op";
      Status(SocialStatus::Unavailable, "Steam is needed for co-op");
      if (self_test) test_log("Steam self-test: init failed");
      if (initialized) api.SteamAPI_Shutdown();
      return;
    }
    Status(SocialStatus::Idle, "Steam connected");
    if (self_test) {
      test_log("Steam self-test: init OK");
#if defined(KK_DEV_TOOLS)
      if (loopback_test) { test_result=LoopbackCheck(); stop=true; }
      else
#endif
      {
        test_log("Steam self-test: friend count=" + std::to_string(std::max(0, api.SteamAPI_ISteamFriends_GetFriendCount(friends, k_EFriendFlagImmediate))));
        test_log("Steam self-test: persona state=" + std::to_string(int(api.SteamAPI_ISteamFriends_GetPersonaState(friends))));
        Command({SocialAction::CreateLobby});
      }
    }
    while (!stop) {
      std::deque<PendingCommand> pending;
      { std::lock_guard lock(mutex); pending.swap(commands); }
      for (auto& p : pending) {
        if (stop) break;
        if (p.compatibility && CheckCompatibility(options.compatibility, *p.compatibility) != RejectReason::None) {
          const bool had_lobby = model.lobby || create_call || join_call || validating;
          if (had_lobby) Leave("Gameplay settings changed; open a new lobby");
          options.compatibility = std::move(*p.compatibility);
          if (had_lobby) continue;
        }
        Command(p.command);
      }
      Pump();
      const auto now = Now();
      if ((create_call || join_call || validating) && now > operation_deadline) {
        // Keep a bounded list so late successes are left, while allowing a fresh request.
        validating = joining = joining_owner = 0; RetireCalls();
        Status(SocialStatus::Idle, "Steam connected"); Error("Steam lobby request timed out");
        if (self_test) stop = true;
      }
      if (!self_test) {
        if (now >= next_friends) { Friends(); next_friends = now + 5; }
        if (model.lobby && now >= next_lobby) { RefreshLobby(); next_lobby = now + .5; }
        TickSession();
      }
      std::unique_lock lock(mutex);
      wake.wait_for(lock, std::chrono::milliseconds(16), [&] { return stop || !commands.empty(); });
    }
    if (model.lobby) Leave();
    else StopSession();
    api.SteamAPI_ISteamFriends_ClearRichPresence(friends);
    // Drain late async successes during a bounded shutdown window, always freeing callback memory.
    const auto until = Now() + .2;
    while ((create_call || join_call || !abandoned.empty()) && Now() < until) { Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    api.SteamAPI_Shutdown(); initialized = false;
    if (self_test) test_log(test_result == 0 ? "Steam self-test: shutdown OK" : "Steam self-test: failed; shutdown OK");
  }
  bool Send(uint64_t peer, int channel, std::span<const uint8_t> bytes, bool reliable) override {
    SteamNetworkingIdentity id{}; id.SetSteamID64(peer);
    const auto result = api.SteamAPI_ISteamNetworkingMessages_SendMessageToUser(messages, id, bytes.data(), uint32(bytes.size()),
        reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoDelay, channel);
    return result == k_EResultOK || (!reliable && result == k_EResultIgnored);
  }
  bool Receive(int channel, SteamBackendMessage& out) override {
    SteamNetworkingMessage_t* msg = nullptr;
    if (api.SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(messages, channel, &msg, 1) <= 0 || !msg) return false;
    out.peer = msg->m_identityPeer.GetSteamID64(); out.channel = msg->m_nChannel;
    out.reliable = (msg->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0;
    out.oversized = msg->m_cbSize <= 0 || msg->m_cbSize > int(kMaxPacketBytes) || !msg->m_pData;
    if (!out.oversized) { const auto* p = static_cast<const uint8_t*>(msg->m_pData); out.bytes.assign(p, p + msg->m_cbSize); }
    msg->Release(); return true;
  }
  bool Accept(uint64_t peer) override {
    SteamNetworkingIdentity id{}; id.SetSteamID64(peer);
    return api.SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(messages, id);
  }
  void Close(uint64_t peer) override {
    SteamNetworkingIdentity id{}; id.SetSteamID64(peer); api.SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(messages, id);
  }
  TransportStats Stats(uint64_t peer) const override {
    SteamNetworkingIdentity id{}; id.SetSteamID64(peer); SteamNetConnectionRealTimeStatus_t quick{};
    api.SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(messages, id, nullptr, &quick);
    TransportStats s; s.ping_ms = std::max(0, quick.m_nPing);
    s.loss = quick.m_flConnectionQualityLocal >= 0 ? std::clamp(1.0 - quick.m_flConnectionQualityLocal, 0.0, 1.0) : 0;
    return s;
  }
#endif
};

SteamRuntime::SteamRuntime(SteamRuntimeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
SteamRuntime::~SteamRuntime() = default;
SocialModel SteamRuntime::Snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->published; }
bool SteamRuntime::Act(SocialCommand command) {
  std::optional<Compatibility> current;
  if (impl_->options.enabled && impl_->options.read_compatibility &&
      (command.action == SocialAction::CreateLobby || command.action == SocialAction::Accept ||
       command.action == SocialAction::SetReady || command.action == SocialAction::Start)) current = impl_->options.read_compatibility();
  return impl_->Enqueue(command, std::move(current));
}
void SteamRuntime::PublishPose(std::optional<PlayerPose> pose, const char* failure) {
  std::lock_guard lock(impl_->mutex); impl_->local_pose = pose; impl_->local_failure = failure;
}
SteamSessionView SteamRuntime::SessionSnapshot() const { std::lock_guard lock(impl_->mutex); return impl_->session_view; }
bool SteamRuntime::EnemyConnectionReady(double now) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->session_view.state == SessionState::Connected && now-impl_->session_view.updated <= .5;
}
void SteamRuntime::PublishEnemies(const enemies::Frame& frame) {
  std::lock_guard lock(impl_->mutex); impl_->local_enemies = frame;
}
bool SteamRuntime::SubmitShot(const PendingShot& shot) { return impl_->shots.Submit(shot); }
void SteamRuntime::PublishCheats(CheatSet cheats) { std::lock_guard lock(impl_->mutex); impl_->local_cheats = cheats; }
void SteamRuntime::PublishInventory(const InventoryState& inventory) { impl_->shots.PublishInventory(inventory); }
bool SteamRuntime::BeginFlow(SessionStart start, std::vector<uint8_t> save) { return impl_->flow.Begin(start, std::move(save)); }
bool SteamRuntime::SubmitFlow(Message m) { return impl_->flow.Submit(std::move(m)); }
bool SteamRuntime::BeginPlan(const StartPlan& plan) { return impl_->flow.BeginPlan(plan); }
handover::PlanPhase SteamRuntime::PlanState(StartPlan* plan, const char** failure) const { return impl_->flow.PlanState(plan, failure); }
bool SteamRuntime::Resume(bool host, uint64_t partner, uint64_t token) {
#if KK_HAS_STEAM
  { std::lock_guard lock(impl_->mutex); impl_->resume_host = host; impl_->resume_token = token; }
#else
  (void)host; (void)token;
#endif
  return impl_->Enqueue({SocialAction::Resume, partner}, std::nullopt);
}
void SteamRuntime::PublishCamera(std::optional<CameraPose> camera) { impl_->camera.Publish(std::move(camera)); }
std::optional<CameraPose> SteamRuntime::HostCamera(double now) const { return impl_->camera.Sample(now); }
CameraDiagnostics SteamRuntime::CameraInfo() const { return impl_->camera.Info(); }
FlowView SteamRuntime::FlowSnapshot() const { return impl_->flow.Snapshot(); }
std::vector<ReceivedShot> SteamRuntime::TakeShots() { return impl_->shots.Take(); }
bool SteamRuntime::SubmitShotReceipt(const ShotReceipt& receipt) { return impl_->shots.SubmitReceipt(receipt); }
std::vector<ShotReceipt> SteamRuntime::TakeShotReceipts() { return impl_->shots.TakeReceipts(); }
bool SteamRuntime::SubmitCombat(Message m, double now) { return impl_->combat.Submit(std::move(m), now); }
std::vector<ReceivedCombat> SteamRuntime::TakeCombat() { return impl_->combat.Take(); }
void SteamRuntime::ClearCombat() { impl_->combat.Clear(); }
bool SteamRuntime::SubmitOwnership(Message m) { return impl_->ownership.Submit(std::move(m)); }
std::vector<Message> SteamRuntime::TakeOwnership() { return impl_->ownership.Take(); }
void SteamRuntime::ClearOwnership() { impl_->ownership.Clear(); }
bool SteamRuntime::SubmitStory(Message m) { return impl_->story.Submit(std::move(m)); }
std::vector<Message> SteamRuntime::TakeStory() { return impl_->story.Take(); }
void SteamRuntime::Shutdown() { impl_->Shutdown(); }
#if defined(KK_DEV_TOOLS)
int SteamRuntime::SelfTest(std::function<void(const std::string&)> log, bool loopback) {
#if KK_HAS_STEAM
  { std::lock_guard lock(impl_->mutex);
    if (impl_->used || impl_->stopped || !impl_->options.enabled) { log("Steam self-test: disabled or already used"); return 1; }
    impl_->used = impl_->self_test = true; impl_->test_log = std::move(log);
    impl_->loopback_test = loopback;
    impl_->worker = std::thread([p = impl_.get()] { p->Run(); });
  }
  impl_->worker.join();
  return impl_->test_result;
#else
  (void)loopback;
  log("Steam self-test: Steam not available in this build"); return 1;
#endif
}
#endif
}  // namespace kk::coop
