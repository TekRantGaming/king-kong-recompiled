// kkshaders: the shader translator's command-line tool.
//
//   kkshaders info <container> [--raw-vs | --raw-ps]
//   kkshaders translate <container> [-o out.hlsl] [--raw-vs | --raw-ps]
//   kkshaders compile <container> --dxc <dir> [--out <dir>] [--spirv-val <exe>] [--raw-vs | --raw-ps]
//   kkshaders db-check <xeshaders.bin>
//   kkshaders db-build <xeshaders.bin> --dxc <dir> [--out <dir>] [--spirv-val <exe>] [--jobs N] [--limit N] [--both]
//   kkshaders db-structure <xeshaders.bin>
//   kkshaders xsh <file.xsh> [<file.xsh> ...] --dxc <dir> [--out <dir>] [--spirv-val <exe>] [--jobs N] [--database <xeshaders.bin>]
//   kkshaders pack-lookup <pack> [<hash> ...]
//
// --raw-vs / --raw-ps read the file as bare big-endian microcode (no container).
// Everything written under --out is derived from the game's files: keep it out of git.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "kkshaders/cache.h"
#include "kkshaders/compiler.h"
#include "kkshaders/container.h"
#include "kkshaders/translator.h"

using namespace kkshaders;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;

    bool has(const std::string& f) const { return flags.count(f) != 0; }
    std::string get(const std::string& o, const std::string& d = "") const {
        auto it = options.find(o);
        return it == options.end() ? d : it->second;
    }
};

Args parseArgs(int argc, char** argv, int first) {
    static const std::set<std::string> withValue = {"-o", "--out", "--dxc", "--spirv-val", "--jobs", "--limit", "--database"};
    Args a;
    for (int i = first; i < argc; i++) {
        std::string s = argv[i];
        if (withValue.count(s) && i + 1 < argc) a.options[s] = argv[++i];
        else if (s.size() > 1 && s[0] == '-') a.flags.insert(s);
        else a.positional.push_back(s);
    }
    return a;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

void writeFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), std::streamsize(size));
}

std::string hex16(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

const char* dimName(TextureDimension d) {
    static const char* names[] = {"1D", "2D", "3D", "Cube"};
    return names[uint32_t(d) & 3];
}

ParseResult loadShader(const Args& a, const std::string& path) {
    std::vector<uint8_t> data;
    if (!readFile(path, data)) {
        ParseResult r;
        r.error = "cannot read " + path;
        return r;
    }
    if (a.has("--raw-vs")) return parseMicrocode(ShaderKind::Vertex, data.data(), data.size());
    if (a.has("--raw-ps")) return parseMicrocode(ShaderKind::Pixel, data.data(), data.size());
    return parseContainer(data.data(), data.size());
}

void printInfo(const ShaderInfo& info) {
    std::printf("%s shader, microcode %u bytes, hash %s, input hash %s\n", info.kind == ShaderKind::Vertex ? "vertex" : "pixel",
                info.physicalSize, hex16(info.ucodeHash).c_str(), hex16(translationInputHash(info)).c_str());
    if (info.rawMicrocode) std::printf("  bare microcode\n");
    if (!info.target.empty()) std::printf("  target %s\n", info.target.c_str());
    static const char* sets[] = {"bool", "int4", "float4", "sampler"};
    for (const auto& c : info.constants)
        std::printf("  constant %-32s %-7s %u (%u)\n", c.name.c_str(), sets[uint32_t(c.registerSet) & 3], c.registerIndex, c.registerCount);
    for (const auto& l : info.literals)
        std::printf("  literal c%u = %08X %08X %08X %08X\n", l.registerIndex, l.value[0], l.value[1], l.value[2], l.value[3]);
    for (const auto& [i, v] : info.loopLiterals) std::printf("  literal i%u = %08X\n", i, v);
    for (const auto& [i, v] : info.boolLiterals) std::printf("  literal bool dword %u = %08X\n", i, v);
    for (const auto& f : info.fetches)
        std::printf("  vfetch @%u reads %s%u (class %u)\n", f.address, declUsageName(f.usage), f.usageIndex, f.classHint);
    for (const auto& i : info.interpolators)
        std::printf("  interpolator r%u %s%u mask %X\n", i.reg, declUsageName(i.usage), i.usageIndex, i.mask);
    if (info.readsPixelPosition) std::printf("  pixel position in r%u\n", info.pixelPositionRegister);
}

void printBindings(const ShaderBindings& b) {
    for (size_t i = 0; i < b.vertexBindings.size(); i++) {
        const auto& v = b.vertexBindings[i];
        if (v.raw) std::printf("  vertex binding %zu: fetch constant %u\n", i, v.fetchConstant);
        else std::printf("  vertex binding %zu: %s%u\n", i, declUsageName(v.usage), v.usageIndex);
    }
    for (size_t i = 0; i < b.samplers.size(); i++) {
        const auto& s = b.samplers[i];
        std::printf("  sampler %zu: slot %u %s filters mag %u min %u mip %u aniso %u\n", i, s.slot, dimName(s.dimension), s.magFilter,
                    s.minFilter, s.mipFilter, s.anisoFilter);
    }
    for (const auto& t : b.textures) std::printf("  texture slot %u %s\n", t.slot, dimName(t.dimension));
    if (b.kind == ShaderKind::Pixel) std::printf("  outputs %X\n", b.pixelOutputs);
    else std::printf("  interpolators %05X\n", b.interpolators);
    std::printf("  %u temporaries%s%s\n", b.tempRegisters, b.dynamicRegisters ? ", dynamic indexing" : "",
                b.generalControlFlow ? ", general control flow" : "");
    for (const auto& w : b.warnings) std::printf("  warning: %s\n", w.c_str());
}

bool setupDxc(const Args& a) {
    std::string error;
    if (!loadDxc(a.get("--dxc"), &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::printf("DXC %s\n", dxcVersion().c_str());
    return true;
}

// One shader through the whole pipeline with validation; returns the failing stage or "".
struct FullResult {
    std::string stage;
    std::string error;
    CompiledShader shader;
};

FullResult buildAndValidate(const ShaderInfo& info, Compiler& compiler, const std::string& spirvVal) {
    FullResult f;
    BuildResult r = buildShader(info, compiler, true);
    f.shader = std::move(r.shader);
    if (!r.ok) {
        f.stage = r.stage;
        f.error = r.error;
        return f;
    }
    if (!isDxilSigned(f.shader.dxil)) {
        f.stage = "dxil-signature";
        f.error = "DXIL is not signed by the validator";
        return f;
    }
    std::string messages;
    if (!compiler.validateDxil(f.shader.dxil, &messages)) {
        f.stage = "dxil-validate";
        f.error = messages;
        return f;
    }
    if (!spirvVal.empty() && !validateSpirvExternal(spirvVal, f.shader.spirv, &messages)) {
        f.stage = "spirv-val";
        f.error = messages;
        return f;
    }
    return f;
}

// A short, number-free form of an error for counting failures by cause.
std::string causeOf(const std::string& stage, const std::string& error) {
    std::string line = error.substr(0, error.find('\n'));
    // DXC messages start with "hlsl.hlsl:12:34: error: ..."
    size_t e = line.find("error:");
    if (e != std::string::npos) line = line.substr(e);
    line = std::regex_replace(line, std::regex("[0-9]+"), "N");
    line = std::regex_replace(line, std::regex("'[^']*'"), "'...'");
    if (line.size() > 120) line.resize(120);
    return stage + ": " + line;
}

int cmdInfo(const Args& a) {
    if (a.positional.empty()) return 2;
    ParseResult p = loadShader(a, a.positional[0]);
    if (!p.ok) {
        std::fprintf(stderr, "%s\n", p.error.c_str());
        return 1;
    }
    printInfo(p.info);
    TranslateResult t = translate(p.info);
    if (!t.ok) {
        std::printf("translation failed: %s\n", t.error.c_str());
        return 1;
    }
    printBindings(t.bindings);
    return 0;
}

int cmdTranslate(const Args& a) {
    if (a.positional.empty()) return 2;
    ParseResult p = loadShader(a, a.positional[0]);
    if (!p.ok) {
        std::fprintf(stderr, "%s\n", p.error.c_str());
        return 1;
    }
    TranslateResult t = translate(p.info);
    if (!t.ok) {
        std::fprintf(stderr, "translation failed: %s\n", t.error.c_str());
        return 1;
    }
    std::string out = a.get("-o");
    if (out.empty()) std::fwrite(t.hlsl.data(), 1, t.hlsl.size(), stdout);
    else writeFile(out, t.hlsl.data(), t.hlsl.size());
    return 0;
}

int cmdCompile(const Args& a) {
    if (a.positional.empty() || !setupDxc(a)) return 2;
    ParseResult p = loadShader(a, a.positional[0]);
    if (!p.ok) {
        std::fprintf(stderr, "%s\n", p.error.c_str());
        return 1;
    }
    Compiler compiler;
    FullResult f = buildAndValidate(p.info, compiler, a.get("--spirv-val"));
    fs::path out = a.get("--out", ".");
    std::string base = hex16(p.info.ucodeHash);
    if (!f.shader.hlsl.empty()) writeFile(out / (base + ".hlsl"), f.shader.hlsl.data(), f.shader.hlsl.size());
    if (!f.stage.empty()) {
        std::fprintf(stderr, "%s failed:\n%s\n", f.stage.c_str(), f.error.c_str());
        return 1;
    }
    writeFile(out / (base + ".dxil"), f.shader.dxil.data(), f.shader.dxil.size());
    writeFile(out / (base + ".spv"), f.shader.spirv.data(), f.shader.spirv.size());
    std::printf("ok: DXIL %zu bytes (signed), SPIR-V %zu bytes\n", f.shader.dxil.size(), f.shader.spirv.size());
    printBindings(f.shader.bindings);
    return 0;
}

int cmdDbCheck(const Args& a) {
    if (a.positional.empty()) return 2;
    Database db;
    std::string error;
    if (!db.load(a.positional[0], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::map<uint32_t, int> kinds;
    std::map<std::string, int> errors;
    int ok = 0, failed = 0, secondOk = 0;
    for (size_t i = 0; i < db.entries().size(); i++) {
        const auto& e = db.entries()[i];
        kinds[e.kind]++;
        if (e.kind != 1 && e.kind != 2) continue;
        auto c = db.container(e);
        ParseResult p = parseContainer(c);
        bool sizesAddUp = p.ok && size_t(p.info.virtualSize) + p.info.physicalSize == c.size();
        if (e.kind == 1) {
            auto b = db.secondContainer(e);
            sizesAddUp = sizesAddUp && 8 + c.size() + b.size() == db.raw(e).size();
            if (parseContainer(b).ok) secondOk++;
        }
        if (p.ok && sizesAddUp) {
            ok++;
        } else {
            failed++;
            errors[p.ok ? "sizes do not add up" : p.error]++;
            if (failed <= 20) std::printf("entry %zu (kind %u): %s\n", i, e.kind, p.ok ? "sizes do not add up" : p.error.c_str());
        }
    }
    std::printf("%zu entries, %zu HLSL sources\n", db.entries().size(), db.sources().size());
    for (const auto& [k, n] : kinds) std::printf("  kind %u: %d\n", k, n);
    std::printf("parsed %d, failed %d (second vertex containers parsed: %d)\n", ok, failed, secondOk);
    for (const auto& [e, n] : errors) std::printf("  %5d  %s\n", n, e.c_str());
    return failed ? 1 : 0;
}

struct Job {
    size_t entry;
    std::string label;
    ShaderInfo info;
    uint32_t family = 0;
};

int runJobs(std::vector<Job>& jobs, const Args& a, const fs::path& out, const std::string& title) {
    std::string spirvVal = a.get("--spirv-val");
    unsigned jobsCount = unsigned(std::stoul(a.get("--jobs", std::to_string(std::max(1u, std::thread::hardware_concurrency())))));
    std::atomic<size_t> next{0}, done{0};
    std::mutex lock;
    std::map<std::string, int> causes;
    std::map<std::string, std::pair<int, int>> byFamily;  // label prefix -> ok, total
    std::map<std::string, int> warnings;
    std::vector<CompiledShader> built;
    std::vector<std::string> failures;
    int general = 0, dynamic = 0;
    auto start = std::chrono::steady_clock::now();

    auto worker = [&] {
        Compiler compiler;
        for (;;) {
            size_t i = next++;
            if (i >= jobs.size()) break;
            Job& job = jobs[i];
            FullResult f = buildAndValidate(job.info, compiler, spirvVal);
            std::lock_guard<std::mutex> g(lock);
            auto& fam = byFamily[job.label.substr(0, job.label.find(' '))];
            fam.second++;
            if (f.stage.empty()) {
                fam.first++;
                general += f.shader.bindings.generalControlFlow;
                dynamic += f.shader.bindings.dynamicRegisters;
                for (const auto& w : f.shader.bindings.warnings) warnings[w]++;
                f.shader.hlsl.clear();
                built.push_back(std::move(f.shader));
            } else {
                causes[causeOf(f.stage, f.error)]++;
                failures.push_back(job.label + ": " + f.stage);
                std::string name = "failed/" + std::to_string(job.entry) + "-" + hex16(job.info.ucodeHash);
                if (!f.shader.hlsl.empty()) writeFile(out / (name + ".hlsl"), f.shader.hlsl.data(), f.shader.hlsl.size());
                std::string text = job.label + "\n" + f.stage + "\n" + f.error;
                writeFile(out / (name + ".txt"), text.data(), text.size());
            }
            size_t d = ++done;
            if (d % 250 == 0) std::printf("  %zu / %zu\n", d, jobs.size()), std::fflush(stdout);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobsCount; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::ostringstream report;
    report << title << "\n";
    report << "DXC " << dxcVersion() << "; spirv-val " << (spirvVal.empty() ? "not run" : spirvVal) << "\n";
    report << "built " << built.size() << " of " << jobs.size() << " (DXIL signed and validated, SPIR-V "
           << (spirvVal.empty() ? "validated by DXC" : "validated by DXC and spirv-val") << ") in " << seconds << " s\n";
    report << "general control flow: " << general << ", dynamic register indexing: " << dynamic << "\n";
    report << "\nby family:\n";
    for (const auto& [f, n] : byFamily) report << "  " << f << " " << n.first << " / " << n.second << "\n";
    if (!causes.empty()) {
        report << "\nfailures by cause:\n";
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto& [c, n] : causes) sorted.push_back({n, c});
        std::sort(sorted.rbegin(), sorted.rend());
        for (const auto& [n, c] : sorted) report << "  " << n << "  " << c << "\n";
    }
    if (!warnings.empty()) {
        report << "\nwarnings:\n";
        for (const auto& [w, n] : warnings) report << "  " << n << "  " << w << "\n";
    }
    std::string text = report.str();
    std::fwrite(text.data(), 1, text.size(), stdout);
    writeFile(out / "report.txt", text.data(), text.size());

    std::string error;
    fs::path packPath = out / "kkshaders.pack";
    if (!ShaderPack::write(packPath, built, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("pack: %s (%zu shaders)\n", packPath.string().c_str(), built.size());
    return built.size() == jobs.size() ? 0 : 1;
}

int cmdDbBuild(const Args& a) {
    if (a.positional.empty() || !setupDxc(a)) return 2;
    Database db;
    std::string error;
    if (!db.load(a.positional[0], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    fs::path out = a.get("--out", "kkshaders-out");
    size_t limit = size_t(std::stoull(a.get("--limit", "0")));
    std::vector<Job> jobs;
    int parseFailures = 0;
    for (size_t i = 0; i < db.entries().size(); i++) {
        const auto& e = db.entries()[i];
        if (e.kind != 1 && e.kind != 2) continue;
        ShaderKind kind = e.kind == 1 ? ShaderKind::Vertex : ShaderKind::Pixel;
        uint32_t family = Database::familyIndex(e.kind == 1 ? e.vertexKey : e.pixelKey);
        std::string label = db.familyName(kind, family) + " entry " + std::to_string(i);
        std::vector<std::span<const uint8_t>> containers = {db.container(e)};
        if (a.has("--both") && e.kind == 1) containers.push_back(db.secondContainer(e));
        for (auto c : containers) {
            ParseResult p = parseContainer(c);
            if (!p.ok) {
                std::printf("entry %zu: parse error: %s\n", i, p.error.c_str());
                parseFailures++;
                continue;
            }
            jobs.push_back({i, label, std::move(p.info), family});
        }
        if (limit && jobs.size() >= limit) break;
    }
    std::printf("%zu shaders to build (%d parse failures)\n", jobs.size(), parseFailures);
    int rc = runJobs(jobs, a, out, "xeshaders.bin: " + a.positional[0]);
    return parseFailures ? 1 : rc;
}

// Rough structural check against the shipped HLSL sources: every constant and sampler a
// family's shaders declare must be named in the family's source, and the pixel outputs used.
int cmdDbStructure(const Args& a) {
    if (a.positional.empty()) return 2;
    Database db;
    std::string error;
    if (!db.load(a.positional[0], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    struct Family {
        std::set<std::string> names;
        std::set<std::string> samplers;
        uint32_t outputs = 0;
        int shaders = 0;
    };
    std::map<std::string, Family> families;
    for (const auto& e : db.entries()) {
        if (e.kind != 1 && e.kind != 2) continue;
        ShaderKind kind = e.kind == 1 ? ShaderKind::Vertex : ShaderKind::Pixel;
        std::string name = db.familyName(kind, Database::familyIndex(e.kind == 1 ? e.vertexKey : e.pixelKey));
        ParseResult p = parseContainer(db.container(e));
        if (!p.ok) continue;
        Family& f = families[name];
        f.shaders++;
        for (const auto& c : p.info.constants) (c.registerSet == RegisterSet::Sampler ? f.samplers : f.names).insert(c.name);
        if (kind == ShaderKind::Pixel) {
            TranslateResult t = translate(p.info);
            if (t.ok) f.outputs |= t.bindings.pixelOutputs;
        }
    }
    int missing = 0;
    for (const auto& [name, f] : families) {
        const DatabaseSource* source = nullptr;
        for (const auto& s : db.sources()) {
            std::string n = s.name.substr(0, s.name.rfind('.'));
            if (n == name) source = &s;
        }
        std::printf("%s: %d shaders, %zu constants, %zu samplers, pixel outputs %X%s\n", name.c_str(), f.shaders, f.names.size(),
                    f.samplers.size(), f.outputs, source ? "" : " (no source of that name)");
        if (!source) continue;
        auto check = [&](const std::string& n) {
            std::regex word("\\b" + std::regex_replace(n, std::regex("[\\[\\]\\.$]"), "\\$&") + "\\b");
            if (!std::regex_search(source->text, word)) {
                std::printf("  not in %s: %s\n", source->name.c_str(), n.c_str());
                missing++;
            }
        };
        for (const auto& n : f.names) check(n);
        for (const auto& n : f.samplers) check(n);
    }
    std::printf("%d names not found in the sources\n", missing);
    return missing ? 1 : 0;
}

int cmdXsh(const Args& a) {
    if (a.positional.empty() || !setupDxc(a)) return 2;
    std::set<uint64_t> databaseHashes;
    if (!a.get("--database").empty()) {
        Database db;
        std::string error;
        if (!db.load(a.get("--database"), &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        for (const auto& e : db.entries()) {
            if (e.kind != 1 && e.kind != 2) continue;
            ParseResult p = parseContainer(db.container(e));
            if (p.ok) databaseHashes.insert(p.info.ucodeHash);
        }
    }
    std::vector<Job> jobs;
    std::set<uint64_t> seen;
    int inDatabase = 0, badHash = 0;
    for (const auto& path : a.positional) {
        std::vector<uint8_t> d;
        if (!readFile(path, d) || d.size() < 8 || std::memcmp(d.data(), "XESH", 4) != 0) {
            std::fprintf(stderr, "%s: not a shader storage file\n", path.c_str());
            return 1;
        }
        size_t at = 8;
        while (at + 12 <= d.size()) {
            uint64_t hash = 0;
            uint32_t word = 0;
            std::memcpy(&hash, d.data() + at, 8);
            std::memcpy(&word, d.data() + at + 8, 4);
            uint32_t count = word & 0x7FFFFFFFu;
            if (at + 12 + size_t(count) * 4 > d.size()) break;
            const uint8_t* ucode = d.data() + at + 12;
            at += 12 + size_t(count) * 4;
            if (!seen.insert(hash).second) continue;
            ParseResult p = parseMicrocode((word >> 31) ? ShaderKind::Pixel : ShaderKind::Vertex, ucode, size_t(count) * 4);
            if (!p.ok) continue;
            if (p.info.ucodeHash != hash) {
                badHash++;
                continue;
            }
            if (databaseHashes.count(hash)) {
                inDatabase++;
                if (!a.has("--all")) continue;
            }
            std::string label = std::string(p.info.kind == ShaderKind::Pixel ? "pixel" : "vertex") + " " + hex16(hash);
            jobs.push_back({jobs.size(), label, std::move(p.info), 0});
        }
    }
    std::printf("%zu records to build (%d also in the database%s, %d with a wrong hash)\n", jobs.size(), inDatabase,
                a.has("--all") ? ", built too" : ", skipped", badHash);
    return runJobs(jobs, a, a.get("--out", "kkshaders-xsh-out"), "shader cache records");
}

int cmdPackLookup(const Args& a) {
    if (a.positional.empty()) return 2;
    ShaderPack pack;
    std::string error;
    auto t0 = std::chrono::steady_clock::now();
    if (!pack.open(a.positional[0], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    double openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::vector<ShaderPack::Key> keys = pack.keys();
    if (a.positional.size() > 1) {
        keys.clear();
        for (size_t i = 1; i < a.positional.size(); i++) keys.push_back({std::stoull(a.positional[i], nullptr, 16), 0});
    }
    double worst = 0, total = 0;
    size_t found = 0;
    for (const auto& k : keys) {
        CompiledShader s;
        auto t = std::chrono::steady_clock::now();
        bool ok = pack.find(k.ucodeHash, k.inputHash, s);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        worst = std::max(worst, ms);
        total += ms;
        found += ok;
    }
    std::printf("open %.3f ms; %zu / %zu found; lookup + decode: average %.4f ms, worst %.4f ms\n", openMs, found, keys.size(),
                keys.empty() ? 0.0 : total / double(keys.size()), worst);
    return found == keys.size() && worst < 1.0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: kkshaders info|translate|compile|db-check|db-build|db-structure|xsh|pack-lookup ...\n"
                     "(see the comment at the top of tool/main.cpp)\n");
        return 2;
    }
    std::string command = argv[1];
    Args a = parseArgs(argc, argv, 2);
    if (command == "info") return cmdInfo(a);
    if (command == "translate") return cmdTranslate(a);
    if (command == "compile") return cmdCompile(a);
    if (command == "db-check") return cmdDbCheck(a);
    if (command == "db-build") return cmdDbBuild(a);
    if (command == "db-structure") return cmdDbStructure(a);
    if (command == "xsh") return cmdXsh(a);
    if (command == "pack-lookup") return cmdPackLookup(a);
    std::fprintf(stderr, "unknown command %s\n", command.c_str());
    return 2;
}
