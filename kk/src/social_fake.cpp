#include "social_fake.h"
#if defined(KK_DEV_TOOLS)
#include <algorithm>
#include <array>

namespace kk::coop {
namespace {
class Preview final : public ISocialPreview {
 public:
  Preview() { Scenario("invites"); }
  ~Preview() override { self_.reset(); peers_.clear(); }
  bool Scenario(std::string_view scene) override {
    static constexpr std::array scenes = {"invites", "host", "guest", "ready", "offline", "needed",
        "unavailable", "mismatch", "left", "busy", "starting", "long"};
    if (std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) return false;
    // Build and check a complete replacement before exposing it to the drawer.
    // Never report success after a failed Create/Accept/Ready command.
    auto world = MakeFakeSocialWorld();
    std::vector<std::unique_ptr<FakeSocialService>> peers;
    const Compatibility c{kProtocolVersion, "preview", "preview", "preview"};
    const bool unavailable = scene == "needed" || scene == "unavailable";
    auto self = std::make_unique<FakeSocialService>(world, 1,
        scene == "long" ? "Your exceptionally long preview persona name" : "You", c, true, !unavailable);
    for (int i = 0; i < 5; ++i) {
      const char* names[] = {"Alex", "Morgan with a very long friend name", "Sam", "Charlie", "Robin"};
      const char* long_names[] = {"Alex with an exceptionally long invited persona name",
          "Morgan with a very long friend name", "Sam with another exceptionally long persona name",
          "Charlie with an exceptionally long busy persona name", "Robin with a very long offline name"};
      Compatibility peer_c = c;
      if (scene == "mismatch" && i == 0) peer_c.build = "different-preview";
      peers.push_back(std::make_unique<FakeSocialService>(world, SocialId(i + 2),
          scene == "long" ? long_names[i] : names[i], peer_c));
      if (!peers.back()->Act({SocialAction::Open})) return false;
    }
    if (self->Act({SocialAction::Open}) == unavailable) return false;
    auto& friend_host = *peers.front();
    if (scene == "invites" || scene == "long" || scene == "guest" || scene == "left" || scene == "mismatch") {
      if (!friend_host.Act({SocialAction::CreateLobby}) || !friend_host.Act({SocialAction::Invite, 1})) return false;
    }
    if (scene == "guest" || scene == "left") {
      if (!self->Act({SocialAction::Accept, friend_host.Snapshot().lobby}) ||
          !friend_host.Act({SocialAction::SetReady, 0, true})) return false;
      if (scene == "left" && !friend_host.Act({SocialAction::Leave})) return false;
    }
    if (scene == "mismatch") {
      if (self->Act({SocialAction::Accept, friend_host.Snapshot().lobby}) ||
          self->Snapshot().last_error != "Lobby unavailable or incompatible") return false;
    }
    if (scene == "host" || scene == "ready" || scene == "starting") {
      if (!self->Act({SocialAction::CreateLobby})) return false;
      if (scene != "host") {
        auto& guest = *peers[1];
        if (!self->Act({SocialAction::Invite, 3}) || !guest.Act({SocialAction::Accept, self->Snapshot().lobby}) ||
            !self->Act({SocialAction::SetReady, 0, true}) || !guest.Act({SocialAction::SetReady, 0, true})) return false;
      }
      if (scene == "starting" && !self->Act({SocialAction::Start})) return false;
    }
    const auto model = self->Snapshot();
    if (scene == "host" && (!model.lobby || model.owner != model.self || model.members.size() != 1)) return false;
    if (scene == "guest" && (!model.lobby || model.owner == model.self || model.members.size() != 2)) return false;
    if (scene == "ready" && !model.can_start) return false;
    if (scene == "starting" && (model.status != SocialStatus::Starting || !model.start_requested)) return false;
    if (scene == "left" && (model.lobby || !model.members.empty() || model.status != SocialStatus::Ended)) return false;
    self_.reset(); peers_.clear();
    world_ = std::move(world); peers_ = std::move(peers); self_ = std::move(self); scenario_ = scene;
    return true;
  }
  SocialModel Snapshot() const override {
    auto m = self_->Snapshot(); m.overlay_available = true;
    for (auto& f : m.friends) {
      f.state = f.id == 2 ? FriendState::InGame : f.id == 3 ? FriendState::Online :
                f.id == 4 ? FriendState::Away : f.id == 5 ? FriendState::Busy : FriendState::Offline;
      f.avatar_width = f.avatar_height = 8;
      f.avatar_rgba.resize(8 * 8 * 4);
      for (size_t i = 0; i < f.avatar_rgba.size(); i += 4) {
        f.avatar_rgba[i] = uint8_t(55 + f.id * 20); f.avatar_rgba[i + 1] = 110;
        f.avatar_rgba[i + 2] = uint8_t(160 - f.id * 10); f.avatar_rgba[i + 3] = 255;
      }
      if (scenario_ == "offline") f.state = FriendState::Offline;
    }
    if (scenario_ == "needed" || scenario_ == "unavailable") {
      m.status = SocialStatus::Unavailable;
      m.status_text = scenario_ == "needed" ? "Steam is needed for co-op" : "Steam not available in this build";
    } else if (scenario_ == "busy") m.status = SocialStatus::Creating;  // synchronous fake has no pending request
    return m;
  }
  bool Act(SocialCommand c) override {
    if (c.action == SocialAction::Open) return self_->Act(c);
    if (c.action == SocialAction::Decline && scenario_ == "busy") return self_->Act(c);
    if (scenario_ == "needed" || scenario_ == "unavailable" || scenario_ == "busy") return false;
    if (c.action == SocialAction::OverlayInvite) return true;
    return self_->Act(c);
  }
 private:
  std::shared_ptr<FakeSocialWorld> world_;
  std::vector<std::unique_ptr<FakeSocialService>> peers_;
  std::unique_ptr<FakeSocialService> self_;
  std::string scenario_;
};
}
std::unique_ptr<ISocialPreview> MakeSocialPreview() { return std::make_unique<Preview>(); }
}  // namespace kk::coop
#endif
