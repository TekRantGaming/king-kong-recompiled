// King Kong native renderer: the shader pack, built on the player's machine.
//
// The pack (kkshaders/cache.h, ShaderPack) holds every shader of the game's Shaders/xeshaders.bin,
// translated and compiled for one backend: SPIR-V for Vulkan, DXIL for D3D12. It is derived from
// the game's files, so the port never ships one: the launcher builds it from the player's own copy
// of the game into the user's cache folder (<cache root>/native-shaders), and the plugin looks for
// it there (docs/shaders.md, "The pack on the player's machine").
//
// A stamp file beside the pack (<pack>.stamp, text) says what it was built from: the backend, the
// translator hash, the DXC that compiled it, the database's hash and whether the render scale
// aware pixel variants are in it. The pack is current when all of them match what a build would
// use now; otherwise it is rebuilt. The stamp is written last, so a pack without one (a build that
// stopped half way) counts as missing.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace kkshaders {

enum class PackBackend : uint8_t { Spirv = 0, Dxil = 1 };

const char* packBackendName(PackBackend backend);   // "spirv" / "dxil"
const char* packFileName(PackBackend backend);      // "kkshaders-spirv.pack" / "kkshaders-dxil.pack"
// The folder the pack goes in under the cache root.
constexpr const char* kPackFolderName = "native-shaders";

struct PackStamp {
    PackBackend backend = PackBackend::Spirv;
    uint64_t translator = 0;    // translatorHash()
    std::string dxc;            // dxcIdentity()
    uint64_t database = 0;      // XXH3-64 of xeshaders.bin
    bool scaled = false;        // render scale aware pixel variants included
    uint32_t shaders = 0;       // entries in the pack (information only, not compared)

    std::string toText() const;
    static bool parse(const std::string& text, PackStamp& out);
};

enum class PackState : uint8_t { Current = 0, Missing = 1, Stale = 2 };

struct PackCheck {
    PackState state = PackState::Missing;
    std::string reason;         // why it is missing or stale, for the log
};

// Is a pack with stamp `found` (null: no pack or no stamp) good for `wanted`? A pack with the
// scaled variants also serves a request without them.
PackCheck comparePackStamp(const PackStamp* found, const PackStamp& wanted);

struct PackBuildOptions {
    std::filesystem::path database;  // the game's Shaders/xeshaders.bin
    std::filesystem::path folder;    // where the pack goes (<cache root>/native-shaders)
    PackBackend backend = PackBackend::Spirv;
    bool scaled = false;             // also the render scale aware pixel shaders
    unsigned threads = 0;            // 0: one per hardware thread
    bool lowPriority = false;        // worker threads below normal priority (a background build)
    std::string dxc;                 // DXC's folder or library for loadDxc (empty: default search)
};

struct PackBuildProgress {
    enum Phase : int { kStarting = 0, kReading = 1, kCompiling = 2, kWriting = 3, kFinished = 4 };
    std::atomic<int> phase{kStarting};
    std::atomic<uint32_t> done{0};    // shaders compiled (or failed) so far
    std::atomic<uint32_t> total{0};   // shaders to compile (known once the database is read)
    std::atomic<uint32_t> failed{0};
    std::atomic<bool> cancel{false};  // set by anyone: the build stops soon and writes nothing
};

struct PackBuildResult {
    bool ok = false;            // the pack is there and current (built now, or already)
    bool upToDate = false;      // nothing was built: the pack was already current
    bool cancelled = false;
    std::string error;
    std::filesystem::path pack;
    PackStamp stamp;
    uint32_t compiled = 0;      // distinct shaders compiled
    uint32_t failed = 0;        // translation or compile failures (compiled on first use instead)
    uint32_t parseFailures = 0; // database entries that did not parse
    unsigned threads = 0;
    double seconds = 0;
    uint64_t bytes = 0;         // pack file size
};

std::filesystem::path packPath(const PackBuildOptions& options);
std::filesystem::path stampPath(const std::filesystem::path& pack);  // <pack>.stamp

// The stamp found beside a pack (false: no pack, no stamp, or unreadable).
bool readPackStamp(const std::filesystem::path& pack, PackStamp& out);

// The stamp a build with these options would write now. Loads DXC and hashes the database.
bool wantedPackStamp(const PackBuildOptions& options, PackStamp& out, std::string* error);

// Whether the pack in options.folder is current for these options.
PackCheck checkPack(const PackBuildOptions& options);

// Builds the pack unless it is already current (`force`: always). Runs on the calling thread
// with options.threads workers; reports in `progress` (optional) and stops when its `cancel`
// is set. The pack and its stamp are written to temporary names and renamed into place, so a
// reader never sees half a pack; nothing is written when cancelled or when nothing compiled.
PackBuildResult buildPack(const PackBuildOptions& options, PackBuildProgress* progress = nullptr, bool force = false);

}  // namespace kkshaders
