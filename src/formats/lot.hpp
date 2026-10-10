#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;

// Build-mode content of a lot package (Neighborhoods/N###/Lots/*.package),
// from the lot's "world database" blocks. Every resource starts with a 64-byte
// file name (zeros), then the block id and version.
//
// Coordinates are in tiles, X and Y along the lot's width and height, Z up.
// Levels are stories: 0 is the ground (the terrain), 1 the first floor above
// it, -1 the underground level a pool adds (then `minLevel` is -1). Arrays are
// indexed by `level - minLevel`. A house built on a foundation has its ground
// floor on level 1, 0.75 above the terrain; the foundation walls are level 0.
struct Lot {
    uint32_t width = 0, height = 0; // tiles
    int minLevel = 0;
    int levelCount = 0; // levels in the arrays, from minLevel up

    // 3ARY 0x01: elevation of every grid point, [level][x][y] over (width+1) x (height+1).
    std::vector<float> elevations;
    // 3ARY 0x00: floor of every tile, [level][x][y], four quarter-tile triangles
    // (west, north, east, south) as SMAP 0x0E references; 0 is no floor.
    std::vector<std::array<uint16_t, 4>> floors;

    // SMAP 0x0E / 0x0D: pattern reference -> pattern name ("sidewalk", "blank")
    // or catalog GUID as 8 lowercase hex digits ("ccc26098").
    std::map<uint16_t, std::string> floorPatterns, wallPatterns;

    struct WallVertex {
        float x = 0, y = 0;
        int level = 0;
    };
    // A wall, fence or other partition between two vertices (WGRA), with its
    // type and coverings from the wall layer (WLL 0x04).
    struct Wall {
        uint32_t id = 0;
        WallVertex a, b;
        uint32_t type = 0; // see Lot::WallType; larger values are fence GUIDs
        // Wall pattern references (wallPatterns) for the right and the left
        // side seen from a to b; 0xFFFF for none.
        std::array<uint16_t, 2> patterns{0xFFFF, 0xFFFF};
    };
    enum WallType : uint32_t { Normal = 1, Attic = 3, DeckSkirt = 4, Foundation = 0x17, Pool = 0x1D };
    std::vector<Wall> walls;

    // Terrain paints (LTTX + 2ARY 0x5CBC...): the base texture covers the lot
    // and each paint is blended over it by its mask over (maskWidth x
    // maskHeight) points, 4 per tile. Unlike the 3D arrays, 2D arrays are
    // stored row by row: [y][x].
    std::string terrainBase; // TXTR name without "_txtr"
    struct TerrainPaint {
        std::string texture;
        std::vector<uint8_t> mask;
    };
    std::vector<TerrainPaint> paints;
    uint32_t maskWidth = 0, maskHeight = 0;

    // Objects placed on the lot (cObject, 0xFA1C39F7, one per object instance;
    // cAnimatable/cPerson blocks may wrap it). Position is in lot space (tiles,
    // Z up, same as the elevations) and rotation a unit quaternion (x, y, z, w)
    // about the model's origin. Objects with no model (controllers, phone
    // services, ...) have an empty `model`; ones not in the world (inventory,
    // carried, hidden) sit at the origin.
    struct Object {
        uint32_t instance = 0;
        std::string definition; // OBJD file name + "_objt" ("Trash Can - Outside_objt")
        std::string model;      // CRES name without "_cres"; "nullModel" for invisible ones
        // Design choices: subset -> material (TXMT name without "_txmt") that
        // replaces the SHPE's material for that subset.
        std::map<std::string, std::string> materials;
        std::array<float, 3> position{0, 0, 0};
        std::array<float, 4> rotation{0, 0, 0, 1};
        bool person = false; // a Sim (cPerson): models are body parts
    };
    std::vector<Object> objects;

    std::vector<std::string> warnings; // parts that could not be read

    static Lot load(DbpfPackage& package);

    bool hasLevel(int level) const { return level >= minLevel && level < minLevel + levelCount; }
    // Elevation of grid point (x, y) on a level; clamped to the grid.
    float elevation(int level, int x, int y) const;
    const std::array<uint16_t, 4>& floor(int level, uint32_t x, uint32_t y) const;
};

} // namespace os2
