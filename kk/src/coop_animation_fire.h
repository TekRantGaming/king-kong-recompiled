#pragma once
#include <cstdint>

namespace kk::coop::puppet {
// Game-thread native firing evidence, independent of shared enemy readiness.
// A nested NPC callback cannot mark the enclosing local player's shot.
class FireCapture {
  static inline thread_local FireCapture* current_ = nullptr;
  FireCapture* previous_;
  bool own_, ray_ = false;
 public:
  explicit FireCapture(bool own) : previous_(current_), own_(own) { current_=own ? this : nullptr; }
  ~FireCapture() { current_=previous_; }
  FireCapture(const FireCapture&)=delete;
  FireCapture& operator=(const FireCapture&)=delete;
  static void Ray(uint32_t caller) {
    // The three gun resolvers inside H_callback_tir, after native ammo/fire checks.
    if (current_ && (caller==0x82696D10 || caller==0x8269727C || caller==0x826977E8)) current_->ray_=true;
  }
  bool Fired(bool same_world, bool spent_ammo) const { return own_ && same_world && (ray_ || spent_ammo); }
};
}  // namespace kk::coop::puppet
