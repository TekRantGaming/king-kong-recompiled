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
        "unavailable", "mismatch", "left", "busy", "starting"};
    if (std::find(scenes.begin(), scenes.end(), scene) == scenes.end()) return false;
    self_.reset(); peers_.clear(); world_ = MakeFakeSocialWorld(); scenario_ = scene;
    const Compatibility c{kProtocolVersion, "preview", "preview", "preview"};
    self_ = std::make_unique<FakeSocialService>(world_, 1, "You", c);
    for (int i = 0; i < 5; ++i) {
      const char* names[] = {"Alex", "Morgan", "Sam", "Charlie", "Robin"};
      peers_.push_back(std::make_unique<FakeSocialService>(world_, SocialId(i + 2), names[i], c));
    }
    self_->Act({SocialAction::Open});
    auto& friend_host = *peers_.front();
    friend_host.Act({SocialAction::Open}); friend_host.Act({SocialAction::CreateLobby});
    friend_host.Act({SocialAction::Invite, 1});
    if (scene == "guest") {
      self_->Act({SocialAction::Accept, friend_host.Snapshot().lobby});
      friend_host.Act({SocialAction::SetReady, 0, true});
    }
    if (scene == "host" || scene == "ready" || scene == "starting") {
      self_->Act({SocialAction::CreateLobby});
      if (scene != "host") {
        // Use another friend, since Alex already owns the incoming invite lobby.
        auto& guest = *peers_[1];
        self_->Act({SocialAction::Invite, 3}); guest.Act({SocialAction::Accept, self_->Snapshot().lobby});
        self_->Act({SocialAction::SetReady, 0, true}); guest.Act({SocialAction::SetReady, 0, true});
      }
    }
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
    } else if (scenario_ == "mismatch") m.last_error = "Lobby compatibility changed";
    else if (scenario_ == "left") { m.status = SocialStatus::Ended; m.last_error = "The host left the lobby"; }
    else if (scenario_ == "busy") m.status = SocialStatus::Creating;
    else if (scenario_ == "starting") { m.status = SocialStatus::Starting; m.can_start = false; }
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
