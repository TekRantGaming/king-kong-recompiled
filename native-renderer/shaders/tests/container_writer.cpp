#include "container_writer.h"

#include <cstring>

namespace ctest {

namespace {

struct Be {
    std::vector<uint8_t> b;
    size_t size() const { return b.size(); }
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) {
        u8(v >> 8);
        u8(v);
    }
    void u32(uint32_t v) {
        u16(v >> 16);
        u16(v);
    }
    void set32(size_t at, uint32_t v) {
        b[at] = uint8_t(v >> 24);
        b[at + 1] = uint8_t(v >> 16);
        b[at + 2] = uint8_t(v >> 8);
        b[at + 3] = uint8_t(v);
    }
    void str(const std::string& s) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    void align(size_t n) {
        while (b.size() % n) b.push_back(0);
    }
    void append(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
};

// A standard D3DXSHADER_CONSTANTTABLE, big-endian, offsets from its start.
std::vector<uint8_t> writeCtab(const Spec& spec) {
    Be t;
    const size_t n = spec.constants.size();
    const size_t infoOffset = 28, typeOffset = infoOffset + n * 20, stringsOffset = typeOffset + n * 16;
    t.u32(28);                 // size of the header
    t.u32(0);                  // creator (patched)
    t.u32(spec.vertex ? 0xFFFE0300u : 0xFFFF0300u);
    t.u32(uint32_t(n));
    t.u32(uint32_t(infoOffset));
    t.u32(0);                  // flags
    t.u32(0);                  // target (patched)
    std::vector<uint32_t> nameOffsets(n);
    Be strings;
    size_t creator = stringsOffset + strings.size();
    strings.str("kkshaders test");
    size_t target = stringsOffset + strings.size();
    strings.str(spec.vertex ? "vs_3_0" : "ps_3_0");
    for (size_t i = 0; i < n; i++) {
        nameOffsets[i] = uint32_t(stringsOffset + strings.size());
        strings.str(spec.constants[i].name);
    }
    for (size_t i = 0; i < n; i++) {
        const auto& c = spec.constants[i];
        t.u32(nameOffsets[i]);
        t.u16(c.registerSet);
        t.u16(c.registerIndex);
        t.u16(c.registerCount);
        t.u16(0);
        t.u32(uint32_t(typeOffset + i * 16));
        t.u32(0);
    }
    for (size_t i = 0; i < n; i++) {
        const auto& c = spec.constants[i];
        t.u16(c.typeClass);
        t.u16(c.type);
        t.u16(c.rows);
        t.u16(c.columns);
        t.u16(c.elements);
        t.u16(0);
        t.u32(0);
    }
    t.append(strings.b);
    t.align(4);
    t.set32(4, spec.strippedTable ? 0u : uint32_t(creator));
    t.set32(24, spec.strippedTable ? 0x5F465552u : uint32_t(target));  // "_FUR", as in the database
    return t.b;
}

}  // namespace

std::vector<uint8_t> writeContainer(const Spec& spec) {
    Be c;
    for (int i = 0; i < 6; i++) c.u32(0);  // header, patched below

    // Register block: {u64 dirty mask, u64, u32 list bytes, u32, list}.
    uint32_t blockOffset = 0;
    if (!spec.floatLiterals.empty() || !spec.loopLiterals.empty() || !spec.boolLiterals.empty()) {
        blockOffset = uint32_t(c.size());
        Be list;
        for (const auto& [reg, v] : spec.floatLiterals) {
            uint32_t absReg = reg + (spec.vertex ? 0 : 256);
            list.u16(768 + absReg * 16);
            list.u16(4);
            for (uint32_t x : v) list.u32(x);
        }
        for (const auto& [index, v] : spec.boolLiterals) {
            list.u16(8960 + index * 4);
            list.u16(1);
            list.u32(v);
        }
        for (const auto& [index, v] : spec.loopLiterals) {
            list.u16(8992 + index * 4);
            list.u16(1);
            list.u32(v);
        }
        list.u32(0);  // end of plain writes
        list.u32(0);  // no masked writes
        c.u32(0xFFFFFFFFu);
        c.u32(0);
        c.u32(0);
        c.u32(0);
        c.u32(uint32_t(list.size()));
        c.u32(0);
        c.append(list.b);
    }

    uint32_t ctabOffset = 0;
    if (!spec.noConstantTable) {
        ctabOffset = uint32_t(c.size());
        std::vector<uint8_t> ctab = writeCtab(spec);
        c.u32(uint32_t(ctab.size()));
        c.append(ctab);
    }

    // Binding table.
    uint32_t tableOffset = uint32_t(c.size());
    uint32_t nInterp = uint32_t(spec.interpolators.size());
    if (spec.vertex) {
        c.u32((nInterp ? nInterp - 1 : 0) << 20);  // SQ_PROGRAM_CNTL-like
        for (int i = 1; i < 6; i++) c.u32(0);
        c.u32(0);  // fetch list offset (words after word 10)
        c.u32(uint32_t(spec.fetches.size()));
        c.u32(nInterp + uint32_t(spec.extraBindingWords.size()));
        c.u32(0);
        for (const auto& f : spec.fetches) c.u32((f.address & 0xFFF) | (f.usage << 12) | (f.usageIndex << 16) | (f.classHint << 20));
        for (const auto& i : spec.interpolators) c.u32(i.usageIndex | (i.usage << 4) | (i.reg << 8) | (i.mask << 12));
        for (uint32_t w : spec.extraBindingWords) c.u32(w);
    } else {
        c.u32(spec.paramGen ? (1u << 18) : 0u);
        c.u32(nInterp << 8);
        for (int i = 2; i < 8; i++) c.u32(0);
        for (const auto& i : spec.interpolators) c.u32(i.usageIndex | (i.usage << 4) | (i.reg << 8) | (i.mask << 12));
    }
    uint32_t virtualSize = uint32_t(c.size());
    for (uint32_t v : spec.ucode) c.u32(v);

    c.set32(0, spec.vertex ? 0x102A0E01u : 0x102A0E00u);
    c.set32(4, virtualSize);
    c.set32(8, uint32_t(spec.ucode.size() * 4));
    c.set32(12, blockOffset);
    c.set32(16, ctabOffset);
    c.set32(20, tableOffset);
    return c.b;
}

std::vector<uint8_t> writeDatabase(const std::vector<std::pair<std::string, std::string>>& sources, const std::vector<DbEntry>& entries) {
    Be d;
    d.u32(0x32424453u);  // 'SDB2'
    d.u32(0);
    d.u32(uint32_t(sources.size()));
    d.u32(uint32_t(entries.size()));
    d.u32(24);           // source table
    d.u32(0);            // entry table (patched)
    for (const auto& [name, text] : sources) {
        std::string n = name;
        n.resize(64, '\0');
        d.b.insert(d.b.end(), n.begin(), n.end());
        d.u32(0);
        d.u32(2u);  // family mask: family 1
        d.u32(name.rfind("ps", 0) == 0 ? 2u : 1u);  // kind
        d.u32(uint32_t(text.size()));
        d.b.insert(d.b.end(), text.begin(), text.end());
    }
    d.align(4);
    std::vector<std::pair<uint32_t, uint32_t>> placed;
    for (const auto& e : entries) {
        d.align(4);
        uint32_t offset = uint32_t(d.size());
        if (e.kind == 1) {
            d.u32(uint32_t(e.container.size()));
            d.u32(uint32_t(e.container.size()));
            d.append(e.container);
            d.append(e.container);  // the unused B copy
        } else {
            d.append(e.container);
        }
        placed.push_back({offset, uint32_t(d.size()) - offset});
    }
    d.align(4);
    d.set32(20, uint32_t(d.size()));
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        d.u32(e.kind);
        d.u32(e.kind == 1 ? uint32_t(e.key >> 32) : 0);
        d.u32(e.kind == 1 ? uint32_t(e.key) : 0);
        d.u32(e.kind == 2 ? uint32_t(e.key >> 32) : 0);
        d.u32(e.kind == 2 ? uint32_t(e.key) : 0);
        d.u32(0);
        d.u32(0);
        d.u32(placed[i].first);
        d.u32(placed[i].second);
    }
    return d.b;
}

}  // namespace ctest
