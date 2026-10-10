#include "lot_model.hpp"

#include "config_text.hpp"
#include "lot.hpp"
#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxmt = 0x49596978, kTypeCatalogPattern = 0xCCA8E925;

// Wall slab half thickness, in tiles (inferred from screenshots of the game).
constexpr float kWallHalfThickness = 0.075f;
// Ground floors sit on the terrain; lift them so they win the depth test
// against the terrain paints drawn after them.
constexpr float kGroundFloorLift = 0.005f;
// Terrain textures repeat every this many tiles (not found in the data yet).
constexpr float kTerrainTextureTiles = 4.0f;
constexpr float kStoryHeight = 3.0f;

// `kind` "floorPattern" / "wallPattern" blocks of a catalog script, each
// followed by a `material "..."` line.
std::map<std::string, std::string> loadPatternScript(const std::filesystem::path& path, std::string_view kind) {
    std::map<std::string, std::string> out;
    if (!std::filesystem::exists(path))
        return out;
    std::string current;
    forEachConfigLine(readTextFile(path), [&](std::string_view line) {
        const auto t = tokenizeConfigLine(line);
        if (t.size() < 2 || t[0].kind != ConfigToken::Word || t[1].kind != ConfigToken::Word)
            return;
        if (t[0].word == kind)
            current = lowerAscii(t[1].word);
        else if (t[0].word == "material" && !current.empty())
            out[current] = t[1].word;
    });
    return out;
}

std::optional<uint32_t> parseGuid(const std::string& s) {
    if (s.empty() || s.size() > 8 || !std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)); }))
        return std::nullopt;
    return uint32_t(std::stoul(s, nullptr, 16));
}

MaterialDefinition terrainMaterial(const std::string& texture, bool blended) {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatBaseTextureName", texture}, {"stdMatDiffCoef", "0.8,0.8,0.8,1"}};
    if (blended)
        m.properties.emplace_back("stdMatAlphaBlendMode", "blend");
    return m;
}

using Vec3f = std::array<float, 3>;

struct MeshBuilder {
    MeshGroup mesh;

    uint32_t vertex(const Vec3f& p, const Vec3f& n, float u, float v) {
        mesh.positions.push_back(p);
        mesh.normals.push_back(n);
        mesh.uvs.push_back({u, v});
        return uint32_t(mesh.positions.size() - 1);
    }
    void triangle(uint32_t a, uint32_t b, uint32_t c) { mesh.triangles.insert(mesh.triangles.end(), {a, b, c}); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        triangle(a, b, c);
        triangle(a, c, d);
    }
};

Vec3f normalOf(const Vec3f& a, const Vec3f& b, const Vec3f& c) {
    const Vec3f u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    Vec3f n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0)
        for (auto& x : n)
            x /= len;
    return n;
}

// Terrain grid over level 0; `keep(x, y)` picks the tiles to emit.
template <typename Keep>
MeshGroup terrainMesh(const Lot& lot, Keep&& keep) {
    MeshBuilder b;
    const int w = int(lot.width), h = int(lot.height);
    for (int x = 0; x <= w; ++x)
        for (int y = 0; y <= h; ++y) {
            const float dx = lot.elevation(0, x + 1, y) - lot.elevation(0, x - 1, y);
            const float dy = lot.elevation(0, x, y + 1) - lot.elevation(0, x, y - 1);
            const float len = std::sqrt(dx * dx + dy * dy + 4.0f);
            b.vertex({float(x), float(y), lot.elevation(0, x, y)}, {-dx / len, -dy / len, 2.0f / len},
                     float(x) / kTerrainTextureTiles, float(y) / kTerrainTextureTiles);
        }
    auto at = [&](int x, int y) { return uint32_t(x * (h + 1) + y); };
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y)
            if (keep(x, y))
                b.quad(at(x, y), at(x + 1, y), at(x + 1, y + 1), at(x, y + 1));
    return std::move(b.mesh);
}

// Quarter-tile triangles (west, north, east, south) of each floor tile, with
// the tile's own texture coordinates, one mesh per material.
void addFloors(const Lot& lot, const PatternCatalog& patterns, std::map<std::string, MeshBuilder>& out,
               std::vector<std::string>& warnings) {
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.floorPatterns) {
        const std::string m = patterns.floorMaterial(pattern);
        if (m.empty())
            warnings.push_back("floor pattern " + pattern + " has no material");
        materials[ref] = m;
    }
    for (int level = lot.minLevel; level < lot.minLevel + lot.levelCount; ++level) {
        const float lift = level == 0 ? kGroundFloorLift : 0.0f;
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y) {
                const auto& quarters = lot.floor(level, x, y);
                if (quarters == std::array<uint16_t, 4>{})
                    continue;
                const int ix = int(x), iy = int(y);
                const float z00 = lot.elevation(level, ix, iy) + lift, z10 = lot.elevation(level, ix + 1, iy) + lift,
                            z11 = lot.elevation(level, ix + 1, iy + 1) + lift,
                            z01 = lot.elevation(level, ix, iy + 1) + lift;
                const float fx = float(x), fy = float(y);
                const Vec3f p00{fx, fy, z00}, p10{fx + 1, fy, z10}, p11{fx + 1, fy + 1, z11}, p01{fx, fy + 1, z01},
                    c{fx + 0.5f, fy + 0.5f, (z00 + z10 + z11 + z01) / 4};
                // Counter-clockwise from above: west, north, east, south.
                const std::array<std::array<Vec3f, 2>, 4> edges{{{p00, p01}, {p01, p11}, {p11, p10}, {p10, p00}}};
                for (size_t q = 0; q < 4; ++q) {
                    if (!quarters[q])
                        continue;
                    auto it = materials.find(quarters[q]);
                    if (it == materials.end() || it->second.empty())
                        continue;
                    MeshBuilder& b = out["floor:" + it->second];
                    const Vec3f& a = edges[q][1];
                    const Vec3f& e = edges[q][0];
                    const Vec3f n = normalOf(a, e, c);
                    auto v = [&](const Vec3f& p) { return b.vertex(p, n, p[0] - fx, 1.0f - (p[1] - fy)); };
                    b.triangle(v(a), v(e), v(c));
                }
            }
    }
}

// Walls as slabs: one face per side with that side's covering, plus a top.
// Each slab is stretched by its half thickness at both ends to close corners;
// texture coordinates follow world position, so overlaps line up.
void addWalls(const Lot& lot, const PatternCatalog& patterns, std::map<std::string, MeshBuilder>& out,
              std::vector<std::string>& warnings) {
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.wallPatterns)
        materials[ref] = patterns.wallMaterial(pattern);
    size_t fences = 0, attic = 0;
    for (const auto& w : lot.walls) {
        if (w.type > 0xFFFF) { // fence GUID
            ++fences;
            continue;
        }
        if (w.type == Lot::Attic) { // gable walls take their shape from the roof
            ++attic;
            continue;
        }
        float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        dx /= len, dy /= len;
        const int level = w.a.level;
        auto ground = [&](const Lot::WallVertex& v, int l) {
            const int x = int(std::lround(v.x)), y = int(std::lround(v.y));
            return lot.hasLevel(l) ? lot.elevation(l, x, y) : lot.elevation(level, x, y) + kStoryHeight;
        };
        const float za0 = ground(w.a, level), zb0 = ground(w.b, level);
        const float za1 = ground(w.a, level + 1), zb1 = ground(w.b, level + 1);
        const float t = kWallHalfThickness;
        const float ax = w.a.x - dx * t, ay = w.a.y - dy * t, bx = w.b.x + dx * t, by = w.b.y + dy * t;
        const float nx = dy, ny = -dx; // right of a -> b

        for (int side = 0; side < 2; ++side) {
            auto it = materials.find(w.patterns[size_t(side)]);
            if (it == materials.end() || it->second.empty())
                continue;
            MeshBuilder& b = out["wall:" + it->second];
            const float s = side == 0 ? 1.0f : -1.0f;
            const Vec3f n{nx * s, ny * s, 0};
            const Vec3f pa0{ax + nx * t * s, ay + ny * t * s, za0}, pb0{bx + nx * t * s, by + ny * t * s, zb0};
            const Vec3f pa1{pa0[0], pa0[1], za1}, pb1{pb0[0], pb0[1], zb1};
            // u runs along the wall in world units (right to left as seen from the face).
            auto u = [&](const Vec3f& p) { return s * (p[0] * dx + p[1] * dy); };
            auto vv = [&](const Vec3f& p, float top) { return (top - p[2]) / kStoryHeight; };
            const uint32_t i0 = b.vertex(pa0, n, u(pa0), vv(pa0, za1)), i1 = b.vertex(pb0, n, u(pb0), vv(pb0, zb1)),
                           i2 = b.vertex(pb1, n, u(pb1), 0), i3 = b.vertex(pa1, n, u(pa1), 0);
            if (side == 0)
                b.quad(i0, i1, i2, i3);
            else
                b.quad(i1, i0, i3, i2);
        }
        MeshBuilder& top = out["walltop:"];
        const Vec3f up{0, 0, 1};
        const uint32_t r0 = top.vertex({ax + nx * t, ay + ny * t, za1}, up, 0, 0),
                       r1 = top.vertex({bx + nx * t, by + ny * t, zb1}, up, 0, 0),
                       l1 = top.vertex({bx - nx * t, by - ny * t, zb1}, up, 0, 0),
                       l0 = top.vertex({ax - nx * t, ay - ny * t, za1}, up, 0, 0);
        top.quad(r0, r1, l1, l0);
    }
    if (fences)
        warnings.push_back(std::to_string(fences) + " fence segments not drawn");
    if (attic)
        warnings.push_back(std::to_string(attic) + " attic walls not drawn");
}

} // namespace

PatternCatalog::PatternCatalog(ResourceIndex& index, const std::filesystem::path& resRoot) {
    const auto scripts = resRoot / "Catalog/Scripts";
    floors_ = loadPatternScript(scripts / "floorPatterns.txt", "floorPattern");
    walls_ = loadPatternScript(scripts / "wallPatterns.txt", "wallPattern");
    for (const auto& loc : index.ofType(kTypeCatalogPattern)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto guid = props.find("guid"), material = props.find("material");
        if (guid == props.end() || material == props.end())
            continue;
        try {
            byGuid_[uint32_t(std::stoul(guid->second))] = material->second;
        } catch (const std::logic_error&) {
        }
    }
}

std::string PatternCatalog::material(const std::map<std::string, std::string>& scripts,
                                     const std::string& pattern) const {
    if (auto it = scripts.find(lowerAscii(pattern)); it != scripts.end())
        return it->second;
    if (auto guid = parseGuid(pattern))
        if (auto it = byGuid_.find(*guid); it != byGuid_.end())
            return it->second;
    return pattern;
}

ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns) {
    ObjectModel model;
    model.name = "lot";
    model.warnings = lot.warnings;

    // Terrain: the base everywhere, then each paint over the tiles its mask touches.
    if (!lot.terrainBase.empty()) {
        model.materials["terrain:" + lot.terrainBase] = terrainMaterial(lot.terrainBase, false);
        ObjectModel::Part base;
        base.material = "terrain:" + lot.terrainBase;
        base.mesh = terrainMesh(lot, [](int, int) { return true; });
        model.parts.push_back(std::move(base));
    }
    const uint32_t mw = lot.maskWidth, mh = lot.maskHeight;
    const float spacing = lot.width ? float(lot.width) / float(std::max(mw, 2u) - 1) : 1.0f;
    for (const auto& paint : lot.paints) {
        if (std::none_of(paint.mask.begin(), paint.mask.end(), [](uint8_t v) { return v != 0; }))
            continue;
        ObjectModel::AlphaMask mask;
        mask.width = mw, mask.height = mh, mask.spacing = spacing;
        mask.values = paint.mask;
        auto touched = [&](int tx, int ty) {
            const int x0 = int(std::floor(float(tx) / spacing)), x1 = int(std::ceil(float(tx + 1) / spacing));
            const int y0 = int(std::floor(float(ty) / spacing)), y1 = int(std::ceil(float(ty + 1) / spacing));
            for (int x = std::max(x0, 0); x <= std::min(x1, int(mw) - 1); ++x)
                for (int y = std::max(y0, 0); y <= std::min(y1, int(mh) - 1); ++y)
                    if (mask.values[size_t(y) * mw + size_t(x)])
                        return true;
            return false;
        };
        ObjectModel::Part part;
        part.material = "terrain:" + paint.texture;
        part.mesh = terrainMesh(lot, touched);
        part.mask = int(model.masks.size());
        model.masks.push_back(std::move(mask));
        model.materials[part.material] = terrainMaterial(paint.texture, true);
        model.parts.push_back(std::move(part));
    }

    std::map<std::string, MeshBuilder> built;
    addFloors(lot, patterns, built, model.warnings);
    addWalls(lot, patterns, built, model.warnings);
    for (auto& [key, b] : built) {
        ObjectModel::Part part;
        part.material = key.substr(key.find(':') + 1);
        part.node = key;
        part.mesh = std::move(b.mesh);
        model.parts.push_back(std::move(part));
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        auto loc = index.findByName(part.material + "_txmt", kTypeTxmt);
        if (!loc) {
            model.warnings.push_back("TXMT not found: " + part.material);
            continue;
        }
        const auto bytes = index.read(*loc);
        Rcol rc(bytes);
        const auto* block = rc.find("cMaterialDefinition");
        if (!block) {
            model.warnings.push_back("TXMT without cMaterialDefinition: " + part.material);
            continue;
        }
        model.materials.emplace(part.material, MaterialDefinition::parse(block->body));
    }
    return model;
}

} // namespace os2
