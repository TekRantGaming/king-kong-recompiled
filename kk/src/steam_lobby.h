#pragma once

#include "coop_protocol.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kk::coop {
using SocialId = uint64_t;  // Opaque UI identity. Never format this or names into logs.
using LobbyKeys = std::map<std::string, std::string>;
inline constexpr char kProductKey[] = "kkrc_product";
inline constexpr char kProductValue[] = "kkrc1";
inline constexpr char kReadyKey[] = "kkrc_ready";
inline constexpr char kPresenceKey[] = "kkrc_game";
LobbyKeys EncodeLobbyKeys(const Compatibility& compatibility);
bool DecodeLobbyKeys(const LobbyKeys& keys, Compatibility& out);
bool CompatibleLobby(const LobbyKeys& keys, const Compatibility& local);
bool ParseLobbyId(std::string_view text, SocialId& out);
bool ParseLobbyConnect(std::string_view text, SocialId& out);
// Removes +connect_lobby and its argument before the regular cvar parser sees it.
// Malformed/duplicate intents are consumed but return zero. Does not join or initialize Steam.
SocialId ConsumeLobbyArgument(std::vector<std::string>& args);
std::string ShortSocialHash(SocialId id);

enum class SocialStatus { Disabled, Dormant, Unavailable, Idle, Creating, Joining, Lobby, Starting, Connected, Ended };
enum class FriendState { Offline, Online, InGame, Away, Busy };
struct SocialFriend {
  SocialId id = 0;
  std::string name;
  FriendState state = FriendState::Offline;
  bool in_our_game = false;
  uint32_t avatar_width = 0, avatar_height = 0;
  std::vector<uint8_t> avatar_rgba;
};
struct SocialMember { SocialId id = 0; std::string name; bool ready = false; };
struct SocialInvite { SocialId lobby = 0, inviter = 0; };
struct LobbyChoice {
  uint64_t revision = 0;
  int chapter = -1;
  bool continue_game = false;
  bool valid() const { return revision && chapter >= 0 && chapter < 42; }
  bool operator==(const LobbyChoice&) const = default;
};
std::string EncodeLobbyChoice(LobbyChoice choice);
LobbyChoice DecodeLobbyChoice(std::string_view value);
struct SocialModel {
  SocialStatus status = SocialStatus::Dormant;
  std::string status_text, last_error;
  SocialId self = 0, lobby = 0, owner = 0;
  std::vector<SocialFriend> friends;
  std::vector<SocialMember> members;
  std::vector<SocialInvite> pending_invites;
  bool can_start = false, start_requested = false, overlay_available = false;
  bool direct = false;
  std::string direct_addresses, direct_code;
  uint16_t direct_port = 27080;
  LobbyChoice choice;
};
bool CanStartLobby(const SocialModel& model);
enum class SocialAction { Open, CreateLobby, Invite, OverlayInvite, Accept, Decline, SetReady, Start, Leave, SelectChapter };
struct SocialCommand { SocialAction action = SocialAction::Open; SocialId id = 0; bool ready = false; LobbyChoice choice; };
// Actions enqueue work, snapshots are value copies. Call Open on the first UI use.
// Start requests the transport/session handshake only; it never boots a chapter or writes saves.
class ISocialService {
 public:
  virtual ~ISocialService() = default;
  virtual SocialModel Snapshot() const = 0;
  virtual bool Act(SocialCommand command) = 0;
};

// Shared deterministic, SDK-free fake. No thread, socket, game or Steam calls.
struct FakeSocialWorld;
std::shared_ptr<FakeSocialWorld> MakeFakeSocialWorld();
class FakeSocialService final : public ISocialService {
 public:
  FakeSocialService(std::shared_ptr<FakeSocialWorld> world, SocialId self, std::string name,
                    Compatibility compatibility, bool enabled = true, bool available = true);
  ~FakeSocialService() override;
  FakeSocialService(const FakeSocialService&) = delete;
  FakeSocialService& operator=(const FakeSocialService&) = delete;
  SocialModel Snapshot() const override;
  bool Act(SocialCommand command) override;
 private:
  std::shared_ptr<FakeSocialWorld> world_;
  SocialId self_;
};
}  // namespace kk::coop
