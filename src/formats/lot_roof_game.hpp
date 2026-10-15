// The game's own roofs (nTSWorld::c...RoofType and nTSWorld::cWorldRoof for
// the shapes, nTSSG::c...Roof for the mesh), read from the Mac binary:
// docs/roof-fence-water-mac.md §1.5. Types 1-6 (the base game's roofs); the
// diagonal and pagoda types stay on the OpenTS2 port in lot_roof.cpp.
#pragma once

#include "gmdc.hpp"
#include "lot.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace os2::roofgame {

constexpr float kNoElevation = -std::numeric_limits<float>::max();

// FUN_014cbe6c: two floats equal within 32 units in the last place.
bool nearlyEqual(float a, float b);

// One roof as the world knows it (the cBaseRoofType object at +8 of each
// nTSWorld::c...RoofType; FUN_019734cc and the type constructors).
struct Roof {
    uint32_t id = 0;
    int level = 0;
    uint32_t type = 0; // Lot::RoofType, 1-6
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool eaves = true;      // +0x24 (the drawn roofs and the tool's both have it)
    bool orientation = false; // +0x25: false = the slopes run along x
    bool xUp = false, yUp = false; // +0x26/+0x27: x1 >= x0, y1 >= y0
    float slope = 0.5f;     // +0x28
    float base = 0;         // +0x4c: the floor of `level` at (int(x0), int(y0))
    float minX = 0, minY = 0, maxX = 0, maxY = 0; // FUN_015aafb8 / 015aaf40 / 015ab0a8 / 015ab030
    int w = 0, h = 0;       // grid points, one per half tile
    std::vector<float> grid; // ComputeGridElevation: notches from the eave tip

    static bool supported(uint32_t type) { return type >= Lot::ShedGable && type <= Lot::Mansard; }
    Roof(const Lot::Roof& r, float baseElevation);

    float notch(int i, int j) const { return grid[size_t(j) * size_t(w) + size_t(i)]; }
    // cBaseRoofType::GetElevation(i, j) (BV 0x9c6308): base + s*g -/+ 0.42 + f(s).
    float gridElevation(int i, int j) const;
    float maxElevation() const; // GetMaxElevation (slot 0x3c)
    // GetElevation(x, y, margin) (slot 0x28, FUN_009d2cf4): the grid's two
    // triangles per half tile, the diagonal by the grid parity; kNoElevation
    // outside the bounds shrunk by `margin`.
    float elevation(float x, float y, float margin) const;
    bool contains(float x, float y) const; // slot 0x40 (0x9cb4d4)
    bool inEaves(float x, float y) const;  // slot 0x48 (0x9c57e8)
    int onPeak(float x, float y) const;     // slot 0x38: face mask, 0 if not
    int onDiagonal(float x, float y) const; // slot 0x34
    int mansardTop() const;                 // k of the mansard grid (0x9c6568)
};

// nTSWorld::cWorldRoof: the lot's roofs of types 1-6.
class World {
public:
    void add(const Roof& r) { roofs_.push_back(r); }
    const std::vector<Roof>& roofs() const { return roofs_; }
    bool empty() const { return roofs_.empty(); }

    // GetSampledElevationByRoofID(id, x, y, level, true) (0xc25590): the
    // roof's elevation with a 0.5 margin for roofs above `level`.
    float sampledElevation(size_t roof, float x, float y, int level) const;
    // IsAnyRoofAtLocation(location, true) (0xc25398): a roof of `level`
    // contains the point (eaves included).
    bool anyRoofAt(float x, float y, int level) const;
    // The highest roof DoesLocationHasClearanceHeight (0xc2435c) measures
    // over a point on `level` (roofs of levels L-1..L+2, from 0;
    // GetSampledElevationByRoofIDForClearance 0xc236b4), and whether any
    // roof stands over it or the point a level down (else it is clear).
    bool anyRoofAtOrBelow(float x, float y, int level) const;
    float clearanceTop(float x, float y, int level) const;

    // IsTriangleCenterLessThanAnyOtherRoofTriangleCenter (0xc63558).
    // `level` is the location's (the roof's own unless given: the
    // roof-meets-roof test asks it of another roof at its own level).
    bool centerBelowOtherRoof(size_t roof, float cx, float cy, bool lowerLayer, float* own = nullptr,
                              int level = std::numeric_limits<int>::min()) const;

    // The four meshes nTSSG::c...Roof::FillMeshBuilderData builds for one roof.
    struct Meshes {
        MeshGroup top, under, upperStrip, lowerStrip;
    };
    void build(size_t roof, Meshes& out) const;

private:
    std::vector<Roof> roofs_;
    std::vector<size_t> near(int level) const; // AddRoofIdToArray(level, true): levels L-1..L+2
    friend struct TriangleBuilder;
};

} // namespace os2::roofgame
