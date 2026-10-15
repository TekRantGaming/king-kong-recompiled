#include "kkshaders/pack_builder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <xxh3.h>

#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/lighting.h"
#include "kkshaders/translator.h"
#include "kkshaders/tessellation.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kkshaders {

namespace fs = std::filesystem;

namespace {

constexpr const char* kStampHeader = "kkshaders pack stamp 1";

std::string hex16(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

bool parseHex(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = (v << 4) | uint64_t(d);
    }
    out = v;
    return true;
}

bool readBytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool writeText(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    return f.write(text.data(), std::streamsize(text.size())) && f.flush();
}

unsigned long processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

void lowerThisThreadsPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    // Linux: the nice value is per thread.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

// Replaces `to` with `from` (same folder). Removes `from` when it fails.
bool moveInto(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    if (error) *error = "cannot replace " + to.string() + ": " + ec.message();
    std::error_code ignored;
    fs::remove(from, ignored);
    return false;
}

uint64_t lightingStampHash() {
    const std::string& text = lightingText();
    const uint64_t h = XXH3_64bits(text.data(), text.size());
    return h ? h : 1;
}

}  // namespace

const char* packBackendName(PackBackend backend) { return backend == PackBackend::Dxil ? "dxil" : "spirv"; }

const char* packFileName(PackBackend backend) {
    return backend == PackBackend::Dxil ? "kkshaders-dxil.pack" : "kkshaders-spirv.pack";
}

std::string PackStamp::toText() const {
    std::ostringstream s;
    s << kStampHeader << "\n";
    s << "backend " << packBackendName(backend) << "\n";
    s << "translator " << hex16(translator) << "\n";
    s << "dxc " << dxc << "\n";
    s << "database " << hex16(database) << "\n";
    s << "scaled " << (scaled ? 1 : 0) << "\n";
    s << "motion " << (motion ? 1 : 0) << "\n";
    s << "tessellation " << tessellation << "\n";
    if (lighting) s << "lighting " << hex16(lighting) << "\n";
    s << "shaders " << shaders << "\n";
    return s.str();
}

bool PackStamp::parse(const std::string& text, PackStamp& out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kStampHeader) return false;
    PackStamp s;
    bool haveBackend = false, haveTranslator = false, haveDxc = false, haveDatabase = false, haveScaled = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space), value = space == std::string::npos ? "" : line.substr(space + 1);
        if (key == "backend") {
            if (value == "spirv") s.backend = PackBackend::Spirv;
            else if (value == "dxil") s.backend = PackBackend::Dxil;
            else return false;
            haveBackend = true;
        } else if (key == "translator") {
            if (!parseHex(value, s.translator)) return false;
            haveTranslator = true;
        } else if (key == "dxc") {
            s.dxc = value;
            haveDxc = true;
        } else if (key == "database") {
            if (!parseHex(value, s.database)) return false;
            haveDatabase = true;
        } else if (key == "scaled") {
            if (value != "0" && value != "1") return false;
            s.scaled = value == "1";
            haveScaled = true;
        } else if (key == "motion") {
            if (value != "0" && value != "1") return false;
            s.motion = value == "1";
        } else if (key == "tessellation") {
            s.tessellation = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "lighting") {
            if (!parseHex(value, s.lighting)) return false;
        } else if (key == "shaders") {
            s.shaders = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
        }
        // Unknown keys are ignored (a newer port's stamp read by an older one).
    }
    if (!haveBackend || !haveTranslator || !haveDxc || !haveDatabase || !haveScaled) return false;
    out = std::move(s);
    return true;
}

PackCheck comparePackStamp(const PackStamp* found, const PackStamp& wanted) {
    PackCheck c;
    if (!found) {
        c.state = PackState::Missing;
        c.reason = "no pack";
        return c;
    }
    c.state = PackState::Stale;
    if (found->backend != wanted.backend)
        c.reason = std::string("built for ") + packBackendName(found->backend) + ", not " + packBackendName(wanted.backend);
    else if (found->translator != wanted.translator)
        c.reason = "built by another translator (" + hex16(found->translator) + ", now " + hex16(wanted.translator) + ")";
    else if (found->dxc != wanted.dxc)
        c.reason = "built with another DXC (" + found->dxc + ", now " + wanted.dxc + ")";
    else if (found->database != wanted.database)
        c.reason = "the game's shader database changed (" + hex16(found->database) + ", now " + hex16(wanted.database) + ")";
    else if (wanted.scaled && !found->scaled)
        c.reason = "without the render scale aware pixel shaders";
    else if (wanted.tessellation != found->tessellation)
        c.reason = "without the current Ann tessellation stages";
    else if (wanted.motion && !found->motion)
        c.reason = "without the motion vertex shaders";
    else if (wanted.lighting && found->lighting != wanted.lighting)
        c.reason = found->lighting ? "with an older version of the lighting options' water shaders" : "without the lighting options' water shaders";
    else {
        c.state = PackState::Current;
        c.reason = "current";
    }
    return c;
}

fs::path packPath(const PackBuildOptions& options) { return options.folder / packFileName(options.backend); }

fs::path stampPath(const fs::path& pack) {
    fs::path p = pack;
    p += ".stamp";
    return p;
}

bool readPackStamp(const fs::path& pack, PackStamp& out) {
    std::error_code ec;
    if (!fs::is_regular_file(pack, ec)) return false;
    std::vector<uint8_t> bytes;
    if (!readBytes(stampPath(pack), bytes)) return false;
    return PackStamp::parse(std::string(bytes.begin(), bytes.end()), out);
}

bool wantedPackStamp(const PackBuildOptions& options, PackStamp& out, std::string* error) {
    if (!loadDxc(options.dxc, error)) return false;
    std::vector<uint8_t> bytes;
    if (!readBytes(options.database, bytes)) {
        if (error) *error = "cannot read " + options.database.string();
        return false;
    }
    out = PackStamp{};
    out.backend = options.backend;
    out.translator = translatorHash();
    out.dxc = dxcIdentity();
    out.database = XXH3_64bits(bytes.data(), bytes.size());
    out.scaled = options.scaled;
    out.motion = options.motion;
    out.tessellation = 2;
    out.lighting = options.lighting ? lightingStampHash() : 0;
    return true;
}

PackCheck checkPack(const PackBuildOptions& options) {
    PackStamp wanted;
    std::string error;
    if (!wantedPackStamp(options, wanted, &error)) {
        PackCheck c;
        c.state = PackState::Missing;
        c.reason = error;
        return c;
    }
    PackStamp found;
    const bool have = readPackStamp(packPath(options), found);
    return comparePackStamp(have ? &found : nullptr, wanted);
}

PackBuildResult buildPack(const PackBuildOptions& options, PackBuildProgress* progress, bool force) {
    PackBuildProgress local;
    PackBuildProgress& p = progress ? *progress : local;
    PackBuildResult r;
    r.pack = packPath(options);
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&]() -> PackBuildResult& {
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        p.phase = PackBuildProgress::kFinished;
        return r;
    };
    p.phase = PackBuildProgress::kStarting;

    // DXC and the database: the stamp this build writes.
    if (!loadDxc(options.dxc, &r.error)) return finish();
    std::vector<uint8_t> bytes;
    if (!readBytes(options.database, bytes)) {
        r.error = "cannot read " + options.database.string();
        return finish();
    }
    r.stamp.backend = options.backend;
    r.stamp.translator = translatorHash();
    r.stamp.dxc = dxcIdentity();
    r.stamp.database = XXH3_64bits(bytes.data(), bytes.size());
    r.stamp.scaled = options.scaled;
    r.stamp.motion = options.motion;
    r.stamp.tessellation = 2;
    r.stamp.lighting = options.lighting ? lightingStampHash() : 0;
    if (!force) {
        PackStamp found;
        const bool have = readPackStamp(r.pack, found);
        if (comparePackStamp(have ? &found : nullptr, r.stamp).state == PackState::Current) {
            r.ok = r.upToDate = true;
            r.stamp = found;
            std::error_code ec;
            r.bytes = fs::file_size(r.pack, ec);
            return finish();
        }
    }

    // Every shader of the database once (the same microcode with the same inputs is one
    // translation), and with `scaled` the aware variant of every pixel shader beside it.
    p.phase = PackBuildProgress::kReading;
    Database db;
    if (!db.loadBytes(std::move(bytes), &r.error)) {
        r.error = options.database.string() + ": " + r.error;
        return finish();
    }
    std::vector<ShaderInfo> jobs;
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (const DatabaseEntry& e : db.entries()) {
        if (e.kind != 1 && e.kind != 2) continue;
        ParseResult parsed = db.parse(e);
        if (!parsed.ok) {
            r.parseFailures++;
            continue;
        }
        if (seen.insert({parsed.info.ucodeHash, translationInputHash(parsed.info)}).second) jobs.push_back(parsed.info);
        // The lighting options' water shaders with their reflection hook (both scales when scaled).
        if (options.lighting && parsed.info.kind == ShaderKind::Pixel) {
            const WaterReflectionHook hook = waterReflectionHookFor(parsed.info.ucodeHash);
            if (hook.enabled()) {
                ShaderInfo hooked = parsed.info;
                hooked.waterReflection = hook;
                if (seen.insert({hooked.ucodeHash, translationInputHash(hooked)}).second) jobs.push_back(hooked);
                if (options.scaled) {
                    hooked.renderScaleAware = true;
                    if (seen.insert({hooked.ucodeHash, translationInputHash(hooked)}).second) jobs.push_back(hooked);
                }
            }
        }
        if (options.scaled && parsed.info.kind == ShaderKind::Pixel) {
            ShaderInfo aware = std::move(parsed.info);
            aware.renderScaleAware = true;
            if (seen.insert({aware.ucodeHash, translationInputHash(aware)}).second) jobs.push_back(std::move(aware));
        }
    }
    if (jobs.empty()) {
        r.error = "the database has no shaders";
        return finish();
    }

    // Compile, one DXC instance per worker.
    p.total = uint32_t(jobs.size());
    if (options.motion)
        for (const auto& job : jobs) if (job.kind == ShaderKind::Vertex) ++p.total;
    p.done = 0;
    p.failed = 0;
    p.phase = PackBuildProgress::kCompiling;
    unsigned threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    threads = std::max(1u, std::min<unsigned>(threads, unsigned(jobs.size())));
    r.threads = threads;
    const uint32_t targets = options.backend == PackBackend::Dxil ? kBuildDxil : kBuildSpirv;
    std::atomic<size_t> next{0};
    std::mutex lock;
    std::vector<CompiledShader> built;
    built.reserve(jobs.size());
    std::string firstError;
    auto worker = [&] {
        if (options.lowPriority) lowerThisThreadsPriority();
        Compiler compiler;
        std::vector<CompiledShader> mine;
        while (!p.cancel) {
            const size_t i = next++;
            if (i >= jobs.size()) break;
            BuildResult b = compiler.ok() ? buildShader(jobs[i], compiler, false, targets) : BuildResult{};
            if (b.ok) {
                if (options.motion && jobs[i].kind == ShaderKind::Pixel) {
                    ShaderInfo source = jobs[i];
                    for (bool scaled : {false, true}) {
                        source.renderScaleAware = scaled;
                        b.shader.packAliases.emplace_back(source.ucodeHash ^
                            (jobs[i].renderScaleAware ? kScaledPackTag : kUnscaledPackTag),
                            translationInputHash(source));
                    }
                }
                mine.push_back(std::move(b.shader));
            } else {
                p.failed++;
                std::lock_guard<std::mutex> g(lock);
                if (firstError.empty()) {
                    char hash[32];
                    std::snprintf(hash, sizeof(hash), "%016llX", static_cast<unsigned long long>(jobs[i].ucodeHash));
                    firstError = std::string(hash) + " " + (compiler.ok() ? b.stage + ": " + b.error.substr(0, 300) : "DXC did not start");
                }
            }
            p.done++;
            if (jobs[i].kind == ShaderKind::Vertex && annTessellationShader(jobs[i].ucodeHash) && !p.cancel) {
                std::string error;
                if (!buildAnnTessellationShaders(jobs[i], compiler, targets, mine, error)) {
                    p.failed++;
                    std::lock_guard<std::mutex> g(lock);
                    if (firstError.empty()) firstError = "Ann tessellation: " + error.substr(0,300);
                }
            }
            if (options.motion && jobs[i].kind == ShaderKind::Vertex && !p.cancel) {
                BuildResult m = buildMotionShader(jobs[i], compiler, targets);
                if (m.ok) mine.push_back(std::move(m.shader));
                else {
                    p.failed++;
                    std::lock_guard<std::mutex> g(lock);
                    if (firstError.empty()) firstError = "motion: " + m.stage + ": " + m.error.substr(0, 300);
                }
                p.done++;
            }
        }
        std::lock_guard<std::mutex> g(lock);
        for (auto& s : mine) built.push_back(std::move(s));
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    r.compiled = uint32_t(built.size());
    r.failed = p.failed;
    if (p.cancel) {
        r.cancelled = true;
        r.error = "cancelled";
        return finish();
    }
    if (built.empty()) {
        r.error = "no shader compiled: " + firstError;
        return finish();
    }

    // Write under temporary names, then move into place: first the pack (with the old stamp
    // gone, so a reader never takes the new pack for the old one's build), then its stamp.
    p.phase = PackBuildProgress::kWriting;
    std::error_code ec;
    fs::create_directories(options.folder, ec);
    const std::string tag = ".partial-" + std::to_string(processId());
    fs::path packTemp = r.pack;
    packTemp += tag;
    if (!ShaderPack::write(packTemp, built, &r.error)) {
        fs::remove(packTemp, ec);
        return finish();
    }
    built.clear();
    built.shrink_to_fit();
    {
        ShaderPack check;
        if (!check.open(packTemp, &r.error)) {
            fs::remove(packTemp, ec);
            return finish();
        }
        r.stamp.shaders = uint32_t(check.size());
    }
    const fs::path stamp = stampPath(r.pack);
    fs::remove(stamp, ec);
    if (!moveInto(packTemp, r.pack, &r.error)) return finish();
    fs::path stampTemp = stamp;
    stampTemp += tag;
    if (!writeText(stampTemp, r.stamp.toText())) {
        r.error = "cannot write " + stampTemp.string();
        fs::remove(stampTemp, ec);
        return finish();
    }
    if (!moveInto(stampTemp, stamp, &r.error)) return finish();
    r.bytes = fs::file_size(r.pack, ec);
    r.ok = true;
    if (r.failed && !firstError.empty()) r.error = std::to_string(r.failed) + " shaders failed (first: " + firstError + ")";
    return finish();
}

}  // namespace kkshaders
