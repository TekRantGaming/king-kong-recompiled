// Pre-task-340 implementation, an independent bit-for-bit oracle for the micro-benchmark.
#pragma once
#include "backend/upscale_motion.h"
#include "backend/tests/hotspot_bench.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace nr::test {
class ReferenceMotionHistory {
 public:
  // A new frame: this frame's records become the previous frame's.
  void BeginFrame(uint64_t frame);
  // Records a draw of the current frame; returns its index among this frame's draws with the same
  // key (its ordinal).
  uint32_t Record(const MotionDrawKey& key, const VsConstants& constants);
  // Last frame's constants for a draw of this frame recorded with `ordinal`: when last frame had as
  // many draws with this key as this frame has so far, the one with the same ordinal (draw order is
  // stable frame to frame); else the one whose constants are closest. Null when last frame had none.
  // `final_count`: how many draws of this key this frame has in all (when known), else 0.
  const VsConstants* FindPrevious(const MotionDrawKey& key, uint32_t ordinal, const VsConstants& current,
                                  uint32_t final_count = 0) const;
  // How many draws this frame recorded with a key.
  uint32_t CountThisFrame(const MotionDrawKey& key) const;
  uint64_t frame() const { return frame_; }
  size_t previous_draws() const { return previous_count_; }
  size_t current_draws() const { return current_count_; }

 private:
  struct Slot {
    MotionDrawKey key;
    std::vector<VsConstants> constants;  // in draw order
    // The closest-constants search's last answer + 1: draws keep their order when some are culled,
    // so the next draw's match is usually there (only where the search starts; task 319).
    mutable size_t next_guess = 0;
    // How many leading floats all its draws share (bit for bit; SIZE_MAX: not measured yet).
    mutable size_t common_prefix = SIZE_MAX;
  };
  using Map = std::unordered_map<uint64_t, std::vector<Slot>>;  // hash -> keys with that hash
  static const Slot* Find(const Map& m, const MotionDrawKey& key);
  static Slot& FindOrAdd(Map& m, const MotionDrawKey& key);
  // Empties every slot, keeping its storage; with drop_unused, removes slots already empty.
  static void Recycle(Map& m, bool drop_unused);
  Map current_, previous_;
  size_t current_count_ = 0, previous_count_ = 0;
  uint64_t frame_ = UINT64_MAX;
};

NR_NOINLINE void ReferenceMotionHistory::BeginFrame(uint64_t frame) {
  if (frame == frame_) return;
  // Only a frame that follows directly keeps its predecessor: after a gap the "previous" draws are
  // older than one frame and their motion would be wrong.
  const bool follows = frame_ != UINT64_MAX && frame == frame_ + 1;
  previous_.swap(current_);
  previous_count_ = follows ? current_count_ : 0;
  if (!follows) Recycle(previous_, false);
  // current_ now holds the frame before last: its storage is reused (task 319). Clearing the maps
  // freed every slot's constants each frame (4 KiB a draw, megabytes in a busy scene), and growing
  // them again touched fresh pages: tens of thousands of page faults a second on the render thread.
  Recycle(current_, true);
  current_count_ = 0;
  frame_ = frame;
}

void ReferenceMotionHistory::Recycle(Map& m, bool drop_unused) {
  // Slots keep their key and their vectors' capacity; a slot already empty (its key had no draw in
  // that frame either) is dropped, so keys that stopped drawing do not pile up.
  for (auto it = m.begin(); it != m.end();) {
    std::vector<Slot>& list = it->second;
    if (drop_unused)
      list.erase(std::remove_if(list.begin(), list.end(), [](const Slot& s) { return s.constants.empty(); }),
                 list.end());
    for (Slot& s : list) {
      s.constants.clear();
      s.next_guess = 0;
      s.common_prefix = SIZE_MAX;
    }
    it = list.empty() ? m.erase(it) : std::next(it);
  }
}

const ReferenceMotionHistory::Slot* ReferenceMotionHistory::Find(const Map& m, const MotionDrawKey& key) {
  auto it = m.find(key.Hash());
  if (it == m.end()) return nullptr;
  for (const Slot& s : it->second)
    if (s.key == key) return &s;
  return nullptr;
}

ReferenceMotionHistory::Slot& ReferenceMotionHistory::FindOrAdd(Map& m, const MotionDrawKey& key) {
  std::vector<Slot>& list = m[key.Hash()];
  for (Slot& s : list)
    if (s.key == key) return s;
  list.push_back({key, {}});
  return list.back();
}

NR_NOINLINE uint32_t ReferenceMotionHistory::Record(const MotionDrawKey& key, const VsConstants& constants) {
  Slot& s = FindOrAdd(current_, key);
  s.constants.push_back(constants);
  ++current_count_;
  return uint32_t(s.constants.size() - 1);
}

NR_NOINLINE uint32_t ReferenceMotionHistory::CountThisFrame(const MotionDrawKey& key) const {
  const Slot* s = Find(current_, key);
  return s ? uint32_t(s->constants.size()) : 0;
}

NR_NOINLINE const VsConstants* ReferenceMotionHistory::FindPrevious(const MotionDrawKey& key, uint32_t ordinal, const VsConstants& current,
                                               uint32_t final_count) const {
  const Slot* s = Find(previous_, key);
  if (!s || s->constants.empty()) return nullptr;
  const size_t n = s->constants.size();
  if (n == 1) return &s->constants[0];
  if (final_count && final_count == n && ordinal < n) return &s->constants[ordinal];
  // The instance set changed (culling): the closest constants (sum of absolute differences; the
  // first of equals). The draw with the same ordinal is measured first: it is usually the closest
  // or near it, so the others stop after a few registers (task 319: with a thousand draws of one
  // key, measuring from the first draw cost the render thread tens of milliseconds a frame). The
  // result is the same as measuring in order: sums only grow, the same registers are added in the
  // same order, and an equal sum wins only for an earlier draw.
  const size_t first = std::min<size_t>(ordinal, n - 1);
  // The leading floats every draw shares (the camera's registers, typically) add the same terms in
  // the same order for every draw: their sum is worked out once and each draw starts from it.
  if (s->common_prefix == SIZE_MAX) {
    size_t p = current.size();
    const VsConstants& c0 = s->constants[0];
    for (size_t i = 1; i < n && p; ++i) {
      const VsConstants& c = s->constants[i];
      size_t k = 0;
      while (k < p && std::memcmp(&c[k], &c0[k], sizeof(float)) == 0) ++k;
      p = k;
    }
    s->common_prefix = p;
  }
  const size_t prefix = s->common_prefix;
  double prefix_d = 0;
  for (size_t k = 0; k < prefix; ++k) prefix_d += std::fabs(double(s->constants[0][k]) - double(current[k]));
  const auto distance = [&](size_t i, double limit, bool equal_continues) {
    const auto go = [&](double d) { return d < limit || (equal_continues && d == limit); };
    // The shared part: the same partial sums for every draw, so stopping inside it is stopping
    // where the full loop would (the sums only grow; a stop there leaves a sum at or past the limit).
    if (!go(prefix_d)) {
      double d = 0;
      for (size_t k = 0; k < prefix && go(d); ++k) d += std::fabs(double(s->constants[i][k]) - double(current[k]));
      return d;
    }
    double d = prefix_d;
    const VsConstants& c = s->constants[i];
    for (size_t k = prefix; k < c.size() && go(d); ++k) d += std::fabs(double(c[k]) - double(current[k]));
    return d;
  };
  size_t best = first;
  double best_d = distance(first, INFINITY, false);
  if (const size_t guess = s->next_guess; guess < n && guess != first && best_d > 0 && !std::isnan(best_d)) {
    const double d = distance(guess, best_d, guess < best);
    if (d < best_d || (guess < best && d == best_d)) best_d = d, best = guess;
  }
  if (std::isnan(best_d)) {
    // Not a number in its constants (never seen): measure in order, as before.
    best = 0;
    best_d = INFINITY;
    for (size_t i = 0; i < n; ++i)
      if (const double d = distance(i, best_d, false); d < best_d) best_d = d, best = i;
    return &s->constants[best];
  }
  for (size_t i = 0; i < n && best_d > 0; ++i) {
    if (i == first) continue;
    const bool earlier = i < best;
    const double d = distance(i, best_d, earlier);
    if (d < best_d || (earlier && d == best_d)) {
      best_d = d;
      best = i;
    }
  }
  if (best_d == 0) {
    // An exact match: the first of the exact matches, as measuring in order would give.
    for (size_t i = 0; i < best; ++i)
      if (distance(i, 0, true) == 0) {
        best = i;
        break;
      }
  }
  s->next_guess = best + 1;
  return &s->constants[best];
}

}
