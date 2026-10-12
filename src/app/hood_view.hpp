#pragma once

// The neighborhood view (--hood <N001|folder>): the neighborhood's terrain,
// roads, water, trees and props, and every lot's imposter, loaded into the
// renderer (formats/hood_scene.hpp builds the models), lit by the
// "neighbourhood" lighting state of Lighting.txt (always day, as in the game)
// and its light map, with the "Neighborhood" camera of
// UserData/Cameras/NeighborhoodCamera.txt starting where the neighborhood
// package's saved camera (0xEC44BDDC) looks.
//
// Lots are picked as cTSNHoodUserToolBase::PickLot does: the nearest hit of
// the mouse ray on an imposter's triangles (its walls, roofs and ground; the
// game intersects the scene graph and climbs to the node's "LotData"
// property set, docs/neighborhood-mac.md 6), or on the footprint of a lot
// without one. Imposters are placed
// as SetOrientationAndTranslationForImposterNode does (section 4): turned
// -(front edge mod 4) x 90 degrees about Z and moved onto the lot's
// footprint (cells x 10), at the lot's elevation plus Neighborhood.ini's
// ImposterZOffset. The camera keeps its pivot inside the "constrain"
// fractions of the terrain (section 5).

#include "formats/hood_lots.hpp"
#include "formats/hood_terrain.hpp"
#include "formats/neighborhood.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <map>
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
    int instance = -1;             // the imposter's renderer instance
    std::vector<simd_float3> triangles; // the imposter's triangles in the world (3 per), for picking
    bool terrainHidden = false;         // its imposter terrain replaces the neighborhood terrain
    int indexSlot = -1;                 // its lot file's resource index (HoodView)
};

// The imposter's placement for a lot info (SetOrientationAndTranslationFor-
// ImposterNode) and the lot's footprint in the world.
simd_float4x4 lotTransform(const hood::LotInfo& info, float zOffset, simd_float2& footLo, simd_float2& footHi);

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

    // The lot whose imposter (or footprint) the ray hits first, or null.
    const HoodLot* pick(simd_float3 origin, simd_float3 dir) const;
    // HighlightLot's terrain highlight over a lot's footprint (null: none).
    // Inferred look: a white rectangle at alpha 0.3, HighlightOffsetAboveTerrain
    // (0.6) over the lot (NHoodTerrainHighlightMaterial's texture is code-set).
    void highlight(const HoodLot* lot);
    // A point to frame a lot from (middle of its box top).
    simd_float3 lotCentre(const HoodLot& lot) const;

    // The lot tools (formats/hood_lots.hpp): what they check against, the
    // footprint highlight (null hides it) and the ground under a ray.
    const hood::PlacementWorld& placementWorld() const { return place_; }
    // "Ver Amigos": the lots of a family's friends (kind 1) and relatives (2),
    // each lot highlighted in its colour; empty hides them all.
    void showFriends(const std::vector<std::pair<const HoodLot*, int>>& lots);
    void showPlacement(const hood::TileRect* rect, bool ok, float height);

    // Decorations (F3, formats/hood_lots.hpp DecorProduct). The footprint of
    // a product at (x, y) turned `rotation` degrees (the model's box turned),
    // on the terrain; the product's model name is its `model` + "_cres".
    hood::OccupantPosition decorationFootprint(const hood::DecorProduct& product, float x, float y, float rotation);
    // The decoration standing in the view and in the checks (occupants_), after
    // addDecoration wrote it; returns its renderer instance.
    int addDecoration(const hood::DecorProduct& product, const hood::OccupantPosition& at, float rotation);
    // The view's side of removeDecoration (formats/hood_lots.hpp): the instance and the record go.
    void removeDecoration(bool flora, size_t index);
    // The ghost under the mouse while placing (null hides it); green when it may stand, red when not.
    void showDecorationGhost(const hood::DecorProduct* product, float x, float y, float rotation, bool ok);
    // The renderer model of a product (-1: none) and its local bounds (min xyz, max xyz), for its thumbnail.
    int decorationModel(const hood::DecorProduct& product);
    std::array<float, 6> decorationBounds(const hood::DecorProduct& product);
    // The occupant nearest to (x, y) within its footprint: {flora?, index}, for the hand and eyedropper.
    std::optional<std::pair<bool, size_t>> decorationAt(float x, float y) const;
    const hood::Occupants& occupants() const { return occupants_; }
    std::optional<simd_float2> groundAt(simd_float3 origin, simd_float3 dir) const;
    const hood::Terrain& rawTerrain() const { return rawTerrain_; }

    // Rereads the neighborhood package after a family moved or a lot was
    // bulldozed (formats/hood_moves.hpp): the lots' families, and the lots
    // that are gone leave the view.
    void reload();
    // The neighborhood folder (the user's copy).
    std::filesystem::path folder() const { return hood_.package.parent_path(); }

    // Loading report (counts, missing models).
    const std::vector<std::string>& report() const { return report_; }

private:
    hood::Neighborhood hood_;
    hood::Terrain terrain_, rawTerrain_;
    hood::PlacementWorld place_;
    int plopOk_ = -1, plopBad_ = -1;
    hood::Occupants occupants_;
    std::optional<hood::SavedCamera> saved_;
    std::vector<HoodLot> lots_;
    std::vector<std::unique_ptr<ResourceIndex>> lotIndexes_; // imposter textures load through them
    std::vector<std::string> report_;
    Renderer* renderer_ = nullptr;
    int modelId(const std::string& cres);
    ResourceIndex* index_ = nullptr;
    std::map<std::string, int> modelIds_;
    std::map<std::string, std::array<float, 6>> modelBox_; // local bounds (min xyz, max xyz) by CRES
    int ghost_ = -1, ghostModel_ = -1;
    std::vector<int> floraInst_, propInst_; // each occupant's instance (-1: none)
    int friendModels_[2] = {-1, -1};
    std::vector<int> friendBoxes_;
    int highlight_ = -1; // renderer instance
    const HoodLot* highlighted_ = nullptr;
};

} // namespace os2
