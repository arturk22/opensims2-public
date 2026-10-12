#pragma once

// The neighborhood's ground and what stands on it, as the neighborhood view
// draws them: the terrain height map (0xABCB5DA4), the occupants (0xABD0DC63:
// trees, roads, bridges and props), the neighborhood objects' catalog
// (0x6D619378 property sets: GUID -> model) and the terrain tunables of
// TSData/Sys/NeighborhoodTerrain.ini, Neighborhood.ini and Terrain.ini.
//
// Formats as OpenTS2 reads them (NeighborhoodTerrainCodec,
// NeighborhoodDecorationsCodec, NeighborhoodObjectCodec), checked here to
// consume every byte of the four base-game neighborhoods.
//
// Space: the game's (Z up, 1 unit per lot tile); the terrain grid is
// `kHoodCellSize` units per cell (a 10x10-tile lot is one cell), vertex
// (i, j) at (i * 10, j * 10, height).

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

namespace hoodtype {
constexpr uint32_t Terrain = 0xABCB5DA4;     // height map ("NHTG")
constexpr uint32_t Occupants = 0xABD0DC63;   // trees, roads, bridges, props ("NHTR")
constexpr uint32_t ObjectEntry = 0x6D619378; // neighborhood object catalog entry (XML property set)
constexpr uint32_t SavedCamera = 0xEC44BDDC; // camera state (also 3 per lot)
} // namespace hoodtype

namespace hood {

constexpr float kHoodCellSize = 10.0f; // neighborhood.matShad terrainCellSize

struct Terrain {
    uint32_t version = 0;
    uint32_t width = 0, height = 0; // cells
    float seaLevel = 0;
    std::string type; // "Temperate", "Desert", ...
    uint32_t gridWidth = 0, gridHeight = 0; // vertices (cells + 1)
    std::vector<float> heights;             // gridWidth x gridHeight, x fastest
    std::vector<uint8_t> vertexBytes;       // version 4: one byte per vertex (unknown; zero in the base game)

    static Terrain parse(std::span<const uint8_t> data);
    // parse's inverse (cTSNHoodTerrain::Write: version 4, both layers).
    std::vector<uint8_t> write() const;

    float vertex(int i, int j) const; // clamped to the grid
    // Height at a world point as GetTerrainHeight: 0 outside the terrain, else
    // over the tile's triangle (split along its (0,0)-(1,1) diagonal).
    float heightAt(float x, float y) const;
    float worldWidth() const { return float(width) * kHoodCellSize; }
    float worldHeight() const { return float(height) * kHoodCellSize; }
};

struct OccupantPosition {
    std::array<float, 3> position{}; // world
    std::array<float, 2> boundsMin{}, boundsMax{};
};

struct Flora {
    OccupantPosition at;
    float rotation = 0; // degrees about Z
    uint32_t guid = 0;  // neighborhood object
};

// One road tile: the quad already laid on the terrain, with its UVs, and the
// piece (texture "<type>_roads_<piece | 4>").
struct Road {
    OccupantPosition at;
    std::array<std::array<float, 3>, 4> corners{};
    std::array<std::array<float, 2>, 4> uvs{};
    uint32_t piece = 0, underTexture = 0;
    uint8_t flags = 0, connections = 0;
    struct Addon {
        uint32_t id = 0;
        std::array<float, 3> a{};
        std::array<float, 2> b{};
    };
    std::vector<Addon> addons;
};

// A road with a model ("RoadOccupantWithModel"): only bridges, "%04x-bridge"
// of the piece's high 16 bits.
struct Bridge {
    Road road;
    std::array<float, 3> offset{};
    std::array<float, 4> orientation{0, 0, 0, 1}; // quaternion x, y, z, w
    std::array<float, 3> unknown{};
};

struct Prop {
    OccupantPosition at;
    uint32_t guid = 0;
    float rotation = 0;
};

struct Occupants {
    uint32_t version = 0;
    uint16_t floraVersion = 0, roadVersion = 0, bridgeVersion = 0, propVersion = 0;
    std::vector<Flora> flora;
    std::vector<Road> roads;
    std::vector<Bridge> bridges;
    std::vector<Prop> props;
    // Where each flora and prop record and the two counts sit in the
    // resource (for removeOccupants).
    std::vector<std::pair<size_t, size_t>> floraSpans, propSpans; // [begin, end)
    size_t floraCountAt = 0, propCountAt = 0;

    static Occupants parse(std::span<const uint8_t> data);
    // The resource without the flora and props of the given indices.
    static std::vector<uint8_t> removeOccupants(std::span<const uint8_t> data, const Occupants& parsed,
                                                const std::vector<size_t>& flora, const std::vector<size_t>& props);
};

// Neighborhood objects (catalog 0x6D619378): GUID -> CRES name ("..._cres").
std::map<uint32_t, std::string> loadNeighborhoodObjects(ResourceIndex& index);

// The saved neighborhood camera (0xEC44BDDC in the neighborhood package):
// inferred from N001-N003, where the floats sit after a 0x40-byte zero
// block and 9 header bytes: zoom distance, then (after a u32) heading and
// pitch in radians and the pivot.
struct SavedCamera {
    float distance = 0;
    float heading = 0, pitch = 0; // radians
    std::array<float, 3> pivot{};
    static bool parse(std::span<const uint8_t> data, SavedCamera& out);
};

// TSData/Sys/NeighborhoodTerrain.ini, Neighborhood.ini and Terrain.ini.
struct TerrainSettings {
    struct Paint {
        float maxStrength = 1;
        std::string texture; // "nh-temperate-wet": variants "-00", "-01", ...
        int variants = 1;
        std::string lotPaint;
        std::vector<std::array<float, 2>> spread; // (distance, strength) points; last strength is 0
        float spreadAt(float distance) const;
        std::string variantTexture(int v) const; // "nh-temperate-wet-01_txtr"
    };
    std::map<uint32_t, Paint> paints;
    struct TypeInfo {
        uint32_t cliff = 0x10, beach = 0x11;
    };
    std::map<std::string, TypeInfo> types;
    // [PaintMapTable_<type>]: 32 altitude rows x 16 road proximity columns of paint IDs.
    std::map<std::string, std::vector<std::vector<uint32_t>>> paintMaps;

    float maxBeachAltitude = 4;
    float maxBeachWidthInCells = 2;
    float heightRangeForTemperature = 500;
    float cliffAngleMin = 52, cliffAngleMax = 56;
    float waterDepthThresholdForPaint = 50;
    float maxRoadProximity = 20;
    float textureVariantGridSize = 13;
    // Neighborhood.ini
    float imposterZOffset = 0.5f;
    // [LotPlopTunables] and [SmoothingUnderNewLot] (the lot tools; the
    // binary's defaults without the ini): degrees, world units, degrees, passes.
    float maxLotSlope = 30, highlightOffset = 3;
    float smoothingTalusAngle = 2;
    int smoothingPasses = 8;
    // Terrain.ini [WaterRenderParameters]
    std::vector<std::array<float, 2>> waterAlphaVsDepth; // (depth, alpha 0-255)

    static TerrainSettings load(const std::filesystem::path& sysDir);
    static TerrainSettings parse(const std::string& terrainIni, const std::string& neighborhoodIni = {},
                                 const std::string& lotTerrainIni = {});
};

} // namespace hood
} // namespace os2
