#include "hood_view.hpp"
#include "app/scene_loader.hpp"
#include "formats/graphics_options.hpp"

#include "formats/camera_config.hpp"
#include "formats/config_text.hpp"
#include "formats/dbpf.hpp"
#include "formats/hood_lot_terrain.hpp"
#include "formats/hood_scene.hpp"
#include "formats/image.hpp"
#include "formats/lighting_config.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_image.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace os2 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTypeImg = 0x856DDBAC, kTypeTxtr = 0x1C4A276C;
constexpr uint32_t kLightingGroup = 0x8BA01057;
constexpr uint32_t kNhoodLightMap = 0x0BE702EF; // Lighting.txt nhoodLightMapID
constexpr float kNHoodShadowStrength = 0.47f;   // Lighting.txt hmNHoodShadowStrength

simd_float4x4 translation(simd_float3 t) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(t, 1);
    return m;
}

simd_float4x4 rotationZ(float degrees) {
    const float r = degrees * float(M_PI) / 180.0f, c = std::cos(r), s = std::sin(r);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = simd_make_float4(c, s, 0, 0);
    m.columns[1] = simd_make_float4(-s, c, 0, 0);
    return m;
}

simd_float4x4 rotationQuat(const std::array<float, 4>& q) {
    simd_quatf quat = simd_quaternion(q[0], q[1], q[2], q[3]);
    return simd_matrix4x4(simd_normalize(quat));
}

std::pair<simd_float3, simd_float3> bounds(const ObjectModel& m, const simd_float4x4& at) {
    simd_float3 lo = simd_make_float3(INFINITY, INFINITY, INFINITY), hi = -lo;
    for (const auto& p : m.parts)
        for (const auto& v : p.mesh.positions) {
            const simd_float4 w = simd_mul(at, simd_make_float4(v[0], v[1], v[2], 1));
            lo = simd_min(lo, w.xyz);
            hi = simd_max(hi, w.xyz);
        }
    return {lo, hi};
}

std::optional<RgbaImage> readImage(ResourceIndex& index, const Tgi& tgi) {
    auto bytes = index.read(tgi);
    if (!bytes)
        return std::nullopt;
    return decodeUiImage(*bytes);
}

RgbaImage readTexture(ResourceIndex& index, const std::string& name) {
    if (auto loc = index.findByName(name, kTypeTxtr))
        try {
            return loadTexture(index.read(*loc), &index);
        } catch (const std::exception&) {
        }
    return {};
}

bool rayBox(simd_float3 o, simd_float3 d, simd_float3 lo, simd_float3 hi, float& t) {
    float t0 = 0, t1 = 1e30f;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-9f) {
            if (o[k] < lo[k] || o[k] > hi[k])
                return false;
            continue;
        }
        float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a), t1 = std::min(t1, b);
        if (t0 > t1)
            return false;
    }
    t = t0;
    return true;
}

// Möller-Trumbore, both faces.
bool rayTriangle(simd_float3 o, simd_float3 d, simd_float3 a, simd_float3 b, simd_float3 c, float& t) {
    const simd_float3 e1 = b - a, e2 = c - a, p = simd_cross(d, e2);
    const float det = simd_dot(e1, p);
    if (std::abs(det) < 1e-9f)
        return false;
    const float inv = 1.0f / det;
    const simd_float3 s = o - a;
    const float u = simd_dot(s, p) * inv;
    if (u < 0 || u > 1)
        return false;
    const simd_float3 q = simd_cross(s, e1);
    const float v = simd_dot(d, q) * inv;
    if (v < 0 || u + v > 1)
        return false;
    t = simd_dot(e2, q) * inv;
    return t > 0;
}

} // namespace

simd_float4x4 lotTransform(const hood::LotInfo& info, float zOffset, simd_float2& footLo, simd_float2& footHi) {
    // SetOrientationAndTranslationForImposterNode (BV 0x769640): the
    // imposter is turned by -(front edge - creation front edge) quarters
    // about Z and moved onto the lot's rectangle (x, y)..(x + w, y + d) x 10,
    // the base size swapped when that turn is odd (cTSLotInfo::
    // GetCurrentSize), at the lot's elevation plus ImposterZOffset.
    const int turns = (int(info.frontEdge) - int(info.creationFrontEdge) + 4) & 3;
    const float W = float(info.width) * hood::kHoodCellSize, D = float(info.depth) * hood::kHoodCellSize;
    const simd_float3 corner = simd_make_float3(float(info.x) * hood::kHoodCellSize, float(info.y) * hood::kHoodCellSize,
                                                info.heightOffset + zOffset);
    const simd_float4x4 rot = rotationZ(-float(turns) * 90.0f);
    simd_float3 low = simd_make_float3(INFINITY, INFINITY, 0);
    for (const simd_float2 c : {simd_make_float2(0, 0), simd_make_float2(W, 0), simd_make_float2(0, D),
                                simd_make_float2(W, D)}) {
        const simd_float4 r = simd_mul(rot, simd_make_float4(c.x, c.y, 0, 1));
        low.x = std::min(low.x, r.x), low.y = std::min(low.y, r.y);
    }
    const simd_float4x4 at = simd_mul(translation(corner - low), rot);
    const float fw = float(info.currentWidth()) * hood::kHoodCellSize, fd = float(info.currentDepth()) * hood::kHoodCellSize;
    footLo = corner.xy;
    footHi = corner.xy + simd_make_float2(fw, fd);
    return at;
}

fs::path neighborhoodFolder(const fs::path& resDir, const std::string& query) {
    const fs::path q(query);
    if (fs::is_directory(q))
        return q;
    if (fs::is_regular_file(q))
        return q.parent_path();
    const fs::path dir = resDir / "UserData" / "Neighborhoods" / query;
    if (fs::is_directory(dir))
        return dir;
    throw std::runtime_error("--hood: no neighborhood \"" + query + "\" in " + (resDir / "UserData/Neighborhoods").string());
}

HoodView::HoodView(Renderer& renderer, ResourceIndex& index, const fs::path& resDir, const fs::path& sysDir,
                   const std::string& query)
    : renderer_(&renderer), index_(&index) {
    const auto start = std::chrono::steady_clock::now();
    const fs::path dir = neighborhoodFolder(resDir, query);
    hood_ = hood::Neighborhood::load(dir, true); // with the Sims' names (the family bin lists them)
    DbpfPackage pkg(hood_.package);
    bool haveTerrain = false, haveOccupants = false;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type == hoodtype::Terrain)
            terrain_ = hood::Terrain::parse(pkg.read(e)), haveTerrain = true;
        else if (e.tgi.type == hoodtype::Occupants)
            occupants_ = hood::Occupants::parse(pkg.read(e)), haveOccupants = true;
        else if (e.tgi.type == hoodtype::SavedCamera) {
            hood::SavedCamera c;
            if (hood::SavedCamera::parse(pkg.read(e), c))
                saved_ = c;
        }
    }
    if (!haveTerrain)
        throw std::runtime_error(hood_.package.string() + ": no neighborhood terrain");
    (void)haveOccupants;
    const hood::TerrainSettings settings = hood::TerrainSettings::load(sysDir);
    rawTerrain_ = terrain_; // as saved, for the lot tools' checks
    // FlattenTerrainUnderLot for every lot (formats/hood_lot_terrain.hpp).
    if (const size_t n = hood::flattenTerrainUnderLots(terrain_, hood_.lots, occupants_))
        report_.push_back(std::to_string(n) + " terrain vertices flattened under the lots");

    loading::phase("hood: terrain");
    // Lighting: the "neighbourhood" state and its light map.
    const LightingConfig lighting = loadLightingConfig(resDir / "Lights/Lighting.txt");
    const LightingState* state = lighting.state("neighbourhood");
    if (!state)
        state = lighting.state("day");
    if (state)
        renderer.setLighting(*state);
    const RgbaImage lookup = readImage(index, Tgi{kTypeImg, kLightingGroup, kNhoodLightMap, 0}).value_or(RgbaImage{});
    if (!lookup.width)
        report_.push_back("no neighborhood light map image (IMG 0x0BE702EF)");
    const RgbaImage gradient = readTexture(index, "neighborhood-underwater-gradient_txtr");
    RgbaImage lightMap; // (the imposters take its shade, below)
    if (state) {
        lightState_ = *state, lightLookup_ = lookup, lightGradient_ = gradient;
        const RgbaImage& lm = lightMap = hood::computeHoodLightMap(terrain_, *state, lookup, gradient,
                                                                      graphicsOptions().heightMapShadows ? kNHoodShadowStrength : 0.0f);
        renderer.setLightMap(0, lm);
        renderer.setLightMap(1, lm);
        if (const char* dump = std::getenv("OS2_HOOD_LIGHTMAP_DUMP"))
            writePng(dump, lm);
    }
    renderer.hideTestScene();

    // Ground, roads and water.
    // The lots first: which tiles their imposter terrains hide.
    struct Pending {
        HoodLot lot;
        ObjectModel model;
        simd_float4x4 at = matrix_identity_float4x4;
    };
    std::vector<Pending> pending;
    std::vector<uint8_t> hidden(size_t(terrain_.width) * terrain_.height, 0);
    std::vector<MeshGroup> patches;
    size_t imposters = 0;
    for (const auto& l : hood_.lots) {
        Pending p;
        HoodLot& lot = p.lot;
        lot.id = l.id;
        lot.name = l.name.empty() ? l.info.name : l.name;
        lot.description = l.description;
        lot.type = l.info.type;
        lot.package = l.package;
        lot.width = l.info.width, lot.depth = l.info.depth;
        if (!l.families.empty()) {
            lot.family = l.families.front();
            if (const auto* f = hood_.family(lot.family))
                lot.familyName = f->name;
        }
        p.at = lotTransform(l.info, settings.imposterZOffset, lot.footLo, lot.footHi);
        lot.lo = simd_make_float3(lot.footLo, l.info.heightOffset);
        lot.hi = simd_make_float3(lot.footHi, l.info.heightOffset + 3);
        lot.ground = l.info.heightOffset;
        if (!l.package.empty() && fs::exists(l.package)) {
            try {
                auto lotIndex = std::make_unique<ResourceIndex>(std::vector<fs::path>{l.package}, &index);
                p.model = loadObjectModel(*lotIndex, "#0xFFFFFFFF!imposter_cres");
                for (const auto& w : p.model.warnings)
                    report_.push_back(l.package.filename().string() + ": " + w);
                lotIndexes_.push_back(std::move(lotIndex));
                lot.indexSlot = int(lotIndexes_.size()) - 1;
            } catch (const std::exception& e) {
                report_.push_back(l.package.filename().string() + ": " + e.what());
            }
        }
        if (!p.model.parts.empty()) {
            // HideOrShowTerrainUnderImposter: the lot's rectangle loses its
            // terrain when the imposter's ground makes an imposter terrain.
            MeshGroup ground;
            for (const auto& part : p.model.parts) {
                auto mt = p.model.materials.find(part.material);
                if (mt == p.model.materials.end() || mt->second.materialType != "ImposterTerrainMaterial")
                    continue;
                const uint32_t base = uint32_t(ground.positions.size());
                for (const auto& v : part.mesh.positions) {
                    const simd_float4 w = simd_mul(p.at, simd_make_float4(v[0], v[1], v[2], 1));
                    // UpdateImposterTerrainTransform (BV 0x766a40): the
                    // imposter terrain sits ImposterZOffset below the imposter.
                    ground.positions.push_back({w.x, w.y, w.z - settings.imposterZOffset});
                }
                for (uint32_t i : part.mesh.triangles)
                    ground.triangles.push_back(base + i);
            }
            const uint32_t fw = l.info.currentWidth(), fd = l.info.currentDepth();
            if (hood::needsImposterTerrain(ground.positions.size(), fw, fd)) {
                // Inferred: the rectangle without its road strip
                // (cTSLotInfo::GetBoundingRectExcludingRoads), which the
                // imposter's ground does not cover.
                uint32_t x0 = l.info.x, y0 = l.info.y, x1 = l.info.x + fw, y1 = l.info.y + fd;
                const int turns = (int(l.info.frontEdge) - int(l.info.creationFrontEdge) + 4) & 3;
                for (int e = 0; e < 4; ++e)
                    if (l.info.roads & (1u << e))
                        switch ((e + turns) & 3) {
                        case 0: ++x0; break;
                        case 1: --y1; break;
                        case 2: --x1; break;
                        default: ++y0; break;
                        }
                for (uint32_t j = y0; j < y1 && j < terrain_.height; ++j)
                    for (uint32_t i = x0; i < x1 && i < terrain_.width; ++i)
                        hidden[size_t(j) * terrain_.width + i] = 1;
                patches.push_back(std::move(ground));
                lot.terrainHidden = true;
            }
        }
        pending.push_back(std::move(p));
    }
    {
        const ObjectModel terrainModel =
            hood::buildHoodTerrainModel(terrain_, occupants_, settings, &report_, &hidden);
        renderer.addInstance(renderer.addObject(terrainModel, index), matrix_identity_float4x4);
        for (const auto& mesh : patches)
            renderer.addInstance(renderer.addObject(hood::buildHoodTerrainPatch(terrainModel, terrain_, mesh), index),
                                 matrix_identity_float4x4);
        if (!patches.empty())
            report_.push_back(std::to_string(patches.size()) + " lots hide the terrain under their imposter terrain");
    }
    renderer.addInstance(renderer.addObject(hood::buildHoodRoadModel(terrain_, occupants_), index),
                         matrix_identity_float4x4);
    renderer.addInstance(renderer.addObject(hood::buildHoodWaterModel(terrain_), index), matrix_identity_float4x4);

    loading::phase("hood: lots");
    // Trees, props and bridges: one model per CRES.
    const auto objects = hood::loadNeighborhoodObjects(index);
    auto model = [&](const std::string& cres) { return modelId(cres); };
    size_t placed = 0, missing = 0;
    auto place = [&](uint32_t guid, const hood::OccupantPosition& at, float rotation) {
        auto it = objects.find(guid);
        const int id = it == objects.end() ? -1 : model(it->second);
        if (id < 0) {
            ++missing;
            return -1;
        }
        const auto& p = at.position;
        ++placed;
        return renderer.addInstance(id, simd_mul(translation(simd_make_float3(p[0], p[1], p[2])), rotationZ(rotation)));
    };
    for (const auto& f : occupants_.flora)
        floraInst_.push_back(place(f.guid, f.at, f.rotation));
    for (const auto& p : occupants_.props)
        propInst_.push_back(place(p.guid, p.at, p.rotation));
    for (const auto& b : occupants_.bridges) {
        char name[32];
        std::snprintf(name, sizeof name, "%04x-bridge_cres", b.road.piece >> 16);
        const int id = model(name);
        if (id < 0)
            continue;
        const auto& p = b.road.at.position;
        const simd_float3 at = simd_make_float3(p[0] + b.offset[0], p[1] + b.offset[1], p[2] + b.offset[2]);
        renderer.addInstance(id, simd_mul(translation(at), rotationQuat(b.orientation)));
        ++placed;
    }

    // The lots' imposters.
    for (auto& p : pending) {
        HoodLot& lot = p.lot;
        if (!p.model.parts.empty() && lot.indexSlot >= 0) {
            ObjectModel& m = p.model;
            // ImposterColorScalar: the terrain's shade under the lot (cTerrainShadeLight).
            const auto shade = hood::terrainShade(terrain_, lightMap, lookup, (lot.footLo.x + lot.footHi.x) / 2,
                                                  (lot.footLo.y + lot.footHi.y) / 2);
            if (shade[0] < 0.99f || shade[1] < 0.99f || shade[2] < 0.99f)
                report_.push_back("lot " + std::to_string(lot.id) + " in the terrain's shade: " +
                                  std::to_string(shade[0]).substr(0, 4) + " " + std::to_string(shade[1]).substr(0, 4) +
                                  " " + std::to_string(shade[2]).substr(0, 4));
            hood::imposterMaterials(m, hood_.prefix + "_Lot" + std::to_string(lot.id), shade);
            const int id = renderer.addObject(m, *lotIndexes_[size_t(lot.indexSlot)]);
            lot.instance = renderer.addInstance(id, p.at);
            const auto [lo, hi] = bounds(m, p.at);
            for (const auto& part : m.parts)
                for (size_t k = 0; k + 2 < part.mesh.triangles.size(); k += 3)
                    for (int c = 0; c < 3; ++c) {
                        const auto& v = part.mesh.positions[part.mesh.triangles[k + size_t(c)]];
                        lot.triangles.push_back(simd_mul(p.at, simd_make_float4(v[0], v[1], v[2], 1)).xyz);
                    }
            lot.lo = simd_min(lot.lo, lo);
            lot.hi = simd_max(lot.hi, hi);
            lot.imposter = true;
            ++imposters;
        }
        lots_.push_back(std::move(lot));
    }
    // The lot tools' checks (formats/hood_lots.hpp).
    place_.hood = &hood_;
    place_.terrain = &rawTerrain_;
    place_.occupants = &occupants_;
    place_.objects = hood::loadOccupantFlags(index);
    hood::applyTunables(place_, settings);
    // The lot highlight: a unit square, scaled onto the lot when shown.
    // AddLotPlopHighlight's footprint: the same square, green where the lot
    // may go and red where not (inferred colours).
    auto squareModel = [&](const char* material, const char* colour, const char* alpha) {
        ObjectModel m;
        m.name = material;
        ObjectModel::Part part;
        part.node = part.shape = "highlight";
        part.material = material;
        part.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        part.mesh.normals.assign(4, {0, 0, 1});
        part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        part.mesh.triangles = {0, 1, 2, 0, 2, 3};
        m.parts.push_back(part);
        MaterialDefinition mat;
        mat.materialType = "StandardMaterial";
        mat.properties = {{"stdMatDiffCoef", colour},          {"stdMatUntexturedDiffAlpha", alpha},
                          {"stdMatAlphaBlendMode", "blend"}, {"stdMatLightingEnabled", "false"},
                          {"stdMatCullMode", "none"},        {"stdMatLayer", "-1"}};
        m.materials[part.material] = mat;
        return renderer.addObject(m, index);
    };
    auto square = [&](const char* material, const char* colour, const char* alpha) {
        simd_float4x4 away = matrix_identity_float4x4;
        away.columns[0].x = away.columns[1].y = 0;
        return renderer.addInstance(squareModel(material, colour, alpha), away);
    };
    // "Ver Amigos": the lots' DefaultLotHighlight (Materials.package's
    // neighborhood script: NHoodTerrainHighlightMaterial, the texture
    // "lotselection-good" times (1, 1, 1, 0.25), unlit, blended; laid flat
    // over the lot here, its texture once across the lot: inferred). The
    // arches are built per lot pair in showFriends.
    {
        ObjectModel m;
        m.name = "DefaultLotHighlight";
        ObjectModel::Part part;
        part.node = part.shape = "highlight";
        part.material = "DefaultLotHighlight";
        part.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        part.mesh.normals.assign(4, {0, 0, 1});
        part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        part.mesh.triangles = {0, 1, 2, 0, 2, 3};
        m.parts.push_back(part);
        MaterialDefinition mat;
        mat.materialType = "StandardMaterial";
        mat.properties = {{"stdMatBaseTextureEnabled", "true"}, {"stdMatBaseTextureName", "lotselection-good"},
                          {"stdMatDiffCoef", "1,1,1,1"},          {"stdMatAlphaMultiplier", "0.25"},
                          {"stdMatAlphaBlendMode", "blend"},      {"stdMatLightingEnabled", "false"},
                          {"stdMatCullMode", "none"},             {"stdMatLayer", "-1"}};
        m.materials[part.material] = mat;
        friendModels_[0] = renderer.addObject(m, index);
    }
    plopOk_ = square("nhood-lot-plop-ok", "0.35,1,0.35,1", "0.45");
    plopBad_ = square("nhood-lot-plop-bad", "1,0.3,0.25,1", "0.45");
    {
        ObjectModel m;
        m.name = "lot highlight";
        ObjectModel::Part part;
        part.node = part.shape = "highlight";
        part.material = "nhood-lot-highlight";
        part.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        part.mesh.normals.assign(4, {0, 0, 1});
        part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        part.mesh.triangles = {0, 1, 2, 0, 2, 3};
        m.parts.push_back(part);
        MaterialDefinition mat;
        mat.materialType = "StandardMaterial";
        mat.properties = {{"stdMatDiffCoef", "1,1,1,1"},           {"stdMatUntexturedDiffAlpha", "0.3"},
                          {"stdMatAlphaBlendMode", "blend"},       {"stdMatLightingEnabled", "false"},
                          {"stdMatCullMode", "none"},              {"stdMatLayer", "-1"}};
        m.materials[part.material] = mat;
        simd_float4x4 away = matrix_identity_float4x4;
        away.columns[0].x = away.columns[1].y = 0;
        highlight_ = renderer.addInstance(renderer.addObject(m, index), away);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "neighborhood %s: %ux%u cells (%s), %zu roads, %zu bridges, %zu objects placed (%zu without a "
                  "model), %zu lots (%zu imposters) in %.2f s",
                  hood_.prefix.c_str(), terrain_.width, terrain_.height, terrain_.type.c_str(), occupants_.roads.size(),
                  occupants_.bridges.size(), placed, missing, lots_.size(), imposters, secs);
    report_.insert(report_.begin(), buf);
}

HoodView::~HoodView() = default;

const HoodLot* HoodView::lot(uint32_t id) const {
    for (const auto& l : lots_)
        if (l.id == id)
            return &l;
    return nullptr;
}

void HoodView::highlight(const HoodLot* lot) {
    if (lot == highlighted_ || highlight_ < 0)
        return;
    highlighted_ = lot;
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0].x = m.columns[1].y = 0; // hidden: collapsed
    if (lot) {
        m.columns[0].x = lot->footHi.x - lot->footLo.x;
        m.columns[1].y = lot->footHi.y - lot->footLo.y;
        m.columns[3] = simd_make_float4(lot->footLo.x, lot->footLo.y, lot->ground + 0.6f, 1);
    }
    renderer_->setInstanceTransform(highlight_, m);
}

int HoodView::modelId(const std::string& cres) {
    auto it = modelIds_.find(cres);
    if (it != modelIds_.end())
        return it->second;
    int id = -1;
    try {
        const ObjectModel m = loadObjectModel(*index_, cres);
        if (!m.parts.empty()) {
            id = renderer_->addObject(m, *index_);
            std::array<float, 6> box{INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
            for (const auto& part : m.parts)
                for (const auto& v : part.mesh.positions)
                    box = {std::min(box[0], v[0]), std::min(box[1], v[1]), std::min(box[2], v[2]),
                           std::max(box[3], v[0]), std::max(box[4], v[1]), std::max(box[5], v[2])};
            modelBox_[cres] = box;
        } else {
            report_.push_back(cres + ": no parts");
        }
    } catch (const std::exception& e) {
        report_.push_back(cres + ": " + e.what());
    }
    return modelIds_[cres] = id;
}

hood::OccupantPosition HoodView::decorationFootprint(const hood::DecorProduct& product, float x, float y, float rotation) {
    hood::OccupantPosition at;
    // The occupant's height (docs/neighborhood-mac.md 15.4): the terrain, or
    // the water's surface over it for the land-only and water-only products.
    const float ground = rawTerrain_.heightAt(x, y);
    at.position = {x, y, product.surface == 2 ? ground : std::max(ground, rawTerrain_.seaLevel)};
    const std::string cres = product.model + "_cres";
    modelId(cres);
    std::array<float, 4> box{-3.5f, -3.5f, 3.5f, 3.5f};
    if (auto it = modelBox_.find(cres); it != modelBox_.end() && std::isfinite(it->second[0]))
        box = {it->second[0], it->second[1], it->second[3], it->second[4]};
    const float a = rotation * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    float lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (float px : {box[0], box[2]})
        for (float py : {box[1], box[3]}) {
            const float rx = px * c - py * s, ry = px * s + py * c;
            lo[0] = std::min(lo[0], rx), hi[0] = std::max(hi[0], rx);
            lo[1] = std::min(lo[1], ry), hi[1] = std::max(hi[1], ry);
        }
    at.boundsMin = {x + lo[0], y + lo[1]};
    at.boundsMax = {x + hi[0], y + hi[1]};
    return at;
}

int HoodView::decorationModel(const hood::DecorProduct& product) { return modelId(product.model + "_cres"); }

std::array<float, 6> HoodView::decorationBounds(const hood::DecorProduct& product) {
    const std::string cres = product.model + "_cres";
    modelId(cres);
    if (auto it = modelBox_.find(cres); it != modelBox_.end() && std::isfinite(it->second[0]))
        return it->second;
    return {-3.5f, -3.5f, 0, 3.5f, 3.5f, 5};
}

int HoodView::addDecoration(const hood::DecorProduct& product, const hood::OccupantPosition& at, float rotation) {
    if (product.flora()) {
        hood::Flora f;
        f.at = at, f.rotation = rotation, f.guid = product.guid;
        occupants_.flora.push_back(f);
    } else {
        hood::Prop p;
        p.at = at, p.guid = product.guid, p.rotation = rotation;
        occupants_.props.push_back(p);
    }
    const int id = decorationModel(product);
    int inst = -1;
    if (id >= 0) {
        const auto& p = at.position;
        inst = renderer_->addInstance(id, simd_mul(translation(simd_make_float3(p[0], p[1], p[2])), rotationZ(rotation)));
    }
    (product.flora() ? floraInst_ : propInst_).push_back(inst);
    return inst;
}

void HoodView::removeDecoration(bool flora, size_t index) {
    auto& inst = flora ? floraInst_ : propInst_;
    if (index >= inst.size())
        return;
    if (inst[index] >= 0) {
        simd_float4x4 away = matrix_identity_float4x4;
        away.columns[0].x = away.columns[1].y = away.columns[2].z = 0;
        renderer_->setInstanceTransform(inst[index], away); // instances cannot be removed: collapsed
    }
    inst.erase(inst.begin() + std::ptrdiff_t(index));
    if (flora)
        occupants_.flora.erase(occupants_.flora.begin() + std::ptrdiff_t(index));
    else
        occupants_.props.erase(occupants_.props.begin() + std::ptrdiff_t(index));
}

void HoodView::showDecorationGhost(const hood::DecorProduct* product, float x, float y, float rotation, bool ok) {
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[0].x = away.columns[1].y = away.columns[2].z = 0;
    if (ghost_ < 0) {
        if (!product)
            return;
        const int id = decorationModel(*product);
        if (id < 0)
            return;
        ghost_ = renderer_->addInstance(id, away);
        ghostModel_ = id;
    }
    if (!product) {
        renderer_->setInstanceTransform(ghost_, away);
        return;
    }
    const int id = decorationModel(*product);
    if (id < 0)
        return;
    if (id != ghostModel_)
        renderer_->setInstanceModel(ghost_, id), ghostModel_ = id;
    (void)ok; // the model is shown as it is; the footprint box carries the colour
    renderer_->setInstanceTransform(ghost_, simd_mul(translation(simd_make_float3(x, y, rawTerrain_.heightAt(x, y))), rotationZ(rotation)));
}

std::optional<std::pair<bool, size_t>> HoodView::decorationAt(float x, float y) const {
    std::optional<std::pair<bool, size_t>> best;
    float bestD = 1e9f;
    auto test = [&](const hood::OccupantPosition& at, bool flora, size_t i) {
        if (x < at.boundsMin[0] || x > at.boundsMax[0] || y < at.boundsMin[1] || y > at.boundsMax[1])
            return;
        const float d = std::hypot(x - at.position[0], y - at.position[1]);
        if (d < bestD)
            bestD = d, best = std::pair{flora, i};
    };
    for (size_t i = 0; i < occupants_.flora.size(); ++i)
        test(occupants_.flora[i].at, true, i);
    for (size_t i = 0; i < occupants_.props.size(); ++i)
        test(occupants_.props[i].at, false, i);
    return best;
}

void HoodView::showFriends(const HoodLot* from, const std::vector<const HoodLot*>& lots) {
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[0].x = away.columns[1].y = away.columns[2].z = 0;
    for (int inst : friendBoxes_)
        renderer_->setInstanceTransform(inst, away);
    for (int inst : friendArches_)
        renderer_->setInstanceTransform(inst, away);
    while (friendBoxes_.size() < lots.size())
        friendBoxes_.push_back(renderer_->addInstance(friendModels_[0], away));
    while (friendArches_.size() < lots.size())
        friendArches_.push_back(renderer_->addInstance(friendModels_[0], away)); // its arch model comes below
    // A lot's middle: its rectangle's (cTSLotInfo vtable 0x30), the terrain
    // there less 2 (FLOAT_0274b484).
    auto middle = [&](const HoodLot& l) {
        float x = (l.footLo.x + l.footHi.x) / 2, y = (l.footLo.y + l.footHi.y) / 2;
        if (const hood::Lot* info = hood_.lot(l.id)) {
            const hood::TileRect r = hood::lotRect(info->info);
            x = (float(r.x0) + float(r.width()) * 0.5f) * hood::kHoodCellSize;
            y = (float(r.y0) + float(r.depth()) * 0.5f) * hood::kHoodCellSize;
        }
        return simd_make_float3(x, y, rawTerrain_.heightAt(x, y) - 2.0f);
    };
    for (size_t i = 0; i < lots.size(); ++i) {
        const HoodLot& lot = *lots[i];
        simd_float4x4 m = matrix_identity_float4x4;
        m.columns[0].x = lot.footHi.x - lot.footLo.x;
        m.columns[1].y = lot.footHi.y - lot.footLo.y;
        m.columns[3] = simd_make_float4(lot.footLo.x, lot.footLo.y, lot.ground + 0.7f, 1);
        renderer_->setInstanceTransform(friendBoxes_[i], m);
        if (!from)
            continue;
        const simd_float3 a = middle(*from), b = middle(lot);
        std::printf("opensims2: relationship arch from lot %u (%.0f, %.0f, %.1f) to lot %u (%.0f, %.0f, %.1f)\n", from->id,
                    a.x, a.y, a.z, lot.id, b.x, b.y, b.z);
        // c3DUIManager::AddArch(from, to, "RelationshipArchGreen", 1.0, 8,
        // 32, 45 degrees): a tube round a Bezier (hood::relationshipArchMesh)
        // in the material of the neighborhood script: StandardMaterial,
        // diffuse (0.3, 1, 0.3), specular (1, 1, 1) at power 20, alpha 0.6.
        ObjectModel arch;
        arch.name = "RelationshipArchGreen";
        ObjectModel::Part part;
        part.node = part.shape = "arch";
        part.material = "RelationshipArchGreen";
        part.mesh = hood::relationshipArchMesh({a.x, a.y, a.z}, {b.x, b.y, b.z});
        arch.parts.push_back(part);
        MaterialDefinition mat;
        mat.materialType = "StandardMaterial";
        mat.properties = {{"stdMatDiffCoef", "0.3,1,0.3"},     {"stdMatSpecCoef", "1,1,1"},
                          {"stdMatSpecPower", "20"},           {"stdMatAlphaMultiplier", "0.6"},
                          {"stdMatAlphaBlendMode", "blend"}};
        arch.materials[part.material] = mat;
        if (i < friendArchModels_.size()) {
            renderer_->replaceObject(friendArchModels_[i], arch, *index_);
        } else {
            friendArchModels_.push_back(renderer_->addObject(arch, *index_));
            renderer_->setInstanceModel(friendArches_[i], friendArchModels_[i]);
        }
        renderer_->setInstanceTransform(friendArches_[i], matrix_identity_float4x4);
    }
}

void HoodView::showPlacement(const hood::TileRect* rect, bool ok, float height) {
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[0].x = away.columns[1].y = 0;
    renderer_->setInstanceTransform(plopOk_, away);
    renderer_->setInstanceTransform(plopBad_, away);
    if (!rect)
        return;
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[0].x = float(rect->width()) * hood::kHoodCellSize;
    m.columns[1].y = float(rect->depth()) * hood::kHoodCellSize;
    // HighlightOffsetAboveTerrain (Neighborhood.ini [LotPlopTunables]): 0.6.
    m.columns[3] = simd_make_float4(float(rect->x0) * hood::kHoodCellSize, float(rect->y0) * hood::kHoodCellSize,
                                    height + 0.6f, 1);
    renderer_->setInstanceTransform(ok ? plopOk_ : plopBad_, m);
}

std::optional<simd_float2> HoodView::groundAt(simd_float3 origin, simd_float3 dir) const {
    // Marching the ray down to the terrain (or the sea).
    dir = simd_normalize(dir);
    float prev = 0;
    for (float t = 1; t < 4000; t += 2) {
        const simd_float3 p = origin + dir * t;
        if (p.x < 0 || p.y < 0 || p.x > terrain_.worldWidth() || p.y > terrain_.worldHeight()) {
            if (t > 2000)
                break;
            prev = t;
            continue;
        }
        if (p.z <= std::max(terrain_.heightAt(p.x, p.y), terrain_.seaLevel)) {
            // Refine between the last two steps.
            float lo = prev, hi = t;
            for (int k = 0; k < 12; ++k) {
                const float mid = (lo + hi) / 2;
                const simd_float3 q = origin + dir * mid;
                (q.z <= std::max(terrain_.heightAt(q.x, q.y), terrain_.seaLevel) ? hi : lo) = mid;
            }
            const simd_float3 q = origin + dir * hi;
            return simd_make_float2(q.x, q.y);
        }
        prev = t;
    }
    return std::nullopt;
}

void HoodView::refreshLightMap() {
    if (!lightState_ || !renderer_)
        return;
    const RgbaImage lm = hood::computeHoodLightMap(terrain_, *lightState_, lightLookup_, lightGradient_,
                                                   graphicsOptions().heightMapShadows ? kNHoodShadowStrength : 0.0f);
    renderer_->setLightMap(0, lm);
    renderer_->setLightMap(1, lm);
}

void HoodView::reload() {
    hood_ = hood::Neighborhood::load(hood_.package.parent_path(), true);
    highlight(nullptr);
    std::vector<HoodLot> kept;
    for (auto& lot : lots_) {
        const hood::Lot* l = hood_.lot(lot.id);
        if (!l) {
            // Bulldozed: the imposter goes (collapsed to nothing).
            if (lot.instance >= 0) {
                simd_float4x4 away = matrix_identity_float4x4;
                away.columns[0].x = away.columns[1].y = away.columns[2].z = 0;
                renderer_->setInstanceTransform(lot.instance, away);
            }
            continue;
        }
        lot.family = 0;
        lot.familyName.clear();
        if (!l->families.empty()) {
            lot.family = l->families.front();
            if (const auto* f = hood_.family(lot.family))
                lot.familyName = f->name;
        }
        kept.push_back(std::move(lot));
    }
    lots_ = std::move(kept);
}

std::string HoodView::displayName() const { return hood_.name.empty() ? hood_.prefix : hood_.name; }

std::unique_ptr<LotCamera> HoodView::makeCamera(const fs::path& camerasTxt) const {
    const auto cameras = loadCameraConfig(camerasTxt);
    const CameraDef* def = findCamera(cameras, "Neighborhood");
    if (!def)
        throw std::runtime_error("no Neighborhood camera in " + camerasTxt.string());
    auto cam = std::make_unique<LotCamera>(*def);
    // SetBoundedCOIPosition: x in [c0 W, W - c2 W], y in [c1 H, H - c3 H] by
    // the camera's "constrain" line (not kept by the config parser).
    std::array<float, 4> c{0, 0, 0, 0};
    {
        const fs::path nh = camerasTxt.parent_path() / "NeighborhoodCamera.txt";
        if (fs::exists(nh))
            forEachConfigLine(readTextFile(nh), [&](std::string_view line) {
                const auto t = tokenizeConfigLine(line);
                if (t.size() >= 5 && t[0].kind == ConfigToken::Word && t[0].word == "constrain")
                    for (size_t i = 0; i < 4; ++i)
                        c[i] = configNumber(t[i + 1]);
            });
    }
    const float W = terrain_.worldWidth(), H = terrain_.worldHeight();
    cam->setBounds(simd_make_float2(c[0] * W, c[1] * H), simd_make_float2(W - c[2] * W, H - c[3] * H));
    const hood::Terrain* t = &terrain_;
    cam->setFloorSampler([t](float x, float y) { return std::max(t->heightAt(x, y), t->seaLevel); });
    simd_float3 pivot = simd_make_float3(terrain_.worldWidth() / 2, terrain_.worldHeight() / 2, 0);
    if (saved_) {
        pivot.x = saved_->pivot[0], pivot.y = saved_->pivot[1];
        // Nearest zoom level of the saved distance.
        int level = def->defaultZoom;
        float best = 1e30f;
        for (size_t i = 0; i < def->distances.size(); ++i)
            if (float d = std::abs(def->distances[i] - saved_->distance); d < best)
                best = d, level = int(i);
        cam->zoomBy(level - def->defaultZoom);
        cam->freeRotate(saved_->heading * 180.0f / float(M_PI) - cam->yaw());
    }
    pivot.z = std::max(terrain_.heightAt(pivot.x, pivot.y), terrain_.seaLevel);
    cam->setPivot(pivot);
    cam->snap();
    return cam;
}

const HoodLot* HoodView::pick(simd_float3 origin, simd_float3 dir) const {
    const HoodLot* best = nullptr;
    float bestT = 1e30f;
    for (const auto& l : lots_) {
        float t;
        if (!rayBox(origin, dir, l.lo, l.hi, t) || t >= bestT)
            continue;
        if (l.triangles.empty()) {
            // No imposter: the footprint at the lot's elevation.
            if (std::abs(dir.z) > 1e-6f) {
                const float tg = (l.ground - origin.z) / dir.z;
                const simd_float3 p = origin + dir * tg;
                if (tg > 0 && tg < bestT && p.x >= l.footLo.x && p.x <= l.footHi.x && p.y >= l.footLo.y &&
                    p.y <= l.footHi.y)
                    bestT = tg, best = &l;
            }
            continue;
        }
        for (size_t k = 0; k + 2 < l.triangles.size(); k += 3)
            if (rayTriangle(origin, dir, l.triangles[k], l.triangles[k + 1], l.triangles[k + 2], t) && t < bestT)
                bestT = t, best = &l;
    }
    return best;
}

simd_float3 HoodView::lotCentre(const HoodLot& lot) const {
    return simd_make_float3((lot.lo.x + lot.hi.x) / 2, (lot.lo.y + lot.hi.y) / 2, lot.hi.z);
}

} // namespace os2
