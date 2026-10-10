#include "font.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace os2 {

// ---------------------------------------------------------------------------
// MXF

std::vector<uint8_t> decodeMxf(std::span<const uint8_t> d) {
    if (d.size() < 9 || std::memcmp(d.data(), "MXFN", 4) != 0)
        throw FontError("not an MXF font");
    const uint8_t key = d[8];
    std::vector<uint8_t> out(d.begin() + 9, d.end());
    for (auto& b : out)
        b ^= key;
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// Code pages

// Windows-1252 bytes 0x80-0x9F as Unicode (0 = undefined).
constexpr uint16_t kCp1252High[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

std::optional<uint8_t> toCp1252(char32_t c) {
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF))
        return uint8_t(c);
    for (int i = 0; i < 32; ++i)
        if (kCp1252High[i] && char32_t(kCp1252High[i]) == c)
            return uint8_t(0x80 + i);
    return std::nullopt;
}

// Adobe StandardEncoding (seac names its base and accent by these codes).
const char* standardEncoding(int code) {
    static const char* ascii[95] = {
        "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright", "parenleft",
        "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at",
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
        "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "quoteleft", "a",
        "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w",
        "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde"};
    if (code >= 32 && code <= 126)
        return ascii[code - 32];
    switch (code) {
    case 161: return "exclamdown";
    case 162: return "cent";
    case 163: return "sterling";
    case 164: return "fraction";
    case 165: return "yen";
    case 166: return "florin";
    case 167: return "section";
    case 168: return "currency";
    case 169: return "quotesingle";
    case 170: return "quotedblleft";
    case 171: return "guillemotleft";
    case 172: return "guilsinglleft";
    case 173: return "guilsinglright";
    case 174: return "fi";
    case 175: return "fl";
    case 177: return "endash";
    case 178: return "dagger";
    case 179: return "daggerdbl";
    case 180: return "periodcentered";
    case 182: return "paragraph";
    case 183: return "bullet";
    case 184: return "quotesinglbase";
    case 185: return "quotedblbase";
    case 186: return "quotedblright";
    case 187: return "guillemotright";
    case 188: return "ellipsis";
    case 189: return "perthousand";
    case 191: return "questiondown";
    case 193: return "grave";
    case 194: return "acute";
    case 195: return "circumflex";
    case 196: return "tilde";
    case 197: return "macron";
    case 198: return "breve";
    case 199: return "dotaccent";
    case 200: return "dieresis";
    case 202: return "ring";
    case 203: return "cedilla";
    case 205: return "hungarumlaut";
    case 206: return "ogonek";
    case 207: return "caron";
    case 208: return "emdash";
    case 225: return "AE";
    case 227: return "ordfeminine";
    case 232: return "Lslash";
    case 233: return "Oslash";
    case 234: return "OE";
    case 235: return "ordmasculine";
    case 241: return "ae";
    case 245: return "dotlessi";
    case 248: return "lslash";
    case 249: return "oslash";
    case 250: return "oe";
    case 251: return "germandbls";
    default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// TrueType

uint16_t be16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size())
        throw FontError("truncated font");
    return uint16_t(d[o] << 8 | d[o + 1]);
}
int16_t bes16(std::span<const uint8_t> d, size_t o) { return int16_t(be16(d, o)); }
uint32_t be32(std::span<const uint8_t> d, size_t o) { return uint32_t(be16(d, o)) << 16 | be16(d, o + 2); }

class TrueTypeFont : public Font {
public:
    explicit TrueTypeFont(std::vector<uint8_t> bytes) : data_(std::move(bytes)) {
        std::span<const uint8_t> d = data_;
        const uint16_t numTables = be16(d, 4);
        for (uint16_t i = 0; i < numTables; ++i) {
            const size_t rec = 12 + size_t(i) * 16;
            const std::string tag(reinterpret_cast<const char*>(&d[rec]), 4);
            const uint32_t off = be32(d, rec + 8), len = be32(d, rec + 12);
            if (size_t(off) + len > d.size())
                throw FontError("table " + tag + " out of bounds");
            tables_[tag] = d.subspan(off, len);
        }
        for (const char* t : {"head", "hhea", "hmtx", "maxp", "cmap"})
            if (!tables_.count(t))
                throw FontError(std::string("missing table ") + t);
        auto head = tables_["head"], hhea = tables_["hhea"], maxp = tables_["maxp"];
        unitsPerEm = be16(head, 18);
        longLoca_ = bes16(head, 50) != 0;
        ascent = bes16(hhea, 4);
        descent = bes16(hhea, 6);
        numHMetrics_ = be16(hhea, 34);
        glyphCount = be16(maxp, 4);
        readCmap(tables_["cmap"]);
        readName();
        if (name.empty())
            name = "TrueType";
    }

    std::optional<uint32_t> glyph(char32_t c) const override {
        auto it = cmap_.find(c);
        if (it == cmap_.end() && symbol_) // symbol fonts map into U+F0xx
            it = cmap_.find(0xF000 | c);
        if (it == cmap_.end() || it->second == 0)
            return std::nullopt;
        return it->second;
    }

    float advance(uint32_t g) const override {
        auto hmtx = tables_.at("hmtx");
        if (numHMetrics_ == 0)
            return 0;
        const size_t i = std::min<size_t>(g, numHMetrics_ - 1u);
        return be16(hmtx, i * 4);
    }

    GlyphPath outline(uint32_t g) const override {
        GlyphPath p;
        addGlyph(p, g, 1, 0, 0, 1, 0, 0, 0);
        return p;
    }

private:
    std::vector<uint8_t> data_;
    std::map<std::string, std::span<const uint8_t>> tables_;
    std::unordered_map<char32_t, uint32_t> cmap_;
    bool longLoca_ = false, symbol_ = false;
    uint16_t numHMetrics_ = 0;

    void readName() {
        auto it = tables_.find("name");
        if (it == tables_.end())
            return;
        auto t = it->second;
        const uint16_t count = be16(t, 2), strings = be16(t, 4);
        for (uint16_t i = 0; i < count; ++i) {
            const size_t r = 6 + size_t(i) * 12;
            const uint16_t platform = be16(t, r), language = be16(t, r + 4), nameId = be16(t, r + 6), len = be16(t, r + 8),
                           off = be16(t, r + 10);
            if (nameId != 4 || size_t(strings) + off + len > t.size())
                continue;
            std::string s;
            if (platform == 1)
                s.assign(reinterpret_cast<const char*>(&t[strings + off]), len);
            else
                for (size_t k = 0; k + 1 < len; k += 2) {
                    const uint16_t ch = be16(t, strings + off + k);
                    s += ch < 0x80 ? char(ch) : '?';
                }
            if (!s.empty() && (name.empty() || s.find('?') == std::string::npos)) {
                name = s;
                if (platform == 3 && language == 0x0409) // US English
                    return;
            }
        }
    }

    void readCmap(std::span<const uint8_t> t) {
        const uint16_t n = be16(t, 2);
        // Prefer Unicode full repertoire (format 12), then Unicode BMP, then symbol, then Mac Roman.
        int best = -1;
        size_t bestOff = 0;
        for (uint16_t i = 0; i < n; ++i) {
            const uint16_t platform = be16(t, 4 + size_t(i) * 8), enc = be16(t, 6 + size_t(i) * 8);
            const uint32_t off = be32(t, 8 + size_t(i) * 8);
            if (off >= t.size())
                continue;
            const uint16_t format = be16(t, off);
            int score = -1;
            if ((platform == 3 && enc == 10) || (platform == 0 && format == 12))
                score = 5;
            else if ((platform == 3 && enc == 1) || platform == 0)
                score = 4;
            else if (platform == 3 && enc == 0)
                score = 3;
            else if (platform == 1 && enc == 0)
                score = 1;
            if (score > best && (format == 0 || format == 4 || format == 6 || format == 12)) {
                best = score;
                bestOff = off;
                symbol_ = platform == 3 && enc == 0;
            }
        }
        if (best < 0)
            throw FontError("no usable cmap");
        auto s = t.subspan(bestOff);
        switch (be16(s, 0)) {
        case 0:
            for (int c = 0; c < 256; ++c)
                cmap_[char32_t(c)] = s[6 + size_t(c)];
            break;
        case 4: {
            const size_t segs = be16(s, 6) / 2;
            const size_t ends = 14, starts = ends + segs * 2 + 2, deltas = starts + segs * 2, ranges = deltas + segs * 2;
            for (size_t i = 0; i < segs; ++i) {
                const uint16_t end = be16(s, ends + i * 2), start = be16(s, starts + i * 2);
                const uint16_t delta = be16(s, deltas + i * 2), range = be16(s, ranges + i * 2);
                for (uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
                    uint16_t g;
                    if (range == 0)
                        g = uint16_t(c + delta);
                    else {
                        const size_t at = ranges + i * 2 + range + (c - start) * 2;
                        if (at + 2 > s.size())
                            continue;
                        g = be16(s, at);
                        if (g)
                            g = uint16_t(g + delta);
                    }
                    if (g)
                        cmap_[c] = g;
                }
            }
            break;
        }
        case 6: {
            const uint16_t first = be16(s, 6), count = be16(s, 8);
            for (uint16_t i = 0; i < count; ++i)
                cmap_[char32_t(first + i)] = be16(s, 10 + size_t(i) * 2);
            break;
        }
        case 12: {
            const uint32_t groups = be32(s, 12);
            for (uint32_t i = 0; i < groups; ++i) {
                const uint32_t start = be32(s, 16 + size_t(i) * 12), end = be32(s, 20 + size_t(i) * 12),
                               g = be32(s, 24 + size_t(i) * 12);
                for (uint32_t c = start; c <= end && c - start < 0x10000; ++c)
                    cmap_[c] = g + (c - start);
            }
            break;
        }
        }
    }

    std::span<const uint8_t> glyphData(uint32_t g) const {
        auto loca = tables_.find("loca"), glyf = tables_.find("glyf");
        if (loca == tables_.end() || glyf == tables_.end() || g >= glyphCount)
            return {};
        uint32_t a, b;
        if (longLoca_) {
            a = be32(loca->second, size_t(g) * 4);
            b = be32(loca->second, size_t(g) * 4 + 4);
        } else {
            a = uint32_t(be16(loca->second, size_t(g) * 2)) * 2;
            b = uint32_t(be16(loca->second, size_t(g) * 2 + 2)) * 2;
        }
        if (b <= a || b > glyf->second.size())
            return {};
        return glyf->second.subspan(a, b - a);
    }

    // Appends glyph g transformed by [a b; c d] + (e, f).
    void addGlyph(GlyphPath& p, uint32_t g, float a, float b, float c, float d, float e, float f, int depth) const {
        auto s = glyphData(g);
        if (s.size() < 10 || depth > 8)
            return;
        const int16_t contours = bes16(s, 0);
        auto xf = [&](float x, float y) { return std::array<float, 2>{a * x + c * y + e, b * x + d * y + f}; };
        if (contours < 0) {
            size_t o = 10;
            for (;;) {
                const uint16_t flags = be16(s, o), sub = be16(s, o + 2);
                o += 4;
                float dx, dy;
                if (flags & 1) {
                    dx = bes16(s, o);
                    dy = bes16(s, o + 2);
                    o += 4;
                } else {
                    dx = int8_t(s[o]);
                    dy = int8_t(s[o + 1]);
                    o += 2;
                }
                float m0 = 1, m1 = 0, m2 = 0, m3 = 1;
                auto f2dot14 = [&](size_t at) { return float(bes16(s, at)) / 16384.0f; };
                if (flags & 8) {
                    m0 = m3 = f2dot14(o);
                    o += 2;
                } else if (flags & 0x40) {
                    m0 = f2dot14(o);
                    m3 = f2dot14(o + 2);
                    o += 4;
                } else if (flags & 0x80) {
                    m0 = f2dot14(o);
                    m1 = f2dot14(o + 2);
                    m2 = f2dot14(o + 4);
                    m3 = f2dot14(o + 6);
                    o += 8;
                }
                if (!(flags & 2))
                    dx = dy = 0; // point matching: not supported
                // Compose: child -> [m] + (dx,dy) -> parent [a b c d e f].
                const auto off = xf(dx, dy);
                addGlyph(p, sub, a * m0 + c * m1, b * m0 + d * m1, a * m2 + c * m3, b * m2 + d * m3, off[0], off[1],
                         depth + 1);
                if (!(flags & 0x20))
                    break;
            }
            return;
        }
        const size_t n = size_t(contours);
        std::vector<uint16_t> ends(n);
        for (size_t i = 0; i < n; ++i)
            ends[i] = be16(s, 10 + i * 2);
        const size_t points = n ? size_t(ends[n - 1]) + 1 : 0;
        size_t o = 10 + n * 2;
        o += 2 + be16(s, o); // instructions
        std::vector<uint8_t> flags;
        flags.reserve(points);
        while (flags.size() < points) {
            if (o >= s.size())
                return;
            const uint8_t fl = s[o++];
            flags.push_back(fl);
            if (fl & 8) {
                if (o >= s.size())
                    return;
                for (uint8_t r = s[o++]; r > 0 && flags.size() < points; --r)
                    flags.push_back(fl);
            }
        }
        std::vector<float> xs(points), ys(points);
        int v = 0;
        for (size_t i = 0; i < points; ++i) {
            if (flags[i] & 2) {
                if (o >= s.size())
                    return;
                v += (flags[i] & 16) ? s[o] : -int(s[o]);
                ++o;
            } else if (!(flags[i] & 16)) {
                v += bes16(s, o);
                o += 2;
            }
            xs[i] = float(v);
        }
        v = 0;
        for (size_t i = 0; i < points; ++i) {
            if (flags[i] & 4) {
                if (o >= s.size())
                    return;
                v += (flags[i] & 32) ? s[o] : -int(s[o]);
                ++o;
            } else if (!(flags[i] & 32)) {
                v += bes16(s, o);
                o += 2;
            }
            ys[i] = float(v);
        }
        size_t start = 0;
        for (size_t ci = 0; ci < n; ++ci) {
            const size_t end = ends[ci];
            if (end < start || end >= points)
                return;
            const size_t count = end - start + 1;
            auto pt = [&](size_t k) { return xf(xs[start + k], ys[start + k]); };
            auto on = [&](size_t k) { return (flags[start + k] & 1) != 0; };
            // Start on an on-curve point (the first or the last), else between the last and first.
            std::array<float, 2> s0;
            size_t from = 0, to = count; // points visited after the start
            if (on(0)) {
                s0 = pt(0);
                from = 1;
            } else if (on(count - 1)) {
                s0 = pt(count - 1);
                to = count - 1;
            } else {
                const auto a0 = pt(count - 1), a1 = pt(0);
                s0 = {(a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2};
            }
            p.moveTo(s0[0], s0[1]);
            std::optional<std::array<float, 2>> ctrl;
            for (size_t k = from; k < to; ++k) {
                const auto q = pt(k);
                if (on(k)) {
                    if (ctrl)
                        p.quadTo((*ctrl)[0], (*ctrl)[1], q[0], q[1]);
                    else
                        p.lineTo(q[0], q[1]);
                    ctrl.reset();
                } else {
                    if (ctrl)
                        p.quadTo((*ctrl)[0], (*ctrl)[1], ((*ctrl)[0] + q[0]) / 2, ((*ctrl)[1] + q[1]) / 2);
                    ctrl = q;
                }
            }
            if (ctrl)
                p.quadTo((*ctrl)[0], (*ctrl)[1], s0[0], s0[1]);
            else
                p.lineTo(s0[0], s0[1]);
            p.close();
            start = end + 1;
        }
    }
};

// ---------------------------------------------------------------------------
// Type 1

std::vector<uint8_t> decrypt(std::span<const uint8_t> in, uint16_t r, size_t skip) {
    std::vector<uint8_t> out;
    out.reserve(in.size());
    for (uint8_t c : in) {
        out.push_back(uint8_t(c ^ (r >> 8)));
        r = uint16_t((c + r) * 52845u + 22719u);
    }
    if (skip >= out.size())
        return {};
    out.erase(out.begin(), out.begin() + long(skip));
    return out;
}

class Type1Font : public Font {
public:
    explicit Type1Font(std::span<const uint8_t> file) {
        std::string clear;
        std::vector<uint8_t> enc;
        if (file.size() > 6 && file[0] == 0x80) {
            for (size_t p = 0; p + 2 <= file.size() && file[p] == 0x80;) {
                const uint8_t type = file[p + 1];
                if (type == 3)
                    break;
                if (p + 6 > file.size())
                    throw FontError("truncated PFB");
                const uint32_t len = uint32_t(file[p + 2]) | uint32_t(file[p + 3]) << 8 | uint32_t(file[p + 4]) << 16 |
                                     uint32_t(file[p + 5]) << 24;
                p += 6;
                if (p + len > file.size())
                    throw FontError("truncated PFB segment");
                if (type == 1)
                    clear.append(reinterpret_cast<const char*>(&file[p]), len);
                else if (type == 2)
                    enc.insert(enc.end(), file.begin() + long(p), file.begin() + long(p + len));
                p += len;
            }
        } else {
            // PFA: hex after "eexec".
            const std::string all(file.begin(), file.end());
            const size_t e = all.find("eexec");
            if (e == std::string::npos)
                throw FontError("no eexec section");
            clear = all.substr(0, e + 5);
            int hi = -1;
            for (size_t i = e + 5; i < all.size(); ++i) {
                const int v = std::isxdigit(static_cast<unsigned char>(all[i])) ? std::stoi(std::string(1, all[i]), nullptr, 16) : -1;
                if (v < 0)
                    continue;
                if (hi < 0)
                    hi = v;
                else {
                    enc.push_back(uint8_t(hi << 4 | v));
                    hi = -1;
                }
            }
        }
        parseClear(clear);
        parsePrivate(decrypt(enc, 55665, 4));
        glyphCount = glyphs_.size();
    }

    std::optional<uint32_t> glyph(char32_t c) const override {
        const auto code = toCp1252(c);
        if (!code || encoding_[*code].empty())
            return std::nullopt;
        return byName(encoding_[*code]);
    }

    float advance(uint32_t g) const override {
        if (g >= glyphs_.size())
            return 0;
        GlyphPath p;
        return run(g, p, 0, 0, 0).width;
    }

    GlyphPath outline(uint32_t g) const override {
        GlyphPath p;
        if (g < glyphs_.size())
            run(g, p, 0, 0, 0);
        return p;
    }

private:
    struct Glyph {
        std::string name;
        std::vector<uint8_t> code;
    };
    std::vector<Glyph> glyphs_;
    std::unordered_map<std::string, uint32_t> names_;
    std::vector<std::vector<uint8_t>> subrs_;
    std::array<std::string, 256> encoding_;
    int lenIV_ = 4;

    std::optional<uint32_t> byName(const std::string& n) const {
        auto it = names_.find(n);
        if (it == names_.end())
            return std::nullopt;
        return it->second;
    }

    void parseClear(const std::string& t) {
        if (auto p = t.find("/FontName"); p != std::string::npos) {
            const size_t s = t.find('/', p + 9);
            if (s != std::string::npos) {
                size_t e = s + 1;
                while (e < t.size() && !std::isspace(static_cast<unsigned char>(t[e])))
                    ++e;
                name = t.substr(s + 1, e - s - 1);
            }
        }
        if (auto p = t.find("/FontMatrix"); p != std::string::npos) {
            const size_t s = t.find_first_of("[{", p);
            if (s != std::string::npos) {
                const float a = std::strtof(t.c_str() + s + 1, nullptr);
                if (a > 0)
                    unitsPerEm = 1.0f / a;
            }
        }
        if (auto p = t.find("/FontBBox"); p != std::string::npos) {
            const size_t s = t.find_first_of("[{", p);
            if (s != std::string::npos) {
                char* end = nullptr;
                const char* q = t.c_str() + s + 1;
                float v[4];
                for (float& x : v) {
                    x = std::strtof(q, &end);
                    q = end;
                }
                descent = v[1];
                ascent = v[3];
            }
        }
        const size_t e = t.find("/Encoding");
        if (e == std::string::npos)
            return;
        if (t.compare(e, 26, "/Encoding StandardEncoding") == 0) {
            for (int c = 0; c < 256; ++c)
                if (const char* n = standardEncoding(c))
                    encoding_[size_t(c)] = n;
            return;
        }
        const size_t stop = t.find("readonly def", e);
        for (size_t p = t.find("dup ", e); p != std::string::npos && p < stop; p = t.find("dup ", p + 4)) {
            char* end = nullptr;
            const long code = std::strtol(t.c_str() + p + 4, &end, 10);
            const char* slash = end;
            while (*slash == ' ')
                ++slash;
            if (*slash != '/' || code < 0 || code > 255)
                continue;
            const char* ne = slash + 1;
            while (*ne && !std::isspace(static_cast<unsigned char>(*ne)))
                ++ne;
            encoding_[size_t(code)] = std::string(slash + 1, ne);
        }
    }

    // Reads "<int> <int> RD " style headers in the private dict.
    static bool readInt(const std::vector<uint8_t>& d, size_t& p, long& v) {
        while (p < d.size() && std::isspace(d[p]))
            ++p;
        size_t s = p;
        if (p < d.size() && d[p] == '-')
            ++p;
        while (p < d.size() && std::isdigit(d[p]))
            ++p;
        if (p == s)
            return false;
        v = std::strtol(std::string(d.begin() + long(s), d.begin() + long(p)).c_str(), nullptr, 10);
        return true;
    }
    static std::string readToken(const std::vector<uint8_t>& d, size_t& p) {
        while (p < d.size() && std::isspace(d[p]))
            ++p;
        size_t s = p;
        while (p < d.size() && !std::isspace(d[p]))
            ++p;
        return std::string(d.begin() + long(s), d.begin() + long(p));
    }
    // After "<len> RD ": skips the single space and takes len bytes.
    std::vector<uint8_t> readBinary(const std::vector<uint8_t>& d, size_t& p, long len) const {
        ++p;
        if (len < 0 || p + size_t(len) > d.size())
            throw FontError("truncated charstring");
        std::vector<uint8_t> bin(d.begin() + long(p), d.begin() + long(p + size_t(len)));
        p += size_t(len);
        return lenIV_ < 0 ? bin : decrypt(bin, 4330, size_t(lenIV_));
    }

    static size_t find(const std::vector<uint8_t>& d, std::string_view s, size_t from = 0) {
        auto it = std::search(d.begin() + long(std::min(from, d.size())), d.end(), s.begin(), s.end());
        return it == d.end() ? std::string::npos : size_t(it - d.begin());
    }

    void parsePrivate(const std::vector<uint8_t>& d) {
        if (size_t p = find(d, "/lenIV"); p != std::string::npos) {
            p += 6;
            long v;
            if (readInt(d, p, v))
                lenIV_ = int(v);
        }
        if (size_t p = find(d, "/Subrs"); p != std::string::npos) {
            p += 6;
            long count;
            if (readInt(d, p, count) && count > 0 && count < 65536) {
                subrs_.resize(size_t(count));
                for (long i = 0; i < count; ++i) {
                    p = find(d, "dup", p);
                    if (p == std::string::npos)
                        break;
                    p += 3;
                    long idx, len;
                    if (!readInt(d, p, idx) || !readInt(d, p, len))
                        break;
                    readToken(d, p); // RD or -|
                    auto bin = readBinary(d, p, len);
                    if (idx >= 0 && idx < count)
                        subrs_[size_t(idx)] = std::move(bin);
                }
            }
        }
        size_t p = find(d, "/CharStrings");
        if (p == std::string::npos)
            throw FontError("no CharStrings");
        p += 12;
        long count;
        if (!readInt(d, p, count))
            throw FontError("bad CharStrings");
        for (long i = 0; i < count; ++i) {
            while (p < d.size() && d[p] != '/')
                ++p;
            if (p >= d.size())
                break;
            ++p;
            size_t e = p;
            while (e < d.size() && !std::isspace(d[e]))
                ++e;
            std::string n(d.begin() + long(p), d.begin() + long(e));
            p = e;
            long len;
            if (!readInt(d, p, len))
                break;
            readToken(d, p);
            auto bin = readBinary(d, p, len);
            names_[n] = uint32_t(glyphs_.size());
            glyphs_.push_back({std::move(n), std::move(bin)});
        }
        if (glyphs_.empty())
            throw FontError("no glyphs");
    }

    struct State {
        std::vector<float> stack;
        std::vector<float> ps; // othersubr results, popped by "pop"
        float x = 0, y = 0, width = 0, sbx = 0;
        float ox = 0, oy = 0; // origin offset (seac accents)
        bool open = false, flex = false, done = false;
        std::vector<std::array<float, 2>> flexPts;
    };

    struct Result {
        float width = 0, sbx = 0;
    };

    Result run(uint32_t g, GlyphPath& path, float ox, float oy, int depth) const {
        State st;
        st.ox = ox;
        st.oy = oy;
        exec(glyphs_[g].code, st, path, depth, 0);
        if (st.open)
            path.close();
        return {st.width, st.sbx};
    }

    void lineTo(State& st, GlyphPath& path, float dx, float dy) const {
        st.x += dx;
        st.y += dy;
        path.lineTo(st.x + st.ox, st.y + st.oy);
    }
    void curveTo(State& st, GlyphPath& path, float a, float b, float c, float d, float e, float f) const {
        const float x1 = st.x + a, y1 = st.y + b, x2 = x1 + c, y2 = y1 + d;
        st.x = x2 + e;
        st.y = y2 + f;
        path.cubicTo(x1 + st.ox, y1 + st.oy, x2 + st.ox, y2 + st.oy, st.x + st.ox, st.y + st.oy);
    }
    void moveTo(State& st, GlyphPath& path, float dx, float dy) const {
        st.x += dx;
        st.y += dy;
        if (st.flex) {
            st.flexPts.push_back({st.x, st.y});
            return;
        }
        if (st.open)
            path.close();
        path.moveTo(st.x + st.ox, st.y + st.oy);
        st.open = true;
    }

    void exec(const std::vector<uint8_t>& code, State& st, GlyphPath& path, int depth, int callDepth) const {
        if (callDepth > 10)
            throw FontError("subroutine nesting too deep");
        auto& s = st.stack;
        auto arg = [&](size_t i) { return i < s.size() ? s[i] : 0.0f; };
        for (size_t i = 0; i < code.size() && !st.done;) {
            const uint8_t v = code[i++];
            if (v >= 32) {
                if (v <= 246)
                    s.push_back(float(int(v) - 139));
                else if (v <= 250) {
                    if (i >= code.size())
                        break;
                    s.push_back(float((int(v) - 247) * 256 + code[i++] + 108));
                } else if (v <= 254) {
                    if (i >= code.size())
                        break;
                    s.push_back(float(-(int(v) - 251) * 256 - code[i++] - 108));
                } else {
                    if (i + 4 > code.size())
                        break;
                    const int32_t n = int32_t(uint32_t(code[i]) << 24 | uint32_t(code[i + 1]) << 16 |
                                              uint32_t(code[i + 2]) << 8 | code[i + 3]);
                    i += 4;
                    s.push_back(float(n));
                }
                continue;
            }
            switch (v) {
            case 1: // hstem
            case 3: // vstem
                break;
            case 4: moveTo(st, path, 0, arg(0)); break;
            case 5: lineTo(st, path, arg(0), arg(1)); break;
            case 6: lineTo(st, path, arg(0), 0); break;
            case 7: lineTo(st, path, 0, arg(0)); break;
            case 8: curveTo(st, path, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5)); break;
            case 9:
                if (st.open)
                    path.close();
                st.open = false;
                break;
            case 10: { // callsubr
                if (s.empty())
                    throw FontError("callsubr without index");
                const size_t idx = size_t(s.back());
                s.pop_back();
                if (idx >= subrs_.size())
                    throw FontError("bad subr index");
                exec(subrs_[idx], st, path, depth, callDepth + 1);
                continue; // keep the stack: subroutines pass values through it
            }
            case 11: return; // return
            case 12: {
                if (i >= code.size())
                    break;
                const uint8_t e = code[i++];
                switch (e) {
                case 0: case 1: case 2: break; // dotsection, vstem3, hstem3
                case 6: { // seac: asb adx ady bchar achar
                    const float asb = arg(0), adx = arg(1), ady = arg(2);
                    const char* bn = standardEncoding(int(arg(3)));
                    const char* an = standardEncoding(int(arg(4)));
                    auto bg = bn ? byName(bn) : std::nullopt;
                    auto ag = an ? byName(an) : std::nullopt;
                    if (depth > 0 || !bg || !ag)
                        throw FontError("bad seac " + std::to_string(int(arg(3))) + " " + std::to_string(int(arg(4))));
                    const Result base = run(*bg, path, st.ox, st.oy, depth + 1);
                    run(*ag, path, st.ox + adx - asb + base.sbx, st.oy + ady, depth + 1);
                    st.width = base.width;
                    st.sbx = base.sbx;
                    st.open = false;
                    st.done = true;
                    break;
                }
                case 7: // sbw
                    st.sbx = arg(0);
                    st.x = arg(0);
                    st.y = arg(1);
                    st.width = arg(2);
                    break;
                case 12: // div
                    if (s.size() >= 2) {
                        const float b = s.back();
                        s.pop_back();
                        s.back() = b != 0 ? s.back() / b : 0;
                    }
                    continue;
                case 16: { // callothersubr: args... n othersubr#
                    if (s.size() < 2)
                        throw FontError("bad callothersubr");
                    const int other = int(s.back());
                    s.pop_back();
                    const int n = int(s.back());
                    s.pop_back();
                    if (n < 0 || size_t(n) > s.size())
                        throw FontError("bad callothersubr count");
                    std::vector<float> args(s.end() - n, s.end());
                    s.resize(s.size() - size_t(n));
                    st.ps.clear();
                    if (other == 1) { // start flex
                        st.flex = true;
                        st.flexPts.clear();
                    } else if (other == 2) { // flex point: added by the preceding rmoveto
                    } else if (other == 0 && st.flex) { // end flex: reference point + 6 curve points
                        st.flex = false;
                        if (st.flexPts.size() >= 7) {
                            const auto& f = st.flexPts;
                            const size_t b = f.size() - 6;
                            for (size_t k = b; k < b + 6; k += 3)
                                path.cubicTo(f[k][0] + st.ox, f[k][1] + st.oy, f[k + 1][0] + st.ox, f[k + 1][1] + st.oy,
                                             f[k + 2][0] + st.ox, f[k + 2][1] + st.oy);
                            st.x = f.back()[0];
                            st.y = f.back()[1];
                        }
                        st.ps = {st.y, st.x}; // "pop pop setcurrentpoint" reads x then y
                    } else {
                        // Hint replacement (3) and the rest: hand the arguments back.
                        st.ps.assign(args.rbegin(), args.rend());
                    }
                    continue;
                }
                case 17: // pop
                    if (!st.ps.empty()) {
                        s.push_back(st.ps.back());
                        st.ps.pop_back();
                    } else {
                        s.push_back(0);
                    }
                    continue;
                case 33: // setcurrentpoint
                    st.x = arg(0);
                    st.y = arg(1);
                    break;
                default: break;
                }
                break;
            }
            case 13: // hsbw
                st.sbx = arg(0);
                st.x = arg(0);
                st.y = 0;
                st.width = arg(1);
                break;
            case 14: // endchar
                st.done = true;
                break;
            case 21: moveTo(st, path, arg(0), arg(1)); break;
            case 22: moveTo(st, path, arg(0), 0); break;
            case 30: curveTo(st, path, 0, arg(0), arg(1), arg(2), arg(3), 0); break;
            case 31: curveTo(st, path, arg(0), 0, arg(1), arg(2), 0, arg(3)); break;
            default: break;
            }
            s.clear();
        }
    }
};

// ---------------------------------------------------------------------------
// Rasterizer (signed-area accumulation, after font-rs)

struct Raster {
    int w, h;
    std::vector<float> a;
    Raster(int w_, int h_) : w(w_), h(h_), a(size_t(w_) * size_t(h_) + 4, 0.0f) {}

    void line(float x0, float y0, float x1, float y1) {
        if (y0 == y1)
            return;
        float dir = 1;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1;
        }
        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0)
            x -= y0 * dxdy;
        const int yStart = std::max(0, int(y0)), yEnd = std::min(h, int(std::ceil(y1)));
        for (int y = yStart; y < yEnd; ++y) {
            const size_t row = size_t(y) * size_t(w);
            const float dy = std::min(float(y + 1), y1) - std::max(float(y), y0);
            const float xnext = x + dxdy * dy;
            const float d = dy * dir;
            const float xa = std::min(x, xnext), xb = std::max(x, xnext);
            const float xaf = std::floor(xa);
            const int xai = int(xaf);
            const float xbc = std::ceil(xb);
            const int xbi = int(xbc);
            if (xai < 0 || xbi > w) {
                x = xnext;
                continue;
            }
            if (xbi <= xai + 1) {
                const float xmf = 0.5f * (x + xnext) - xaf;
                a[row + size_t(xai)] += d - d * xmf;
                a[row + size_t(xai) + 1] += d * xmf;
            } else {
                const float s = 1.0f / (xb - xa);
                const float x0f = xa - xaf;
                const float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
                const float x1f = xb - xbc + 1;
                const float am = 0.5f * s * x1f * x1f;
                a[row + size_t(xai)] += d * a0;
                if (xbi == xai + 2) {
                    a[row + size_t(xai) + 1] += d * (1 - a0 - am);
                } else {
                    const float a1 = s * (1.5f - x0f);
                    a[row + size_t(xai) + 1] += d * (a1 - a0);
                    for (int xi = xai + 2; xi < xbi - 1; ++xi)
                        a[row + size_t(xi)] += d * s;
                    const float a2 = a1 + float(xbi - xai - 3) * s;
                    a[row + size_t(xbi) - 1] += d * (1 - a2 - am);
                }
                a[row + size_t(xbi)] += d * am;
            }
            x = xnext;
        }
    }
};

} // namespace

std::unique_ptr<Font> Font::load(std::span<const uint8_t> f) {
    if (f.size() >= 4 && ((f[0] == 0 && f[1] == 1 && f[2] == 0 && f[3] == 0) || std::memcmp(f.data(), "true", 4) == 0))
        return std::make_unique<TrueTypeFont>(std::vector<uint8_t>(f.begin(), f.end()));
    if (f.size() >= 2 && (f[0] == 0x80 || (f[0] == '%' && f[1] == '!')))
        return std::make_unique<Type1Font>(f);
    throw FontError("unknown font format");
}

std::unique_ptr<Font> Font::loadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FontError("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "MXFN", 4) == 0)
        bytes = decodeMxf(bytes);
    return load(bytes);
}

GlyphBitmap rasterizeGlyph(const Font& font, uint32_t glyph, float ppem, float dx) {
    GlyphBitmap out;
    const float scale = ppem / font.unitsPerEm;
    out.advance = font.advance(glyph) * scale;
    const GlyphPath path = font.outline(glyph);

    // Flatten to polylines in pixel space (y down).
    std::vector<std::vector<std::array<float, 2>>> polys;
    float cx = 0, cy = 0;
    auto P = [&](float x, float y) { return std::array<float, 2>{x * scale + dx, -y * scale}; };
    for (const auto& c : path.cmds) {
        switch (c.op) {
        case GlyphPath::Op::Move:
            polys.push_back({P(c.x, c.y)});
            break;
        case GlyphPath::Op::Line:
            if (polys.empty())
                polys.push_back({P(cx, cy)});
            polys.back().push_back(P(c.x, c.y));
            break;
        case GlyphPath::Op::Quad:
        case GlyphPath::Op::Cubic: {
            if (polys.empty())
                polys.push_back({P(cx, cy)});
            const auto p0 = polys.back().back();
            const auto p1 = P(c.x1, c.y1), p3 = P(c.x, c.y);
            const auto p2 = c.op == GlyphPath::Op::Cubic ? P(c.x2, c.y2) : p1;
            const float len = std::hypot(p1[0] - p0[0], p1[1] - p0[1]) + std::hypot(p2[0] - p1[0], p2[1] - p1[1]) +
                              std::hypot(p3[0] - p2[0], p3[1] - p2[1]);
            const int n = std::clamp(int(len / 2.0f) + 1, 1, 64);
            for (int k = 1; k <= n; ++k) {
                const float t = float(k) / float(n), u = 1 - t;
                std::array<float, 2> q;
                for (int i = 0; i < 2; ++i)
                    q[size_t(i)] = c.op == GlyphPath::Op::Quad
                                       ? u * u * p0[size_t(i)] + 2 * u * t * p1[size_t(i)] + t * t * p3[size_t(i)]
                                       : u * u * u * p0[size_t(i)] + 3 * u * u * t * p1[size_t(i)] +
                                             3 * u * t * t * p2[size_t(i)] + t * t * t * p3[size_t(i)];
                polys.back().push_back(q);
            }
            break;
        }
        case GlyphPath::Op::Close:
            break;
        }
        if (c.op != GlyphPath::Op::Close)
            cx = c.x, cy = c.y;
    }
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (const auto& poly : polys)
        for (const auto& p : poly) {
            minx = std::min(minx, p[0]);
            maxx = std::max(maxx, p[0]);
            miny = std::min(miny, p[1]);
            maxy = std::max(maxy, p[1]);
        }
    if (minx > maxx)
        return out; // blank glyph (space)
    const int x0 = int(std::floor(minx)) - 1, y0 = int(std::floor(miny)) - 1;
    out.width = int(std::ceil(maxx)) + 1 - x0 + 1;
    out.height = int(std::ceil(maxy)) + 1 - y0;
    out.left = x0;
    out.top = -y0;
    Raster r(out.width, out.height);
    for (const auto& poly : polys)
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % poly.size()]; // closes the contour
            r.line(a[0] - float(x0), a[1] - float(y0), b[0] - float(x0), b[1] - float(y0));
        }
    out.alpha.resize(size_t(out.width) * size_t(out.height));
    float acc = 0;
    for (size_t i = 0; i < out.alpha.size(); ++i) {
        acc += r.a[i];
        out.alpha[i] = uint8_t(std::lround(std::min(std::fabs(acc), 1.0f) * 255));
    }
    return out;
}

std::u32string decodeUtf8(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        char32_t cp = n <= 0 ? c : char32_t(c & (0x3F >> n));
        bool ok = n >= 0 && i + size_t(n) < s.size();
        for (int k = 1; ok && k <= n; ++k) {
            const uint8_t cc = uint8_t(s[i + size_t(k)]);
            ok = (cc & 0xC0) == 0x80;
            cp = cp << 6 | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += size_t(n) + 1;
    }
    return out;
}

TextMetrics measureText(const Font& font, std::u32string_view text, float ppem, float charSpacing) {
    TextMetrics m;
    const float scale = ppem / font.unitsPerEm;
    m.ascent = font.ascent * scale;
    m.descent = -font.descent * scale;
    for (char32_t c : text) {
        auto g = font.glyph(c);
        if (!g) {
            ++m.missing;
            continue;
        }
        m.width += font.advance(*g) * scale + charSpacing;
    }
    return m;
}

float drawText(RgbaImage& img, const Font& font, std::u32string_view text, float x, float baseline, float ppem,
               const uint8_t rgba[4], float charSpacing) {
    for (char32_t c : text) {
        auto g = font.glyph(c);
        if (!g)
            continue;
        const float fx = std::floor(x);
        const GlyphBitmap gb = rasterizeGlyph(font, *g, ppem, x - fx);
        const int ox = int(fx) + gb.left, oy = int(std::lround(baseline)) - gb.top;
        for (int y = 0; y < gb.height; ++y) {
            const int py = oy + y;
            if (py < 0 || py >= int(img.height))
                continue;
            for (int xx = 0; xx < gb.width; ++xx) {
                const int px = ox + xx;
                if (px < 0 || px >= int(img.width))
                    continue;
                const float a = float(gb.alpha[size_t(y) * size_t(gb.width) + size_t(xx)]) / 255.0f * float(rgba[3]) / 255.0f;
                if (a <= 0)
                    continue;
                uint8_t* o = &img.pixels[(size_t(py) * img.width + size_t(px)) * 4];
                for (int k = 0; k < 3; ++k)
                    o[k] = uint8_t(std::lround(float(rgba[k]) * a + float(o[k]) * (1 - a)));
                o[3] = uint8_t(std::lround(255 * (a + float(o[3]) / 255.0f * (1 - a))));
            }
        }
        x += gb.advance + charSpacing;
    }
    return x;
}

// ---------------------------------------------------------------------------
// FontStyle.ini

std::vector<FontStyle> parseFontStyles(std::string_view ini) {
    std::vector<FontStyle> out;
    bool inStyles = false;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string_view::npos)
            end = ini.size();
        std::string line(ini.substr(pos, end - pos));
        pos = end + 1;
        if (auto c = line.find(';'); c != std::string::npos)
            line.resize(c);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        size_t b = 0;
        while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b])))
            ++b;
        line.erase(0, b);
        if (line.empty())
            continue;
        if (line[0] == '[') {
            inStyles = line == "[Font Styles]";
            continue;
        }
        const size_t eq = line.find('=');
        if (!inStyles || eq == std::string::npos)
            continue;
        FontStyle st;
        st.name = line.substr(0, eq);
        while (!st.name.empty() && std::isspace(static_cast<unsigned char>(st.name.back())))
            st.name.pop_back();
        // Fields: quoted strings, then the GUID.
        std::vector<std::string> quoted;
        size_t p = eq + 1;
        for (;;) {
            const size_t q = line.find('"', p);
            if (q == std::string::npos)
                break;
            const size_t q2 = line.find('"', q + 1);
            if (q2 == std::string::npos)
                break;
            quoted.push_back(line.substr(q + 1, q2 - q - 1));
            p = q2 + 1;
        }
        if (quoted.size() < 3)
            continue;
        // The face list is one string ("Arta, Arial") or several (the file has "face",	"11").
        for (size_t s = 0; s <= quoted[0].size();) {
            size_t e = std::min(quoted[0].find(',', s), quoted[0].size());
            std::string f = quoted[0].substr(s, e - s);
            f.erase(0, f.find_first_not_of(' '));
            f.erase(f.find_last_not_of(' ') + 1);
            if (!f.empty())
                st.faces.push_back(f);
            s = e + 1;
        }
        st.size = std::atoi(quoted[1].c_str());
        st.flags = quoted[2];
        std::string lower;
        for (char ch : st.flags)
            lower += char(std::tolower(static_cast<unsigned char>(ch)));
        st.bold = lower.find("bold") != std::string::npos;
        st.italic = lower.find("italic") != std::string::npos;
        if (auto g = line.find("0x", p); g != std::string::npos)
            st.guid = uint32_t(std::strtoul(line.c_str() + g, nullptr, 16));
        out.push_back(std::move(st));
    }
    return out;
}

FontLibrary::FontLibrary(const std::filesystem::path& dir) : dir_(dir) {
    std::ifstream in(dir / "FontStyle.ini", std::ios::binary);
    if (in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        styles_ = parseFontStyles(text);
    }
}

const FontStyle* FontLibrary::style(std::string_view key) const {
    auto lower = [](std::string_view s) {
        std::string o(s);
        for (auto& c : o)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    const std::string k = lower(key);
    if (k.starts_with("0x")) {
        const uint32_t guid = uint32_t(std::strtoul(k.c_str(), nullptr, 16));
        for (const auto& s : styles_)
            if (s.guid == guid)
                return &s;
        return nullptr;
    }
    for (const auto& s : styles_)
        if (lower(s.name) == k)
            return &s;
    return nullptr;
}

const Font* FontLibrary::face(const std::string& face, bool bold) {
    const std::string key = face + (bold ? "|b" : "");
    auto it = faces_.find(key);
    if (it != faces_.end())
        return it->second.get();
    std::unique_ptr<Font> f;
    for (const auto& file : {face + (bold ? " (bold)" : "") + ".mxf", face + ".mxf", face + " (bold).mxf"}) {
        if (!std::filesystem::exists(dir_ / file))
            continue;
        try {
            f = Font::loadFile(dir_ / file);
            break;
        } catch (const FontError&) {
        }
    }
    return faces_.emplace(key, std::move(f)).first->second.get();
}

const Font* FontLibrary::font(const FontStyle& style) {
    for (const auto& f : style.faces)
        if (const Font* font = face(f, style.bold))
            return font;
    return nullptr;
}

} // namespace os2
