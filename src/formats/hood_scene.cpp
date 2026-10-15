#include "hood_scene.hpp"

#include "lighting_config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace os2::hood {

namespace {

using Vec3 = std::array<float, 3>;

constexpr float kPaintTextureScale = 13.0f; // neighborhood.matShad nhoodPaintTextureScale (tiles)
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


Vec3 normalAt(const Terrain& t, float x, float y) {
    const float e = kHoodCellSize * 0.5f;
    const float dx = t.heightAt(x + e, y) - t.heightAt(x - e, y);
    const float dy = t.heightAt(x, y + e) - t.heightAt(x, y - e);
    return normalize({-dx, -dy, 2 * e});
}

MaterialDefinition terrainMaterial(const std::string& texture, bool blended, int layer, float scale, bool clamp,
                                   const char* blendMode = "blend") {
    MaterialDefinition m;
    m.materialType = "Terrain";
    // The shader draws light-mapped surfaces as 2 x texture x light map x diffuse.
    m.properties = {{"stdMatBaseTextureName", texture},
                    {"stdMatDiffCoef", "0.5,0.5,0.5,1"},
                    {"osTextureScale", fmt("%.8g", scale)},
                    {"stdMatLayer", std::to_string(layer)},
                    {"stdMatCullMode", "none"}};
    if (blended)
        m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", blendMode}, {"osMaskAlphaOnly", "true"}});
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

std::vector<std::array<float, 3>> terrainNormals(const Terrain& t) {
    const int tw = int(t.width), th = int(t.height), w = int(t.gridWidth);
    std::vector<std::array<float, 3>> out(size_t(t.gridWidth) * t.gridHeight, std::array<float, 3>{0, 0, 1});
    if (tw <= 0 || th <= 0 || t.heights.size() != out.size())
        return out;
    auto cross = [](Vec3 a, Vec3 b) {
        return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    // Per tile: [0] the upper face (0,0)-(1,1)-(0,1) = D x A, [1] the lower
    // face (0,0)-(1,0)-(1,1) = B x D (BV FUN_004d1e00).
    std::vector<std::array<Vec3, 2>> faces(size_t(tw) * size_t(th));
    const float c = kHoodCellSize;
    for (int j = 0; j < th; ++j)
        for (int i = 0; i < tw; ++i) {
            auto h = [&](int x, int y) { return t.heights[size_t(y) * size_t(w) + size_t(x)]; };
            const float h00 = h(i, j);
            const Vec3 A{0, c, h(i, j + 1) - h00}, D{c, c, h(i + 1, j + 1) - h00}, B{c, 0, h(i + 1, j) - h00};
            faces[size_t(j) * size_t(tw) + size_t(i)] = {normalize(cross(D, A)), normalize(cross(B, D))};
        }
    for (int y = 0; y <= th; ++y)
        for (int x = 0; x <= tw; ++x) {
            Vec3 sum{0, 0, 0};
            auto add = [&](int tx, int ty, int face) {
                const Vec3& f = faces[size_t(ty) * size_t(tw) + size_t(tx)][size_t(face)];
                for (size_t k = 0; k < 3; ++k)
                    sum[k] += f[k];
            };
            if (x > 0) {
                if (y < th)
                    add(x - 1, y, 1);
                if (y > 0)
                    add(x - 1, y - 1, 1), add(x - 1, y - 1, 0);
            }
            if (x < tw) {
                if (y < th)
                    add(x, y, 0), add(x, y, 1);
                if (y > 0)
                    add(x, y - 1, 0);
            }
            if (sum[0] == 0 && sum[1] == 0 && sum[2] == 0)
                sum = {1, 1, 1};
            out[size_t(y) * size_t(w) + size_t(x)] = normalize(sum);
        }
    return out;
}

uint32_t GameRandom::next(uint32_t n) {
    if (n == 0)
        return 0;
    const uint64_t product = uint64_t(state) * 0x41c64e6dull + 0x3039ull;
    state = uint32_t(product);
    const uint32_t r = uint32_t(product >> 16);
    return uint32_t((uint64_t(n) * r) >> 32);
}

uint32_t clockSeed() {
    const uint32_t c = uint32_t(std::clock());
    return uint32_t(std::time(nullptr)) ^ (c << 16 | c);
}

TerrainVariants TerrainVariants::make(const Terrain& t, const TerrainSettings& s, uint32_t seed) {
    TerrainVariants v;
    v.grid = uint32_t(std::max(1, int(s.textureVariantGridSize)));
    v.cellsX = (t.width + v.grid - 1) / v.grid;
    v.cellsY = (t.height + v.grid - 1) / v.grid;
    GameRandom rng{seed};
    v.bytes.resize(size_t(v.cellsX) * v.cellsY);
    for (auto& b : v.bytes)
        b = uint8_t(rng.next(256));
    return v;
}

int TerrainVariants::variant(uint32_t tileX, uint32_t tileY, int variants) const {
    if (variants <= 1 || bytes.empty())
        return 0;
    const uint32_t cx = std::min(tileX / grid, cellsX - 1), cy = std::min(tileY / grid, cellsY - 1);
    return int(bytes[size_t(cy) * cellsX + cx]) % variants;
}

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
    const std::vector<std::array<float, 3>> normals = terrainNormals(t);
    auto depth = [&](size_t v) { return t.seaLevel - t.heights[v]; };
    auto isBeach = [&](size_t v) {
        return t.heights[v] <= t.seaLevel + s.maxBeachAltitude && water[v] <= s.maxBeachWidthInCells;
    };

    // AddLandTypeAtVertex (BV 0x4d0d20): up to 3 (paint, weight) pairs per
    // vertex, filled in order, the weight kept as u16(w x 255 + 1); the same
    // paint keeps the larger weight, a full vertex trades its first lightest
    // pair for a heavier one.
    struct Pair {
        uint32_t paint;
        uint16_t weight;
    };
    std::vector<std::array<Pair, 3>> pairs(n, {Pair{0, 0}, Pair{0, 0}, Pair{0, 0}});
    auto add = [&](size_t v, uint32_t p, float wgt) {
        const uint16_t q = uint16_t(uint32_t(double(wgt) * 255.0 + 1.0));
        auto& ps = pairs[v];
        for (auto& pr : ps) {
            if (pr.paint == 0) {
                pr = {p, q};
                return;
            }
            if (pr.paint == p) {
                if (q > pr.weight)
                    pr.weight = q;
                return;
            }
        }
        size_t low = 0;
        for (size_t k = 1; k < 3; ++k)
            if (ps[k].weight < ps[low].weight)
                low = k;
        if (ps[low].weight < q)
            ps[low] = {p, q};
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
            const float nz = std::abs(normals[v][2]);
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

    // NormalizePaintWeights (BV 0x4d3478): each weight x 250 / the vertex's
    // sum, truncated, is the paint's alpha byte there (SetPaintValue).
    std::map<uint32_t, size_t> slot;
    auto slotOf = [&](uint32_t p) {
        auto it = slot.find(p);
        if (it != slot.end())
            return it->second;
        out.paints.push_back(p);
        out.alpha.emplace_back(n, uint8_t(0));
        return slot[p] = out.paints.size() - 1;
    };
    for (size_t v = 0; v < n; ++v) {
        uint32_t sum = 0;
        for (const auto& q : pairs[v]) {
            if (q.paint == 0)
                break;
            sum += q.weight;
        }
        if (sum == 0)
            continue; // deep water: no paint (the canvas is black)
        for (const auto& q : pairs[v]) {
            if (q.paint == 0)
                break;
            out.alpha[slotOf(q.paint)][v] = uint8_t(uint32_t(q.weight) * 250u / sum);
        }
    }
    return out;
}

namespace {

// The black canvas under the paints (NeighborhoodCanvas: z only, black).
MaterialDefinition canvasMaterial() {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatDiffCoef", "0,0,0,1"},
                    {"stdMatLightingEnabled", "false"},
                    {"stdMatCullMode", "none"},
                    {"stdMatLayer", std::to_string(kTerrainLayer)}};
    return m;
}

constexpr const char* kCanvas = "nhood-canvas";

// A paint part's variant and variant count, from its shape "paint v n".
bool paintVariant(const std::string& shape, int& variant, int& variants) {
    return std::sscanf(shape.c_str(), "paint %d %d", &variant, &variants) == 2;
}

} // namespace

ObjectModel buildHoodTerrainModel(const Terrain& t, const Occupants& o, const TerrainSettings& s,
                                  const TerrainVariants& variants, std::vector<std::string>* report,
                                  const std::vector<uint8_t>* hiddenTiles) {
    ObjectModel model;
    model.name = "neighborhood terrain";
    const size_t w = t.gridWidth, h = t.gridHeight;
    const std::vector<std::array<float, 3>> normals = terrainNormals(t);
    MeshGroup mesh;
    for (size_t j = 0; j < h; ++j)
        for (size_t i = 0; i < w; ++i) {
            const float x = float(i) * kHoodCellSize, y = float(j) * kHoodCellSize;
            mesh.positions.push_back({x, y, t.heights[j * w + i]});
            mesh.normals.push_back(normals[j * w + i]);
            mesh.uvs.push_back({x, y});
            mesh.uvs2.push_back(hoodLightMapUv(t, x, y));
        }
    auto hidden = [&](size_t i, size_t j) {
        return hiddenTiles && j * (w - 1) + i < hiddenTiles->size() && (*hiddenTiles)[j * (w - 1) + i];
    };
    auto tile = [&](size_t i, size_t j, std::vector<uint32_t>& tris) {
        const uint32_t a = uint32_t(j * w + i), b = a + 1, c = a + uint32_t(w) + 1, d = a + uint32_t(w);
        tris.insert(tris.end(), {a, b, c, a, c, d});
    };

    // The canvas: every shown tile, black, writing depth.
    {
        ObjectModel::Part part;
        part.node = "terrain";
        part.shape = "canvas";
        part.material = kCanvas;
        part.mesh = mesh;
        for (size_t j = 0; j + 1 < h; ++j)
            for (size_t i = 0; i + 1 < w; ++i)
                if (!hidden(i, j))
                    tile(i, j, part.mesh.triangles);
        model.materials[kCanvas] = canvasMaterial();
        model.parts.push_back(std::move(part));
    }

    const PaintWeights pw = computePaintWeights(t, o, s);
    for (size_t slot = 0; slot < pw.paints.size(); ++slot) {
        const uint32_t id = pw.paints[slot];
        auto it = s.paints.find(id);
        if (it == s.paints.end()) {
            if (report)
                report->push_back("paint " + fmt("0x%02X", id) + " is not in [PaintInfos]");
            continue;
        }
        const auto& alpha = pw.alpha[slot];
        // The paint's alpha map: one value per vertex, at the tile's middle.
        ObjectModel::AlphaMask mask;
        mask.width = uint32_t(w), mask.height = uint32_t(h), mask.spacing = kHoodCellSize;
        mask.originX = mask.originY = kHoodCellSize / 2;
        mask.values = alpha;
        // GetPaintsInArea over [x - 1, x + 2] x [y - 1, y + 2]: the tiles the paint reaches.
        auto present = [&](size_t i, size_t j) {
            const size_t x0 = i > 0 ? i - 1 : 0, y0 = j > 0 ? j - 1 : 0;
            const size_t x1 = std::min(i + 2, w - 1), y1 = std::min(j + 2, h - 1);
            for (size_t y = y0; y <= y1; ++y)
                for (size_t x = x0; x <= x1; ++x)
                    if (alpha[y * w + x])
                        return true;
            return false;
        };
        const int count = std::max(1, it->second.variants);
        const int maskIndex = int(model.masks.size());
        model.masks.push_back(std::move(mask));
        for (int v = 0; v < count; ++v) {
            ObjectModel::Part part;
            part.node = "terrain";
            part.shape = "paint " + std::to_string(v) + " " + std::to_string(count);
            const std::string texture = stripTxtr(it->second.variantTexture(v));
            part.material = "nhood-paint-" + texture;
            part.mesh = mesh;
            part.mask = maskIndex;
            size_t tiles = 0;
            for (size_t j = 0; j + 1 < h; ++j)
                for (size_t i = 0; i + 1 < w; ++i)
                    if (!hidden(i, j) && variants.variant(uint32_t(i), uint32_t(j), count) == v && present(i, j))
                        tile(i, j, part.mesh.triangles), ++tiles;
            if (report)
                report->push_back(texture + ": " + std::to_string(tiles) + " tiles");
            if (part.mesh.triangles.empty())
                continue;
            model.materials[part.material] =
                terrainMaterial(texture, true, kTerrainLayer, 1 / kPaintTextureScale, false, "additive");
            model.parts.push_back(std::move(part));
        }
    }
    if (model.parts.size() < 2)
        throw std::runtime_error("neighborhood terrain: no paints");
    return model;
}

bool needsImposterTerrain(size_t groundVertices, uint32_t widthTiles, uint32_t depthTiles) {
    return groundVertices != 4 && groundVertices != size_t(widthTiles + 1) * size_t(depthTiles + 1);
}

ObjectModel buildHoodTerrainPatch(const ObjectModel& terrain, const Terrain& t, const MeshGroup& world,
                                  const TerrainVariants& variants) {
    ObjectModel model;
    model.name = "lot imposter terrain";
    MeshGroup mesh = world;
    mesh.uvs.clear(), mesh.uvs2.clear(), mesh.normals.clear();
    const std::vector<std::array<float, 3>> normals = terrainNormals(t);
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& p : mesh.positions) {
        mesh.uvs.push_back({p[0], p[1]});
        mesh.uvs2.push_back(hoodLightMapUv(t, p[0], p[1]));
        const int i = std::clamp(int(std::lround(p[0] / kHoodCellSize)), 0, int(t.gridWidth) - 1);
        const int j = std::clamp(int(std::lround(p[1] / kHoodCellSize)), 0, int(t.gridHeight) - 1);
        mesh.normals.push_back(normals[size_t(j) * t.gridWidth + size_t(i)]);
        x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
    }
    if (mesh.positions.empty())
        return model;
    // The tile each triangle is in (by its centre), for its variant.
    std::vector<std::array<uint32_t, 2>> tiles;
    for (size_t k = 0; k + 2 < mesh.triangles.size(); k += 3) {
        float cx = 0, cy = 0;
        for (size_t c = 0; c < 3; ++c)
            cx += mesh.positions[mesh.triangles[k + c]][0] / 3, cy += mesh.positions[mesh.triangles[k + c]][1] / 3;
        tiles.push_back({uint32_t(std::clamp(int(cx / kHoodCellSize), 0, int(t.width) - 1)),
                         uint32_t(std::clamp(int(cy / kHoodCellSize), 0, int(t.height) - 1))});
    }
    for (const auto& part : terrain.parts) {
        ObjectModel::Part p = part;
        p.mesh = mesh;
        int variant = 0, count = 1;
        if (paintVariant(part.shape, variant, count)) {
            p.mesh.triangles.clear();
            for (size_t k = 0; k < tiles.size(); ++k)
                if (variants.variant(tiles[k][0], tiles[k][1], count) == variant)
                    p.mesh.triangles.insert(p.mesh.triangles.end(), mesh.triangles.begin() + std::ptrdiff_t(k * 3),
                                            mesh.triangles.begin() + std::ptrdiff_t(k * 3 + 3));
            if (p.mesh.triangles.empty())
                continue;
        }
        if (part.mask >= 0 && size_t(part.mask) < terrain.masks.size()) {
            // The mask over the patch only (and a value round it).
            const auto& m = terrain.masks[size_t(part.mask)];
            const int mx0 = std::max(0, int(std::floor((x0 - m.originX) / m.spacing)) - 1);
            const int my0 = std::max(0, int(std::floor((y0 - m.originY) / m.spacing)) - 1);
            const int mx1 = std::min(int(m.width) - 1, int(std::ceil((x1 - m.originX) / m.spacing)) + 1);
            const int my1 = std::min(int(m.height) - 1, int(std::ceil((y1 - m.originY) / m.spacing)) + 1);
            ObjectModel::AlphaMask c;
            c.width = uint32_t(std::max(1, mx1 - mx0 + 1)), c.height = uint32_t(std::max(1, my1 - my0 + 1));
            c.spacing = m.spacing;
            c.originX = m.originX + float(mx0) * m.spacing, c.originY = m.originY + float(my0) * m.spacing;
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
                const Vec3 n = normalAt(t, x, y); // (central differences)
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

std::array<float, 3> terrainShade(const Terrain& t, const RgbaImage& lightMap, const RgbaImage& lookup, float x, float y) {
    std::array<float, 3> out{1, 1, 1};
    if (!lightMap.width || !lookup.width)
        return out;
    const auto uv = hoodLightMapUv(t, x, y);
    const auto here = sample(lightMap, uv[0], uv[1]);
    const auto open = sample(lookup, 0.5f, 0.5f);
    for (size_t k = 0; k < 3; ++k)
        out[k] = open[k] > 1e-3f ? std::clamp(here[k] / open[k], 0.4f, 1.0f) : 1.0f;
    return out;
}

void imposterMaterials(ObjectModel& model, const std::string& scope, std::array<float, 3> shade) {
    char coef[64];
    std::snprintf(coef, sizeof coef, "%g,%g,%g,1", shade[0], shade[1], shade[2]);
    auto unlit = [&](const std::string& texture, bool shaded = true) {
        MaterialDefinition m;
        m.materialType = "StandardMaterial";
        m.properties = {{"stdMatBaseTextureName", scope + "!" + texture},
                        {"stdMatBaseTextureEnabled", "true"},
                        {"stdMatLightingEnabled", "false"},
                        {"stdMatDiffCoef", shaded ? coef : "1,1,1,1"}};
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
            m = unlit("terrain", false);
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
                MaterialDefinition m = unlit("terrainLmap", false);
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

MeshGroup relationshipArchMesh(std::array<float, 3> from, std::array<float, 3> to, float radius, float angleDegrees,
                               int along, int around) {
    MeshGroup m;
    m.name = "arch";
    along = std::max(along, 2), around = std::max(around, 3);
    const float dx = to[0] - from[0], dy = to[1] - from[1];
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float top = std::max(from[2], to[2]) + std::tan(angleDegrees * float(M_PI) / 180.0f) * dist * 0.25f;
    const std::array<std::array<float, 3>, 4> p{{from,
                                                 {from[0] + dx * 0.25f, from[1] + dy * 0.25f, top},
                                                 {from[0] + dx * 0.75f, from[1] + dy * 0.75f, top},
                                                 to}};
    auto bez = [&](float t, size_t k) {
        const float u = 1 - t;
        return u * u * u * p[0][k] + 3 * u * u * t * p[1][k] + 3 * u * t * t * p[2][k] + t * t * t * p[3][k];
    };
    auto tangent = [&](float t, size_t k) {
        const float u = 1 - t;
        return 3 * u * u * (p[1][k] - p[0][k]) + 6 * u * t * (p[2][k] - p[1][k]) + 3 * t * t * (p[3][k] - p[2][k]);
    };
    auto norm = [](std::array<float, 3> v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return l > 1e-6f ? std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l} : std::array<float, 3>{0, 0, 1};
    };
    auto cross = [](std::array<float, 3> a, std::array<float, 3> b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    for (int i = 0; i < along; ++i) {
        const float t = float(i) / float(along - 1);
        const std::array<float, 3> c{bez(t, 0), bez(t, 1), bez(t, 2)};
        const auto dir = norm({tangent(t, 0), tangent(t, 1), tangent(t, 2)});
        // The ring's frame: across the arch (horizontal) and the tangent's
        // normal in the arch's plane.
        const auto side = norm(cross(dir, {0, 0, 1}));
        const auto up = norm(cross(side, dir));
        for (int j = 0; j < around; ++j) {
            const float a = 2.0f * float(M_PI) * float(j) / float(around - 1);
            const std::array<float, 3> n{std::cos(a) * side[0] + std::sin(a) * up[0],
                                         std::cos(a) * side[1] + std::sin(a) * up[1],
                                         std::cos(a) * side[2] + std::sin(a) * up[2]};
            m.positions.push_back({c[0] + n[0] * radius, c[1] + n[1] * radius, c[2] + n[2] * radius});
            m.normals.push_back(n);
            m.uvs.push_back({t, float(j) / float(around - 1)});
        }
    }
    for (int i = 0; i + 1 < along; ++i)
        for (int j = 0; j + 1 < around; ++j) {
            const uint32_t a = uint32_t(i * around + j), b = a + 1;
            const uint32_t c = a + uint32_t(around), d = b + uint32_t(around);
            m.triangles.insert(m.triangles.end(), {a, c, d, a, d, b});
        }
    return m;
}

} // namespace os2::hood
