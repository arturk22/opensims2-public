#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "xform.hpp"

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

    // 3ARY 0x15: the rotation of every quarter tile's floor pattern, laid
    // out as `floors`: 1, 2, 4 or 8 (0, 90, 180, 270 degrees; the
    // cTSMainToolbar rotation mode the floor tool's `,`/`.` cycle). The
    // shipped lots hold 1 everywhere (0 in a few tiles). Inferred from the
    // data: one byte per quarter, the values of the toolbar's modes.
    std::vector<std::array<uint8_t, 4>> floorRotations;

    // 3ARY 0x03: the locked tiles (cWorldDB +0xa0, a byte per tile,
    // [level][x][y] over width x height; non-zero = locked), read by
    // cWorldDB::InitForNewLot from the lot template (turned with the lot)
    // and kept in the lot file. cIWorldDB slot 0xc4 (IsLocked(tile), the
    // symxfer's "HasFloorTest") reads it: the build tools refuse a locked
    // tile (partitions 0x45, vertices 0x62) and Buy mode too
    // (IsOnLockedTile 0x02), unless the toolbar's TilesUnlocked cheat is on.
    // The templates lock the 10-tile road strip (docs/buy-build-mac.md
    // §7.20). Empty when the lot has none.
    std::vector<uint8_t> locked;
    bool tileLocked(int level, int x, int y) const {
        if (locked.empty() || !hasLevel(level) || x < 0 || y < 0 || x >= int(width) || y >= int(height))
            return false;
        return locked[(size_t(level - minLevel) * width + size_t(x)) * height + size_t(y)] != 0;
    }

    // Room ID of every quarter tile, laid out as `floors`; 0 is outdoors.
    // The game keeps three room maps (cPartitionManager, docs/buy-build-mac.md
    // §7.3), each flooded from the walls of its own wall graph: 3ARY 0x09
    // (`rooms`, room definition 2) from the partitions that block placement
    // (WGRA 5: every wall but the deck skirts, and fences), which the VM's
    // objects take as their room; 0x0A (definition 1) from the load-bearing
    // ones (WGRA 6); 0x0B (definition 0, `roomLayers`) from the light-blocking
    // ones (WGRA 7: walls, attic and foundation walls, no fences), which the
    // light maps use, so a fenced yard is a room of its own in 0x09 and
    // outdoors (0) in 0x0B.
    std::vector<std::array<uint16_t, 4>> rooms;

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
        bool layered = true; // has a wall layer entry (some walls of the graph have none)
    };
    enum WallType : uint32_t { Normal = 1, Attic = 3, DeckSkirt = 4, Foundation = 0x17, Pool = 0x1D };
    std::vector<Wall> walls;

    // Every wall graph (WGRA) as read. The game's partition manager keeps
    // one per class of partition (docs/buy-build-mac.md §7): instances 5 and
    // 0x18 hold every partition by its wall ID (the IDs of the wall layer),
    // the others a subset with IDs of their own: 6 load-bearing walls, 7
    // light-blocking ones, 0x16 floor-to-floor walls of standard thickness,
    // 0x17 attic walls, 8 deck skirts, 0x13 none seen. Each line keeps the
    // room on its right and on its left (seen from a to b) from the room
    // array of its graph: 5 -> 3ARY 0x09, 6 -> 0x0A, 7 -> 0x0B (found in the
    // saved lots). Build mode edits them (lot_edit.hpp) and lot_write.hpp
    // writes them back.
    struct WallGraph {
        uint32_t instance = 0, version = 2;
        // 0, 0, lowest level, two sizes (units unknown), level count, next
        // vertex ID, next line ID.
        std::array<int32_t, 8> header{};
        struct Vertex {
            uint32_t id = 0;
            float x = 0, y = 0;
            int level = 0;
        };
        std::vector<Vertex> vertices;
        std::vector<int32_t> rooms; // every room ID of the graph's room array
        struct Line {
            uint32_t id = 0, a = 0;
            int32_t roomRight = 0;
            uint32_t b = 0;
            int32_t roomLeft = 0;
        };
        std::vector<Line> lines;
    };
    std::vector<WallGraph> wallGraphs;
    // The room arrays of the other graphs (3ARY 0x0A, 0x0B), laid out as `rooms`.
    std::map<uint32_t, std::vector<std::array<uint16_t, 4>>> roomLayers;
    // Wall layer entries of IDs no graph has (stale ones the game keeps),
    // written back as they are.
    struct LayerEntry {
        uint32_t type = 0;
        std::array<uint16_t, 2> patterns{0xFFFF, 0xFFFF};
    };
    std::map<uint32_t, LayerEntry> staleWallLayer;
    // Fences are walls whose type is the fence's catalog GUID (0x2CB230B8
    // property sets); some also carry coverings (half walls).
    static bool isFence(uint32_t type) { return type > 0xFF; }

    // FPL 0xAB4BA572 (cFencePostLayer, instance 0x11): one post per fence
    // corner or end, with the fence GUID.
    struct FencePost {
        int level = 0;
        float x = 0, y = 0;
        uint32_t fence = 0;
    };
    std::vector<FencePost> fencePosts;

    // ROOF 0xAB9406AA: one entry per roof drawn in build mode, over the
    // rectangle from (x0, y0) to (x1, y1) on `level` (its base is that level's
    // floor). `pattern` is the catalog GUID of the roof (0xACA8EA06 property
    // sets); the game uses one pattern for the whole lot.
    struct Roof {
        uint32_t id = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int level = 0, levelTo = 0;
        uint32_t type = 0; // see RoofType
        uint32_t pattern = 0;
        float angle = 0.5f; // rise per half tile (version 2+)
        uint32_t style = 0; // version 3+
    };
    enum RoofType : uint32_t {
        ShedGable = 1, ShedHip, ShortGable, LongGable, Hip, Mansard, Cone, Dome, Octagonal,
        DiagonalLongGable, DiagonalShortGable, DiagonalHip, DiagonalShedGable, DiagonalShedHip,
        GreenhouseLongGable, PagodaHip, PagodaLongGable, PagodaShedGable, DiagonalPagodaHip,
        DiagonalPagodaLongGable, DiagonalPagodaShedGable
    };
    std::vector<Roof> roofs;

    // POOL 0x0C900FDB: one entry per pool, its tiles from (x, y) to
    // (x + width, y + height) on `level`; the water surface is `depth` below
    // that level's floor.
    struct SwimmingPool {
        uint32_t id = 0;
        float x = 0, y = 0;
        int level = 0;
        uint32_t width = 0, height = 0;
        float depth = 0;
        uint32_t unknown1 = 1, unknown2 = 0; // around the depth (1 and 0 in the saved lots)
    };
    std::vector<SwimmingPool> pools;

    // 2ARY 0x3B76: terrain water level at every grid point, [y][x] over
    // (width+1) x (height+1); water shows where it is above the terrain.
    std::vector<float> water;

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
        // The model's bones as the object was last posed (name, local
        // transform; nTSSG::cObject's saved transform nodes): what the game
        // draws it with until it plays another animation.
        std::vector<std::pair<std::string, Xform>> nodes;
    };
    std::vector<Object> objects;

    std::vector<std::string> warnings; // parts that could not be read

    static Lot load(DbpfPackage& package);

    bool hasLevel(int level) const { return level >= minLevel && level < minLevel + levelCount; }
    // Elevation of grid point (x, y) on a level; clamped to the grid.
    float elevation(int level, int x, int y) const;
    // Elevation at a point inside the grid, bilinear between the grid points.
    float elevationAt(int level, float x, float y) const;
    // Water level at grid point (x, y); -infinity without water data.
    float waterLevel(int x, int y) const;
    const std::array<uint16_t, 4>& floor(int level, uint32_t x, uint32_t y) const;
};

} // namespace os2
