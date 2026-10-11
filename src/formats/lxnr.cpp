#include "lxnr.hpp"

#include "binreader.hpp"
#include "cas_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace os2::cas {

namespace {

constexpr uint32_t kKeye = 0x6B657965, kMstr = 0x6D737472, kData = 0x64617461;

struct Writer {
    std::vector<uint8_t> out;
    void u16(uint16_t v) {
        out.push_back(uint8_t(v));
        out.push_back(uint8_t(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(uint8_t(v >> (8 * i)));
    }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
};

} // namespace

FaceStructure FaceStructure::parse(std::span<const uint8_t> bytes) {
    BinReader r(bytes);
    FaceStructure s;
    s.version = r.u32();
    if (s.version < 3 || s.version > 5)
        throw FormatError("LxNR version " + std::to_string(s.version) + " not supported");
    const uint32_t n = r.u32();
    if (n > 4096)
        throw FormatError("LxNR: too many keys");
    for (uint32_t i = 0; i < n; ++i)
        s.keys.push_back(r.u32());
    if (r.u32() != kKeye)
        throw FormatError("LxNR: no 'keye'");
    const uint16_t count = r.u16();
    std::vector<Vec3> pool(count);
    for (auto& p : pool)
        p = {r.f32(), r.f32(), r.f32()};
    if (r.u32() != kMstr)
        throw FormatError("LxNR: no 'mstr'");
    for (uint32_t k : s.keys) {
        const uint16_t m = r.u16();
        std::vector<Vertex> verts;
        verts.reserve(m / 2);
        for (uint16_t i = 0; i + 1 < m; i += 2) {
            const uint16_t a = r.u16(), b = r.u16();
            if (a >= pool.size() || b >= pool.size())
                throw FormatError("LxNR: index out of the pool");
            verts.push_back({pool[a], pool[b][0], pool[b][1]});
        }
        if (m % 2)
            r.u16();
        s.data[k] = std::move(verts);
    }
    if (r.u32() != kData)
        throw FormatError("LxNR: no 'data'");
    return s;
}

std::vector<uint8_t> FaceStructure::write() const {
    // The pool: equal vectors once (the writer's epsilon test; exact here,
    // with a hash on the bits, then the game's tolerance within a bucket).
    std::vector<Vec3> pool;
    std::unordered_map<uint64_t, std::vector<uint16_t>> buckets;
    auto index = [&](const Vec3& v) -> uint16_t {
        const auto q = [](float f) { return int64_t(std::lround(double(f) * 1e5)); };
        const uint64_t h = uint64_t(q(v[0])) * 73856093u ^ uint64_t(q(v[1])) * 19349663u ^ uint64_t(q(v[2])) * 83492791u;
        auto& b = buckets[h];
        for (uint16_t i : b)
            if (std::fabs(pool[i][0] - v[0]) < 1e-5f && std::fabs(pool[i][1] - v[1]) < 1e-5f &&
                std::fabs(pool[i][2] - v[2]) < 1e-5f)
                return i;
        if (pool.size() >= 0xFFFF)
            throw std::runtime_error("LxNR: more than 65535 distinct vectors");
        pool.push_back(v);
        b.push_back(uint16_t(pool.size() - 1));
        return b.back();
    };
    std::vector<std::vector<uint16_t>> lists;
    for (uint32_t k : keys) {
        std::vector<uint16_t> l;
        if (auto it = data.find(k); it != data.end())
            for (const auto& v : it->second) {
                l.push_back(index(v.delta));
                l.push_back(index({v.u, v.v, 0}));
            }
        if (l.size() > 0xFFFF)
            throw std::runtime_error("LxNR: key with too many vertices");
        lists.push_back(std::move(l));
    }
    Writer w;
    w.u32(version);
    w.u32(uint32_t(keys.size()));
    for (uint32_t k : keys)
        w.u32(k);
    w.u32(kKeye);
    w.u16(uint16_t(pool.size()));
    for (const auto& p : pool)
        for (float f : p)
            w.f32(f);
    w.u32(kMstr);
    for (const auto& l : lists) {
        w.u16(uint16_t(l.size()));
        for (uint16_t i : l)
            w.u16(i);
    }
    w.u32(kData);
    return std::move(w.out);
}

int FaceStructure::ageIndex(uint32_t ageBit) {
    switch (ageBit) {
    case age::Toddler: return 0;
    case age::Child: return 1;
    case age::Teen: return 2;
    case age::Adult: return 3;
    case age::Elder: return 4;
    case age::Baby: return 5;
    case age::YoungAdult: return 3; // the editor maps index 7 to 3 (FUN_00802d5c)
    default: return -1;
    }
}

uint32_t FaceStructure::key(uint32_t ageBit, uint32_t genderBit, bool lowDetail) {
    return uint32_t(std::max(0, ageIndex(ageBit))) | (genderBit == gender::Male ? 0x100u : 0u) |
           (lowDetail ? 0xF0000u : 0u);
}

const std::vector<FaceStructure::Vertex>* FaceStructure::find(uint32_t ageBit, uint32_t genderBit, bool lowDetail) const {
    auto it = data.find(key(ageBit, genderBit, lowDetail));
    return it == data.end() || it->second.empty() ? nullptr : &it->second;
}

void FaceStructure::set(uint32_t k, std::vector<Vertex> vertices) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end())
        keys.push_back(k);
    data[k] = std::move(vertices);
}

} // namespace os2::cas
