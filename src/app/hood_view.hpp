#pragma once

// The neighborhood view (--hood <N001|folder>): the neighborhood's terrain,
// roads, water, trees and props, and every lot's imposter, loaded into the
// renderer (formats/hood_scene.hpp builds the models), lit by the
// "neighbourhood" lighting state of Lighting.txt (always day, as in the game)
// and its light map, with the "Neighborhood" camera of
// UserData/Cameras/NeighborhoodCamera.txt starting where the neighborhood
// package's saved camera (0xEC44BDDC) looks.
//
// Lots are picked by the box of their imposter (or, without one, their
// footprint); the game intersects the scene graph and climbs to the node's
// "LotData" property set (docs/neighborhood-mac.md 6). Imposters are placed
// as SetOrientationAndTranslationForImposterNode does (section 4): turned
// -(front edge mod 4) x 90 degrees about Z and moved onto the lot's
// footprint (cells x 10), at the lot's elevation plus Neighborhood.ini's
// ImposterZOffset. The camera keeps its pivot inside the "constrain"
// fractions of the terrain (section 5).

#include "formats/hood_terrain.hpp"
#include "formats/neighborhood.hpp"

#include <simd/simd.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class LotCamera;
class Renderer;
class ResourceIndex;

struct HoodLot {
    uint32_t id = 0;
    std::string name, description;
    uint8_t type = 0;          // lot info type: 0 residential, 1 community
    uint32_t family = 0;       // first family living here (0: none)
    std::string familyName;    // its name
    std::filesystem::path package; // the lot file (empty if missing)
    simd_float3 lo{}, hi{};    // world box (imposter or footprint)
    simd_float2 footLo{}, footHi{}; // footprint (x, y)
    float ground = 0;          // the lot's elevation
    bool imposter = false;
    uint32_t width = 0, depth = 0; // cells
};

// The folder of a neighborhood given as "N001" (under
// UserData/Neighborhoods), a folder or its _Neighborhood.package.
std::filesystem::path neighborhoodFolder(const std::filesystem::path& resDir, const std::string& query);

class HoodView {
public:
    // `resDir` is TSData/Res, `sysDir` TSData/Sys.
    HoodView(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir,
             const std::filesystem::path& sysDir, const std::string& query);
    ~HoodView();

    const hood::Neighborhood& neighborhood() const { return hood_; }
    const hood::Terrain& terrain() const { return terrain_; }
    const std::vector<HoodLot>& lots() const { return lots_; }
    const HoodLot* lot(uint32_t id) const;
    // Neighborhood name (CTSS), else the prefix.
    std::string displayName() const;

    // The Neighborhood camera, starting at the saved view.
    std::unique_ptr<LotCamera> makeCamera(const std::filesystem::path& camerasTxt) const;

    // The lot whose box the ray enters first, or null.
    const HoodLot* pick(simd_float3 origin, simd_float3 dir) const;
    // HighlightLot's terrain highlight over a lot's footprint (null: none).
    // Inferred look: a white rectangle at alpha 0.3, HighlightOffsetAboveTerrain
    // (0.6) over the lot (NHoodTerrainHighlightMaterial's texture is code-set).
    void highlight(const HoodLot* lot);
    // A point to frame a lot from (middle of its box top).
    simd_float3 lotCentre(const HoodLot& lot) const;

    // Loading report (counts, missing models).
    const std::vector<std::string>& report() const { return report_; }

private:
    hood::Neighborhood hood_;
    hood::Terrain terrain_;
    hood::Occupants occupants_;
    std::optional<hood::SavedCamera> saved_;
    std::vector<HoodLot> lots_;
    std::vector<std::unique_ptr<ResourceIndex>> lotIndexes_; // imposter textures load through them
    std::vector<std::string> report_;
    Renderer* renderer_ = nullptr;
    int highlight_ = -1; // renderer instance
    const HoodLot* highlighted_ = nullptr;
};

} // namespace os2
