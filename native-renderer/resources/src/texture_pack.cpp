#include "kknr/texture_pack.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <xxh3.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace kknr {
namespace {
constexpr uint64_t kImageLimit = 128ull << 20, kReadyLimit = 256ull << 20;
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool Fail(std::string& why, const char* message) { why = message; return false; }
int Family(HostFormat f) {
  using H = HostFormat;
  switch (f) {
    case H::BC1_UNORM: return 1;
    case H::BC2_UNORM: case H::BC3_UNORM: case H::RGBA8_UNORM: case H::BC7_UNORM: return 2;
    case H::BC5_UNORM: case H::RG8_UNORM: return 3;
    case H::BC5_SNORM: case H::RG8_SNORM: return 4;
    case H::R8_UNORM: return 5;
    default: return 0;
  }
}
bool Compatible(HostFormat from, HostFormat to) {
  const int a = Family(from), b = Family(to);
  return a && (a == b || (a == 1 && b == 2));
}
bool Hex(const std::string& s, uint64_t& value) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), value, 16);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
bool BfName(const std::string& name) {
  uint64_t a, b;
  return name.size() == 20 && name.substr(0, 3) == "bf_" && name[11] == '_' &&
      Hex(name.substr(3, 8), a) && Hex(name.substr(12, 8), b);
}
bool ReadBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const auto size = f.tellg();
  if (size < 0 || uint64_t(size) > kImageLimit + 148) return false;
  out.resize(size_t(size)); f.seekg(0);
  return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}
uint32_t MipCount(uint32_t w, uint32_t h) {
  uint32_t n = 1;
  while (w > 1 || h > 1) { w = std::max(w / 2, 1u); h = std::max(h / 2, 1u); ++n; }
  return n;
}
bool AddLevel(HostTextureData& t, uint32_t level, uint32_t w, uint32_t h) {
  const auto& f = GetHostFormatInfo(t.plan.format);
  const uint32_t row = ((w + f.block_size - 1) / f.block_size) * f.bytes_per_block;
  const uint32_t size = row * ((h + f.block_size - 1) / f.block_size);
  if (size > kImageLimit - t.bytes.size()) return false;
  t.subresources.push_back({level, 0, w, h, 1, row, size, t.bytes.size(), size});
  t.bytes.resize(t.bytes.size() + size);
  return true;
}
// Decode BC1/3/5 if an author omitted mips (BC7 DDS must carry its chain).
bool Uncompress(HostTextureData& t) {
  const int family = Family(t.plan.format);
  const auto old = std::move(t);
  t = {}; t.plan = old.plan;
  t.plan.format = family <= 2 ? HostFormat::RGBA8_UNORM :
      family == 3 ? HostFormat::RG8_UNORM : HostFormat::RG8_SNORM;
  const uint32_t channels = family <= 2 ? 4 : 2;
  if (!AddLevel(t, 0, old.plan.width, old.plan.height)) return false;
  const auto& sub = old.subresources[0];
  const auto& f = GetHostFormatInfo(old.plan.format);
  for (uint32_t by = 0; by < (sub.height + 3) / 4; ++by)
    for (uint32_t bx = 0; bx < (sub.width + 3) / 4; ++bx) {
      const auto* block = old.bytes.data() + sub.offset + by * sub.row_pitch + bx * f.bytes_per_block;
      uint8_t pixels[64]{};
      if (old.plan.format == HostFormat::BC1_UNORM) DecodeBC1(block, pixels);
      else if (old.plan.format == HostFormat::BC3_UNORM) DecodeBC3(block, pixels);
      else if (old.plan.format == HostFormat::BC5_UNORM) DecodeBC5(block, pixels);
      else if (old.plan.format == HostFormat::BC5_SNORM) DecodeBC5Signed(block, reinterpret_cast<int8_t*>(pixels));
      else return false;
      for (uint32_t y = 0; y < 4 && by * 4 + y < sub.height; ++y)
        for (uint32_t x = 0; x < 4 && bx * 4 + x < sub.width; ++x)
          std::memcpy(t.bytes.data() + ((by * 4 + y) * sub.width + bx * 4 + x) * channels,
                      pixels + (y * 4 + x) * channels, channels);
    }
  return true;
}
bool GenerateMips(HostTextureData& t, std::string& why) {
  const uint32_t count = MipCount(t.plan.width, t.plan.height);
  if (t.plan.levels == count) return true;
  if (t.plan.format == HostFormat::BC7_UNORM)
    return Fail(why, "BC7 DDS requires a complete mip chain");
  if (GetHostFormatInfo(t.plan.format).block_size == 4 && !Uncompress(t))
    return Fail(why, "cannot decode DDS for mip generation");
  // A partial chain is regenerated consistently from level zero.
  const auto base_size = t.subresources[0].size;
  t.bytes.resize(base_size); t.subresources.resize(1);
  const uint32_t channels = GetHostFormatInfo(t.plan.format).bytes_per_block;
  const int family = Family(t.plan.format);
  const bool normal = family == 3 || family == 4, signed_xy = family == 4;
  for (uint32_t level = 1; level < count; ++level) {
    const auto prev = t.subresources.back();
    const uint32_t w = std::max(prev.width / 2, 1u), h = std::max(prev.height / 2, 1u);
    if (!AddLevel(t, level, w, h)) return Fail(why, "generated mips exceed 128 MiB");
    const auto dst = t.subresources.back();
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
      // Integer area bins include the final row/column of odd-sized images.
      const uint32_t x0 = x * prev.width / w, x1 = (x + 1) * prev.width / w;
      const uint32_t y0 = y * prev.height / h, y1 = (y + 1) * prev.height / h;
      float sum[4]{}; uint32_t n = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx) {
        const auto* p = t.bytes.data() + prev.offset + sy * prev.row_pitch + sx * channels;
        if (normal) {
          const float nx = signed_xy ? std::max(-1.f, int8_t(p[0]) / 127.f) : p[0] / 127.5f - 1;
          const float ny = signed_xy ? std::max(-1.f, int8_t(p[1]) / 127.f) : p[1] / 127.5f - 1;
          sum[0] += nx; sum[1] += ny; sum[2] += std::sqrt(std::max(0.f, 1 - nx * nx - ny * ny));
        } else for (uint32_t c = 0; c < channels; ++c) sum[c] += p[c];
        ++n;
      }
      auto* p = t.bytes.data() + dst.offset + y * dst.row_pitch + x * channels;
      if (normal) {
        const float len = std::sqrt(sum[0]*sum[0] + sum[1]*sum[1] + sum[2]*sum[2]);
        for (int c = 0; c < 2; ++c) {
          const float v = len > 0.000001f ? sum[c] / len : 0;
          p[c] = signed_xy ? uint8_t(int8_t(std::lround(v * 127))) : uint8_t(std::lround((v + 1) * 127.5f));
        }
      } else for (uint32_t c = 0; c < channels; ++c) p[c] = uint8_t(std::lround(sum[c] / n));
    }
  }
  t.plan.levels = count;
  return true;
}
bool ReadDds(const std::vector<uint8_t>& b, HostTextureData& t, std::string& why) {
  if (b.size() < 128 || U32(b.data()) != 0x20534444 || U32(b.data()+4) != 124 || U32(b.data()+76) != 32)
    return Fail(why, "invalid DDS header");
  auto word = [&](size_t o) { return U32(b.data()+o); };
  t.plan.width = word(16); t.plan.height = word(12); t.plan.levels = std::max(word(28), 1u);
  if (!t.plan.width || !t.plan.height || t.plan.width > 8192 || t.plan.height > 8192 ||
      word(24) > 1 || word(112) || t.plan.levels > MipCount(t.plan.width, t.plan.height))
    return Fail(why, "DDS must be a bounded single 2D image");
  const auto fourcc = word(84);
  size_t at = 128;
  if ((word(80) & 4) && fourcc == 0x30315844) {
    if (b.size() < 148 || word(132) != 3 || word(136) || word(140) != 1 || word(144) > 3)
      return Fail(why, "unsupported DDS DX10 shape/alpha mode");
    switch (word(128)) {
      case 71: t.plan.format = HostFormat::BC1_UNORM; break;
      case 77: t.plan.format = HostFormat::BC3_UNORM; break;
      case 83: t.plan.format = HostFormat::BC5_UNORM; break;
      case 84: t.plan.format = HostFormat::BC5_SNORM; break;
      case 98: t.plan.format = HostFormat::BC7_UNORM; break;
      case 28: t.plan.format = HostFormat::RGBA8_UNORM; break;
      case 49: t.plan.format = HostFormat::RG8_UNORM; break;
      case 51: t.plan.format = HostFormat::RG8_SNORM; break;
      case 61: t.plan.format = HostFormat::R8_UNORM; break;
      default: return Fail(why, "DDS requires linear BC1/BC3/BC5/BC7 or raw UNORM/RG SNORM; sRGB changes sampling");
    }
    // Premultiplied alpha would change the game's blend semantics.
    if (word(144) == 2) return Fail(why, "premultiplied DDS is incompatible");
    at = 148;
  } else if ((word(80) & 4) && fourcc == 0x31545844) t.plan.format = HostFormat::BC1_UNORM;
  else if ((word(80) & 4) && fourcc == 0x35545844) t.plan.format = HostFormat::BC3_UNORM;
  else if ((word(80) & 4) && (fourcc == 0x32495441 || fourcc == 0x55354342)) t.plan.format = HostFormat::BC5_UNORM;
  else return Fail(why, "unsupported DDS pixel format");
  if (GetHostFormatInfo(t.plan.format).block_size == 4 && ((t.plan.width & 3) || (t.plan.height & 3)))
    return Fail(why, "BC base dimensions must be multiples of four; use PNG/raw DDS for odd sizes");
  for (uint32_t l = 0; l < t.plan.levels; ++l) {
    if (!AddLevel(t, l, std::max(t.plan.width >> l, 1u), std::max(t.plan.height >> l, 1u)))
      return Fail(why, "DDS exceeds 128 MiB");
    const auto& s = t.subresources.back();
    if (s.size > b.size() - std::min(at, b.size())) return Fail(why, "truncated DDS mip data");
    std::memcpy(t.bytes.data()+s.offset, b.data()+at, s.size); at += s.size;
  }
  return at == b.size() || Fail(why, "unexpected trailing DDS data");
}
bool ReadPng(const std::vector<uint8_t>& b, HostTextureData& t, HostFormat original, std::string& why) {
  static const uint8_t sig[] = {137,80,78,71,13,10,26,10};
  if (b.size() < 33 || std::memcmp(b.data(), sig, 8) || std::memcmp(b.data()+12, "IHDR", 4) || b[24] != 8)
    return Fail(why, "requires an 8-bit PNG");
  // Colour originals may use alpha even when a preview looks opaque. Refuse
  // containers that discarded it rather than silently substituting opaque W.
  if (Family(original) <= 2 && b[25] != 4 && b[25] != 6)
    return Fail(why, "colour PNG requires an explicit alpha channel");
#ifdef _WIN32
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct ComScope { bool owned; ~ComScope() { if (owned) CoUninitialize(); } } scope{SUCCEEDED(com)};
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> convert;
  UINT w = 0, h = 0, frames = 0;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(b.data()), DWORD(b.size()))) ||
      FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
      FAILED(decoder->GetFrameCount(&frames)) || frames != 1 || FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 8192 || h > 8192 || uint64_t(w)*h*4 > kImageLimit ||
      FAILED(factory->CreateFormatConverter(&convert)) ||
      FAILED(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    return Fail(why, "PNG decode/size failure");
  std::vector<uint8_t> rgba(size_t(w)*h*4);
  if (FAILED(convert->CopyPixels(nullptr, w*4, UINT(rgba.size()), rgba.data()))) return Fail(why, "PNG pixel decode failed");
  const int family = Family(original);
  t.plan.width = w; t.plan.height = h;
  t.plan.format = family == 3 ? HostFormat::RG8_UNORM : family == 4 ? HostFormat::RG8_SNORM :
                  family == 5 ? HostFormat::R8_UNORM : HostFormat::RGBA8_UNORM;
  if (!AddLevel(t, 0, w, h)) return Fail(why, "PNG too large");
  const auto channels = GetHostFormatInfo(t.plan.format).bytes_per_block;
  for (size_t i = 0; i < size_t(w)*h; ++i) for (uint32_t c = 0; c < channels; ++c)
    t.bytes[i*channels+c] = family == 4 ? uint8_t(int8_t(std::lround((rgba[i*4+c]/127.5f-1)*127))) : rgba[i*4+c];
  return true;
#else
  return Fail(why, "PNG decoder unavailable on this platform; use DDS");
#endif
}
std::string RequestKey(uint64_t hash, const HostTexturePlan& p) {
  return TexturePackKey(hash, "") + ":" + std::to_string(unsigned(p.format)) + ":" +
      std::to_string(p.width) + ":" + std::to_string(p.height);
}
std::filesystem::path Utf8Path(const std::string& text) {
  return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()),text.size()));
}
}  // namespace

bool TexturePackEligible(const HostTexturePlan& p) {
  return p.dimension == Dimension::k2D && p.depth == 1 && p.layers == 1 && !p.min_level && Family(p.format);
}
uint64_t TextureBaseHash(const HostTextureData& data) {
  if (!TexturePackEligible(data.plan)) return 0;
  const auto* s = data.Find(0, 0);
  if (!s || s->offset > data.bytes.size() || s->size > data.bytes.size()-s->offset) return 0;
  return XXH3_64bits(data.bytes.data()+s->offset, s->size);
}
std::string TexturePackKey(uint64_t hash, const std::string& name) {
  char text[17]; std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(hash));
  return std::string(text) + (name.empty() ? "" : "__" + name);
}
bool TexturePackEntry::Matches(const HostTexturePlan& p) const {
  return TexturePackEligible(p) && p.format == format && p.width == width && p.height == height;
}
std::vector<std::string> SplitPackFolders(const std::string& folders) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= folders.size()) {
    size_t end = folders.find('|', start);
    if (end == std::string::npos) end = folders.size();
    if (end > start) out.push_back(folders.substr(start, end - start));
    start = end + 1;
  }
  return out;
}
bool TexturePackManifest::Read(const std::filesystem::path& folder, std::string& why) {
  entries.clear();
  std::vector<uint8_t> bytes;
  if (!ReadBytes(folder / "pack.txt", bytes) || bytes.size() > (8u << 20)) return Fail(why, "missing/oversized pack.txt");
  std::istringstream in(std::string(bytes.begin(), bytes.end()));
  std::string line;
  if (!std::getline(in, line) || (line != "KKTP 1" && line != "KKTP 1\r")) return Fail(why, "unsupported pack version");
  std::set<std::string> keys;
  std::map<std::string, std::string> aliases;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line == "\r") continue;
    std::istringstream row(line); std::string hash, format, extra;
    TexturePackEntry e;
    if (!(row >> hash >> e.name >> format >> e.width >> e.height >> e.file) || (row >> extra) ||
        hash.size() != 16 || !Hex(hash, e.hash) || !e.hash || !BfName(e.name) || !e.width || !e.height ||
        e.width > 8192 || e.height > 8192 || entries.size() >= kMaxEntries)
      return Fail(why, "invalid manifest row");
    for (unsigned i = 1; i < unsigned(HostFormat::COUNT); ++i)
      if (format == GetHostFormatInfo(HostFormat(i)).name) e.format = HostFormat(i);
    HostTexturePlan p; p.format=e.format; p.width=e.width; p.height=e.height;
    const auto key = TexturePackKey(e.hash, e.name);
    const auto ext = std::filesystem::path(e.file).extension().string();
    if (!TexturePackEligible(p) || (ext != ".dds" && ext != ".png") ||
        e.file != key + ext || !keys.insert(key).second)
      return Fail(why, "unsupported format, duplicate key or filename does not match identity");
    // Runtime has canonical content, not BF offsets. Two identical originals
    // cannot select different replacements; reject the ambiguity explicitly.
    const auto id = RequestKey(e.hash, p);
    if (aliases.contains(id)) return Fail(why, "duplicate original content: keep one canonical BF alias");
    aliases[id] = e.file;
    entries.push_back(std::move(e));
  }
  return true;
}
bool TexturePackManifest::Write(const std::filesystem::path& folder) const {
  std::ofstream out(folder / "pack.txt", std::ios::binary);
  out << "KKTP 1\n# hash bf_name original_host_format original_width original_height replacement_file\n";
  for (const auto& e : entries) out << TexturePackKey(e.hash, "") << ' ' << e.name << ' ' << GetHostFormatInfo(e.format).name << ' '
      << e.width << ' ' << e.height << ' ' << e.file << '\n';
  return bool(out);
}
bool LoadTextureReplacement(const std::filesystem::path& folder, const TexturePackEntry& entry,
                            HostTextureData& out, std::string& why) {
  out = {};
  std::error_code ec;
  // Resolve links before opening so an identity filename cannot escape a pack.
  const auto root = std::filesystem::canonical(folder, ec);
  if (ec) return Fail(why, "pack folder unavailable");
  const auto path = std::filesystem::canonical(root / entry.file, ec);
  if (ec || path.parent_path() != root) return Fail(why, "replacement absent or outside pack");
  std::vector<uint8_t> bytes;
  if (!ReadBytes(path, bytes)) return Fail(why, "replacement unreadable/too large");
  const bool png = path.extension() == ".png";
  if (!(png ? ReadPng(bytes, out, entry.format, why) : ReadDds(bytes, out, why))) return false;
  if (!Compatible(entry.format, out.plan.format)) return Fail(why, "format family/alpha/signedness mismatch");
  if (!GenerateMips(out, why)) return false;
  return true;
}

struct TexturePackStream::Impl {
  struct Request { uint64_t hash; HostTexturePlan plan; std::string key; };
  mutable std::mutex mutex;
  std::condition_variable work;
  bool stop = false, configure = false;
  uint64_t generation = 0, ready_bytes = 0;
  std::atomic<uint64_t> events{0};  // task 284: TexturePackStream::Events
  std::string folder, status;
  std::deque<Request> requests;
  std::set<std::string> seen;
  std::map<std::string, std::shared_ptr<const HostTextureData>> ready;
  TextureConversionWorkers workers;
  std::thread thread;
  explicit Impl(unsigned count) : workers(count), thread([this] { Run(); }) {}
  ~Impl() {
    { std::lock_guard lock(mutex); stop=true; }
    work.notify_one(); thread.join();
  }
  void Run() {
    TexturePackManifest manifest;
    // Each entry with the pack folder it came from (several packs: Kong texture-pack mods, task 200).
    std::map<std::string, std::pair<TexturePackEntry, size_t>> index;
    std::vector<std::string> roots;
    for (;;) {
      std::string root; uint64_t epoch; std::vector<Request> batch;
      bool changed;
      {
        std::unique_lock lock(mutex);
        work.wait(lock, [&] { return stop || configure || !requests.empty(); });
        if (stop) return;
        changed=configure; configure=false; root=folder; epoch=generation;
        while (!requests.empty() && batch.size() < 4) { batch.push_back(std::move(requests.front())); requests.pop_front(); }
      }
      if (changed) {
        // The folder setting may name several packs separated by '|' (the player's own pack, then Kong
        // texture-pack mods in load order): all are read, and a later pack's entry for the same original wins.
        // A pack that cannot be read is skipped; the others still load.
        std::string why; index.clear(); roots = SplitPackFolders(root);
        size_t read = 0;
        for (size_t f = 0; f < roots.size(); ++f) {
          manifest.entries.clear();
          std::string pack_why;
          bool ok=false;
          try { ok = manifest.Read(Utf8Path(roots[f]), pack_why); }
          catch (const std::exception& e) { pack_why=e.what(); }
          if (!ok) { why = roots.size() > 1 ? roots[f] + ": " + pack_why : pack_why; continue; }
          ++read;
          for (const auto& e : manifest.entries) {
            HostTexturePlan p; p.format=e.format; p.width=e.width; p.height=e.height;
            index.insert_or_assign(RequestKey(e.hash, p), std::make_pair(e, f));
          }
        }
        std::lock_guard lock(mutex);
        if (epoch == generation)
          status = read ? std::to_string(index.size()) + " entries" +
                              (roots.size() > 1 ? " from " + std::to_string(read) + " of " + std::to_string(roots.size()) + " packs" : "") +
                              (read < roots.size() ? " (" + why + ")" : "")
                        : why;
      }
      struct Result { std::shared_ptr<HostTextureData> data; std::string error; };
      std::vector<Result> results(batch.size());
      struct Context { const std::vector<std::string>* roots; const std::map<std::string,std::pair<TexturePackEntry,size_t>>* index;
        std::vector<Request>* batch; std::vector<Result>* results; } context{&roots,&index,&batch,&results};
      workers.Run(unsigned(batch.size()), &context, [](void* arg, unsigned i) {
        auto& c=*static_cast<Context*>(arg); auto& r=(*c.results)[i]; const auto& request=(*c.batch)[i];
        const auto it=c.index->find(request.key);
        if (it==c.index->end()) return;
        try {
          auto data=std::make_shared<HostTextureData>();
          if (LoadTextureReplacement(Utf8Path((*c.roots)[it->second.second]), it->second.first, *data, r.error)) r.data=std::move(data);
        } catch (const std::exception& e) { r.error=e.what(); }
      });
      {
        std::lock_guard lock(mutex);
        if (epoch != generation) continue;
        for (size_t i=0;i<batch.size();++i) {
          auto& r=results[i];
          if (!r.error.empty()) status = r.error;
          if (!r.data) continue;
          if (!seen.contains(batch[i].key)) continue;  // cancelled while it was being read
          if (r.data->bytes.size() > kReadyLimit-ready_bytes) { seen.erase(batch[i].key); continue; }
          const auto bytes=r.data->bytes.size();
          if (ready.emplace(batch[i].key,std::move(r.data)).second) ready_bytes+=bytes;
        }
        events.fetch_add(1, std::memory_order_release);
      }
    }
  }
};
TexturePackStream::TexturePackStream(unsigned workers) : impl_(new Impl(workers)) {}
TexturePackStream::~TexturePackStream() = default;
void TexturePackStream::Configure(const std::string& folder) {
  std::lock_guard lock(impl_->mutex);
  ++impl_->generation; impl_->folder=folder; impl_->configure=true; impl_->ready_bytes=0;
  impl_->requests.clear(); impl_->seen.clear(); impl_->ready.clear(); impl_->work.notify_one();
}
void TexturePackStream::Request(uint64_t hash, const HostTexturePlan& original) {
  if (!hash || !TexturePackEligible(original)) return;
  auto key=RequestKey(hash, original);
  std::lock_guard lock(impl_->mutex);
  if (impl_->folder.empty() || impl_->requests.size()>=256 || impl_->seen.size()>=65536 || !impl_->seen.insert(key).second) return;
  impl_->requests.push_back({hash,original,std::move(key)}); impl_->work.notify_one();
}
std::shared_ptr<const HostTextureData> TexturePackStream::Take(uint64_t hash, const HostTexturePlan& original, uint64_t max_bytes,
                                                              uint64_t* too_large) {
  if (too_large) *too_large=0;
  std::lock_guard lock(impl_->mutex);
  auto it=impl_->ready.find(RequestKey(hash,original));
  if (it==impl_->ready.end()) return {};
  if (it->second->bytes.size() > max_bytes) {
    if (too_large) {
      *too_large=it->second->bytes.size(); impl_->ready_bytes-=it->second->bytes.size();
      impl_->ready.erase(it); impl_->seen.erase(RequestKey(hash,original));
    }
    return {};
  }
  auto data=std::move(it->second); impl_->ready_bytes-=data->bytes.size(); impl_->ready.erase(it);
  // Allow another cache instance with the same original to request the data.
  impl_->seen.erase(RequestKey(hash,original));
  return data;
}
void TexturePackStream::Cancel(uint64_t hash, const HostTexturePlan& original) {
  if (!hash) return;
  const auto key=RequestKey(hash,original);
  std::lock_guard lock(impl_->mutex);
  auto& q=impl_->requests;
  q.erase(std::remove_if(q.begin(),q.end(),[&](const Impl::Request& r) { return r.key==key; }),q.end());
  if (auto it=impl_->ready.find(key); it!=impl_->ready.end()) {
    impl_->ready_bytes-=it->second->bytes.size(); impl_->ready.erase(it);
  }
  impl_->seen.erase(key);
}
std::string TexturePackStream::Status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }
uint64_t TexturePackStream::Events() const { return impl_->events.load(std::memory_order_acquire); }
}  // namespace kknr
