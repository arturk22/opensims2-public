#include "hood_view.hpp"

#include "formats/camera_config.hpp"
#include "formats/config_text.hpp"
#include "formats/dbpf.hpp"
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

} // namespace

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
    : renderer_(&renderer) {
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
    if (state) {
        const RgbaImage lm = hood::computeHoodLightMap(terrain_, *state, lookup, gradient, kNHoodShadowStrength);
        renderer.setLightMap(0, lm);
        renderer.setLightMap(1, lm);
        if (const char* dump = std::getenv("OS2_HOOD_LIGHTMAP_DUMP"))
            writePng(dump, lm);
    }
    renderer.hideTestScene();

    // Ground, roads and water.
    renderer.addInstance(renderer.addObject(hood::buildHoodTerrainModel(terrain_, occupants_, settings, &report_), index),
                         matrix_identity_float4x4);
    renderer.addInstance(renderer.addObject(hood::buildHoodRoadModel(terrain_, occupants_), index),
                         matrix_identity_float4x4);
    renderer.addInstance(renderer.addObject(hood::buildHoodWaterModel(terrain_), index), matrix_identity_float4x4);

    // Trees, props and bridges: one model per CRES.
    const auto objects = hood::loadNeighborhoodObjects(index);
    std::map<std::string, int> models;
    auto model = [&](const std::string& cres) {
        auto it = models.find(cres);
        if (it != models.end())
            return it->second;
        int id = -1;
        try {
            const ObjectModel m = loadObjectModel(index, cres);
            if (!m.parts.empty())
                id = renderer.addObject(m, index);
            else
                report_.push_back(cres + ": no parts");
        } catch (const std::exception& e) {
            report_.push_back(cres + ": " + e.what());
        }
        return models[cres] = id;
    };
    size_t placed = 0, missing = 0;
    auto place = [&](uint32_t guid, const hood::OccupantPosition& at, float rotation) {
        auto it = objects.find(guid);
        const int id = it == objects.end() ? -1 : model(it->second);
        if (id < 0) {
            ++missing;
            return;
        }
        const auto& p = at.position;
        renderer.addInstance(id, simd_mul(translation(simd_make_float3(p[0], p[1], p[2])), rotationZ(rotation)));
        ++placed;
    };
    for (const auto& f : occupants_.flora)
        place(f.guid, f.at, f.rotation);
    for (const auto& p : occupants_.props)
        place(p.guid, p.at, p.rotation);
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

    // Lots and their imposters.
    size_t imposters = 0;
    for (const auto& l : hood_.lots) {
        HoodLot lot;
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
        const float W = float(l.info.width) * hood::kHoodCellSize, D = float(l.info.depth) * hood::kHoodCellSize;
        const simd_float3 corner = simd_make_float3(float(l.info.x) * hood::kHoodCellSize,
                                                    float(l.info.y) * hood::kHoodCellSize,
                                                    l.info.heightOffset + settings.imposterZOffset);
        // SetOrientationAndTranslationForImposterNode: turned by -(front edge
        // mod 4) x 90 degrees about Z, moved so that the footprint covers
        // (x, y)..(x + w, y + d) tiles x 10 (the model is w x d when not
        // turned a quarter, d x w when it is).
        const int turns = int(l.info.frontEdge % 4);
        const simd_float4x4 rot = rotationZ(-float(turns) * 90.0f);
        const float mw = turns % 2 ? D : W, md = turns % 2 ? W : D;
        simd_float3 low = simd_make_float3(INFINITY, INFINITY, 0);
        for (const simd_float2 c : {simd_make_float2(0, 0), simd_make_float2(mw, 0), simd_make_float2(0, md),
                                    simd_make_float2(mw, md)}) {
            const simd_float4 r = simd_mul(rot, simd_make_float4(c.x, c.y, 0, 1));
            low.x = std::min(low.x, r.x), low.y = std::min(low.y, r.y);
        }
        const simd_float4x4 at = simd_mul(translation(corner - low), rot);
        // Footprint (fallback box).
        const simd_float4 a = simd_mul(at, simd_make_float4(0, 0, 0, 1)), b = simd_mul(at, simd_make_float4(mw, md, 0, 1));
        lot.lo = simd_min(a.xyz, b.xyz);
        lot.hi = simd_max(a.xyz, b.xyz);
        lot.footLo = lot.lo.xy, lot.footHi = lot.hi.xy;
        lot.ground = l.info.heightOffset;
        lot.hi.z += 3;
        if (!l.package.empty() && fs::exists(l.package)) {
            try {
                auto lotIndex = std::make_unique<ResourceIndex>(std::vector<fs::path>{l.package}, &index);
                ObjectModel m = loadObjectModel(*lotIndex, "#0xFFFFFFFF!imposter_cres");
                if (!m.parts.empty()) {
                    hood::imposterMaterials(m, hood_.prefix + "_Lot" + std::to_string(l.id));
                    const int id = renderer.addObject(m, *lotIndex);
                    lot.instance = renderer.addInstance(id, at);
                    const auto [lo, hi] = bounds(m, at);
                    lot.lo = simd_min(lot.lo, lo);
                    lot.hi = simd_max(lot.hi, hi);
                    lot.imposter = true;
                    ++imposters;
                }
                for (const auto& w : m.warnings)
                    report_.push_back(l.package.filename().string() + ": " + w);
                lotIndexes_.push_back(std::move(lotIndex));
            } catch (const std::exception& e) {
                report_.push_back(l.package.filename().string() + ": " + e.what());
            }
        }
        lots_.push_back(std::move(lot));
    }
    // The lot highlight: a unit square, scaled onto the lot when shown.
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
        if (rayBox(origin, dir, l.lo, l.hi, t) && t < bestT)
            bestT = t, best = &l;
    }
    return best;
}

simd_float3 HoodView::lotCentre(const HoodLot& lot) const {
    return simd_make_float3((lot.lo.x + lot.hi.x) / 2, (lot.lo.y + lot.hi.y) / 2, lot.hi.z);
}

} // namespace os2
