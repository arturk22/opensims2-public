#pragma once

// Renderable models of the neighborhood view, built from hood_terrain.hpp's
// data with materials the renderer already draws (the game draws them with
// the shaders of neighborhood.matShad, NHoodRoadMaterialDef and
// imposter.matShad in Materials.package):
//
// - Terrain (NeighborhoodCanvas + NeighborhoodTerrainPaint +
//   NHoodTerrainLighting): the canvas is black and every paint is added
//   over it, its texture repeating every `nhoodPaintTextureScale` (13) tiles
//   and scaled by its alpha map; the result is multiplied by the
//   neighborhood light map ("nhoodLightMap", one texel run over the 128
//   cells). Here the paints' weights are normalised and drawn as an opaque
//   first paint with the others blended over it ("over" with alpha =
//   weight / running sum, which adds up to the same), all light-mapped.
//   The paints are decided at load time as cTSNHoodTerrain::DecideSurfaceCover
//   does (docs/neighborhood-mac.md 2.3): per vertex the cliff paint where
//   |n.z| < cos(CliffAngleMin) (only on that vertex), the beach paint within
//   MaxBeachWidthInCells of water and at most MaxBeachAltitude above it, else
//   [PaintMapTable_<type>] at row 31 - (h - sea) x 31 / HeightRange and the
//   road proximity column; beach and table paints spread by their curves;
//   each vertex keeps its 3 heaviest paints, normalised. Inferred: alpha =
//   weight, the variant hash (the game draws a random byte per cell of
//   TextureVariantGridSize tiles).
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
#include <string>
#include <vector>

namespace os2 {

struct LightingState;

namespace hood {

// Paint weights per terrain vertex (gridWidth x gridHeight each), by paint
// ID, normalised to add up to 1.
struct PaintWeights {
    std::vector<uint32_t> paints;              // paint IDs
    std::vector<std::vector<float>> weights;   // per paint, per vertex
    std::vector<uint32_t> assigned;            // per vertex: the paint it took
};
PaintWeights computePaintWeights(const Terrain& t, const Occupants& o, const TerrainSettings& s);

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
ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  std::vector<std::string>* report = nullptr,
                                  const std::vector<uint8_t>* hiddenTiles = nullptr);
// A lot's imposter terrain (cLotImposterDisplay::TryCreateLotImposterTerrain /
// CreateLotImposterTerrainGeometry, BV 0x7692b8 / 0x7686cc): the imposter's
// "terrain" geometry (`mesh`, in world coordinates) drawn with the
// neighborhood terrain's paints of `terrain` (its layers and alpha masks,
// cropped to the mesh), where the neighborhood terrain is hidden.
ObjectModel buildHoodTerrainPatch(const ObjectModel& terrain, const Terrain& t, const MeshGroup& mesh);
// The game makes a lot's imposter terrain only when the imposter's ground has
// neither 4 vertices (flat) nor one per vertex of the lot's rectangle
// ((w + 1) x (d + 1)); for the others the neighborhood terrain stays.
bool needsImposterTerrain(size_t groundVertices, uint32_t widthTiles, uint32_t depthTiles);
ObjectModel buildHoodRoadModel(const Terrain& t, const Occupants& o);
ObjectModel buildHoodWaterModel(const Terrain& t);

// The neighborhood light map, `size` x `size` texels over the terrain
// (inferred: its generation is not decoded, docs/neighborhood-mac.md 2.4):
// the lighting state's "lightMap" image (0x0BE702EF in lighting.package)
// read at ((n.x + 1) / 2, (n.y + 1) / 2) of the terrain normal (its bright
// corner faces the neighbourhood sun), times 1 - hmNHoodShadowStrength where
// the height map shades the point from the sun, and under the sea the
// underwater gradient (neighborhood-underwater-gradient: depth /
// waterGradientZoneSize down the texture) in place of light. `gradient` may
// be empty.
RgbaImage computeHoodLightMap(const Terrain& t, const LightingState& state, const RgbaImage& lookup,
                              const RgbaImage& gradient, float shadowStrength, uint32_t size = 512);

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
std::array<float, 3> terrainShade(const Terrain& t, const RgbaImage& lightMap, const RgbaImage& lookup, float x, float y);

} // namespace hood
} // namespace os2
