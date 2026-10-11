#include "hood_scene.hpp"

#include "lighting_config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace os2::hood {

namespace {

using Vec3 = std::array<float, 3>;

constexpr float kPaintTextureScale = 13.0f; // neighborhood.matShad nhoodPaintTextureScale (tiles)
constexpr float kMaskSpacing = 2.5f;        // alpha mask resolution (units per value)
constexpr int kTerrainLayer = -8;           // nhoodCanvasLayer
constexpr int kRoadLayer = -1;              // nhoodRoadLayer
constexpr int kWaterLayer = 1;              // nhoodWaterLayer
constexpr int kImposterTerrainLayer = -3;   // nhoodCanvasLayer + 5
constexpr float kWaterGradientZoneSize = 40; // neighborhood.matShad waterGradientZoneSize

std::string fmt(const char* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}

Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        for (auto& x : v)
            x /= l;
    return v;
}

float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 vertexNormal(const Terrain& t, int i, int j) {
    const float dx = t.vertex(i + 1, j) - t.vertex(i - 1, j);
    const float dy = t.vertex(i, j + 1) - t.vertex(i, j - 1);
    return normalize({-dx, -dy, 2 * kHoodCellSize});
}

Vec3 normalAt(const Terrain& t, float x, float y) {
    const float e = kHoodCellSize * 0.5f;
    const float dx = t.heightAt(x + e, y) - t.heightAt(x - e, y);
    const float dy = t.heightAt(x, y + e) - t.heightAt(x, y - e);
    return normalize({-dx, -dy, 2 * e});
}

// Variant of a paint in the grid square holding (x, y) (a fixed hash: the
// game's choice is not decoded).
int variantAt(float x, float y, float grid, int variants, uint32_t paint) {
    if (variants <= 1)
        return 0;
    const int gx = int(std::floor(x / grid)), gy = int(std::floor(y / grid));
    uint32_t h = uint32_t(gx) * 73856093u ^ uint32_t(gy) * 19349663u ^ paint * 83492791u;
    h ^= h >> 13, h *= 0x5bd1e995u, h ^= h >> 15;
    return int(h % uint32_t(variants));
}

MaterialDefinition terrainMaterial(const std::string& texture, bool blended, int layer, float scale, bool clamp) {
    MaterialDefinition m;
    m.materialType = "Terrain";
    // The shader draws light-mapped surfaces as 2 x texture x light map x diffuse.
    m.properties = {{"stdMatBaseTextureName", texture},
                    {"stdMatDiffCoef", "0.5,0.5,0.5,1"},
                    {"osTextureScale", fmt("%.8g", scale)},
                    {"stdMatLayer", std::to_string(layer)},
                    {"stdMatCullMode", "none"}};
    if (blended)
        m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", "blend"}, {"osMaskAlphaOnly", "true"}});
    if (clamp)
        m.properties.insert(m.properties.end(), {{"stdMatBaseTextureAddressingU", "clamp"},
                                                 {"stdMatBaseTextureAddressingV", "clamp"}});
    return m;
}

std::string stripTxtr(std::string s) {
    if (s.size() > 5 && s.compare(s.size() - 5, 5, "_txtr") == 0)
        s.resize(s.size() - 5);
    return s;
}

// Bilinear sample of an RGBA image at (u, v) in [0, 1] (v = 0 at the top row).
std::array<float, 3> sample(const RgbaImage& img, float u, float v) {
    if (!img.width || !img.height)
        return {1, 1, 1};
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(img.width - 1);
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(img.height - 1);
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = std::min(x0 + 1, int(img.width) - 1), y1 = std::min(y0 + 1, int(img.height) - 1);
    const float a = fx - float(x0), b = fy - float(y0);
    std::array<float, 3> out{};
    for (int c = 0; c < 3; ++c) {
        auto px = [&](int x, int y) { return float(img.pixels[(size_t(y) * img.width + size_t(x)) * 4 + size_t(c)]) / 255.0f; };
        out[size_t(c)] = (px(x0, y0) * (1 - a) + px(x1, y0) * a) * (1 - b) + (px(x0, y1) * (1 - a) + px(x1, y1) * a) * b;
    }
    return out;
}

} // namespace

std::vector<float> roadProximity(const Terrain& t, const Occupants& o) {
    std::vector<std::array<float, 2>> centres;
    auto add = [&](const Road& r) {
        float x = 0, y = 0;
        for (const auto& c : r.corners)
            x += c[0] / 4, y += c[1] / 4;
        centres.push_back({x / kHoodCellSize, y / kHoodCellSize});
    };
    for (const auto& r : o.roads)
        add(r);
    for (const auto& b : o.bridges)
        add(b.road);
    std::vector<float> out(size_t(t.gridWidth) * t.gridHeight, 1e6f);
    for (uint32_t j = 0; j < t.gridHeight; ++j)
        for (uint32_t i = 0; i < t.gridWidth; ++i) {
            float best = 1e12f;
            for (const auto& c : centres) {
                const float dx = float(i) - c[0], dy = float(j) - c[1];
                best = std::min(best, dx * dx + dy * dy);
            }
            if (!centres.empty())
                out[size_t(j) * t.gridWidth + i] = std::sqrt(best);
        }
    return out;
}

PaintWeights computePaintWeights(const Terrain& t, const Occupants& o, const TerrainSettings& s) {
    PaintWeights out;
    const size_t w = t.gridWidth, h = t.gridHeight, n = w * h;
    out.assigned.assign(n, 0);
    const auto typeIt = s.types.find(t.type);
    const TerrainSettings::TypeInfo type = typeIt == s.types.end() ? TerrainSettings::TypeInfo{} : typeIt->second;
    const auto mapIt = s.paintMaps.find(t.type);
    if (mapIt == s.paintMaps.end() || mapIt->second.empty())
        throw std::runtime_error("no paint map for terrain type \"" + t.type + "\"");
    const auto& table = mapIt->second;
    const std::vector<float> prox = roadProximity(t, o);

    // Distance (cells) to the nearest vertex under water, for the beach.
    const int beachCells = int(std::ceil(s.maxBeachWidthInCells));
    std::vector<float> strength(n, 1.0f); // cliffness for cliff vertices
    for (size_t j = 0; j < h; ++j)
        for (size_t i = 0; i < w; ++i) {
            const size_t v = j * w + i;
            const float alt = t.heights[v] - t.seaLevel;
            const Vec3 nrm = vertexNormal(t, int(i), int(j));
            const float slope = std::acos(std::clamp(nrm[2], -1.0f, 1.0f)) * 57.29578f;
            const float cliff = std::clamp((slope - s.cliffAngleMin) / std::max(s.cliffAngleMax - s.cliffAngleMin, 1e-3f), 0.0f, 1.0f);
            bool beach = alt < 0;
            if (!beach && alt <= s.maxBeachAltitude)
                for (int dj = -beachCells; dj <= beachCells && !beach; ++dj)
                    for (int di = -beachCells; di <= beachCells && !beach; ++di) {
                        if (float(di * di + dj * dj) > s.maxBeachWidthInCells * s.maxBeachWidthInCells)
                            continue;
                        beach = t.vertex(int(i) + di, int(j) + dj) < t.seaLevel;
                    }
            if (cliff >= 0.5f) {
                out.assigned[v] = type.cliff;
                strength[v] = cliff;
            } else if (beach) {
                out.assigned[v] = type.beach;
            } else {
                const size_t rows = table.size();
                const size_t row = size_t(std::clamp(alt / s.heightRangeForTemperature * float(rows), 0.0f, float(rows - 1)));
                const auto& cols = table[row];
                if (cols.empty())
                    throw std::runtime_error("empty paint map row");
                const size_t col = size_t(std::clamp(prox[v] / s.maxRoadProximity * float(cols.size()), 0.0f,
                                                     float(cols.size() - 1)));
                out.assigned[v] = cols[col];
            }
        }

    // Spread each paint from its vertices by its curve.
    std::map<uint32_t, size_t> slot;
    for (uint32_t p : out.assigned)
        if (!slot.count(p)) {
            slot[p] = out.paints.size();
            out.paints.push_back(p);
        }
    out.weights.assign(out.paints.size(), std::vector<float>(n, 0.0f));
    for (size_t k = 0; k < out.paints.size(); ++k) {
        const uint32_t id = out.paints[k];
        auto pit = s.paints.find(id);
        TerrainSettings::Paint paint;
        if (pit != s.paints.end())
            paint = pit->second;
        const float reach = paint.spread.empty() ? 0.0f : paint.spread.back()[0];
        const int cells = int(std::ceil(reach / kHoodCellSize));
        auto& wk = out.weights[k];
        for (size_t j = 0; j < h; ++j)
            for (size_t i = 0; i < w; ++i) {
                const size_t v = j * w + i;
                if (out.assigned[v] != id)
                    continue;
                for (int dj = -cells; dj <= cells; ++dj)
                    for (int di = -cells; di <= cells; ++di) {
                        const int x = int(i) + di, y = int(j) + dj;
                        if (x < 0 || y < 0 || x >= int(w) || y >= int(h))
                            continue;
                        const float d = std::sqrt(float(di * di + dj * dj)) * kHoodCellSize;
                        const float value = paint.spreadAt(d) * paint.maxStrength * strength[v];
                        float& dst = wk[size_t(y) * w + size_t(x)];
                        dst = std::max(dst, value);
                    }
            }
    }
    for (size_t v = 0; v < n; ++v) {
        float sum = 0;
        for (auto& wk : out.weights)
            sum += wk[v];
        if (sum <= 0) {
            out.weights[slot[out.assigned[v]]][v] = 1;
            continue;
        }
        for (auto& wk : out.weights)
            wk[v] /= sum;
    }
    return out;
}

ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  std::vector<std::string>* report) {
    ObjectModel model;
    model.name = "neighborhood terrain";
    const size_t w = t.gridWidth, h = t.gridHeight;
    MeshGroup mesh;
    for (size_t j = 0; j < h; ++j)
        for (size_t i = 0; i < w; ++i) {
            const float x = float(i) * kHoodCellSize, y = float(j) * kHoodCellSize;
            mesh.positions.push_back({x, y, t.heights[j * w + i]});
            mesh.normals.push_back(vertexNormal(t, int(i), int(j)));
            mesh.uvs.push_back({x, y});
            mesh.uvs2.push_back(hoodLightMapUv(t, x, y));
        }
    for (size_t j = 0; j + 1 < h; ++j)
        for (size_t i = 0; i + 1 < w; ++i) {
            const uint32_t a = uint32_t(j * w + i), b = a + 1, c = a + uint32_t(w) + 1, d = a + uint32_t(w);
            mesh.triangles.insert(mesh.triangles.end(), {a, b, c, a, c, d});
        }

    const PaintWeights pw = computePaintWeights(t, o, s);
    // One layer per (paint, variant), in paint order.
    struct Layer {
        uint32_t paint;
        int variant;
        std::string texture;
        int variants;
    };
    std::vector<Layer> layers;
    for (uint32_t id : pw.paints) {
        auto it = s.paints.find(id);
        if (it == s.paints.end()) {
            if (report)
                report->push_back("paint " + fmt("0x%02X", id) + " is not in [PaintInfos]");
            continue;
        }
        for (int v = 0; v < it->second.variants; ++v)
            layers.push_back({id, v, it->second.variantTexture(v), it->second.variants});
    }
    if (layers.empty())
        throw std::runtime_error("neighborhood terrain: no paints");

    const uint32_t mw = uint32_t(std::lround(t.worldWidth() / kMaskSpacing)) + 1;
    const uint32_t mh = uint32_t(std::lround(t.worldHeight() / kMaskSpacing)) + 1;
    std::vector<float> running(size_t(mw) * mh, 0.0f);
    auto weightAt = [&](size_t paintSlot, float x, float y) {
        const float fx = x / kHoodCellSize, fy = y / kHoodCellSize;
        const int i = std::clamp(int(fx), 0, int(w) - 2), j = std::clamp(int(fy), 0, int(h) - 2);
        const float a = std::clamp(fx - float(i), 0.0f, 1.0f), b = std::clamp(fy - float(j), 0.0f, 1.0f);
        const auto& wk = pw.weights[paintSlot];
        auto at = [&](int x, int y) { return wk[size_t(y) * w + size_t(x)]; };
        return (at(i, j) * (1 - a) + at(i + 1, j) * a) * (1 - b) + (at(i, j + 1) * (1 - a) + at(i + 1, j + 1) * a) * b;
    };
    for (size_t li = 0; li < layers.size(); ++li) {
        const Layer& L = layers[li];
        const size_t paintSlot = size_t(std::find(pw.paints.begin(), pw.paints.end(), L.paint) - pw.paints.begin());
        ObjectModel::AlphaMask mask;
        mask.width = mw, mask.height = mh, mask.spacing = kMaskSpacing;
        mask.values.resize(size_t(mw) * mh);
        size_t covered = 0;
        for (uint32_t y = 0; y < mh; ++y)
            for (uint32_t x = 0; x < mw; ++x) {
                const float wx = float(x) * kMaskSpacing, wy = float(y) * kMaskSpacing;
                float wgt = weightAt(paintSlot, wx, wy);
                if (variantAt(wx, wy, s.textureVariantGridSize, L.variants, L.paint) != L.variant)
                    wgt = 0;
                float& sum = running[size_t(y) * mw + x];
                sum += wgt;
                const float alpha = sum > 0 ? wgt / sum : 0.0f;
                mask.values[size_t(y) * mw + x] = uint8_t(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255));
                covered += wgt > 0;
            }
        const std::string name = "nhood-paint-" + stripTxtr(L.texture);
        const bool first = li == 0;
        model.materials[name] = terrainMaterial(stripTxtr(L.texture), !first, kTerrainLayer, 1 / kPaintTextureScale, false);
        ObjectModel::Part part;
        part.node = part.shape = "terrain";
        part.material = name;
        part.mesh = mesh;
        if (!first) {
            part.mask = int(model.masks.size());
            model.masks.push_back(std::move(mask));
        }
        model.parts.push_back(std::move(part));
        if (report)
            report->push_back(L.texture + ": " + std::to_string(covered) + " of " + std::to_string(size_t(mw) * mh) +
                              " mask values");
    }
    return model;
}

namespace {

// OpenTS2's GetUVsForConnectionFlags: corner UVs of a road tile turned by
// its connections (bit 0: -X, 1: +Y, 2: +X, 3: -Y).
std::array<std::array<float, 2>, 4> roadUvs(uint8_t c) {
    using U = std::array<std::array<float, 2>, 4>;
    U uv{{{1, 1}, {1, 0}, {0, 0}, {0, 1}}};
    if (c & 1)
        uv = U{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    const int n = (c & 1) + ((c >> 1) & 1) + ((c >> 2) & 1) + ((c >> 3) & 1);
    const U r1{{{1, 0}, {0, 0}, {0, 1}, {1, 1}}};
    const U r2{{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    const U r3{{{1, 1}, {1, 0}, {0, 0}, {0, 1}}};
    if (n == 1) {
        if (c == 4)
            uv = r1;
        else if (c == 2)
            uv = r2;
    } else if (n == 2) {
        if (c == 12)
            uv = r1;
        else if (c == 9)
            uv = r3;
        else if (c == 6)
            uv = r2;
    } else if (n == 3) {
        if (c == 13)
            uv = r1;
        else if (c == 11)
            uv = r3;
        else if (c == 14)
            uv = r2;
    }
    return uv;
}

} // namespace

ObjectModel buildHoodRoadModel(const Terrain& t, const Occupants& o) {
    ObjectModel model;
    model.name = "neighborhood roads";
    std::map<std::string, MeshGroup> meshes;
    for (const auto& r : o.roads) {
        char tex[64];
        std::snprintf(tex, sizeof tex, "%s_roads_%08x", t.type.c_str(), r.piece | 4);
        MeshGroup& m = meshes[tex];
        const auto uv = roadUvs(r.connections);
        const uint32_t base = uint32_t(m.positions.size());
        for (size_t k = 0; k < 4; ++k) {
            const auto& c = r.corners[k];
            m.positions.push_back({c[0], c[1], c[2] + 0.05f});
            m.normals.push_back({0, 0, 1});
            m.uvs.push_back(uv[k]);
            m.uvs2.push_back(hoodLightMapUv(t, c[0], c[1]));
        }
        m.triangles.insert(m.triangles.end(), {base, base + 2, base + 3, base + 2, base, base + 1});
    }
    for (auto& [tex, mesh] : meshes) {
        const std::string name = "neighborhood-roads-" + tex;
        model.materials[name] = terrainMaterial(tex, true, kRoadLayer, 1, true);
        ObjectModel::Part part;
        part.node = part.shape = "roads";
        part.material = name;
        part.mesh = std::move(mesh);
        model.parts.push_back(std::move(part));
    }
    return model;
}

ObjectModel buildHoodWaterModel(const Terrain& t) {
    ObjectModel model;
    model.name = "neighborhood water";
    MeshGroup m;
    const float W = t.worldWidth(), H = t.worldHeight(), z = t.seaLevel;
    m.positions = {{0, 0, z}, {W, 0, z}, {W, H, z}, {0, H, z}};
    m.normals.assign(4, {0, 0, 1});
    m.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.triangles = {0, 1, 2, 0, 2, 3};
    MaterialDefinition mat;
    mat.materialType = "StandardMaterial";
    mat.properties = {{"stdMatDiffCoef", "0.4,0.6,0.9,1"},
                      {"stdMatUntexturedDiffAlpha", "0.6"},
                      {"stdMatAlphaBlendMode", "blend"},
                      {"stdMatLightingEnabled", "false"},
                      {"stdMatCullMode", "none"},
                      {"stdMatLayer", std::to_string(kWaterLayer)}};
    model.materials["neighborhood-water"] = mat;
    ObjectModel::Part part;
    part.node = part.shape = "water";
    part.material = "neighborhood-water";
    part.mesh = std::move(m);
    model.parts.push_back(std::move(part));
    return model;
}

RgbaImage computeHoodLightMap(const Terrain& t, const LightingState& state, const RgbaImage& lookup,
                              const RgbaImage& gradient, float shadowStrength, uint32_t size) {
    Vec3 sun{1.75f, -2, 1.5f};
    for (const auto& l : state.lights)
        if (l.name == "sun")
            sun = l.direction;
    const Vec3 L = normalize(sun);
    const Vec3 right = normalize(cross(L, {0, 0, 1}));
    const Vec3 flat = normalize({L[0], L[1], 0});
    RgbaImage img;
    img.width = img.height = size;
    img.pixels.resize(size_t(size) * size * 4);
    const float W = t.worldWidth(), H = t.worldHeight();
    const float step = kHoodCellSize * 0.5f;
    const float rise = L[2] / std::max(std::sqrt(L[0] * L[0] + L[1] * L[1]), 1e-3f); // per unit across
    for (uint32_t py = 0; py < size; ++py)
        for (uint32_t px = 0; px < size; ++px) {
            const float x = (float(px) + 0.5f) / float(size) * W, y = (float(py) + 0.5f) / float(size) * H;
            const float z = t.heightAt(x, y);
            std::array<float, 3> c;
            if (z < t.seaLevel - 0.5f) {
                // NeighborhoodWaterTerrain: the underwater gradient by depth.
                c = gradient.width ? sample(gradient, 0.5f, (t.seaLevel - z) / kWaterGradientZoneSize)
                                   : std::array<float, 3>{0.2f, 0.3f, 0.4f};
            } else {
                const Vec3 n = normalAt(t, x, y);
                float u = std::max(0.0f, dot(n, L) + dot(n, right));
                float v = std::max(0.0f, dot(n, L));
                // Height-map shadow: walk towards the sun over the terrain.
                bool shadow = false;
                for (float d = step; d < 800 && !shadow; d += step) {
                    const float sx = x + flat[0] * d, sy = y + flat[1] * d;
                    if (sx < 0 || sy < 0 || sx > W || sy > H)
                        break;
                    shadow = t.heightAt(sx, sy) > z + 0.5f + d * rise;
                }
                if (shadow)
                    u *= 1 - shadowStrength, v *= 1 - shadowStrength;
                c = sample(lookup, u, 1 - v); // v = 0 at the image's bottom row
            }
            uint8_t* p = &img.pixels[(size_t(py) * size + px) * 4];
            for (int k = 0; k < 3; ++k)
                p[k] = uint8_t(std::lround(std::clamp(c[size_t(k)], 0.0f, 1.0f) * 255));
            p[3] = 255;
        }
    return img;
}

void imposterMaterials(ObjectModel& model, const std::string& scope) {
    auto unlit = [&](const std::string& texture) {
        MaterialDefinition m;
        m.materialType = "StandardMaterial";
        m.properties = {{"stdMatBaseTextureName", scope + "!" + texture},
                        {"stdMatBaseTextureEnabled", "true"},
                        {"stdMatLightingEnabled", "false"},
                        {"stdMatDiffCoef", "1,1,1,1"}};
        return m;
    };
    std::map<std::string, std::string> types;
    for (auto& [name, def] : model.materials) {
        const std::string type = def.materialType;
        types[name] = type;
        const std::string page = def.property("page");
        MaterialDefinition m;
        if (type == "ImposterWallMaterial") {
            m = unlit("walls_" + page);
            m.properties.insert(m.properties.end(), {{"stdMatAlphaTestEnabled", "true"}, {"stdMatAlphaRefValue", "65"}});
        } else if (type == "ImposterDualPackedSliceMaterial") {
            m = unlit("slices_" + page);
            m.properties.insert(m.properties.end(), {{"stdMatAlphaTestEnabled", "true"},
                                                     {"stdMatAlphaRefValue", "128"},
                                                     {"stdMatCullMode", "none"}});
        } else if (type == "ImposterRoofMaterial") {
            m = unlit("roofs");
            m.properties.insert(m.properties.end(), {{"stdMatCullMode", "none"},
                                                     {"stdMatBaseTextureAddressingU", "clamp"},
                                                     {"stdMatBaseTextureAddressingV", "clamp"}});
        } else if (type == "ImposterTerrainMaterial") {
            m = unlit("terrain");
            m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", "blend"},
                                                     {"stdMatCullMode", "none"},
                                                     {"stdMatLayer", std::to_string(kImposterTerrainLayer)},
                                                     {"stdMatBaseTextureAddressingU", "clamp"},
                                                     {"stdMatBaseTextureAddressingV", "clamp"}});
        } else {
            continue;
        }
        m.resource = def.resource;
        def = m;
    }
    // ImposterRoofMaterial's vertex shader: uv = (x / 64, 1 - y / 64).
    for (auto& part : model.parts)
        if (types[part.material] == "ImposterRoofMaterial") {
            part.mesh.uvs.clear();
            for (const auto& p : part.mesh.positions)
                part.mesh.uvs.push_back({p[0] / 64.0f, 1.0f - p[1] / 64.0f});
        }
}

} // namespace os2::hood
