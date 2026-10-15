// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

// The lot's terrain water as the game's cWaterGeometryBuilder draws it
// (docs/roof-fence-water-mac.md 3): a vertex alpha from the water's depth,
// and three terrain paints under and around it (lake bed and beach), all
// tuned by TSData/Sys/Terrain.ini [WaterRenderParameters].

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace os2 {

struct Lot;

struct LotWaterParams {
    // Defaults: the binary's when the ini has no value (FUN_00c42638).
    std::vector<std::array<float, 2>> alphaVsDepth{{0, 0}, {4, 255}}; // (depth, alpha 0-255)
    float shallowMin = -0.1f, shallowMax = 0.6f, deepMin = 0.4f, deepMax = 1.2f;
    float beachStart = -0.2f, beachPeak = -0.02f, beachEnd = -0.2f;
    float maxBeachWidth = 0.8f, beachIntensity = 1.0f;

    // Terrain.ini's text (empty: the defaults).
    static LotWaterParams parse(const std::string& terrainIni);

    // Vertex alpha (0-255) of water `depth` deep: the curve, flat past its ends.
    float alpha(float depth) const;
};

// The paint layers the game keeps for water (cWaterGeometryBuilder +0x140..),
// over the lot's paint grid: `maskWidth` x `maskHeight` points `spacing`
// tiles apart, row by row ([y][x]) like Lot::TerrainPaint::mask.
struct LotWaterPaints {
    static constexpr const char* kShallow = "terrain-seabed-shallow";
    static constexpr const char* kDeep = "terrain-seabed-deep";
    static constexpr const char* kBeach = "terrain-beach";
    std::vector<uint8_t> shallow, deep, beach;
};
LotWaterPaints lotWaterPaints(const Lot& lot, const LotWaterParams& params, uint32_t maskWidth, uint32_t maskHeight,
                              float spacing);

// Water depth (water level - terrain) at grid point (x, y); -inf without water data.
float lotWaterDepth(const Lot& lot, int x, int y);

} // namespace os2
