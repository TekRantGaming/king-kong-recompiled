// kkshaders: the shader translator's command-line tool.
//
//   kkshaders info <container> [--raw-vs | --raw-ps]
//   kkshaders translate <container> [-o out.hlsl] [--raw-vs | --raw-ps]
//   kkshaders compile <container> --dxc <dir> [--out <dir>] [--spirv-val <exe>] [--raw-vs | --raw-ps]
//   kkshaders db-check <xeshaders.bin>
//   kkshaders db-build <xeshaders.bin> --dxc <dir> [--out <dir>] [--spirv-val <exe>] [--jobs N] [--limit N] [--both] [--image <image.bin>]
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
#include <cctype>
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
    static const std::set<std::string> withValue = {"-o", "--out", "--dxc", "--spirv-val", "--jobs", "--limit", "--database", "--image"};
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
    {
        uint64_t dxil[2] = {}, spirv[2] = {}, count[2] = {};
        for (const auto& s : built) {
            int k = s.kind == ShaderKind::Pixel ? 1 : 0;
            dxil[k] += s.dxil.size();
            spirv[k] += s.spirv.size();
            count[k]++;
        }
        for (int k = 0; k < 2; k++)
            if (count[k])
                report << (k ? "pixel" : "vertex") << " shaders: " << count[k] << ", DXIL " << dxil[k] / 1024 << " KB (average "
                       << dxil[k] / count[k] << " bytes), SPIR-V " << spirv[k] / 1024 << " KB (average " << spirv[k] / count[k]
                       << " bytes)\n";
    }
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
    {
        ShaderPack check;
        size_t unique = check.open(packPath, &error) ? check.size() : 0;
        std::printf("pack: %s (%zu shaders built, %zu distinct by microcode and inputs, %llu bytes)\n", packPath.string().c_str(),
                    built.size(), unique, static_cast<unsigned long long>(fs::file_size(packPath)));
    }
    // --split: also one pack per backend (a player only needs the one their renderer uses).
    if (a.has("--split")) {
        for (int backend = 0; backend < 2; backend++) {
            std::vector<CompiledShader> one = built;
            for (auto& s : one) (backend == 0 ? s.spirv : s.dxil).clear();
            fs::path p = out / (backend == 0 ? "kkshaders-dxil.pack" : "kkshaders-spirv.pack");
            if (!ShaderPack::write(p, one, &error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("pack: %s (%llu bytes)\n", p.string().c_str(), static_cast<unsigned long long>(fs::file_size(p)));
        }
    }
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
        for (size_t k = 0; k < containers.size(); k++) {
            ParseResult p = k == 0 ? db.parse(e) : parseContainer(containers[k]);
            if (!p.ok) {
                std::printf("entry %zu: parse error: %s\n", i, p.error.c_str());
                parseFailures++;
                continue;
            }
            jobs.push_back({i, label, std::move(p.info), family});
        }
        if (limit && jobs.size() >= limit) break;
    }
    // The D3D library's own shaders live in the game image (kk/image.bin): find the containers
    // there too (magic, sane sizes, and a clean parse), once per microcode.
    if (!a.get("--image").empty()) {
        std::vector<uint8_t> image;
        if (!readFile(a.get("--image"), image)) {
            std::fprintf(stderr, "cannot read %s\n", a.get("--image").c_str());
            return 1;
        }
        std::set<uint64_t> have;
        for (const auto& j : jobs) have.insert(j.info.ucodeHash);
        int found = 0;
        for (size_t o = 0; o + 24 <= image.size(); o += 4) {
            if (image[o] != 0x10 || image[o + 1] != 0x2A || image[o + 2] != 0x0E || image[o + 3] > 1) continue;
            ParseResult p = parseContainer(image.data() + o, std::min<size_t>(image.size() - o, 0x40000));
            if (!p.ok || !have.insert(p.info.ucodeHash).second) continue;
            char label[64];
            std::snprintf(label, sizeof(label), "image 0x%08zX", size_t(0x82000000u) + o);
            jobs.push_back({o, label, std::move(p.info), 0});
            found++;
        }
        std::printf("%d shaders from the game image\n", found);
    }
    std::printf("%zu shaders to build (%d parse failures)\n", jobs.size(), parseFailures);
    int rc = runJobs(jobs, a, out, "xeshaders.bin: " + a.positional[0]);
    return parseFailures ? 1 : rc;
}

// Structural diff of the translated HLSL against the HLSL sources shipped in the database.
// Per family (the source file and everything it includes) and per shader:
//  - constants: every name of the shader's constant table is declared in the source; every
//    float, bool and loop constant the translated code reads lies in a range of the constant
//    table (or is one of the shader's literal constants, which must not overlap the table);
//  - textures: every texture fetch constant the translated code samples is a sampler register
//    of the constant table, with the dimension of its declared sampler type;
//  - outputs: the pixel outputs written are declared by the source (COLORn, arrays, DEPTH);
//  - interpolators: the vertex outputs and pixel inputs (semantics from the container) are
//    semantics the source declares.
std::string lowerName(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string expandSource(const Database& db, const DatabaseSource& source, std::set<std::string>& visited) {
    if (!visited.insert(lowerName(source.name)).second) return "";
    std::string text = source.text;
    static const std::regex include("^[ \\t]*#include[ \\t]*\"([^\"]+)\"");
    std::string extra;
    std::istringstream lines(source.text);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (!std::regex_search(line, m, include)) continue;  // commented-out includes do not match
        std::string want = lowerName(m[1].str());
        for (const auto& s : db.sources())
            if (lowerName(s.name) == want) extra += "\n" + expandSource(db, s, visited);
    }
    return text + extra;
}

// Semantics declared in a text: name -> number of registers (arrays count).
std::set<std::string> declaredSemantics(const std::string& text, const char* base) {
    std::set<std::string> out;
    std::regex decl(std::string("(\\[\\s*(\\d+)\\s*\\])?\\s*:\\s*") + base + "(\\d*)(_centroid)?\\b", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), decl), end; it != end; ++it) {
        uint32_t count = (*it)[2].matched ? uint32_t(std::stoul((*it)[2].str())) : 1;
        uint32_t first = (*it)[3].str().empty() ? 0 : uint32_t(std::stoul((*it)[3].str()));
        for (uint32_t k = 0; k < count; k++) out.insert(std::string(base) + std::to_string(first + k));
    }
    return out;
}

int cmdDbStructure(const Args& a) {
    if (a.positional.empty()) return 2;
    Database db;
    std::string error;
    if (!db.load(a.positional[0], &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    struct Family {
        const DatabaseSource* source = nullptr;
        std::string text;
        std::string variantText;          // sources that include this one
        std::set<std::string> semantics;  // TEXCOORDn / COLORn declared anywhere
        std::set<uint32_t> outputs;       // pixel: colour targets declared as outputs; 4 = depth
        int shaders = 0, translated = 0, clean = 0;
        uint32_t outputsUsed = 0;
        std::map<std::string, int> problems;
        std::set<std::string> names, samplers;
    };
    std::map<std::string, Family> families;
    std::map<std::string, int> features;  // how many shaders use each feature
    std::ostringstream detail;
    for (size_t index = 0; index < db.entries().size(); index++) {
        const auto& e = db.entries()[index];
        if (e.kind != 1 && e.kind != 2) continue;
        ShaderKind kind = e.kind == 1 ? ShaderKind::Vertex : ShaderKind::Pixel;
        bool pixel = kind == ShaderKind::Pixel;
        std::string name = db.familyName(kind, Database::familyIndex(pixel ? e.pixelKey : e.vertexKey));
        Family& f = families[name];
        if (!f.source) {
            for (const auto& s : db.sources())
                if (lowerName(s.name) == name + ".hlsl") f.source = &s;
            if (f.source) {
                std::set<std::string> visited;
                f.text = expandSource(db, *f.source, visited);
                for (const char* b : {"TEXCOORD", "COLOR"})
                    for (const auto& s : declaredSemantics(f.text, b)) f.semantics.insert(s);
                if (pixel) {
                    // Return semantics of functions, and members of output structs.
                    static const std::regex ret("\\)\\s*:\\s*COLOR(\\d*)\\b", std::regex::icase);
                    for (std::sregex_iterator it(f.text.begin(), f.text.end(), ret), end; it != end; ++it)
                        f.outputs.insert((*it)[1].str().empty() ? 0u : uint32_t(std::stoul((*it)[1].str())));
                    static const std::regex outStruct("struct\\s+PS_?OUT\\w*\\s*\\{([^}]*)\\}", std::regex::icase);
                    for (std::sregex_iterator it(f.text.begin(), f.text.end(), outStruct), end; it != end; ++it) {
                        std::string body = (*it)[1].str();
                        for (const auto& s : declaredSemantics(body, "COLOR")) f.outputs.insert(uint32_t(std::stoul(s.substr(5))));
                        if (std::regex_search(body, std::regex(":\\s*DEPTH", std::regex::icase))) f.outputs.insert(4);
                    }
                    // Entry points that return a float4 without a semantic (psocean, pssprite,
                    // the shadow blur and composite passes) get COLOR0 from the compiler.
                    if (f.outputs.empty()) f.outputs.insert(0);
                }
                // Variants: sources that include this one (vsspg22 and vssymmetry include
                // vsgeneric, psspg2 includes psgeneric) are compiled under its family keys too.
                for (const auto& s : db.sources()) {
                    if (&s == f.source) continue;
                    std::set<std::string> v2;
                    std::string other = expandSource(db, s, v2);
                    if (v2.count(lowerName(f.source->name))) f.variantText += "\n" + other;
                }
            }
        }
        f.shaders++;
        ParseResult p = db.parse(e);
        if (!p.ok) {
            f.problems["parse error"]++;
            continue;
        }
        const ShaderInfo& info = p.info;
        TranslateResult t = translate(info);
        if (!t.ok) {
            f.problems["translation failed"]++;
            continue;
        }
        f.translated++;
        std::vector<std::string> found;
        auto problem = [&](const std::string& kindOfProblem, const std::string& what) {
            f.problems[kindOfProblem]++;
            found.push_back(kindOfProblem + ": " + what);
        };

        // Names.
        for (const auto& c : info.constants) {
            (c.registerSet == RegisterSet::Sampler ? f.samplers : f.names).insert(c.name);
            if (f.source) {
                std::regex word("\\b" + std::regex_replace(c.name, std::regex("[\\[\\]\\.$]"), "\\$&") + "\\b");
                if (!std::regex_search(f.text, word)) {
                    if (std::regex_search(f.variantText, word)) f.problems["(names found only in a source that includes this one)"]++;
                    else problem("constant name not in the source", c.name);
                }
            }
        }
        // Register ranges of the constant table.
        auto inTable = [&](RegisterSet set, uint32_t reg) {
            for (const auto& c : info.constants)
                if (c.registerSet == set && reg >= c.registerIndex && reg < uint32_t(c.registerIndex) + c.registerCount) return &c;
            return static_cast<const ConstantInfo*>(nullptr);
        };
        std::set<uint32_t> literalRegs;
        for (const auto& l : info.literals) {
            literalRegs.insert(l.registerIndex);
            if (const ConstantInfo* c = inTable(RegisterSet::Float4, l.registerIndex))
                problem("literal constant overlaps the constant table", "c" + std::to_string(l.registerIndex) + " " + c->name);
        }
        // The translated code only (the prelude declares the buffers).
        const std::string h = t.hlsl.substr(std::min(t.hlsl.size(), t.hlsl.find("void main(")));
        auto each = [&](const char* pattern, auto fn) {
            std::regex re(pattern);
            for (std::sregex_iterator it(h.begin(), h.end(), re), end; it != end; ++it) fn(*it);
        };
        std::set<std::string> seen;
        each(pixel ? "kk_PC\\[(\\d+)\\]" : "kk_VC\\[(\\d+)\\]", [&](const std::smatch& m) {
            uint32_t r = uint32_t(std::stoul(m[1].str()));
            if (!inTable(RegisterSet::Float4, r) && seen.insert("c" + m[1].str()).second)
                problem("float constant read outside the constant table", "c" + m[1].str());
        });
        each(pixel ? "(?:kk_PSConst|kkConstRel)\\((\\d+) \\+" : "(?:kk_VSConst|kkConstRel)\\((\\d+) \\+", [&](const std::smatch& m) {
            uint32_t r = uint32_t(std::stoul(m[1].str()));
            if (!inTable(RegisterSet::Float4, r) && !literalRegs.count(r) && seen.insert("rel c" + m[1].str()).second)
                problem("relative float constant base outside the constant table", "c" + m[1].str());
        });
        each("kk_BoolConst\\((\\d+)\\)", [&](const std::smatch& m) {
            uint32_t r = uint32_t(std::stoul(m[1].str()));
            uint32_t rel = pixel ? r - 128 : r;
            if ((pixel && r < 128) || (!pixel && r >= 128) || !inTable(RegisterSet::Bool, rel)) {
                if (seen.insert("b" + m[1].str()).second) problem("bool constant outside the constant table", "b" + m[1].str());
            }
        });
        each("kk_LoopConst\\((\\d+)\\)", [&](const std::smatch& m) {
            uint32_t r = uint32_t(std::stoul(m[1].str()));
            uint32_t rel = pixel ? r - 16 : r;
            if ((pixel && r < 16) || (!pixel && r >= 16) || !inTable(RegisterSet::Int4, rel)) {
                if (seen.insert("i" + m[1].str()).second) problem("loop constant outside the constant table", "i" + m[1].str());
            }
        });
        for (const auto& tex : t.bindings.textures) {
            const ConstantInfo* c = inTable(RegisterSet::Sampler, tex.slot);
            if (!c) {
                problem("texture slot without a sampler in the constant table", "s" + std::to_string(tex.slot));
                continue;
            }
            // D3DXPT_SAMPLER 10 (any), 1D 11, 2D 12, 3D 13, CUBE 14.
            static const int want[] = {11, 12, 13, 14};
            // An array's table entry has one type for all its samplers: check single ones only.
            if (c->registerCount == 1 && c->type >= 11 && c->type <= 14 && c->type != want[uint32_t(tex.dimension) & 3] &&
                !(c->type == 12 && tex.dimension == TextureDimension::Tex1D))
                problem("texture dimension differs from the sampler type", c->name + " s" + std::to_string(tex.slot));
        }
        // Outputs and interpolators.
        if (pixel) {
            f.outputsUsed |= t.bindings.pixelOutputs;
            if (f.source) {
                for (uint32_t k = 0; k < 5; k++)
                    if ((t.bindings.pixelOutputs >> k) & 1 && !f.outputs.count(k))
                        problem("pixel output not declared by the source", k == 4 ? "DEPTH" : "COLOR" + std::to_string(k));
            }
        }
        if (f.source) {
            for (const auto& i : info.interpolators) {
                std::string sem = std::string(declUsageName(i.usage)) + std::to_string(i.usageIndex);
                if (!f.semantics.count(sem)) problem(pixel ? "pixel input semantic not in the source" : "vertex output semantic not in the source", sem);
            }
        }
        {
            auto uses = [&](const char* what, bool yes) { features[what] += yes ? 1 : 0; };
            uses("bool constants", h.find("kk_BoolConst(") != std::string::npos);
            uses("loop constants (or loop literals)", h.find("[loop] for (uint kkIt") != std::string::npos);
            uses("relative constant addressing", h.find("Const(") != std::string::npos);
            uses("predicated instructions", h.find("p0)") != std::string::npos);
            uses("kill / discard", h.find("discard") != std::string::npos);
            uses("cube textures", h.find("kk_TexCube[") != std::string::npos);
            uses("3D textures", h.find("kk_Tex3D[") != std::string::npos);
            uses("texture LOD / gradients", h.find("SampleLevel(") != std::string::npos || h.find("SampleGrad(") != std::string::npos);
            uses("pixel position (VPOS)", pixel && info.readsPixelPosition);
            uses("depth output", (t.bindings.pixelOutputs & 16) != 0);
            uses("general control flow", t.bindings.generalControlFlow);
            uses("literal float constants", !info.literals.empty());
            uses("vertex fetches hoisted into one decode loop", h.find("kkIn[") != std::string::npos);
            uses("vertex fetches decoded in place", h.find("kk_FetchElement(") != std::string::npos && h.find("kkIn[") == std::string::npos);
        }
        if (found.empty()) f.clean++;
        else {
            detail << name << " entry " << index << " (" << hex16(info.ucodeHash) << "):\n";
            for (const auto& s : found) detail << "  " << s << "\n";
        }
    }

    std::ostringstream report;
    int totalShaders = 0, totalClean = 0;
    report << "Structural diff against the HLSL sources of " << a.positional[0] << "\n\n";
    for (const auto& [name, f] : families) {
        totalShaders += f.shaders;
        totalClean += f.clean;
        report << name << ": " << f.shaders << " shaders, " << f.translated << " translated, " << f.clean << " clean; "
               << f.names.size() << " constant names, " << f.samplers.size() << " sampler names";
        if (!f.source) report << " (no source of that name)";
        if (name.rfind("ps", 0) == 0) {
            report << "; outputs written";
            for (uint32_t k = 0; k < 5; k++)
                if ((f.outputsUsed >> k) & 1) report << (k == 4 ? " DEPTH" : " COLOR" + std::to_string(k));
            report << ", declared";
            for (uint32_t k : f.outputs) report << (k == 4 ? " DEPTH" : " COLOR" + std::to_string(k));
        }
        report << "\n";
        for (const auto& [p, n] : f.problems) report << "    " << n << "  " << p << "\n";
    }
    report << "\nfeatures (number of shaders):\n";
    for (const auto& [what, n] : features) report << "  " << n << "  " << what << "\n";
    report << "\n" << totalClean << " of " << totalShaders << " shaders match their source structurally\n";
    std::string text = report.str();
    std::fwrite(text.data(), 1, text.size(), stdout);
    if (!a.get("--out").empty()) {
        fs::path out = a.get("--out");
        writeFile(out / "structure.txt", text.data(), text.size());
        std::string d = detail.str();
        writeFile(out / "structure-detail.txt", d.data(), d.size());
    }
    // The differences are a report, not an error (the synthetic test database cannot match);
    // --strict makes them one.
    return a.has("--strict") && totalClean != totalShaders ? 1 : 0;
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
            ParseResult p = db.parse(e);
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

