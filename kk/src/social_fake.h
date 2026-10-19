#pragma once
#include "steam_lobby.h"

#if defined(KK_DEV_TOOLS)
namespace kk::coop {
// UI preview around C06's shared fake world. No SDK, worker or network.
class ISocialPreview : public ISocialService {
 public:
  virtual bool Scenario(std::string_view scenario) = 0;
};
std::unique_ptr<ISocialPreview> MakeSocialPreview();
}  // namespace kk::coop
#endif
