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

namespace {

// Distances by flooding (4 neighbours, +1 a step) from the vertices at 0,
// the others starting at `far` (BV 0x4d098c).
void flood(std::vector<float>& v, size_t w, size_t h) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t j = 0; j < h; ++j)
            for (size_t i = 0; i < w; ++i) {
                float& c = v[j * w + i];
                float best = c;
                if (i > 0) best = std::min(best, v[j * w + i - 1] + 1);
                if (i + 1 < w) best = std::min(best, v[j * w + i + 1] + 1);
                if (j > 0) best = std::min(best, v[(j - 1) * w + i] + 1);
                if (j + 1 < h) best = std::min(best, v[(j + 1) * w + i] + 1);
                if (best < c)
                    c = best, changed = true;
            }
    }
}

} // namespace

std::vector<float> waterProximity(const Terrain& t, const TerrainSettings& s) {
    const size_t w = t.gridWidth, h = t.gridHeight;
    std::vector<float> v(w * h);
    for (size_t k = 0; k < v.size(); ++k)
        v[k] = t.seaLevel - t.heights[k] > 0 ? 0.0f : s.maxBeachWidthInCells + 2;
    flood(v, w, h);
    return v;
}

std::vector<float> roadProximity(const Terrain& t, const Occupants& o, const TerrainSettings& s) {
    const size_t w = t.gridWidth, h = t.gridHeight;
    std::vector<float> v(w * h, s.maxRoadProximity);
    // A vertex whose tile (x..x+1, y..y+1) holds a road is at 0.
    auto mark = [&](const Road& r) {
        float x = 0, y = 0;
        for (const auto& c : r.corners)
            x += c[0] / 4, y += c[1] / 4;
        const int ti = int(std::floor(x / kHoodCellSize)), tj = int(std::floor(y / kHoodCellSize));
        for (int dj = 0; dj <= 1; ++dj)
            for (int di = 0; di <= 1; ++di) {
                const int i = ti + di, j = tj + dj;
                if (i >= 0 && j >= 0 && i < int(w) && j < int(h))
                    v[size_t(j) * w + size_t(i)] = 0;
            }
    };
    for (const auto& r : o.roads)
        mark(r);
    for (const auto& b : o.bridges)
        mark(b.road);
    flood(v, w, h);
    for (auto& x : v)
        x = std::min(15.0f, std::floor(std::min(x, s.maxRoadProximity) * 16 / s.maxRoadProximity));
    return v;
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
    const std::vector<float> road = roadProximity(t, o, s);
    const std::vector<float> water = waterProximity(t, s);
    const float cosMin = std::cos(s.cliffAngleMin * float(M_PI) / 180), cosMax = std::cos(s.cliffAngleMax * float(M_PI) / 180);
    const float k = 1 / std::max(cosMin - cosMax, 1e-6f);
    auto depth = [&](size_t v) { return t.seaLevel - t.heights[v]; };
    auto isBeach = [&](size_t v) {
        return t.heights[v] <= t.seaLevel + s.maxBeachAltitude && water[v] <= s.maxBeachWidthInCells;
    };

    // AddLandTypeAtVertex: up to 3 (paint, weight) pairs per vertex.
    struct Pair {
        uint32_t paint;
        float weight;
    };
    std::vector<std::array<Pair, 3>> pairs(n, {Pair{0, 0}, Pair{0, 0}, Pair{0, 0}});
    auto add = [&](size_t v, uint32_t p, float wgt) {
        auto& ps = pairs[v];
        for (auto& q : ps)
            if (q.weight > 0 && q.paint == p) {
                q.weight = std::max(q.weight, wgt);
                return;
            }
        auto low = std::min_element(ps.begin(), ps.end(), [](const Pair& a, const Pair& b) { return a.weight < b.weight; });
        if (wgt > low->weight)
            *low = {p, wgt};
    };
    auto spread = [&](size_t v, uint32_t p, bool normal) {
        auto it = s.paints.find(p);
        if (it == s.paints.end() || it->second.spread.empty())
            return;
        const auto& paint = it->second;
        const float reach = paint.spread.back()[0];
        const int cells = int(std::ceil(reach / kHoodCellSize));
        const int i0 = int(v % w), j0 = int(v / w);
        for (int dj = -cells; dj <= cells; ++dj)
            for (int di = -cells; di <= cells; ++di) {
                if (!di && !dj)
                    continue;
                const int i = i0 + di, j = j0 + dj;
                if (i < 0 || j < 0 || i >= int(w) || j >= int(h))
                    continue;
                const size_t u = size_t(j) * w + size_t(i);
                if (normal && depth(u) > s.waterDepthThresholdForPaint)
                    continue;
                if (isBeach(u) && depth(u) <= 0)
                    continue;
                const float d = std::sqrt(float(di * di + dj * dj)) * kHoodCellSize;
                const float c = paint.spreadAt(d);
                if (d < reach && c * paint.maxStrength > 0)
                    add(u, p, c);
            }
    };
    for (size_t j = 0; j < h; ++j)
        for (size_t i = 0; i < w; ++i) {
            const size_t v = j * w + i;
            if (depth(v) > s.waterDepthThresholdForPaint)
                continue;
            const float nz = std::abs(vertexNormal(t, int(i), int(j))[2]);
            if (nz < cosMin) {
                float wgt = nz <= cosMax ? 1.0f : (cosMin - nz) * k;
                auto it = s.paints.find(type.cliff);
                wgt *= it == s.paints.end() ? 1.0f : it->second.maxStrength;
                out.assigned[v] = type.cliff;
                if (wgt > 0)
                    add(v, type.cliff, wgt);
            } else if (isBeach(v)) {
                out.assigned[v] = type.beach;
                add(v, type.beach, 1);
                spread(v, type.beach, false);
            } else {
                const int tt = int(std::floor((t.heights[v] - t.seaLevel) * 31 / s.heightRangeForTemperature));
                const size_t row = size_t(std::clamp(31 - tt, 0, int(table.size()) - 1));
                const auto& cols = table[row];
                if (cols.empty())
                    throw std::runtime_error("empty paint map row");
                const uint32_t p = cols[std::min(size_t(road[v]), cols.size() - 1)];
                out.assigned[v] = p;
                add(v, p, 1);
                spread(v, p, true);
            }
        }

    // NormalizePaintWeights.
    std::map<uint32_t, size_t> slot;
    auto slotOf = [&](uint32_t p) {
        auto it = slot.find(p);
        if (it != slot.end())
            return it->second;
        out.paints.push_back(p);
        out.weights.emplace_back(n, 0.0f);
        return slot[p] = out.paints.size() - 1;
    };
    for (size_t v = 0; v < n; ++v) {
        float sum = 0;
        for (const auto& q : pairs[v])
            sum += q.weight;
        if (sum <= 0)
            continue; // deep water: no paint (the canvas is black)
        for (const auto& q : pairs[v])
            if (q.weight > 0)
                out.weights[slotOf(q.paint)][v] = q.weight / sum;
    }
    return out;
}

ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  std::vector<std::string>* report, const std::vector<uint8_t>* hiddenTiles) {
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
            if (hiddenTiles && j * (w - 1) + i < hiddenTiles->size() && (*hiddenTiles)[j * (w - 1) + i])
                continue;
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

bool needsImposterTerrain(size_t groundVertices, uint32_t widthTiles, uint32_t depthTiles) {
    return groundVertices != 4 && groundVertices != size_t(widthTiles + 1) * size_t(depthTiles + 1);
}

ObjectModel buildHoodTerrainPatch(const ObjectModel& terrain, const Terrain& t, const MeshGroup& world) {
    ObjectModel model;
    model.name = "lot imposter terrain";
    MeshGroup mesh = world;
    mesh.uvs.clear(), mesh.uvs2.clear(), mesh.normals.clear();
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& p : mesh.positions) {
        mesh.uvs.push_back({p[0], p[1]});
        mesh.uvs2.push_back(hoodLightMapUv(t, p[0], p[1]));
        const float fx = p[0] / kHoodCellSize, fy = p[1] / kHoodCellSize;
        mesh.normals.push_back(vertexNormal(t, int(std::lround(fx)), int(std::lround(fy))));
        x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
    }
    if (mesh.positions.empty())
        return model;
    for (const auto& part : terrain.parts) {
        ObjectModel::Part p = part;
        p.mesh = mesh;
        if (part.mask >= 0 && size_t(part.mask) < terrain.masks.size()) {
            // The mask over the patch only (and a value round it).
            const auto& m = terrain.masks[size_t(part.mask)];
            const int mx0 = std::max(0, int(std::floor(x0 / m.spacing)) - 1);
            const int my0 = std::max(0, int(std::floor(y0 / m.spacing)) - 1);
            const int mx1 = std::min(int(m.width) - 1, int(std::ceil(x1 / m.spacing)) + 1);
            const int my1 = std::min(int(m.height) - 1, int(std::ceil(y1 / m.spacing)) + 1);
            ObjectModel::AlphaMask c;
            c.width = uint32_t(std::max(1, mx1 - mx0 + 1)), c.height = uint32_t(std::max(1, my1 - my0 + 1));
            c.spacing = m.spacing;
            c.originX = float(mx0) * m.spacing, c.originY = float(my0) * m.spacing;
            c.values.resize(size_t(c.width) * c.height);
            for (uint32_t y = 0; y < c.height; ++y)
                for (uint32_t x = 0; x < c.width; ++x)
                    c.values[size_t(y) * c.width + x] = m.values[size_t(my0 + int(y)) * m.width + size_t(mx0 + int(x))];
            p.mask = int(model.masks.size());
            model.masks.push_back(std::move(c));
        }
        model.parts.push_back(std::move(p));
    }
    model.materials = terrain.materials;
    return model;
}

ObjectModel buildHoodRoadModel(const Terrain& t, const Occupants& o) {
    ObjectModel model;
    model.name = "neighborhood roads";
    std::map<std::string, MeshGroup> meshes;
    for (const auto& r : o.roads) {
        char tex[64];
        std::snprintf(tex, sizeof tex, "%s_roads_%08x", t.type.c_str(), r.piece | 4);
        MeshGroup& m = meshes[tex];
        const uint32_t base = uint32_t(m.positions.size());
        for (size_t k = 0; k < 4; ++k) {
            const auto& c = r.corners[k];
            m.positions.push_back({c[0], c[1], c[2] + 0.05f});
            m.normals.push_back({0, 0, 1});
            m.uvs.push_back(r.uvs[k]);
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
                // Height-map shadow: walk towards the sun over the terrain.
                bool shadow = false;
                for (float d = step; d < 800 && !shadow; d += step) {
                    const float sx = x + flat[0] * d, sy = y + flat[1] * d;
                    if (sx < 0 || sy < 0 || sx > W || sy > H)
                        break;
                    shadow = t.heightAt(sx, sy) > z + 0.5f + d * rise;
                }
                c = sample(lookup, (n[0] + 1) / 2, (n[1] + 1) / 2);
                if (shadow)
                    for (auto& x : c)
                        x *= 1 - shadowStrength;
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
            // Rendered from the lot, the imposter textures have premultiplied
            // alpha: blended as such (one, 1 - alpha) over the alpha test,
            // their soft edges (railings, the watchtower of N002_Lot22)
            // fade into what is behind instead of coming out black.
            m.properties.insert(m.properties.end(), {{"stdMatAlphaTestEnabled", "true"},
                                                     {"stdMatAlphaRefValue", "65"},
                                                     {"stdMatAlphaBlendMode", "premultiplied"}});
        } else if (type == "ImposterDualPackedSliceMaterial") {
            m = unlit("slices_" + page);
            m.properties.insert(m.properties.end(), {{"stdMatAlphaTestEnabled", "true"},
                                                     {"stdMatAlphaRefValue", "128"},
                                                     {"stdMatAlphaBlendMode", "premultiplied"},
                                                     {"stdMatCullMode", "none"}});
        } else if (type == "ImposterRoofMaterial") {
            m = unlit("roofs");
            m.properties.insert(m.properties.end(), {{"stdMatCullMode", "none"},
                                                     {"stdMatBaseTextureAddressingU", "clamp"},
                                                     {"stdMatBaseTextureAddressingV", "clamp"}});
        } else if (type == "ImposterTerrainMaterial") {
            m = unlit("terrain");
            // Premultiplied like the walls (rendered from the lot), over the
            // neighborhood terrain or, where the game hides that, the lot's
            // imposter terrain (buildHoodTerrainPatch).
            m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", "premultiplied"},
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
    // ImposterTerrainMaterial's first pass (includeLightMap): the lot's
    // "terrainLmap" (same texture coordinates, tiled) multiplies what is
    // drawn under the lot (srcFactor(destColor) add dstFactor(invSrcAlpha)):
    // the house's shade on the neighborhood ground. A copy of each ground
    // part, drawn just before it.
    std::vector<ObjectModel::Part> lightMapped;
    for (const auto& part : model.parts)
        if (types[part.material] == "ImposterTerrainMaterial") {
            ObjectModel::Part p = part;
            p.material = part.material + "_lmap";
            if (!model.materials.count(p.material)) {
                MaterialDefinition m = unlit("terrainLmap");
                m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", "multiply"},
                                                         {"stdMatCullMode", "none"},
                                                         {"stdMatLayer", std::to_string(kImposterTerrainLayer - 1)}});
                m.resource = model.materials[part.material].resource;
                model.materials[p.material] = m;
                types[p.material] = "ImposterTerrainLightMap";
            }
            lightMapped.push_back(std::move(p));
        }
    model.parts.insert(model.parts.end(), lightMapped.begin(), lightMapped.end());
    // ImposterRoofMaterial's vertex shader: uv = (x / 64, 1 - y / 64).
    for (auto& part : model.parts)
        if (types[part.material] == "ImposterRoofMaterial") {
            part.mesh.uvs.clear();
            for (const auto& p : part.mesh.positions)
                part.mesh.uvs.push_back({p[0] / 64.0f, 1.0f - p[1] / 64.0f});
        }
}

} // namespace os2::hood
