#pragma once

// The lot's light maps: what the game's walls, floors and terrain are lit by
// instead of lights (docs/shaders-mac.md 5-7, docs/lighting-mac.md 5).
//
// The game keeps floorLightMap_<page> textures (512 x 512, page 0 covering
// the lot at 8 texels per tile: the terrain samples it at position / 64) and
// wallLightMap_<page> textures (wall tiles of 8 x 24 texels, a 3-tile story).
// Here all of them live in one atlas: a floor page per level, stacked, then a
// cell per wall face (a wall side between two grid points) packed below. The
// shaders draw walls as 2 x wallpaper x light map, floors as 2 x base x light
// map x diffuse and the terrain as 2 x 0.9 x light map x canvas, so a
// light map value of 0.5 shows the texture as it is.
//
// The light maps are filled as docs/lighting-mac.md 5 decodes the game's
// cDiffusionManager: a grid of half-tile samples per level, cleared to the
// inside ambient, seeded by each light (max per channel) and propagated
// through the samples of the same room, dividing the light by 1 + k at each
// half-tile step (k = the light's falloff x floorThreshold, x 1.4 on
// diagonals); dark rooms get the unlitRoomColor added as the lighting
// manager does it. See computeLotLightMap for what is inferred.

#include "image.hpp"
#include "lighting_config.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os2 {

struct Lot;

struct LotLightMapLayout {
    // Floor pages: 8 texels per tile (floorTextureSize 512 / 64 tiles) and
    // wall cells: 8 texels per tile along the wall, 8 per tile up
    // (wallTileSizeX 8, wallTileSizeY 24 for 3 tiles).
    static constexpr int kTexelsPerTile = 8;

    uint32_t width = 0, height = 0; // atlas size in texels
    uint32_t lotWidth = 0, lotHeight = 0;
    int minLevel = 0, levels = 0;
    // Room of every quarter tile (west, north, east, south), laid out as
    // Lot::rooms, and the grid points' elevations, as Lot::elevations.
    std::vector<std::array<uint16_t, 4>> rooms;
    std::vector<float> elevations;
    // Floor of every quarter tile (0: none), laid out as Lot::floors, and
    // the material of each (an index into materialColors, 0: unknown, white),
    // for GatherRoomLighting's floor colour (set by buildLotModel).
    std::vector<std::array<uint16_t, 4>> floors;
    std::vector<std::array<uint16_t, 4>> floorMaterials;
    std::vector<std::string> materialNames{""};
    std::vector<std::array<float, 3>> materialColors{{1, 1, 1}}; // mean colour of each material's texture
    uint16_t materialIndex(const std::string& name);
    std::array<float, 3> floorColorAt(int level, float x, float y) const;
    bool hasFloorAt(int level, float x, float y) const;

    // One side of a wall stretch: texels (i, j) of its cell sit at
    // origin + along x (i + 0.5) / 8, at height top - (j + 0.5) / 8 (row 0
    // is the top), with a one-texel border repeating the edge.
    struct WallFace {
        std::array<float, 2> origin{}; // on the face, where `s` is 0
        std::array<float, 2> along{1, 0};
        std::array<float, 2> normal{0, 1};
        float length = 0, base = 0, top = 0;
        int level = 0;
        uint16_t room = 0;    // the room the side faces, 0 outside
        uint16_t material = 0; // index into materialColors (the wallpaper)
        uint32_t x = 0, y = 0; // the cell's corner in the atlas (border included)
        uint32_t w = 0, h = 0; // cell size with the border
        // The wall's tile on the grid (its ends a -> b, as `along` runs; both
        // zero: from origin and length), the wall diffusion's segment.
        std::array<float, 2> wallA{}, wallB{};
    };
    std::vector<WallFace> walls;
    // The partitions that block light (walls.txt blocksLight: walls,
    // attic and foundation walls, pool walls; not fences, deck skirts and
    // the other thin partitions), on grid points: the floor diffusion's
    // neighbour bits (docs/lighting-mac.md 10.4).
    struct LightWall {
        int level = 0;
        int ax = 0, ay = 0, bx = 0, by = 0;
    };
    std::vector<LightWall> lightWalls;

    // The sun's height map (cHeightMapShadow, docs/lighting-mac.md 5.3):
    // what stands over the lot at every ground texel (8 per tile, rows of
    // lotWidth x 8): the roofs and the upper floors (FillFromRooves,
    // FillFromTerrain; -1e9 where nothing; the terrain is added from the
    // elevations), and the walls as segments with their tops at both ends
    // (FillFromWalls), drawn into the grid for the sun's direction when the
    // light maps are made. Filled by buildLotModel (fillSunOccluders);
    // both empty: no sun shadows.
    std::vector<float> occluders;
    float occluderAt(float x, float y) const;
    struct SunWall {
        std::array<float, 2> a{}, b{}; // ends, tiles (grid points)
        float topA = 0, topB = 0;       // the wall's top over each end
    };
    std::vector<SunWall> sunWalls;

    void init(const Lot& lot);
    // Atlas position in texels of a point on a floor page, `x`, `y` in
    // tiles (divide by width/height once every wall is added).
    std::array<float, 2> floorTexel(int level, float x, float y) const;
    // Adds a wall face cell (sized from its length and heights) and returns its index.
    size_t addWall(WallFace face);
    // Atlas position in texels of a point on a wall face: `s` along it, `z` height.
    std::array<float, 2> wallTexel(size_t face, float s, float z) const;
    // Room of the quarter tile under (x, y) on a level, 0 outside the lot.
    uint16_t roomAt(int level, float x, float y) const;
    // Outdoors: room 0 of the light's rooms (3ARY 0x0B); for lots without
    // that map, room 0 or a room of 0x09 most of whose quarter tiles have
    // no floor (inferred).
    bool outdoor(int level, uint16_t room) const;
    // A room of the VM (3ARY 0x09, which fences split) that is outdoors for
    // the light: most of its quarters are outdoors in 0x0B.
    bool outdoorPlacementRoom(uint16_t room) const;
    // Height of a level's floor at (x, y), as Lot::elevationAt.
    float elevationAt(int level, float x, float y) const;

    uint32_t pageHeight() const { return lotHeight * kTexelsPerTile; }
    uint32_t shelfX_ = 0, shelfY_ = 0, shelfH_ = 0; // wall cell packing
    std::vector<uint16_t> outdoorRooms_, outdoorPlacementRooms_;
};

// A light feeding the light maps (a cDiffusionLightSource): a lamp of an
// object's model at its Change Light level, or a window's or door's light
// ("portal"), with its diffusionSource* attributes (lightAttribOverride).
// The global (`set`) parameters of Lighting.txt the light maps use, with
// the game's values as defaults.
struct LightMapGlobals {
    std::array<float, 3> outsideAmbient{0.25f, 0.25f, 0.25f};     // outsideDiffusionAmbientColor x outsideFloorAmbient
    std::array<float, 3> outsideWallAmbient{0.25f, 0.25f, 0.25f}; // ... x outsideWallAmbient
    float terrainStrength = 1;     // the state's direct light on the outdoor floors and terrain
    float floorLightMapScale = 0.5f; // cLightMapManager +0x68: the direct light is stored halved
    float wallLightMapScale = 0.5f;  // cLightMapManager +0x6C (the walls' page scale, also 0.5)
    // Height-map sun shadows (Shadows $Medium/$High: heightMapShadows): a
    // texel d under the shadow's top keeps the sun's weight x max(0, 1 - d
    // x hmSunLightTransitionZone) (LightTerrainTileWithShadows, Mac BV 0x705068).
    bool sunShadows = true;
    float sunTransitionZone = 1;
    float roomSaturation = 0.5f, roomCeilingStrength = 1, roomPortalStrength = 0.75f;
    float brightWallFloor = 1, darkWallFloor = 0.5f; // bright/darkWallFloorContribution
    int propagationSteps = 50;                       // wallPropagationSteps (both grids)
    float spillPlaneOffset = 0.1f;                   // a portal's place in front of its wall
    static LightMapGlobals from(const LightingConfig& config);
};

struct LightMapLight {
    std::array<float, 3> position{};
    std::array<float, 3> color{}; // diffusionSourceIntensity x diffusionSourceColor x level (0-1)
    float falloff = 1;            // diffusionSourceFalloff
    float floorScale = 1;         // diffusionSourceFloorIntensityScale
    float wallScale = 1;          // diffusionSourceWallIntensityScale
    float interFloor = 0.5f;      // diffusionSourceInterFloorFalloff
    bool directional = false;     // diffusionSourceIsDirectional
    float directionalFalloff = 0; // diffusionSourceDirectionalFalloff
    std::array<float, 3> direction{0, 0, -1}; // where the light points (its -Z)
    int level = 0;                // the object's level
    bool portal = false;
};

struct LotLightMap {
    RgbaImage image; // the atlas, RGBA8, alpha 255
    // GatherRoomLighting (docs/lighting-mac.md 4.1): each indoor room's
    // irradiance by the direction the light travels, +X, -X, +Y, -Y, +Z
    // (up from the floor), -Z (down from the ceiling), weighted means of its
    // floor, walls and portals; by (level, room).
    std::map<std::pair<int, uint16_t>, std::array<std::array<float, 3>, 6>> rooms;
};

// The sun's height-map shadow tops over the lot's ground texels (rows of
// lotWidth x 8) for a sun direction `L` towards the sun, as the game's
// FindHeightMapShadows sweeps them (docs/lighting-mac.md 5.3); -1000 where
// there is no shadow (no sun, sun overhead), empty without a height map.
std::vector<float> sunShadowTops(const LotLightMapLayout& layout, std::array<float, 3> L);

// The light maps for a lighting state (docs/lighting-mac.md 4.1, 5, 5.3,
// 5.4). From the binary: the inside ambient (insideAmbientColor x
// insideFloorAmbient / insideWallAmbient) and the outside one
// (outsideDiffusionAmbientColor x outsideFloorAmbient, 0.25); the seeds
// (colour x diffusionSourceFloorIntensityScale, or WallIntensityScale, x the
// modifier's diffusion strength: insideObjects / outsideObjects for lamps,
// insidePortals(Spills) / outsidePortals(Spills) for windows and doors by the
// side they shine into); the propagation rule (if neighbour - c > k c:
// c = neighbour / (1 + k) + 0.01) over up to floorPropagationSteps (50)
// passes with sources of equal falloff together, strongest falloff first,
// k = falloff x the modifier's diffusion falloff x floorThreshold.
// Outdoor texels lose the outside ambient
// again (it only shapes the falloff) and get the state's direct light
// E(n) = (1 - sum w) skylight + sum w colour, w = max(0, n.dir) x intensity
// over sun, sky, fill1, fill2, x terrainStrength x 0.5 (the floor light
// map's scale), n the terrain's normal (up on floors). Portals:
// FindPortalLightValues (outside light coming in: sun x max(0, sun . out) +
// skylight + the light outside, + half of it as spill; a room's light going
// out or to another room: its light map). Room irradiance:
// GatherRoomLighting, floor and wallpaper colours mixed with white by
// roomSaturation, ceiling x roomCeilingStrength, portals x roomPortalStrength.
// Inferred (not decoded):
// - The light directions are normalized (unnormalized, E would pass 1 by
//   day); the sun's height-map shadow (alpha) is left out.
// - Light passes between samples of the same room (or both outdoors): the
//   game's neighbour bits block it at walls.
// - A portal is a window's or door's light with a room on each side (found
//   across it); its light map value is the sample on the side it comes from,
//   taken after the lamps have diffused (the portals then diffuse too).
// - Walls: no grid of their own; each texel takes the max of its ambient,
//   each same-room light's seed divided by 1 + k (k from wallThreshold) per
//   half tile of distance, and the floor under it (x brightWallFloor on walls
//   a light reaches, darkWallFloor on the others) divided likewise going up;
//   walls facing out are lit like the outdoor floor (less the outside wall
//   ambient, plus 0.5 E(normal)).
// Unlit rooms as cLightingManager::FindUnlitRooms / BrightenUnlitRooms
// (docs/lighting-mac.md 6.1): after the room irradiance, a room whose mean
// irradiance's largest channel m is under the threshold t gets
// unlitRoomColor x (1 - m / t) added to its floor and wall texels (x 0.5)
// and to five of its six irradiance colours.
LotLightMap computeLotLightMap(const LotLightMapLayout& layout, const LightingState& state,
                               const std::vector<LightMapLight>& lights, const LightMapGlobals& globals = {});

} // namespace os2
