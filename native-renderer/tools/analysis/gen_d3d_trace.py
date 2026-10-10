# Generates kk/src/dev_d3d_trace.cpp: a developer-only hook on every D3D library function the engine calls
# (d3d_entry_points.json), counting calls per frame and, for windows of frames, logging each call's arguments
# and the bytes of the objects it is passed or returns (see DUMP_ARGS / DUMP_RET below).
# The hooks in CENSUS also feed the resource census (KK_DEV_TEX_CENSUS / KK_DEV_TEX_DUMP, brief 03).
# Usage: python gen_d3d_trace.py <d3d_entry_points.json> <out .cpp>
import json
import sys

entries = json.load(open(sys.argv[1]))
# Hooked elsewhere in the port (one REX_HOOK_RAW per function).
taken = {'sub_821141D8', 'sub_82106DE0', 'sub_821074E8', 'sub_821074F8', 'sub_821074C0'}
names = [e['name'] for e in entries if e['name'] not in taken]
PRESENT = 'sub_821147B8'
assert PRESENT in names, 'present not in the entry points'
# Resource census (KK_DEV_TEX_CENSUS, brief 03): these hooks also record what the call describes.
CENSUS = {
    'sub_82118F78': 'SetTexture',
    'sub_82118B88': 'CreateRenderTarget',
    'sub_82116178': 'Resolve',
    'sub_8210BE38': 'SetIndices',
    'sub_8210BD38': 'SetStreamSource',
    'sub_82111E68': 'SetVertexDeclaration',
    'sub_82118A68': 'CreateTextureInner',
    'sub_82126E70': 'CreateTexture',
    'sub_82109360': 'Lock',
    'sub_821093D0': 'Unlock',
    'sub_8210C378': 'SetRenderTarget',
    'sub_8210C6E0': 'SetDepthStencilSurface',
}
for n in CENSUS:
    assert n in names, n + ' not in the entry points'

CENSUS_CPP = r'''
// ---- Resource census (brief 03) ----
// KK_DEV_TEX_CENSUS=<file>: from launch, one line per distinct thing the engine hands the library: TEX
// (SetTexture: the texture's 16-byte header and its 6 fetch-constant words at +16), RT (CreateRenderTarget
// arguments, the params words and the 64-byte surface), RSV (Resolve arguments and the destination's fetch
// constant), IB / VB (the object's 32 bytes), DECL (the declaration's first 256 bytes), CTEX / CTEXI
// (texture creation, every call), LOCK / UNLOCK (per call site), SRT / SDS (surfaces set as render target
// or depth). Distinct = a new content hash (pointers and reference counts left out where known), so the
// file stays small; each kind stops after a cap.
// KK_DEV_TEX_DUMP=<dir>[,<count>]: also writes the guest memory of distinct textures as they are first bound
// (at most 4 per format / tiling / endian / dimension combination, <count> in all, default 200) as
// <dir>/tex_<base>_<hash>.bin: "KKTX", version 1, the 6 fetch words, base bytes, mip bytes (little-endian
// header), then the base and mip regions exactly as stored (upper-bound sizes, cut at uncommitted memory).
namespace census {

struct Args {
  uint32_t r[8];  // r3..r10
  double f1;
  uint32_t lr;
};

Args Capture(const PPCContext& ctx) {
  return {{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32},
          ctx.f1.f64,
          uint32_t(ctx.lr)};
}

enum Kind { kTex, kRt, kRsv, kIb, kVb, kDecl, kCtex, kCtexi, kLock, kUnlock, kSrt, kSds, kKinds };
const char* const kKindNames[kKinds] = {"TEX", "RT", "RSV", "IB", "VB", "DECL", "CTEX", "CTEXI", "LOCK", "UNLOCK",
                                        "SRT", "SDS"};
const int kKindCaps[kKinds] = {20000, 2000, 2000, 4000, 4000, 2000, 4000, 4000, 500, 500, 2000, 2000};

std::once_flag g_once;
bool g_on = false;
std::mutex g_mutex;
FILE* g_file = nullptr;
std::unordered_set<uint64_t> g_seen;
int g_kind_lines[kKinds] = {};
std::string g_dump_dir;
int g_dump_left = 0;
std::unordered_map<uint32_t, int> g_dump_per_combo;

void InitOnce() {
  if (const char* p = std::getenv("KK_DEV_TEX_CENSUS"); p && *p) g_file = std::fopen(p, "w");
  if (const char* d = std::getenv("KK_DEV_TEX_DUMP"); d && *d) {
    g_dump_dir = d;
    g_dump_left = 200;
    if (const size_t comma = g_dump_dir.find(','); comma != std::string::npos) {
      g_dump_left = std::atoi(g_dump_dir.c_str() + comma + 1);
      g_dump_dir.resize(comma);
    }
  }
  g_on = g_file != nullptr;
  if (g_on) REXLOG_INFO("KK d3d: resource census on");
}

bool On() {
  std::call_once(g_once, InitOnce);
  return g_on;
}

bool Ptr(uint32_t a) { return a >= 0x10000 && a < 0xFFFF0000; }

uint32_t Be32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

void Words(const uint8_t* base, uint32_t addr, uint32_t* out, int n) {
  for (int i = 0; i < n; ++i) out[i] = Ptr(addr) ? Be32(base, addr + 4 * i) : 0;
}

uint64_t Hash(Kind kind, const uint32_t* w, int n, uint64_t h = 0xCBF29CE484222325ull) {
  h ^= uint64_t(kind) << 56;
  for (int i = 0; i < n; ++i) {
    h ^= w[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

std::string Hex(const uint32_t* w, int n) {
  std::string s;
  char b[12];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof(b), i ? " %08X" : "%08X", w[i]);
    s += b;
  }
  return s;
}

std::string Hex1(uint32_t v) { return Hex(&v, 1); }

// Under g_mutex: true (and counted) when the key is new and the kind is under its cap.
bool First(Kind kind, uint64_t key) {
  if (g_kind_lines[kind] >= kKindCaps[kind]) return false;
  if (!g_seen.insert(key).second) return false;
  ++g_kind_lines[kind];
  return true;
}

void Line(Kind kind, const Args& a, const std::string& body) {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  std::fprintf(g_file, "%s t=%.2f f=%llu lr=%08X %s\n", kKindNames[kind], t, (unsigned long long)g_frame, a.lr,
               body.c_str());
  std::fflush(g_file);
}

// Block width, height and bits per texel by format (the SDK's FormatInfo table).
struct Fmt {
  uint8_t bw, bh;
  uint16_t bits;
};
const Fmt kFmts[64] = {
    {1, 1, 1},  {1, 1, 1},  {1, 1, 8},  {1, 1, 16}, {1, 1, 16}, {1, 1, 16}, {1, 1, 32},  {1, 1, 32},
    {1, 1, 8},  {1, 1, 8},  {1, 1, 16}, {2, 1, 16}, {2, 1, 16}, {1, 1, 32}, {1, 1, 32},  {1, 1, 16},
    {1, 1, 32}, {1, 1, 32}, {4, 4, 4},  {4, 4, 8},  {4, 4, 8},  {1, 1, 64}, {1, 1, 32},  {1, 1, 32},
    {1, 1, 16}, {1, 1, 32}, {1, 1, 64}, {1, 1, 16}, {1, 1, 32}, {1, 1, 64}, {1, 1, 16},  {1, 1, 32},
    {1, 1, 64}, {1, 1, 32}, {1, 1, 64}, {1, 1, 128}, {1, 1, 32}, {1, 1, 64}, {1, 1, 128}, {4, 1, 8},
    {2, 1, 16}, {1, 1, 16}, {1, 1, 32}, {1, 1, 8},  {4, 1, 8},  {1, 1, 16}, {1, 1, 16},  {1, 1, 16},
    {1, 1, 32}, {4, 4, 8},  {1, 1, 32}, {4, 4, 4},  {4, 4, 8},  {4, 4, 8},  {1, 1, 32},  {1, 1, 32},
    {1, 1, 32}, {1, 1, 96}, {4, 4, 4},  {4, 4, 4},  {4, 4, 4},  {4, 4, 4},  {1, 1, 32},  {1, 1, 32}};

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t NextPow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) p <<= 1;
  return p;
}

// Upper bounds of the base and mip regions (32x32-block tiles, 4 KB subresources, no mip tail packing).
void RegionSizes(const uint32_t* fc, uint32_t& base_bytes, uint32_t& mip_bytes) {
  const Fmt f = kFmts[fc[1] & 63];
  const uint32_t bpb = uint32_t(f.bits) * f.bw * f.bh / 8;
  const uint32_t dim = (fc[5] >> 9) & 3;
  uint32_t w, h, d = 1, layers = 1;
  if (dim == 0) {
    w = (fc[2] & 0xFFFFFF) + 1;
    h = 1;
  } else if (dim == 2) {
    w = (fc[2] & 0x7FF) + 1;
    h = ((fc[2] >> 11) & 0x7FF) + 1;
    d = (fc[2] >> 22) + 1;
  } else {
    w = (fc[2] & 0x1FFF) + 1;
    h = ((fc[2] >> 13) & 0x1FFF) + 1;
    layers = dim == 3 ? 6 : (((fc[1] >> 10) & 1) ? (fc[2] >> 26) + 1 : 1);
  }
  const uint32_t pitch = std::max(((fc[0] >> 22) & 0x1FF) * 32, w);
  const uint32_t slices = Align(d, dim == 2 ? 4 : 1) * layers;
  auto level_bytes = [&](uint32_t lw, uint32_t lh) {
    return Align(Align((lw + f.bw - 1) / f.bw, 32) * bpb * Align((lh + f.bh - 1) / f.bh, 32), 4096) * slices;
  };
  base_bytes = (fc[1] >> 12) ? level_bytes(pitch, h) : 0;
  mip_bytes = 0;
  if (fc[5] >> 12) {
    uint32_t levels = 0;
    for (uint32_t m = std::max({w, h, d}); m > 1; m >>= 1) ++levels;
    for (uint32_t l = 1; l <= levels; ++l)
      mip_bytes += level_bytes(std::max(NextPow2(w) >> l, 1u), std::max(NextPow2(h) >> l, 1u));
  }
}

// How many of n bytes from p are committed and readable.
size_t Readable(const uint8_t* p, size_t n) {
#if defined(_WIN32)
  size_t ok = 0;
  while (ok < n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p + ok, &mbi, sizeof(mbi))) break;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) break;
    ok = std::min(n, size_t(reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - p));
  }
  return ok;
#else
  return n;
#endif
}

// Under g_mutex.
void DumpTexture(const uint8_t* base, const uint32_t* fc, uint64_t key) {
  if (g_dump_dir.empty() || g_dump_left <= 0) return;
  const uint32_t combo = (fc[1] & 0xFF) | ((fc[0] >> 31) << 8) | (((fc[5] >> 9) & 7) << 9);
  if (++g_dump_per_combo[combo] > 4) return;
  uint32_t base_bytes, mip_bytes;
  RegionSizes(fc, base_bytes, mip_bytes);
  if (base_bytes > (64u << 20) || mip_bytes > (64u << 20)) return;
  // Physical addresses: the 0xA0000000 view maps physical memory from 0.
  const uint8_t* base_ptr = base + 0xA0000000u + (((fc[1] >> 12) & 0x1FFFF) << 12);
  const uint8_t* mip_ptr = base + 0xA0000000u + (((fc[5] >> 12) & 0x1FFFF) << 12);
  if (base_bytes) base_bytes = uint32_t(Readable(base_ptr, base_bytes));
  if (mip_bytes) mip_bytes = uint32_t(Readable(mip_ptr, mip_bytes));
  char name[64];
  std::snprintf(name, sizeof(name), "/tex_%08X_%016llX.bin", ((fc[1] >> 12) & 0x1FFFF) << 12,
                (unsigned long long)key);
  if (FILE* f = std::fopen((g_dump_dir + name).c_str(), "wb")) {
    const uint32_t header[10] = {0x58544B4Bu /* "KKTX" */, 1, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5],
                                 base_bytes, mip_bytes};
    std::fwrite(header, 4, 10, f);
    if (base_bytes) std::fwrite(base_ptr, 1, base_bytes, f);
    if (mip_bytes) std::fwrite(mip_ptr, 1, mip_bytes, f);
    std::fclose(f);
    --g_dump_left;
  }
}

void SetTexture(const Args& a, PPCContext&, uint8_t* base) {
  if (!On() || !Ptr(a.r[2])) return;
  uint32_t w[10];
  Words(base, a.r[2], w, 10);  // header (4 words) + fetch constant (6)
  const uint64_t key = Hash(kTex, w + 4, 6);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kTex, key)) return;
  Line(kTex, a, "s=" + std::to_string(a.r[1]) + " obj=" + Hex1(a.r[2]) + " hdr=" + Hex(w, 4) + " fc=" + Hex(w + 4, 6));
  DumpTexture(base, w + 4, key);
}

void CreateRenderTarget(const Args& a, PPCContext& ctx, uint8_t* base) {
  if (!On()) return;
  uint32_t p[4], s[16];
  Words(base, a.r[4], p, 4);
  Words(base, ctx.r3.u32, s, 16);
  const uint32_t k[11] = {a.r[0], a.r[1], a.r[2], a.r[3], a.r[5], a.r[6], a.r[7], p[0], p[1], p[2], p[3]};
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kRt, Hash(kRt, k, 11))) return;
  Line(kRt, a, "args=" + Hex(a.r, 8) + " params=" + Hex(p, 4) + " ret=" + Hex1(ctx.r3.u32) + " surf=" + Hex(s, 16));
}

void Resolve(const Args& a, PPCContext&, uint8_t* base) {
  if (!On()) return;
  uint32_t rect[4], dest[10], point[2], color[4];
  Words(base, a.r[2], rect, 4);
  Words(base, a.r[3], dest, 10);
  Words(base, a.r[4], point, 2);
  Words(base, a.r[7], color, 4);
  float z = float(a.f1);
  uint32_t zbits;
  std::memcpy(&zbits, &z, 4);
  const uint32_t k[16] = {a.r[1], rect[0], rect[1], rect[2], rect[3], dest[4], dest[5], dest[6], dest[7], dest[8],
                          dest[9], point[0], point[1], a.r[5], a.r[6], zbits};
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kRsv, Hash(kRsv, k, 16))) return;
  Line(kRsv, a, "flags=" + Hex1(a.r[1]) + " rect=" + Hex(rect, 4) + " dest=" + Hex1(a.r[3]) + " fc=" + Hex(dest + 4, 6) +
                    " point=" + Hex(point, 2) + " level=" + Hex1(a.r[5]) + " slice=" + Hex1(a.r[6]) +
                    " color=" + Hex(color, 4) + " z=" + Hex1(zbits) + " args=" + Hex(a.r, 8));
}

void BufferObject(Kind kind, const Args& a, uint8_t* base, uint32_t obj, uint32_t extra) {
  if (!On() || !Ptr(obj)) return;
  uint32_t w[8];
  Words(base, obj, w, 8);
  const uint32_t k[5] = {w[4], w[5], w[6], w[7], extra};
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kind, Hash(kind, k, 5))) return;
  Line(kind, a, "obj=" + Hex1(obj) + " words=" + Hex(w, 8) + " args=" + Hex(a.r, 5));
}

void SetIndices(const Args& a, PPCContext&, uint8_t* base) { BufferObject(kIb, a, base, a.r[1], 0); }
void SetStreamSource(const Args& a, PPCContext&, uint8_t* base) {
  BufferObject(kVb, a, base, a.r[2], a.r[4] | a.r[1] << 24);
}

void SetVertexDeclaration(const Args& a, PPCContext&, uint8_t* base) {
  if (!On() || !Ptr(a.r[1])) return;
  uint32_t w[64];
  Words(base, a.r[1], w, 64);
  const uint32_t refcount = w[1];
  w[1] = 0;
  const uint64_t key = Hash(kDecl, w, 64, a.r[1]);
  w[1] = refcount;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kDecl, key)) return;
  Line(kDecl, a, "obj=" + Hex1(a.r[1]) + " words=" + Hex(w, 64));
}

void Created(Kind kind, const Args& a, PPCContext& ctx, uint8_t* base) {
  if (!On()) return;
  uint32_t w[10];
  Words(base, ctx.r3.u32, w, 10);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kind, Hash(kind, a.r, 8, g_kind_lines[kind]))) return;  // every call (up to the cap)
  Line(kind, a, "args=" + Hex(a.r, 8) + " ret=" + Hex1(ctx.r3.u32) + " words=" + Hex(w, 10));
}
void CreateTextureInner(const Args& a, PPCContext& ctx, uint8_t* base) { Created(kCtexi, a, ctx, base); }
void CreateTexture(const Args& a, PPCContext& ctx, uint8_t* base) { Created(kCtex, a, ctx, base); }

void LockSite(Kind kind, const Args& a, uint8_t* base) {
  if (!On()) return;
  uint32_t w[8];
  Words(base, a.r[0], w, 8);
  const uint32_t k[2] = {a.lr, w[0] & 0xFFFF0000u};
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kind, Hash(kind, k, 2))) return;
  Line(kind, a, "obj=" + Hex1(a.r[0]) + " words=" + Hex(w, 8) + " args=" + Hex(a.r, 8));
}
void Lock(const Args& a, PPCContext&, uint8_t* base) { LockSite(kLock, a, base); }
void Unlock(const Args& a, PPCContext&, uint8_t* base) { LockSite(kUnlock, a, base); }

void Surface(Kind kind, const Args& a, uint8_t* base, uint32_t obj, uint32_t index) {
  if (!On() || !Ptr(obj)) return;
  uint32_t s[16];
  Words(base, obj, s, 16);
  uint32_t k[13];
  k[0] = index;
  std::memcpy(k + 1, s + 4, 12 * 4);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!First(kind, Hash(kind, k, 13))) return;
  Line(kind, a, "index=" + std::to_string(index) + " obj=" + Hex1(obj) + " surf=" + Hex(s, 16));
}
void SetRenderTarget(const Args& a, PPCContext&, uint8_t* base) { Surface(kSrt, a, base, a.r[2], a.r[1]); }
void SetDepthStencilSurface(const Args& a, PPCContext&, uint8_t* base) { Surface(kSds, a, base, a.r[1], 0); }

}  // namespace census
'''

# Object dumps (brief 01: object layouts checked against memory). Function -> [(argument register, bytes)].
# Each distinct (function, register, address, contents) is logged once per window.
DUMP_ARGS = {
    'sub_82118F78': [('r5', 40)],                                # SetTexture: texture
    'sub_8210BD38': [('r5', 20)],                                # SetStreamSource: vertex buffer
    'sub_8210BE38': [('r4', 20)],                                # SetIndices: index buffer
    'sub_8210C378': [('r5', 64)],                                # SetRenderTarget: surface
    'sub_8210C6E0': [('r4', 64)],                                # SetDepthStencilSurface: surface
    'sub_82111E68': [('r4', 192)],                               # SetVertexDeclaration: declaration
    'sub_821108B8': [('r4', 256)],                               # SetVertexShader: shader
    'sub_82110C28': [('r4', 768)],                               # SetPixelShader: shader
    'sub_8210BAC8': [('r4', 24)],                                # SetViewport: D3DVIEWPORT9
    'sub_82115418': [('r5', 16)],                                # Clear: first rect
    'sub_82116178': [('r5', 16), ('r6', 40), ('r7', 8), ('r10', 16)],  # Resolve: rect, texture, point, colour
    'sub_82118B88': [('r7', 12)],                                # CreateRenderTarget: parameters
    'sub_82110C90': [('r3', 192)],                               # CreateVertexDeclaration: elements
    'sub_82111CA0': [('r3', 64)],                                # CreateVertexShader: container
    'sub_82111D90': [('r3', 64)],                                # CreatePixelShader: container
    'sub_8210C130': [],
}
# Function -> bytes of the object it returns in r3.
DUMP_RET = {
    'sub_82118B88': 64,    # CreateRenderTarget
    'sub_82118838': 64,    # GetSurfaceLevel
    'sub_82118900': 64,    # GetCubeMapSurface
    'sub_82118A68': 40,    # CreateTexture
    'sub_821092B0': 20,    # CreateVertexBuffer
    'sub_821093E0': 20,    # CreateIndexBuffer
    'sub_82110C90': 192,   # CreateVertexDeclaration
    'sub_82111CA0': 256,   # CreateVertexShader
    'sub_82111D90': 768,   # CreatePixelShader
    'sub_8210BEB8': 64,    # GetRenderTarget
    'sub_8210BF00': 64,    # GetDepthStencilSurface
}

out = []
out.append('''// Developer-only: KK_DEV_D3D_TRACE=<seconds>[,<frames>][;<seconds>[,<frames>]...] profiles the game's use
// of the XDK Direct3D library (the functions the engine calls, generated by
// native-renderer/tools/analysis/gen_d3d_trace.py from the call graph). Every call is counted per frame;
// in each window (<frames> frames, default 2, starting <seconds> after launch) every call is logged with
// its arguments (r3-r10, f1) and call site (lr), objects passed to or returned by the functions listed in
// the generator are logged as hex ("KK d3d obj:" / "KK d3d ret:", once per distinct content per window),
// and each frame ends with a histogram. The frame boundary is the engine's present call (sub_821147B8).
// KK_DEV_D3D_TRACE_FROM=menu: the window times count from the save menu opening instead of launch.
// KK_DEV_D3D_DUMP=<file>: the device struct (20,608 bytes) is written to <file>.<window> at each window start.
// KK_DEV_TEX_CENSUS=<file> and KK_DEV_TEX_DUMP=<dir>[,<count>]: the resource census (see census below).
#if defined(KK_DEV_TOOLS)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // VirtualQuery (census texture dumps)
#endif
#include "menu_hook.h"

namespace {

struct Entry {
  const char* name;
  std::atomic<uint32_t> frame_calls{0};
  std::atomic<uint64_t> total{0};
};

Entry g_entries[] = {
''')
for n in names:
    out.append(f'    {{"{n}"}},\n')
out.append('''};
constexpr int kCount = int(sizeof(g_entries) / sizeof(g_entries[0]));

struct Window {
  double start;
  int frames;
};
std::vector<Window> g_windows;
size_t g_next_window = 0;
std::atomic<bool> g_window{false};
int g_frames_left = 0;
uint64_t g_frame = 0;
const auto g_start = std::chrono::steady_clock::now();
std::mutex g_seen_mutex;
std::unordered_set<uint64_t> g_seen;
// Seconds after launch when the save menu opened (KK_DEV_D3D_TRACE_FROM=menu), or -1 before that.
std::atomic<double> g_menu_seconds{-1.0};
const bool g_from_menu = [] {
  const char* v = std::getenv("KK_DEV_D3D_TRACE_FROM");
  if (!v || std::strcmp(v, "menu") != 0) return false;
  kk::OnSaveMenuShown([] {
    g_menu_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    REXLOG_INFO("KK d3d: save menu opened at {:.1f} s", g_menu_seconds.load());
  });
  return true;
}();

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  const char* v = std::getenv("KK_DEV_D3D_TRACE");
  if (!v || !*v) return;
  std::string s(v);
  size_t pos = 0;
  while (pos <= s.size()) {
    size_t end = s.find(';', pos);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(pos, end - pos);
    if (!item.empty()) {
      Window w{std::atof(item.c_str()), 2};
      if (size_t comma = item.find(','); comma != std::string::npos)
        w.frames = std::max(1, std::atoi(item.c_str() + comma + 1));
      g_windows.push_back(w);
    }
    pos = end + 1;
  }
}

void OnCall(int i, PPCContext& ctx) {
  Entry& e = g_entries[i];
  e.frame_calls.fetch_add(1, std::memory_order_relaxed);
  e.total.fetch_add(1, std::memory_order_relaxed);
  if (g_window.load(std::memory_order_relaxed)) {
    REXLOG_INFO("KK d3d: {} lr={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} f1={}",
                e.name, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                ctx.r9.u32, ctx.r10.u32, ctx.f1.f64);
  }
}

// Guest heap, stack and image ranges where the engine keeps D3D objects and argument structs.
bool Dumpable(uint32_t addr, uint32_t n) {
  if (addr & 3) return false;
  const uint64_t end = uint64_t(addr) + n;
  return (addr >= 0x40000000u && end <= 0x80000000u) || (addr >= 0x82000000u && end <= 0x84000000u);
}

void DumpObject(int i, const char* kind, const char* reg, uint32_t addr, uint32_t n, uint8_t* base) {
  if (!g_window.load(std::memory_order_relaxed) || !Dumpable(addr, n)) return;
  const uint8_t* p = base + addr;
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(uint64_t(i));
  mix(uint64_t(reg[1]) << 8 | uint64_t(reg[2]));
  mix(addr);
  for (uint32_t k = 0; k < n; ++k) mix(p[k]);
  {
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    if (!g_seen.insert(h).second) return;
  }
  static const char* hexd = "0123456789ABCDEF";
  std::string hex;
  hex.reserve(n * 2 + n / 4);
  for (uint32_t k = 0; k < n; ++k) {
    if (k && (k & 3) == 0) hex += ' ';
    hex += hexd[p[k] >> 4];
    hex += hexd[p[k] & 15];
  }
  REXLOG_INFO("KK d3d {}: {} {}={:08X} n={} {}", kind, g_entries[i].name, reg, addr, n, hex);
}

void DumpDevice(uint32_t device, uint8_t* base, size_t window) {
  const char* path = std::getenv("KK_DEV_D3D_DUMP");
  if (!path || !*path || !device) return;
  const std::string file = std::string(path) + "." + std::to_string(window);
  if (FILE* f = std::fopen(file.c_str(), "wb")) {
    std::fwrite(base + device, 1, 20608, f);
    std::fclose(f);
    REXLOG_INFO("KK d3d: device {:08X} dumped to {}", device, file);
  }
}

void OnPresent(uint32_t device, uint8_t* base) {
  Init();
  ++g_frame;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  const bool window = g_window.load(std::memory_order_relaxed);
  if (window) {
    std::string hist;
    for (int i = 0; i < kCount; ++i) {
      const uint32_t n = g_entries[i].frame_calls.load(std::memory_order_relaxed);
      if (n) hist += std::string(" ") + g_entries[i].name + "=" + std::to_string(n);
    }
    REXLOG_INFO("KK d3d: frame {} end:{}", g_frame, hist);
    if (--g_frames_left <= 0) {
      g_window.store(false, std::memory_order_relaxed);
      std::vector<std::pair<uint64_t, int>> totals;
      for (int i = 0; i < kCount; ++i) totals.push_back({g_entries[i].total.load(), i});
      std::sort(totals.rbegin(), totals.rend());
      std::string s;
      for (auto& [n, i] : totals) if (n) s += std::string(" ") + g_entries[i].name + "=" + std::to_string(n);
      REXLOG_INFO("KK d3d: window end after frame {} ({:.1f} s); totals since launch:{}", g_frame, t, s);
    }
  } else if (g_next_window < g_windows.size() && (!g_from_menu || g_menu_seconds >= 0.0) &&
             t >= g_windows[g_next_window].start + (g_from_menu ? g_menu_seconds.load() : 0.0)) {
    {
      std::lock_guard<std::mutex> lock(g_seen_mutex);
      g_seen.clear();
    }
    g_frames_left = g_windows[g_next_window].frames;
    DumpDevice(device, base, g_next_window);
    REXLOG_INFO("KK d3d: window {} start at frame {} ({:.1f} s), {} frames", g_next_window, g_frame + 1, t,
                g_frames_left);
    ++g_next_window;
    g_window.store(true, std::memory_order_relaxed);
  }
  for (int i = 0; i < kCount; ++i) g_entries[i].frame_calls.store(0, std::memory_order_relaxed);
}

''' + CENSUS_CPP + '''
}  // namespace

#define KK_D3D_HOOK(INDEX, ADDR)                     \\
  REX_EXTERN(__imp__sub_##ADDR);                     \\
  REX_HOOK_RAW(sub_##ADDR) {                         \\
    OnCall(INDEX, ctx);                              \\
    __imp__sub_##ADDR(ctx, base);                    \\
  }

#define KK_D3D_HOOK_PRESENT(INDEX, ADDR)             \\
  REX_EXTERN(__imp__sub_##ADDR);                     \\
  REX_HOOK_RAW(sub_##ADDR) {                         \\
    OnCall(INDEX, ctx);                              \\
    const uint32_t device = ctx.r3.u32;              \\
    __imp__sub_##ADDR(ctx, base);                    \\
    OnPresent(device, base);                         \\
  }

#define KK_D3D_HOOK_CENSUS(INDEX, ADDR, FN)          \\
  REX_EXTERN(__imp__sub_##ADDR);                     \\
  REX_HOOK_RAW(sub_##ADDR) {                         \\
    OnCall(INDEX, ctx);                              \\
    const census::Args args = census::Capture(ctx);  \\
    __imp__sub_##ADDR(ctx, base);                    \\
    census::FN(args, ctx, base);                     \\
  }

''')
NL = '\n'
for i, n in enumerate(names):
    args = list(DUMP_ARGS.get(n, []))
    ret = DUMP_RET.get(n)
    census_fn = CENSUS.get(n)
    if n == PRESENT:
        out.append(f'KK_D3D_HOOK_PRESENT({i}, {n[4:]})' + NL)
    elif not args and not ret and not census_fn:
        out.append(f'KK_D3D_HOOK({i}, {n[4:]})' + NL)
    elif not args and not ret:
        out.append(f'KK_D3D_HOOK_CENSUS({i}, {n[4:]}, {census_fn})' + NL)
    else:
        # Object dumps, and the census record when the function has one.
        lines = [f'REX_EXTERN(__imp__{n});', f'REX_HOOK_RAW({n}) {{', f'  OnCall({i}, ctx);']
        if census_fn:
            lines.append('  const census::Args census_args = census::Capture(ctx);')
        for reg, size in args:
            lines.append(f'  DumpObject({i}, "obj", "{reg}", ctx.{reg}.u32, {size}, base);')
        lines.append(f'  __imp__{n}(ctx, base);')
        if ret:
            lines.append(f'  DumpObject({i}, "ret", "r3", ctx.r3.u32, {ret}, base);')
        if census_fn:
            lines.append(f'  census::{census_fn}(census_args, ctx, base);')
        lines.append('}')
        out.append(NL.join(lines) + NL)
out.append(NL + '#endif' + NL)
open(sys.argv[2], 'w', newline=NL).write(''.join(out))
print(len(names), 'hooks ->', sys.argv[2])
