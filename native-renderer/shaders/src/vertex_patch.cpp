#include "kkshaders/vertex_patch.h"

#include <map>

namespace kkshaders {

namespace {

constexpr uint32_t bits(uint32_t v, uint32_t lo, uint32_t count) { return (v >> lo) & ((1u << count) - 1u); }
void setBits(uint32_t& v, uint32_t lo, uint32_t count, uint32_t value) {
    const uint32_t mask = ((1u << count) - 1u) << lo;
    v = (v & ~mask) | ((value << lo) & mask);
}

}  // namespace

VertexFetchFields readVertexFetch(std::span<const uint32_t> ucode, uint32_t address) {
    VertexFetchFields f;
    if (size_t(address) * 3 + 3 > ucode.size()) return f;
    const uint32_t w0 = ucode[address * 3], w1 = ucode[address * 3 + 1], w2 = ucode[address * 3 + 2];
    f.isFetch = bits(w0, 0, 5) == 0;
    f.mini = bits(w1, 30, 1) != 0;
    f.slot = bits(w0, 20, 5) * 3 + bits(w0, 25, 2);
    f.format = bits(w1, 16, 6);
    f.integer = bits(w1, 13, 1) != 0;
    f.isSigned = bits(w1, 14, 1) != 0;
    f.stride = bits(w2, 0, 8);
    int32_t offset = int32_t(bits(w2, 8, 23));
    if (offset & (1 << 22)) offset -= 1 << 23;
    f.offset = offset;
    int32_t exp = int32_t(bits(w1, 24, 6));
    if (exp & 0x20) exp -= 64;
    f.expAdjust = exp;
    f.dstSwizzle = bits(w1, 0, 12);
    return f;
}

VertexPatchResult patchVertexFetches(const ShaderInfo& info, std::span<const DeclElement> declaration,
                                     std::span<const uint32_t> strides, const VertexPatchOptions& options) {
    VertexPatchResult out;
    out.ucode = info.ucode;
    for (const FetchBinding& b : info.fetches) {
        if (size_t(b.address) * 3 + 3 > out.ucode.size()) continue;
        const DeclElement* element = nullptr;
        for (const DeclElement& e : declaration) {
            if (e.usage == b.usage && e.usageIndex == b.usageIndex) {
                element = &e;
                break;
            }
        }
        if (!element) {
            ++out.missing;
            continue;
        }
        uint32_t& w0 = out.ucode[b.address * 3];
        uint32_t& w1 = out.ucode[b.address * 3 + 1];
        uint32_t& w2 = out.ucode[b.address * 3 + 2];
        const bool mini = bits(w1, 30, 1) != 0;
        if (element->offset & 3) ++out.misaligned;
        setBits(w1, 16, 6, element->format);
        if (options.writeFormatFlags) {
            setBits(w1, 13, 1, element->integer ? 1 : 0);
            setBits(w1, 14, 1, element->isSigned ? 1 : 0);
        }
        if (options.clearExpAdjust) setBits(w1, 24, 6, 0);
        setBits(w2, 8, 23, uint32_t(element->offset / 4));
        if (!mini) {
            const uint32_t slot = 95u - (element->stream & 15u);
            setBits(w0, 20, 5, slot / 3);
            setBits(w0, 25, 2, slot % 3);
            const uint32_t stride = element->stream < strides.size() ? strides[element->stream] : 0;
            if (stride & 3) ++out.misaligned;
            setBits(w2, 0, 8, stride / 4);
        }
        ++out.patched;
    }
    return out;
}

uint64_t hashMicrocode(std::span<const uint32_t> dwords) {
    std::vector<uint8_t> bytes(dwords.size() * 4);
    for (size_t i = 0; i < dwords.size(); ++i) {
        bytes[i * 4] = uint8_t(dwords[i] >> 24);
        bytes[i * 4 + 1] = uint8_t(dwords[i] >> 16);
        bytes[i * 4 + 2] = uint8_t(dwords[i] >> 8);
        bytes[i * 4 + 3] = uint8_t(dwords[i]);
    }
    return hashBytes(bytes.data(), bytes.size());
}

}  // namespace kkshaders
