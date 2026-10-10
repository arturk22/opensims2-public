#include "qfs.hpp"

#include <algorithm>
#include <unordered_map>

namespace os2 {

namespace {
constexpr size_t kHeader = 9; // u32 size + 0x10FB + u24 size
}

bool qfsIsCompressed(std::span<const uint8_t> d) {
    return d.size() >= kHeader && d[4] == 0x10 && d[5] == 0xFB;
}

uint32_t qfsUncompressedSize(std::span<const uint8_t> d) {
    if (!qfsIsCompressed(d))
        throw QfsError("not a QFS stream");
    return (uint32_t(d[6]) << 16) | (uint32_t(d[7]) << 8) | d[8];
}

std::vector<uint8_t> qfsDecompress(std::span<const uint8_t> d) {
    const uint32_t outSize = qfsUncompressedSize(d);
    std::vector<uint8_t> out;
    out.reserve(outSize);

    size_t p = kHeader;
    auto need = [&](size_t n) {
        if (p + n > d.size())
            throw QfsError("QFS stream truncated");
    };
    auto literal = [&](size_t n) {
        need(n);
        out.insert(out.end(), d.begin() + p, d.begin() + p + n);
        p += n;
    };
    auto copy = [&](size_t offset, size_t len) {
        if (offset > out.size())
            throw QfsError("QFS back-reference out of range");
        size_t from = out.size() - offset;
        for (size_t i = 0; i < len; ++i)
            out.push_back(out[from + i]); // may overlap
    };

    while (p < d.size()) {
        const uint8_t b0 = d[p];
        if (b0 < 0x80) {
            need(2);
            const uint8_t b1 = d[p + 1];
            p += 2;
            literal(b0 & 0x03);
            copy(((b0 & 0x60) << 3) + b1 + 1, ((b0 >> 2) & 0x07) + 3);
        } else if (b0 < 0xC0) {
            need(3);
            const uint8_t b1 = d[p + 1], b2 = d[p + 2];
            p += 3;
            literal((b1 >> 6) & 0x03);
            copy(((b1 & 0x3F) << 8) + b2 + 1, (b0 & 0x3F) + 4);
        } else if (b0 < 0xE0) {
            need(4);
            const uint8_t b1 = d[p + 1], b2 = d[p + 2], b3 = d[p + 3];
            p += 4;
            literal(b0 & 0x03);
            copy(((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1, ((b0 & 0x0C) << 6) + b3 + 5);
        } else if (b0 < 0xFC) {
            p += 1;
            literal(((b0 & 0x1F) << 2) + 4);
        } else {
            p += 1;
            literal(b0 & 0x03);
            break;
        }
        if (out.size() > outSize)
            throw QfsError("QFS output overflow");
    }
    if (out.size() != outSize)
        throw QfsError("QFS size mismatch");
    return out;
}

std::vector<uint8_t> qfsCompress(std::span<const uint8_t> in) {
    if (in.size() > 0xFFFFFF)
        throw QfsError("input too large for QFS");

    std::vector<uint8_t> out(kHeader);
    out[4] = 0x10;
    out[5] = 0xFB;
    out[6] = uint8_t(in.size() >> 16);
    out[7] = uint8_t(in.size() >> 8);
    out[8] = uint8_t(in.size());

    std::unordered_map<uint32_t, size_t> last; // 3-byte hash -> last position
    size_t lit = 0, i = 0;

    auto flushLiterals = [&](size_t upTo, bool keepTail) {
        // Emit literal blocks of multiples of 4 (max 112); leave 0-3 for the next command.
        while (upTo - lit >= 4 && (!keepTail || upTo - lit > 3)) {
            size_t n = std::min<size_t>((upTo - lit) & ~size_t(3), 112);
            out.push_back(uint8_t(0xE0 + ((n - 4) >> 2)));
            out.insert(out.end(), in.begin() + lit, in.begin() + lit + n);
            lit += n;
        }
    };

    while (i + 3 <= in.size()) {
        uint32_t key = in[i] | (in[i + 1] << 8) | (in[i + 2] << 16);
        auto it = last.find(key);
        size_t matchLen = 0, offset = 0;
        if (it != last.end() && i - it->second <= 131072) {
            size_t j = it->second;
            size_t maxLen = std::min<size_t>(1028, in.size() - i);
            while (matchLen < maxLen && in[j + matchLen] == in[i + matchLen])
                ++matchLen;
            offset = i - j;
        }
        last[key] = i;

        bool ok = matchLen >= 3 && !(offset > 1024 && matchLen < 4) && !(offset > 16384 && matchLen < 5);
        if (!ok) {
            ++i;
            continue;
        }
        flushLiterals(i, true);
        const size_t plain = i - lit; // 0..3
        const size_t off = offset - 1;
        if (matchLen <= 10 && offset <= 1024) {
            out.push_back(uint8_t(((off >> 3) & 0x60) | ((matchLen - 3) << 2) | plain));
            out.push_back(uint8_t(off));
        } else if (matchLen <= 67 && offset <= 16384) {
            out.push_back(uint8_t(0x80 | (matchLen - 4)));
            out.push_back(uint8_t((plain << 6) | (off >> 8)));
            out.push_back(uint8_t(off));
        } else {
            const size_t l = matchLen - 5;
            out.push_back(uint8_t(0xC0 | ((off >> 12) & 0x10) | ((l >> 6) & 0x0C) | plain));
            out.push_back(uint8_t(off >> 8));
            out.push_back(uint8_t(off));
            out.push_back(uint8_t(l));
        }
        out.insert(out.end(), in.begin() + lit, in.begin() + i);
        i += matchLen;
        lit = i;
    }

    flushLiterals(in.size(), false);
    out.push_back(uint8_t(0xFC | (in.size() - lit)));
    out.insert(out.end(), in.begin() + lit, in.end());

    const uint32_t total = uint32_t(out.size());
    out[0] = uint8_t(total);
    out[1] = uint8_t(total >> 8);
    out[2] = uint8_t(total >> 16);
    out[3] = uint8_t(total >> 24);
    return out;
}

} // namespace os2
