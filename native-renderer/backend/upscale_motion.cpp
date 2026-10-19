#include "backend/upscale_motion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nr {

// ---------------------------------------------------------------- matrices

Mat4 Mat4Identity() {
  Mat4 m{};
  m[0] = m[5] = m[10] = m[15] = 1.0;
  return m;
}

Mat4 Mat4Multiply(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
      r[i * 4 + j] = s;
    }
  return r;
}

bool Mat4Invert(const Mat4& m, Mat4& out) {
  // Gauss-Jordan with partial pivoting, in double.
  double a[4][8];
  double scale = 0;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      a[i][j] = m[i * 4 + j];
      a[i][j + 4] = i == j ? 1.0 : 0.0;
      scale = std::max(scale, std::fabs(m[i * 4 + j]));
    }
  }
  if (!(scale > 0) || !std::isfinite(scale)) return false;
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12 * scale) return false;
    if (pivot != c)
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
    const double inv = 1.0 / a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      if (f == 0) continue;
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
  for (double v : out)
    if (!std::isfinite(v)) return false;
  return true;
}

void Mat4Transform(const Mat4& m, const double v[4], double out[4]) {
  for (int i = 0; i < 4; ++i)
    out[i] = m[i * 4 + 0] * v[0] + m[i * 4 + 1] * v[1] + m[i * 4 + 2] * v[2] + m[i * 4 + 3] * v[3];
}

Mat4 Mat4FromRows(const float rows[16]) {
  Mat4 m;
  for (int i = 0; i < 16; ++i) m[i] = rows[i];
  return m;
}

// ---------------------------------------------------------------- draw identity

uint64_t MotionDrawKey::Hash() const {
  uint64_t h = vs_hash ^ 0x9E3779B97F4A7C15ull;
  auto mix = [&](uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  };
  mix((uint64_t(vertex_buffer) << 32) | vertex_offset);
  mix((uint64_t(vertex_buffer2) << 32) | index_buffer);
  mix((uint64_t(start) << 32) | count);
  mix((uint64_t(uint32_t(base_vertex)) << 32) | primitive);
  return h;
}

bool MotionDrawKey::operator==(const MotionDrawKey& o) const {
  return vs_hash == o.vs_hash && vertex_buffer == o.vertex_buffer && vertex_offset == o.vertex_offset &&
         vertex_buffer2 == o.vertex_buffer2 && index_buffer == o.index_buffer && start == o.start &&
         count == o.count && base_vertex == o.base_vertex && primitive == o.primitive;
}

// ---------------------------------------------------------------- history

void MotionHistory::BeginFrame(uint64_t frame) {
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

void MotionHistory::Recycle(Map& m, bool drop_unused) {
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

const MotionHistory::Slot* MotionHistory::Find(const Map& m, const MotionDrawKey& key) {
  auto it = m.find(key.Hash());
  if (it == m.end()) return nullptr;
  for (const Slot& s : it->second)
    if (s.key == key) return &s;
  return nullptr;
}

MotionHistory::Slot& MotionHistory::FindOrAdd(Map& m, const MotionDrawKey& key) {
  std::vector<Slot>& list = m[key.Hash()];
  for (Slot& s : list)
    if (s.key == key) return s;
  list.push_back({key, {}});
  return list.back();
}

uint32_t MotionHistory::Record(const MotionDrawKey& key, const VsConstants& constants) {
  Slot& s = FindOrAdd(current_, key);
  s.constants.push_back(constants);
  ++current_count_;
  return uint32_t(s.constants.size() - 1);
}

uint32_t MotionHistory::CountThisFrame(const MotionDrawKey& key) const {
  const Slot* s = Find(current_, key);
  return s ? uint32_t(s->constants.size()) : 0;
}

const VsConstants* MotionHistory::FindPrevious(const MotionDrawKey& key, uint32_t ordinal, const VsConstants& current,
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

// ---------------------------------------------------------------- camera vote

void CameraReprojectionVote::BeginFrame(uint64_t frame) {
  if (frame == frame_) return;
  frame_ = frame;
  candidates_.clear();
  offers_ = 0;
}

bool CameraReprojectionVote::Close(const Mat4& a, const Mat4& b) {
  double scale = 0;
  for (int i = 0; i < 16; ++i) scale = std::max({scale, std::fabs(a[i]), std::fabs(b[i])});
  if (scale == 0) return true;
  for (int i = 0; i < 16; ++i)
    if (std::fabs(a[i] - b[i]) > kTolerance * scale) return false;
  return true;
}

void CameraReprojectionVote::Offer(const float current_rows[16], const float previous_rows[16]) {
  Mat4 cur_inv;
  if (!Mat4Invert(Mat4FromRows(current_rows), cur_inv)) return;
  Mat4 r = Mat4Multiply(Mat4FromRows(previous_rows), cur_inv);
  // Normalise the homogeneous scale (r is defined up to a factor only when w is: keep r[15] at 1 when
  // it can be, so equal reprojections compare equal).
  if (std::fabs(r[15]) > 1e-9) {
    const double inv = 1.0 / r[15];
    for (double& v : r) v *= inv;
  }
  ++offers_;
  for (Candidate& c : candidates_) {
    if (Close(c.m, r)) {
      ++c.votes;
      // The members' mean: each draw's matrix carries the rounding of its float constants (worst for
      // objects far from the world's origin), which averages out.
      for (int i = 0; i < 16; ++i) c.sum[i] += r[i];
      return;
    }
  }
  if (candidates_.size() < 16) candidates_.push_back({r, r, 1});
}

bool CameraReprojectionVote::Result(Mat4& reprojection, uint32_t* votes, uint32_t* offers) const {
  if (offers) *offers = offers_;
  if (candidates_.empty()) return false;
  const Candidate* best = &candidates_[0];
  for (const Candidate& c : candidates_)
    if (c.votes > best->votes) best = &c;
  for (int i = 0; i < 16; ++i) reprojection[i] = best->sum[i] / double(best->votes);
  if (votes) *votes = best->votes;
  return true;
}

// ---------------------------------------------------------------- screen

void HostPixelToNdc(const MotionScreen& s, float jitter_x, float jitter_y, double host_x, double host_y,
                    double depth, double ndc[3]) {
  // The host pixel's centre, less the jitter that moved the picture, in guest pixels.
  const double gx = (host_x + 0.5 - double(jitter_x)) / double(s.host_scale_x);
  const double gy = (host_y + 0.5 - double(jitter_y)) / double(s.host_scale_y);
  ndc[0] = (gx - double(s.x_offset)) / double(s.x_scale);
  ndc[1] = (gy - double(s.y_offset)) / double(s.y_scale);
  ndc[2] = s.z_scale != 0 ? (depth - double(s.z_offset)) / double(s.z_scale) : depth;
}

bool CameraMotion(const MotionScreen& s, const Mat4& reprojection, float jitter_x, float jitter_y, double host_x,
                  double host_y, double depth, double motion[2]) {
  double ndc[3];
  HostPixelToNdc(s, jitter_x, jitter_y, host_x, host_y, depth, ndc);
  const double cur[4] = {ndc[0], ndc[1], ndc[2], 1.0};
  double prev[4];
  Mat4Transform(reprojection, cur, prev);
  if (!(prev[3] > 1e-9)) {
    motion[0] = motion[1] = 0;
    return false;
  }
  // Both positions in host pixels of the unjittered frames.
  const double px = (prev[0] / prev[3] * s.x_scale + s.x_offset) * s.host_scale_x;
  const double py = (prev[1] / prev[3] * s.y_scale + s.y_offset) * s.host_scale_y;
  const double cx = (ndc[0] * s.x_scale + s.x_offset) * s.host_scale_x;
  const double cy = (ndc[1] * s.y_scale + s.y_offset) * s.host_scale_y;
  motion[0] = px - cx;
  motion[1] = py - cy;
  return true;
}

}  // namespace nr
