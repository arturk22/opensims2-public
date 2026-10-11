#pragma once

// LxNR (0xCCCEF852, instance 1 of a character file): the "facial structure",
// nTSCASCore::cTSBodyPartData as the Mac BV binary saves and loads it
// (writer at BV 0x18c79b4, reader 0x18c6ae0; both unnamed, found by the
// four-character marks):
//
//   u32 version            3 or 4 in the base game's files (5 in BV); the
//                          reader takes 1..5, 3..5 share this layout
//   u32 n, u32 key[n]      the data keys, in order
//   u32 'keye' (0x6B657965)
//   u16 count, count x (f32 x, f32 y, f32 z)    a pool of distinct vectors
//                          (the writer folds equal ones, |d| < epsilon)
//   u32 'mstr' (0x6D737472)
//   per key: u16 m, m x u16   indices into the pool
//   u32 'data' (0x64617461)
//
// A key's vectors come in pairs per vertex of the face mesh of an age,
// gender and level of detail (cTSCASBodyPartSystem::ConvertFacialDataTo-
// Deltas, BV 0x805854): (position - the neutral face's position, (u, v, 0)).
// The keys seen in the game's characters: age index in the low byte (the
// editor's: 0 toddler, 1 child, 2 teen, 3 adult, 4 elder, 5 baby), 0x100
// for male, 0xF0000 for the low level of detail; every Sim holds its face
// at every age (babies' only when it was made as a baby), which is how the
// face survives growing up. The UV in each pair lets the deltas be laid on
// a mesh whose vertex order differs.

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace os2::cas {

constexpr uint32_t kTypeLxnr = 0xCCCEF852;

struct FaceStructure {
    using Vec3 = std::array<float, 3>;
    struct Vertex {
        Vec3 delta;
        float u = 0, v = 0;
        bool operator==(const Vertex&) const = default;
    };

    uint32_t version = 3;
    std::vector<uint32_t> keys;                    // in file order
    std::map<uint32_t, std::vector<Vertex>> data;  // key -> vertices

    static FaceStructure parse(std::span<const uint8_t> bytes);
    std::vector<uint8_t> write() const;

    // 0 toddler .. 4 elder, 5 baby (-1 for other age bits).
    static int ageIndex(uint32_t ageBit);
    static uint32_t key(uint32_t ageBit, uint32_t genderBit, bool lowDetail = false);
    const std::vector<Vertex>* find(uint32_t ageBit, uint32_t genderBit, bool lowDetail = false) const;
    void set(uint32_t key, std::vector<Vertex> vertices);
};

} // namespace os2::cas
