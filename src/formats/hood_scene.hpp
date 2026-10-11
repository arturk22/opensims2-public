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
//   The alpha maps are computed as NeighborhoodTerrain.ini describes them
//   (inferred, the code is not decoded): each vertex takes the paint of
//   [PaintMapTable_<type>] by altitude above the sea (rows over
//   HeightRangeForTemperatureDistribution) and distance to the nearest road
//   cell (columns over MaxRoadProximityValue cells), the cliff paint where
//   the slope passes CliffAngleMin..Max and the beach paint at most
//   MaxBeachAltitude over the sea within MaxBeachWidthInCells of water (and
//   under it); each paint then spreads by its curve (units) at its
//   MaxStrength. Variants ("-00", "-01", ...) are picked per square of
//   TextureVariantGridSize tiles.
// - Roads (NHoodRoadMaterialDef): each road tile's quad with
//   "<type>_roads_<piece | 4>" clamped, turned by its connections (as
//   OpenTS2 does), alpha blended, x the light map.
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

// Distance in cells from each vertex to the nearest road tile's centre
// (a large value when there are no roads).
std::vector<float> roadProximity(const Terrain& t, const Occupants& o);

// Light-map coordinates of a world point (the map spans the terrain).
inline std::array<float, 2> hoodLightMapUv(const Terrain& t, float x, float y) {
    return {x / t.worldWidth(), y / t.worldHeight()};
}

ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  std::vector<std::string>* report = nullptr);
ObjectModel buildHoodRoadModel(const Terrain& t, const Occupants& o);
ObjectModel buildHoodWaterModel(const Terrain& t);

// The neighborhood light map, `size` x `size` texels over the terrain: the
// lighting state's "lightMap" image (0x0BE702EF in lighting.package) read
// as a 2D look-up by the terrain normal against the sun (u = N.L + N.(L x
// up), v = N.L, as OpenTS2's terrain shader reads it; inferred), with the
// sun's height-map shadow (hmNHoodShadowStrength) pulling the look-up
// towards its dark corner, and under the sea the underwater gradient
// (neighborhood-underwater-gradient: depth / waterGradientZoneSize down the
// texture) in place of light. `gradient` may be empty.
RgbaImage computeHoodLightMap(const Terrain& t, const LightingState& state, const RgbaImage& lookup,
                              const RgbaImage& gradient, float shadowStrength, uint32_t size = 512);

// imposter.matShad's materials, rewritten as materials the renderer draws:
// walls (ImposterWallMaterial: "walls_<page>", alpha test 64), slices
// (ImposterDualPackedSliceMaterial: "slices_<page>", alpha test 127, both
// sides), roofs (ImposterRoofMaterial: "roofs" at model xy / 64, which the
// vertex shader generates: the parts get those texture coordinates here)
// and the lot's ground (ImposterTerrainMaterial: "terrain", blended over the
// neighborhood terrain), all unlit (pre-rendered). `scope` is the lot's
// resource prefix ("N001_Lot11").
void imposterMaterials(ObjectModel& model, const std::string& scope);

} // namespace hood
} // namespace os2
