#pragma once

// The game's light-map diffusion, as nTSSG::cDiffusionManager runs it
// (docs/lighting-mac.md 10.4, 10.5): a grid of half-tile samples per level
// (cFloorColorGrid) whose neighbour bits come from the light-blocking walls,
// seeded by the light sources of each room and relaxed, then copied into the
// floor pages through a 4 x 4 guard per tile; and the walls
// (cWallDiffusionManager): straight runs of wall tiles, 2 x 6 half-tile
// samples each, lit by the sources in front of them, filled from the floor
// light map, propagated inside the run and between stories, and copied into
// 8 x 24 cells. The colours the walls keep are bytes, as the game's.

#include "lot_lightmap.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace os2 {

// A cDiffusionLightSource (docs/lighting-mac.md 10.3).
struct DiffusionSource {
    std::array<float, 3> rgb{};      // colour x intensity (x the modifier's diffusion, x the lamp's level)
    std::array<float, 2> position{}; // the light's (x, y), tiles
    std::array<float, 4> rect{};     // minX, minY, maxX, maxY in the world (diffusionSourceMin/Max placed)
    int level = 0;
    uint16_t room = 0;               // light room (3ARY 0x0B) at the light; 0 outdoors
    float falloff = 1, floorScale = 1, wallScale = 1, interFloor = 0.5f;
    bool directional = false;
    float facing = 0, directionalFalloff = 0; // facing: atan2 of where the light points
};

struct DiffusionParams {
    std::array<float, 3> insideFloor{}, outsideFloor{}; // ambient colour x insideFloorAmbient / outsideFloorAmbient
    std::array<float, 3> insideWall{}, outsideWall{};   // ... x insideWallAmbient / outsideWallAmbient
    float floorThreshold = 0.4f, wallThreshold = 0.4f;
    int steps = 50;                                      // wallPropagationSteps (both grids)
    float brightWallFloor = 1, darkWallFloor = 0.5f;
    float scale = 0.5f;                                  // the light maps' scale (cLightMapManager +0x68 / +0x6C)
    bool darkCorners = true;                             // DarkenCornersNextToDarkWalls (global byte, on)
    // The walls take the floor from the grids (DoDiffusionPassOptimized,
    // the pass the game runs after the first one with Graphics Rules'
    // useDirtyTiles: LookupFloorDiffusion) instead of the floor page's
    // texels (DoDiffusionPass: LookupFloorLightMap); docs/lighting-mac.md 11.3.
    bool optimizedPass = true;
};

// One level's cFloorColorGrid.
struct FloorColorGrid {
    struct Half {
        std::array<float, 3> c{};
        uint8_t nb = 0xFF, sel = 0; // light leaves in direction d; reaches the neighbour's half 1
        bool outside = true, exists = false, dirty = false, next = false;
        uint16_t room = 0;
    };
    struct Sample {
        Half h[2];
    };
    int level = 0;
    int width = 0, height = 0;       // tiles
    std::vector<Sample> samples;     // (2 width) x (2 height), x fastest
    std::vector<uint8_t> tileExists; // width x height, x fastest
    std::vector<uint8_t> tileDiag;   // 0, 1 (wall on x + y = 1), 2 (wall on x = y)
    std::array<float, 3> insideAmbient{}, outsideAmbient{};
    float threshold = 0.4f;
    int steps = 50;

    Sample& at(int x, int y) { return samples[size_t(y) * size_t(2 * width) + size_t(x)]; }
    const Sample& at(int x, int y) const { return samples[size_t(y) * size_t(2 * width) + size_t(x)]; }
    // GetNeighborSample: the half light reaches from (x, y)'s half going `d`, or null.
    const Half* neighbor(int x, int y, int d, int half) const;
    Half* neighbor(int x, int y, int d, int half);

    // UpdateFromWorldDB: tiles with ground, the walls' neighbour bits, rooms.
    void build(const LotLightMapLayout& layout, int level, const DiffusionParams& params);
    // CalculateGrid: the ambient, then the sources seeding this level, smallest falloff first.
    void calculate(const LotLightMapLayout& layout, const std::vector<DiffusionSource>& sources);
    // CopyToTexturePages: the level's page, 8 texels per tile, rows from the
    // tile's -y side, RGB bytes; `written` marks the texels a tile wrote.
    void copy(std::vector<uint8_t>& rgb, std::vector<uint8_t>& written) const;
    // LookupFloorDiffusion (Mac BV 0xee88c4): 2 x the colour of the sample
    // half under (x, y) (the tile's quarter by the offset from its centre,
    // the half by the diagonal), less the outside ambient on an outside
    // half; black where there is none.
    std::array<float, 3> lookup(float x, float y) const;
};

// The floors: a grid per level (layout.levels), seeded and copied.
std::vector<FloorColorGrid> diffuseFloors(const LotLightMapLayout& layout, const std::vector<DiffusionSource>& sources,
                                          const DiffusionParams& params);

// The walls (the faces of layout.lightWalls, the light-blocking and pool
// walls SetupWalls takes; other faces stay black): for every face of
// layout.walls, its cell's texels (w x h, row 0
// at the top as the atlas keeps them), light-map bytes (0.5 x (colour -
// ambient), the outside ambient for walls facing out). `floorTexel(level,
// x, y)` gives the floor page's diffusion (its byte / 255 x 2) at a point.
// With params.optimizedPass the walls take the floor from `floors`
// (LookupFloorDiffusion), else from `floorPages` (LookupFloorLightMap).
// `lookup`, when given, gets each face's LookupWallDiffusion (Mac BV
// 0xeef45c): its segment's sample of column 1, row 3, less the outside
// ambient on walls facing out.
std::vector<std::vector<std::array<uint8_t, 3>>> diffuseWalls(
    const LotLightMapLayout& layout, const std::vector<DiffusionSource>& sources, const DiffusionParams& params,
    const std::vector<FloorColorGrid>& floors, const std::vector<std::vector<uint8_t>>& floorPages,
    std::vector<std::array<float, 3>>* lookup = nullptr);

// The light-room key of a room on a level: 0 outdoors; and of the point
// (x, y) of a level (the game's quarter of the point's tile).
uint16_t lightRoomKey(const LotLightMapLayout& layout, int level, uint16_t room);
uint16_t lightRoomAt(const LotLightMapLayout& layout, int level, float x, float y);
// HasGround (cIWorldDB vtable 0xb8): the ground level, or floor in the
// quarter of the tile under (x, y) (the game's quarter of a point).
bool groundAt(const LotLightMapLayout& layout, int level, float x, float y);

} // namespace os2
