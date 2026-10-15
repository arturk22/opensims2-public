#pragma once

// Renderable models of the neighborhood view, built from hood_terrain.hpp's
// data with materials the renderer already draws (the game draws them with
// the shaders of neighborhood.matShad, NHoodRoadMaterialDef and
// imposter.matShad in Materials.package):
//
// - Terrain (NeighborhoodCanvas + NeighborhoodTerrainPaint +
//   NHoodTerrainLighting): the canvas is black and every paint is added
//   over it (srcAlpha + one), its texture repeating every
//   `nhoodPaintTextureScale` (13) tiles and scaled by its 8-bit alpha map;
//   the result is multiplied by the neighborhood light map
//   ("nhoodLightMap"). The paints are decided at load time as
//   cTSNHoodTerrain::DecideSurfaceCover does (docs/neighborhood-mac.md 2.3):
//   per vertex the cliff paint where |n.z| < cos(CliffAngleMin) (only on
//   that vertex), the beach paint within MaxBeachWidthInCells of water and at
//   most MaxBeachAltitude above it, else [PaintMapTable_<type>] at row
//   31 - (h - sea) x 31 / HeightRange and the road proximity column; beach
//   and table paints spread by their curves; each vertex keeps its 3 heaviest
//   paints (AddLandTypeAtVertex) and NormalizePaintWeights turns them into
//   the alpha bytes (docs/neighborhood-mac.md 20.1). Each tile is drawn with
//   one texture variant per paint, from a byte drawn per cell of
//   TextureVariantGridSize tiles (InitPaintVariants, 20.2).
// - Roads (neighborhoodroads.matShad): each road tile's saved quad and UVs
//   with "<type>_roads_<piece | 4>" clamped, alpha blended, x the light map.
// - Water (NeighborhoodWater's fallback without reflections): a plane at the
//   sea level, colour (0.4, 0.6, 0.9) at alpha 0.6.
// - The light map: computeHoodLightMap.
// - Lot imposters (imposter.matShad): imposterMaterials rewrites their
//   materials for the renderer.

#include "hood_terrain.hpp"
#include "image.hpp"
#include "model.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

struct LightingState;

namespace hood {

// The terrain's vertex normals as cTSNHoodTerrain keeps them (BV
// FUN_004d1e00, docs/neighborhood-mac.md 20.1): two normalised faces per
// tile, split along its (0,0)-(1,1) diagonal, and per vertex the normalised
// sum of the six faces that touch it. gridWidth x gridHeight, x fastest.
std::vector<std::array<float, 3>> terrainNormals(const Terrain& t);

// The paints per terrain vertex (gridWidth x gridHeight each), by paint ID:
// NormalizePaintWeights' alpha byte, floor(weight x 250 / sum) of the
// vertex's (at most 3) quantised weights; 0 where the paint is not.
struct PaintWeights {
    std::vector<uint32_t> paints;              // paint IDs
    std::vector<std::vector<uint8_t>> alpha;   // per paint, per vertex
    std::vector<uint32_t> assigned;            // per vertex: the paint it took
};
PaintWeights computePaintWeights(const Terrain& t, const Occupants& o, const TerrainSettings& s);

// The game's random numbers (FUN_00171674): s = s x 0x41c64e6d + 0x3039,
// next(n) = n x bits 16..47 of that 64-bit product >> 32.
struct GameRandom {
    uint32_t state = 0;
    uint32_t next(uint32_t n);
};
// The seed the game gives it when asked for -1 (FUN_00171508):
// time(0) ^ (clock() << 16 | clock()).
uint32_t clockSeed();

// The texture variants (nTSSG::cNHoodTerrain::InitPaintVariants, BV
// 0x6d8d50): the tiles in cells of `grid` (TextureVariantGridSize, an
// integer), one random byte (next(256)) per cell drawn once per load; a
// tile's variant of a paint with n textures is its cell's byte mod n
// (AddToPaintSurf, BV 0x6da3b8).
struct TerrainVariants {
    uint32_t grid = 4, cellsX = 0, cellsY = 0;
    std::vector<uint8_t> bytes;
    static TerrainVariants make(const Terrain& t, const TerrainSettings& s, uint32_t seed);
    int variant(uint32_t tileX, uint32_t tileY, int variants) const;
};

// Per vertex, ComputeRoadProximityValues: tiles to the nearest road tile by
// flooding, as a paint map column (x 16 / MaxRoadProximityValue, at most 15).
std::vector<float> roadProximity(const Terrain& t, const Occupants& o, const TerrainSettings& s);
// Per vertex, ComputeWaterProximityValues: tiles to the nearest vertex
// under water (flooding from MaxBeachWidthInCells + 2).
std::vector<float> waterProximity(const Terrain& t, const TerrainSettings& s);

// Light-map coordinates of a world point (the map spans the terrain).
inline std::array<float, 2> hoodLightMapUv(const Terrain& t, float x, float y) {
    return {x / t.worldWidth(), y / t.worldHeight()};
}

// `hiddenTiles` (width x height, x fastest; may be null): tiles whose two
// triangles are left out (cLotImposterDisplay::HideOrShowTerrainUnderImposter).
// The model: a black canvas part, then one part per paint and variant
// (shape "paint <variant> <variants>") with that paint's alpha map (one
// value per vertex, at the tile's middle: the game samples its texture at
// vertex / 256 on texel borders) and the tiles of that variant where the
// paint is at a vertex of [x - 1, x + 2] x [y - 1, y + 2] (GetPaintsInArea),
// added over the canvas.
ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  const TerrainVariants& variants, std::vector<std::string>* report = nullptr,
                                  const std::vector<uint8_t>* hiddenTiles = nullptr);
// A lot's imposter terrain (cLotImposterDisplay::TryCreateLotImposterTerrain /
// CreateLotImposterTerrainGeometry, BV 0x7692b8 / 0x7686cc): the imposter's
// "terrain" geometry (`mesh`, in world coordinates) drawn with the
// neighborhood terrain's paints of `terrain` (its layers and alpha masks,
// cropped to the mesh; each triangle with the variant of the tile it is in),
// where the neighborhood terrain is hidden.
ObjectModel buildHoodTerrainPatch(const ObjectModel& terrain, const Terrain& t, const MeshGroup& mesh,
                                  const TerrainVariants& variants);
// The game makes a lot's imposter terrain only when the imposter's ground has
// neither 4 vertices (flat) nor one per vertex of the lot's rectangle
// ((w + 1) x (d + 1)); for the others the neighborhood terrain stays.
bool needsImposterTerrain(size_t groundVertices, uint32_t widthTiles, uint32_t depthTiles);
ObjectModel buildHoodRoadModel(const Terrain& t, const Occupants& o);
ObjectModel buildHoodWaterModel(const Terrain& t);

// A tree's raised patch in the light map's height map
// (cLightingManager::AddBlobToHeightMap, BV 0x7010d4): a rectangle in world
// units and the tree model's height.
struct LightMapBlob {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float height = 0;
};
// SetupFloraLighting (BV 0x785020): the tree's saved bounding rectangle,
// its half sizes x 0.7 round the centre, truncated; only when 10 or more
// across in x or y. `modelHeight`: the model's z extent (ComputeModelHeight).
std::optional<LightMapBlob> floraBlob(const Flora& f, float modelHeight);

struct HoodLightMapParams {
    float shadowStrength = 0.47f;    // Lighting.txt hmNHoodShadowStrength (0: no height-map shadows)
    float blobHeightModifier = 0.09f; // hmBlobHeightModifier
    float blobWidthModifier = 0.05f;  // hmBlobWidthModifier
    std::vector<LightMapBlob> blobs;
};

// The neighborhood light map as cLightingManager::CreateNeighborhoodLightMap
// makes it (BV 0x706920, docs/neighborhood-mac.md 20.3): one texel per
// terrain vertex (the power of 2 >= vertices - 2 a side: 128, the last
// vertex left out), the vertex normal (terrainNormals) bent away from the
// sun by (shadow height - height) x shadowStrength where the height-map
// shadow (cHeightMapShadow over the heights with the trees' blobs) passes
// it, and the lighting state's "lightMap" image (0x0BE702EF) read without
// filtering at (floor((n.x / 2 + 0.5) w) & (w - 1), likewise y), x 256
// rounded, at most 255. Inferred (not in the game's map): under the sea the
// underwater gradient (neighborhood-underwater-gradient: depth /
// waterGradientZoneSize down the texture) in place of light, the lighting
// pass being clipped there. `gradient` may be empty.
RgbaImage computeHoodLightMap(const Terrain& t, const LightingState& state, const RgbaImage& lookup,
                              const RgbaImage& gradient, const HoodLightMapParams& params);

// imposter.matShad's materials, rewritten as materials the renderer draws:
// walls (ImposterWallMaterial: "walls_<page>", alpha test 64), slices
// (ImposterDualPackedSliceMaterial: "slices_<page>", alpha test 127, both
// sides), roofs (ImposterRoofMaterial: "roofs" at model xy / 64, which the
// vertex shader generates: the parts get those texture coordinates here)
// and the lot's ground (ImposterTerrainMaterial: "terrain", premultiplied
// over the neighborhood terrain or the lot's imposter terrain), all unlit (pre-rendered). `scope` is the lot's
// resource prefix ("N001_Lot11").
// `shade` is ImposterColorScalar's colour scalar on the walls, slices and
// roofs: the game's neighbourhood lighting adds a cTerrainShadeLight (a user
// light, kShadeLight) whose colour the imposters take (Mac BV
// cLightingManager::AddOutdoorLights, lighting mode 2); here the terrain's
// shade under the lot (terrainShade).
void imposterMaterials(ObjectModel& model, const std::string& scope, std::array<float, 3> shade = {1, 1, 1});

// The neighbourhood light map's shade at (x, y) relative to open flat
// ground (the light map's (0, 0, 1) normal, unshadowed), per channel and at
// most 1: a lot in a hill's height-map shadow comes out darker, like the
// ground around it (inferred: the light's colour is not decoded).
// The "Ver Amigos" arch (nTSSG::c3DUIManager::AddArch, BV 0x6a9818, built
// by End's FUN_006a9970 and the control points of FUN_006a8284): a tube of
// `radius` round a cubic Bezier from `from` to `to`, the inner control
// points a quarter and three quarters of the way along (horizontally), both
// at max(from z, to z) + tan(angle) x 0.25 x the horizontal distance;
// `along` rings of `around` vertices (the last coinciding with the first).
// ShowFamilyRelationshipsWeb asks for radius 1.0, 45 degrees, 8 and 32
// (which count is which is inferred).
MeshGroup relationshipArchMesh(std::array<float, 3> from, std::array<float, 3> to, float radius = 1.0f,
                               float angleDegrees = 45.0f, int along = 32, int around = 8);
std::array<float, 3> terrainShade(const Terrain& t, const RgbaImage& lightMap, const RgbaImage& lookup, float x, float y);

} // namespace hood
} // namespace os2
