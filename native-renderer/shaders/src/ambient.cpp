// The ambient-only AO's shader list: see kkshaders/ambient.h.

#include "kkshaders/ambient.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace kkshaders {

namespace {

bool parseNumber(const std::string& s, uint32_t max, uint32_t& out) {
    if (s.empty() || s.size() > 5) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = uint32_t(std::strtoul(s.c_str(), nullptr, 10));
    return out <= max;
}

bool parseMask(const std::string& s, uint8_t& out) {
    if (s.empty() || s.size() > 4) return false;
    uint8_t m = 0;
    for (char c : s) {
        switch (std::tolower(static_cast<unsigned char>(c))) {
            case 'r': case 'x': m |= 1; break;
            case 'g': case 'y': m |= 2; break;
            case 'b': case 'z': m |= 4; break;
            case 'a': case 'w': m |= 8; break;
            default: return false;
        }
    }
    out = m;
    return true;
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

}  // namespace

bool parseAmbientTerm(std::string_view text, AmbientTerm& out, std::string* error) {
    std::istringstream words{std::string(text)};
    std::vector<std::string> w;
    for (std::string s; words >> s;) w.push_back(s);
    if (w.empty()) return fail(error, "no term");
    AmbientTerm t;
    size_t used = 2;
    uint32_t n = 0;
    if (w[0] == "output") {
        if (w.size() < 2 || !parseNumber(w[1], 3, n)) return fail(error, "output needs a target 0-3");
        t.kind = AmbientTerm::Kind::Output;
    } else if (w[0] == "constant") {
        if (w.size() < 2 || w[1].size() < 2 || w[1][0] != 'c' || !parseNumber(w[1].substr(1), 255, n))
            return fail(error, "constant needs a register c0-c255");
        t.kind = AmbientTerm::Kind::Constant;
    } else if (w[0] == "interpolator") {
        if (w.size() < 2) return fail(error, "interpolator needs colorN or texcoordN");
        const std::string& s = w[1];
        if (s.rfind("color", 0) == 0 && parseNumber(s.substr(5), 1, n)) {
            t.usage = 10;
        } else if (s.rfind("texcoord", 0) == 0 && parseNumber(s.substr(8), 15, n)) {
            t.usage = 5;
        } else {
            return fail(error, "interpolator needs color0-1 or texcoord0-15");
        }
        t.kind = AmbientTerm::Kind::Interpolator;
    } else if (w[0] == "fetch") {
        if (w.size() < 2 || !parseNumber(w[1], 31, n)) return fail(error, "fetch needs a fetch constant 0-31");
        t.kind = AmbientTerm::Kind::Fetch;
    } else if (w[0] == "register") {
        uint32_t a = 0;
        if (w.size() < 4 || w[1].size() < 2 || w[1][0] != 'r' || !parseNumber(w[1].substr(1), 127, n) || w[2] != "after" ||
            !parseNumber(w[3], 4095, a))
            return fail(error, "register needs rN after <address>");
        t.kind = AmbientTerm::Kind::Register;
        t.address = uint16_t(a);
        used = 4;
    } else {
        return fail(error, "unknown term '" + w[0] + "'");
    }
    t.index = uint8_t(n);
    if (w.size() > used) {
        if (!parseMask(w[used], t.mask)) return fail(error, "bad mask '" + w[used] + "'");
        ++used;
    }
    if (w.size() > used) return fail(error, "unexpected '" + w[used] + "'");
    out = t;
    return true;
}

std::string formatAmbientTerm(const AmbientTerm& t) {
    std::string s;
    switch (t.kind) {
        case AmbientTerm::Kind::None: return "none";
        case AmbientTerm::Kind::Output: s = "output " + std::to_string(t.index); break;
        case AmbientTerm::Kind::Constant: s = "constant c" + std::to_string(t.index); break;
        case AmbientTerm::Kind::Interpolator:
            s = std::string("interpolator ") + (t.usage == 5 ? "texcoord" : "color") + std::to_string(t.index);
            break;
        case AmbientTerm::Kind::Fetch: s = "fetch " + std::to_string(t.index); break;
        case AmbientTerm::Kind::Register:
            s = "register r" + std::to_string(t.index) + " after " + std::to_string(t.address);
            break;
    }
    if (t.mask != 0x7) {
        s += ' ';
        const char* names = "rgba";
        for (int i = 0; i < 4; ++i)
            if (t.mask & (1 << i)) s += names[i];
    }
    return s;
}

AmbientTable parseAmbientTable(const std::string& text, std::vector<std::string>* errors) {
    AmbientTable table;
    std::istringstream lines(text);
    std::string line;
    int number = 0;
    auto error = [&](const std::string& why) {
        if (errors) errors->push_back("line " + std::to_string(number) + ": " + why);
    };
    while (std::getline(lines, line)) {
        ++number;
        if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::string first;
        if (!(words >> first)) continue;
        std::string rest;
        std::getline(words, rest);
        AmbientTerm term;
        std::string why;
        if (first == "family") {
            std::istringstream r(rest);
            std::string name;
            r >> name;
            std::string termText;
            std::getline(r, termText);
            if (name.empty() || !parseAmbientTerm(termText, term, &why)) {
                error(name.empty() ? "family needs a name" : why);
                continue;
            }
            table.families.emplace_back(name, term);
            continue;
        }
        std::string hex = first;
        if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex = hex.substr(2);
        bool ok = !hex.empty() && hex.size() <= 16;
        for (char c : hex) ok = ok && std::isxdigit(static_cast<unsigned char>(c));
        const uint64_t h = ok ? std::strtoull(hex.c_str(), nullptr, 16) : 0;
        if (!ok || !h) {
            error("'" + first + "' is not a microcode hash");
            continue;
        }
        if (!parseAmbientTerm(rest, term, &why)) {
            error(why);
            continue;
        }
        table.byHash[h] = term;
    }
    return table;
}

}  // namespace kkshaders
