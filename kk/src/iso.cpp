#include "iso.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <vector>

namespace kk::iso {
namespace {

constexpr uint64_t kSector = 2048;
constexpr uint64_t kPartitions[] = {0, 0xFD90000, 0x2080000, 0x18300000};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

struct Entry {
  std::string name;
  uint32_t sector = 0, size = 0;
  bool dir = false;
};

class Disc {
 public:
  bool Open(const std::filesystem::path& path) {
    f_.open(path, std::ios::binary);
    if (!f_) return false;
    for (uint64_t p : kPartitions) {
      char m[20];
      if (Read(p + 0x10000, m, sizeof(m)) && !std::memcmp(m, kMagic, sizeof(m))) {
        base_ = p;
        uint8_t vd[0x20];
        Read(p + 0x10000, vd, sizeof(vd));
        std::memcpy(&root_sector_, vd + 0x14, 4);
        std::memcpy(&root_size_, vd + 0x18, 4);
        return true;
      }
    }
    return false;
  }

  std::vector<Entry> List(uint32_t sector, uint32_t size) {
    std::vector<Entry> out;
    if (!size) return out;
    std::vector<uint8_t> t(size);
    if (!Read(base_ + sector * kSector, t.data(), size)) return out;
    std::vector<size_t> stack = {0};
    std::set<size_t> seen;
    while (!stack.empty()) {
      const size_t o = stack.back();
      stack.pop_back();
      if (o + 14 > t.size() || !seen.insert(o).second) continue;
      uint16_t left, right;
      std::memcpy(&left, &t[o], 2);
      std::memcpy(&right, &t[o + 2], 2);
      if (left == 0xFFFF && right == 0xFFFF) continue;
      Entry e;
      std::memcpy(&e.sector, &t[o + 4], 4);
      std::memcpy(&e.size, &t[o + 8], 4);
      e.dir = (t[o + 12] & 0x10) != 0;
      const size_t len = t[o + 13];
      if (o + 14 + len > t.size()) continue;
      e.name.assign(reinterpret_cast<const char*>(&t[o + 14]), len);
      out.push_back(std::move(e));
      if (left && left != 0xFFFF) stack.push_back(size_t(left) * 4);
      if (right && right != 0xFFFF) stack.push_back(size_t(right) * 4);
    }
    return out;
  }

  bool Read(uint64_t offset, void* dst, size_t n) {
    f_.clear();
    f_.seekg(std::streamoff(offset));
    f_.read(static_cast<char*>(dst), std::streamsize(n));
    return size_t(f_.gcount()) == n;
  }

  uint64_t base() const { return base_; }
  uint32_t root_sector() const { return root_sector_; }
  uint32_t root_size() const { return root_size_; }
  std::ifstream& stream() { return f_; }

 private:
  std::ifstream f_;
  uint64_t base_ = 0;
  uint32_t root_sector_ = 0, root_size_ = 0;
};

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

uint32_t ReadTitleId(const std::filesystem::path& image) {
  Disc d;
  if (!d.Open(image)) return 0;
  for (const auto& e : d.List(d.root_sector(), d.root_size())) {
    if (e.dir || e.name.size() != 11 || !std::equal(e.name.begin(), e.name.end(), "default.xex", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) continue;
    std::vector<uint8_t> h(std::min<uint32_t>(e.size, 0x4000));
    if (!d.Read(d.base() + e.sector * kSector, h.data(), h.size()) || std::memcmp(h.data(), "XEX2", 4)) return 0;
    // Optional headers: (key, value) pairs; 0x00040006 = execution info, title ID at +12.
    const uint32_t count = BE32(&h[0x14]);
    for (uint32_t i = 0; i < count && 0x18 + i * 8 + 8 <= h.size(); ++i) {
      const uint32_t key = BE32(&h[0x18 + i * 8]), value = BE32(&h[0x18 + i * 8 + 4]);
      if (key == 0x00040006 && value + 16 <= h.size()) return BE32(&h[value + 12]);
    }
    return 0;
  }
  return 0;
}

std::string Extract(const std::filesystem::path& image, const std::filesystem::path& out_dir, Progress* progress) {
  Disc d;
  if (!d.Open(image)) return "Not an Xbox disc image: " + image.filename().string();
  uint64_t total = 0;
  std::function<void(uint32_t, uint32_t)> count = [&](uint32_t s, uint32_t n) {
    for (const auto& e : d.List(s, n)) e.dir ? count(e.sector, e.size) : void(total += e.size);
  };
  count(d.root_sector(), d.root_size());
  if (progress) {
    progress->bytes_total = total;
    progress->bytes_done = 0;
  }
  std::vector<char> buf(1 << 20);
  std::string error;
  std::function<void(uint32_t, uint32_t, const std::filesystem::path&)> walk =
      [&](uint32_t s, uint32_t n, const std::filesystem::path& dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        for (const auto& e : d.List(s, n)) {
          if (!error.empty()) return;
          if (progress && progress->cancel) {
            error = "Cancelled";
            return;
          }
          const auto path = dir / e.name;
          if (e.dir) {
            walk(e.sector, e.size, path);
            continue;
          }
          std::ofstream out(path, std::ios::binary | std::ios::trunc);
          if (!out) {
            error = "Cannot write " + path.string();
            return;
          }
          uint64_t left = e.size, offset = d.base() + e.sector * kSector;
          while (left) {
            const size_t chunk = size_t(std::min<uint64_t>(left, buf.size()));
            if (!d.Read(offset, buf.data(), chunk)) {
              error = "Disc image ended early reading " + e.name;
              return;
            }
            out.write(buf.data(), std::streamsize(chunk));
            offset += chunk;
            left -= chunk;
            if (progress) progress->bytes_done += chunk;
            if (progress && progress->cancel) {
              error = "Cancelled";
              return;
            }
          }
        }
      };
  walk(d.root_sector(), d.root_size(), out_dir);
  return error;
}

}  // namespace kk::iso
