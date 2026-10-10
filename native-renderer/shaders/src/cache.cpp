#include "kkshaders/cache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <xxh3.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kkshaders {

namespace {

constexpr uint32_t kEntryMagic = 0x45534B4Bu;  // 'KKSE'
constexpr uint32_t kPackMagic = 0x50534B4Bu;   // 'KKSP'
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kEntryHeaderSize = 64;
constexpr size_t kPackHeaderSize = 32;
constexpr size_t kIndexRecordSize = 32;

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (i * 8));
}
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    put32(b, at, uint32_t(v));
    put32(b, at + 4, uint32_t(v >> 32));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32); }

std::string hex16(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
    return buf;
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

}  // namespace

// Entry: header {magic, version, translator hash (8), kind, pad, ucode hash (8), input hash (8),
// bindings size, dxil size, spirv size, hlsl size, payload XXH3 (8), pad (4)} then the payload.
std::vector<uint8_t> encodeEntry(const CompiledShader& s) {
    std::vector<uint8_t> bindings = serializeBindings(s.bindings);
    size_t payload = bindings.size() + s.dxil.size() + s.spirv.size() + s.hlsl.size();
    std::vector<uint8_t> b(kEntryHeaderSize + payload);
    size_t at = kEntryHeaderSize;
    std::memcpy(b.data() + at, bindings.data(), bindings.size());
    at += bindings.size();
    if (!s.dxil.empty()) std::memcpy(b.data() + at, s.dxil.data(), s.dxil.size());
    at += s.dxil.size();
    if (!s.spirv.empty()) std::memcpy(b.data() + at, s.spirv.data(), s.spirv.size());
    at += s.spirv.size();
    if (!s.hlsl.empty()) std::memcpy(b.data() + at, s.hlsl.data(), s.hlsl.size());
    put32(b, 0, kEntryMagic);
    put32(b, 4, kFormatVersion);
    put64(b, 8, translatorHash());
    put32(b, 16, uint32_t(s.kind));
    put64(b, 24, s.ucodeHash);
    put64(b, 32, s.inputHash);
    put32(b, 40, uint32_t(bindings.size()));
    put32(b, 44, uint32_t(s.dxil.size()));
    put32(b, 48, uint32_t(s.spirv.size()));
    put32(b, 52, uint32_t(s.hlsl.size()));
    put64(b, 56, XXH3_64bits(b.data() + kEntryHeaderSize, payload));
    return b;
}

bool decodeEntry(const uint8_t* p, size_t size, CompiledShader& s) {
    if (size < kEntryHeaderSize || get32(p) != kEntryMagic || get32(p + 4) != kFormatVersion) return false;
    if (get64(p + 8) != translatorHash()) return false;
    size_t nb = get32(p + 40), nd = get32(p + 44), ns = get32(p + 48), nh = get32(p + 52);
    size_t payload = nb + nd + ns + nh;
    if (kEntryHeaderSize + payload != size) return false;
    if (XXH3_64bits(p + kEntryHeaderSize, payload) != get64(p + 56)) return false;
    s.kind = ShaderKind(get32(p + 16));
    s.ucodeHash = get64(p + 24);
    s.inputHash = get64(p + 32);
    const uint8_t* at = p + kEntryHeaderSize;
    if (!deserializeBindings(at, nb, s.bindings)) return false;
    at += nb;
    s.dxil.assign(at, at + nd);
    at += nd;
    s.spirv.assign(at, at + ns);
    at += ns;
    s.hlsl.assign(reinterpret_cast<const char*>(at), nh);
    return true;
}

ShaderCache::ShaderCache(std::filesystem::path folder) : folder_(std::move(folder)) {}

std::filesystem::path ShaderCache::pathFor(uint64_t ucodeHash, uint64_t inputHash) const {
    return folder_ / (hex16(ucodeHash) + "-" + hex16(inputHash) + ".kksh");
}

bool ShaderCache::load(uint64_t ucodeHash, uint64_t inputHash, CompiledShader& out) const {
    std::vector<uint8_t> data;
    if (!readFile(pathFor(ucodeHash, inputHash), data)) return false;
    return decodeEntry(data.data(), data.size(), out) && out.ucodeHash == ucodeHash && out.inputHash == inputHash;
}

bool ShaderCache::store(const CompiledShader& shader) const {
    std::error_code ec;
    std::filesystem::create_directories(folder_, ec);
    std::vector<uint8_t> data = encodeEntry(shader);
    std::filesystem::path final = pathFor(shader.ucodeHash, shader.inputHash);
    std::filesystem::path temp = final;
    temp += ".tmp" + std::to_string(reinterpret_cast<uintptr_t>(&data));
    {
        std::ofstream f(temp, std::ios::binary);
        if (!f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) return false;
    }
    std::filesystem::rename(temp, final, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// Pack: header {magic, version, translator hash (8), count, pad, index offset (8)}, the entries,
// then the index sorted by (ucode hash, input hash): {ucode hash, input hash, offset, size, pad}.
struct ShaderPack::Impl {
    const uint8_t* base = nullptr;
    size_t size = 0;
    const uint8_t* index = nullptr;
    size_t count = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif
};

ShaderPack::ShaderPack() : impl_(std::make_unique<Impl>()) {}
ShaderPack::~ShaderPack() { close(); }

bool ShaderPack::write(const std::filesystem::path& path, const std::vector<CompiledShader>& shaders, std::string* error) {
    std::vector<std::pair<Key, size_t>> order;
    for (size_t i = 0; i < shaders.size(); i++) order.push_back({{shaders[i].ucodeHash, shaders[i].inputHash}, i});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first.ucodeHash != b.first.ucodeHash ? a.first.ucodeHash < b.first.ucodeHash : a.first.inputHash < b.first.inputHash;
    });
    std::vector<uint8_t> header(kPackHeaderSize, 0);
    std::vector<uint8_t> index;
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    f.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    uint64_t offset = kPackHeaderSize;
    size_t written = 0;
    for (size_t k = 0; k < order.size(); k++) {
        if (k > 0 && order[k].first.ucodeHash == order[k - 1].first.ucodeHash && order[k].first.inputHash == order[k - 1].first.inputHash)
            continue;  // duplicate
        std::vector<uint8_t> entry = encodeEntry(shaders[order[k].second]);
        while (offset % 16) {
            f.put(0);
            offset++;
        }
        f.write(reinterpret_cast<const char*>(entry.data()), std::streamsize(entry.size()));
        std::vector<uint8_t> rec(kIndexRecordSize, 0);
        put64(rec, 0, order[k].first.ucodeHash);
        put64(rec, 8, order[k].first.inputHash);
        put64(rec, 16, offset);
        put32(rec, 24, uint32_t(entry.size()));
        index.insert(index.end(), rec.begin(), rec.end());
        offset += entry.size();
        written++;
    }
    while (offset % 16) {
        f.put(0);
        offset++;
    }
    f.write(reinterpret_cast<const char*>(index.data()), std::streamsize(index.size()));
    put32(header, 0, kPackMagic);
    put32(header, 4, kFormatVersion);
    put64(header, 8, translatorHash());
    put32(header, 16, uint32_t(written));
    put64(header, 24, offset);
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    if (!f) {
        if (error) *error = "write failed: " + path.string();
        return false;
    }
    return true;
}

bool ShaderPack::open(const std::filesystem::path& path, std::string* error) {
    close();
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        close();
        return false;
    };
#ifdef _WIN32
    impl_->file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) return fail("cannot open " + path.string());
    LARGE_INTEGER size;
    GetFileSizeEx(impl_->file, &size);
    impl_->size = size_t(size.QuadPart);
    impl_->mapping = CreateFileMappingW(impl_->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!impl_->mapping) return fail("cannot map " + path.string());
    impl_->base = static_cast<const uint8_t*>(MapViewOfFile(impl_->mapping, FILE_MAP_READ, 0, 0, 0));
#else
    impl_->fd = ::open(path.c_str(), O_RDONLY);
    if (impl_->fd < 0) return fail("cannot open " + path.string());
    struct stat st;
    fstat(impl_->fd, &st);
    impl_->size = size_t(st.st_size);
    void* p = impl_->size ? mmap(nullptr, impl_->size, PROT_READ, MAP_PRIVATE, impl_->fd, 0) : MAP_FAILED;
    impl_->base = p == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(p);
#endif
    if (!impl_->base) return fail("cannot map " + path.string());
    const uint8_t* h = impl_->base;
    if (impl_->size < kPackHeaderSize || get32(h) != kPackMagic || get32(h + 4) != kFormatVersion)
        return fail(path.string() + " is not a shader pack");
    if (get64(h + 8) != translatorHash()) return fail(path.string() + " was built by another translator version");
    impl_->count = get32(h + 16);
    uint64_t indexOffset = get64(h + 24);
    if (indexOffset + impl_->count * kIndexRecordSize > impl_->size) return fail("shader pack index out of range");
    impl_->index = impl_->base + indexOffset;
    return true;
}

void ShaderPack::close() {
#ifdef _WIN32
    if (impl_->base) UnmapViewOfFile(impl_->base);
    if (impl_->mapping) CloseHandle(impl_->mapping);
    if (impl_->file != INVALID_HANDLE_VALUE) CloseHandle(impl_->file);
    impl_->mapping = nullptr;
    impl_->file = INVALID_HANDLE_VALUE;
#else
    if (impl_->base) munmap(const_cast<uint8_t*>(impl_->base), impl_->size);
    if (impl_->fd >= 0) ::close(impl_->fd);
    impl_->fd = -1;
#endif
    impl_->base = nullptr;
    impl_->index = nullptr;
    impl_->size = 0;
    impl_->count = 0;
}

size_t ShaderPack::size() const { return impl_->count; }

std::vector<ShaderPack::Key> ShaderPack::keys() const {
    std::vector<Key> keys;
    for (size_t i = 0; i < impl_->count; i++) {
        const uint8_t* r = impl_->index + i * kIndexRecordSize;
        keys.push_back({get64(r), get64(r + 8)});
    }
    return keys;
}

bool ShaderPack::find(uint64_t ucodeHash, uint64_t inputHash, CompiledShader& out) const {
    size_t lo = 0, hi = impl_->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint8_t* r = impl_->index + mid * kIndexRecordSize;
        uint64_t h = get64(r), i = get64(r + 8);
        if (h < ucodeHash || (h == ucodeHash && i < inputHash)) lo = mid + 1;
        else hi = mid;
    }
    for (size_t k = lo; k < impl_->count; k++) {
        const uint8_t* r = impl_->index + k * kIndexRecordSize;
        if (get64(r) != ucodeHash) break;
        if (inputHash != 0 && get64(r + 8) != inputHash) break;
        uint64_t offset = get64(r + 16);
        uint32_t size = get32(r + 24);
        if (offset + size > impl_->size) return false;
        return decodeEntry(impl_->base + offset, size, out);
    }
    return false;
}

BuildResult buildShader(const ShaderInfo& info, Compiler& compiler, bool keepHlsl) {
    BuildResult r;
    TranslateResult t = translate(info);
    if (!t.ok) {
        r.stage = "translate";
        r.error = t.error;
        return r;
    }
    r.shader.kind = info.kind;
    r.shader.ucodeHash = info.ucodeHash;
    r.shader.inputHash = translationInputHash(info);
    r.shader.bindings = std::move(t.bindings);
    CompileOutput dxil = compiler.compile(t.hlsl, info.kind, CompileTarget::Dxil);
    if (!dxil.ok) {
        r.stage = "dxil";
        r.error = dxil.messages;
        r.shader.hlsl = std::move(t.hlsl);
        return r;
    }
    CompileOutput spirv = compiler.compile(t.hlsl, info.kind, CompileTarget::Spirv);
    if (!spirv.ok) {
        r.stage = "spirv";
        r.error = spirv.messages;
        r.shader.hlsl = std::move(t.hlsl);
        return r;
    }
    r.shader.dxil = std::move(dxil.blob);
    r.shader.spirv = std::move(spirv.blob);
    if (keepHlsl) r.shader.hlsl = std::move(t.hlsl);
    r.ok = true;
    return r;
}

bool ShaderProvider::get(const ShaderInfo& info, Compiler& compiler, CompiledShader& out, std::string* error) const {
    uint64_t inputHash = translationInputHash(info);
    if (pack_ && pack_->find(info.ucodeHash, inputHash, out)) return true;
    if (cache_ && cache_->load(info.ucodeHash, inputHash, out)) return true;
    BuildResult r = buildShader(info, compiler, false);
    if (!r.ok) {
        if (error) *error = r.stage + ": " + r.error;
        return false;
    }
    if (cache_) cache_->store(r.shader);
    out = std::move(r.shader);
    return true;
}

}  // namespace kkshaders
