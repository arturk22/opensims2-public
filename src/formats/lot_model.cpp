#include "lot_model.hpp"
#include "lot_edit.hpp"

#include "binreader.hpp"
#include "config_text.hpp"
#include "lot.hpp"
#include "lot_roof.hpp"
#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "image.hpp"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <cctype>
#include <cmath>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C, kTypeCatalogPattern = 0xCCA8E925, kTypeCatalogRoof = 0xACA8EA06,
                   kTypeCatalogFence = 0x2CB230B8;

// Wall slab half thickness, in tiles: the game's walls are 0.15 thick
// (cWallGeometryNode's default, Mac BV 0x74ab40).
constexpr float kWallHalfThickness = 0.075f;
// Ground floors sit on the terrain; lift them so they win the depth test
// against the terrain paints drawn after them.
constexpr float kGroundFloorLift = 0.005f;
// terrain.matShad: the canvas texture spans 20 tiles (canvasBaseTextureScale,
// paintTextureScale), its "_detail" texture 4 (canvasDetailTextureScale,
// paintDetailTextureScale); terrainMatDiffCoef 0.9.
constexpr float kTerrainTextureTiles = 20.0f, kTerrainDetailTiles = 4.0f;
constexpr float kStoryHeight = 3.0f;
// Attic walls stop this far below the roof surface (OpenTS2: 2.5 x the wall's
// half thickness), half walls under a fence this far above their floor.
constexpr float kAtticRoofOffset = 0.1875f;
constexpr float kHalfWallHeight = 1.0f;
// Height of walls cut down by the wall modes (OpenTS2's WallsDownHeight).
constexpr float kWallsDownHeight = 0.2f;

// Room of the quarter tile on the `normal` side of a wall through `mid`, the
// quarter (west, north, east, south) that faces the wall; diagonal walls look
// at both quarters on their side. 0 is outdoors.
// `light`: from the light's room map (3ARY 0x0B, room definition 0) when
// the lot has it, as the light maps use; else the VM's (0x09).
uint16_t roomBeside(const Lot& lot, int level, float midX, float midY, float nx, float ny, bool light = false) {
    const auto it = lot.roomLayers.find(0x0B);
    const auto& rooms =
        light && it != lot.roomLayers.end() && it->second.size() == lot.rooms.size() ? it->second : lot.rooms;
    if (rooms.empty() || !lot.hasLevel(level))
        return 0;
    const float px = midX + nx * 0.25f, py = midY + ny * 0.25f;
    if (px < 0 || py < 0 || px >= float(lot.width) || py >= float(lot.height))
        return 0;
    const uint32_t x = uint32_t(px), y = uint32_t(py);
    const auto& q = rooms[(size_t(level - lot.minLevel) * lot.width + x) * lot.height + y];
    // Directions of the quarters; the facing one points back at the wall (-normal).
    const std::array<std::array<float, 2>, 4> dirs{{{-1, 0}, {0, 1}, {1, 0}, {0, -1}}};
    uint16_t room = 0;
    for (size_t i = 0; i < 4; ++i)
        if (dirs[i][0] * -nx + dirs[i][1] * -ny > 0.5f && q[i])
            room = q[i];
    return room;
}

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

// TerrainCanvasShader / TerrainPaint (terrain.matShad), with TerrainLighting
// folded in: (detail over texture by the detail's alpha) x 2 x 0.9 x light
// map. Paints take their alpha from the mask alone and drop alpha <= 10.
MaterialDefinition terrainMaterial(const std::string& texture, bool paint, bool detail) {
    MaterialDefinition m;
    m.materialType = "Terrain";
    m.properties = {{"stdMatBaseTextureName", texture},
                    {"stdMatDiffCoef", "0.9,0.9,0.9,1"},
                    {"osTextureScale", std::to_string(1 / kTerrainTextureTiles)}};
    if (detail)
        m.properties.insert(m.properties.end(), {{"osDetailTextureName", texture + "_detail"},
                                                 {"osDetailTextureScale", std::to_string(1 / kTerrainDetailTiles)}});
    if (paint)
        m.properties.insert(m.properties.end(), {{"stdMatAlphaBlendMode", "blend"},
                                                 {"stdMatAlphaTestEnabled", "true"},
                                                 {"stdMatAlphaRefValue", "11"},
                                                 {"osMaskAlphaOnly", "true"}});
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
    // With light-map coordinates (atlas texels).
    uint32_t vertex(const Vec3f& p, const Vec3f& n, float u, float v, std::array<float, 2> lm) {
        mesh.uvs2.push_back(lm);
        return vertex(p, n, u, v);
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
MeshGroup terrainMesh(const Lot& lot, const LotLightMapLayout& lm, Keep&& keep) {
    MeshBuilder b;
    const int w = int(lot.width), h = int(lot.height);
    for (int x = 0; x <= w; ++x)
        for (int y = 0; y <= h; ++y) {
            const float dx = lot.elevation(0, x + 1, y) - lot.elevation(0, x - 1, y);
            const float dy = lot.elevation(0, x, y + 1) - lot.elevation(0, x, y - 1);
            const float len = std::sqrt(dx * dx + dy * dy + 4.0f);
            // Texture coordinates in tiles: the material scales them (the
            // light map's floor page 0 is at position / 64 in the game).
            b.vertex({float(x), float(y), lot.elevation(0, x, y)}, {-dx / len, -dy / len, 2.0f / len}, float(x),
                     float(y), lm.floorTexel(0, float(x), float(y)));
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
// Levels above `maxLevel` are not drawn (their light-map materials are still set).
void addFloors(const Lot& lot, const PatternCatalog& patterns, LotLightMapLayout& lm,
               std::map<std::string, MeshBuilder>& drawn, std::vector<std::string>& warnings, int maxLevel) {
    std::map<std::string, MeshBuilder> hidden;
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.floorPatterns) {
        const std::string m = patterns.floorMaterial(pattern);
        if (m.empty())
            warnings.push_back("floor pattern " + pattern + " has no material");
        materials[ref] = m;
    }
    for (int level = lot.minLevel; level < lot.minLevel + lot.levelCount; ++level) {
        const float lift = level == 0 ? kGroundFloorLift : 0.0f;
        auto& out = level > maxLevel ? hidden : drawn;
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
                    const size_t at = (size_t(level - lot.minLevel) * lot.width + x) * lot.height + y;
                    if (at < lm.floorMaterials.size())
                        lm.floorMaterials[at][q] = lm.materialIndex(it->second);
                    const Vec3f& a = edges[q][1];
                    const Vec3f& e = edges[q][0];
                    const Vec3f n = normalOf(a, e, c);
                    // Texture top = west edge, right = north: the road tiles' edge
                    // lines (road_sidelines_01 at the west curb, _03 at the east
                    // one) only run along a north-south road this way. The
                    // coordinates run on across tiles, so that materials spanning
                    // several tiles (floorMaterialScaleU/V) continue.
                    // The pattern's rotation (Lot::floorRotations: 1, 2, 4, 8 =
                    // 0, 90, 180, 270 degrees) turns the texture about the
                    // tile's centre.
                    int turns = 0;
                    if (at < lot.floorRotations.size())
                        for (uint8_t r = lot.floorRotations[at][q]; r > 1 && turns < 3; r >>= 1)
                            ++turns;
                    auto v = [&](const Vec3f& p) {
                        float lu = p[1] - fy - 0.5f, lv = p[0] - fx - 0.5f;
                        for (int k = 0; k < turns; ++k)
                            std::swap(lu, lv), lv = -lv;
                        return b.vertex(p, n, fy + 0.5f + lu, fx + 0.5f + lv, lm.floorTexel(level, p[0], p[1]));
                    };
                    b.triangle(v(a), v(e), v(c));
                }
            }
    }
}

// A door or window: the vertices of its visible parts in lot space.
struct WallHole {
    std::vector<Vec3f> points;
    Vec3f lo{}, hi{}; // bounds of the points (see bound())
    std::string cutout;            // its model name: its wall masks' names start with it
    Vec3f origin{}, east{1, 0, 0}; // the object's position and +x axis
    void bound() {
        lo = {1e9f, 1e9f, 1e9f}, hi = {-1e9f, -1e9f, -1e9f};
        for (const auto& p : points)
            for (size_t k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
};
// A cut out of a wall stretch: along it from its start, and heights.
struct Cut {
    float u0, u1, z0, z1;
    size_t hole = 0; // index into the holes
};

// Frames cover this much of the hole's edge (the hole is the object's extent less this).
constexpr float kHoleInset = 0.1f;

// Cuts of the holes whose objects sit across the stretch from (ax, ay) along
// (dx, dy), `length` long, between z `bottom` and `top`.
std::vector<Cut> wallCuts(const std::vector<WallHole>& holes, float ax, float ay, float dx, float dy, float length,
                          float bottom, float top) {
    std::vector<Cut> cuts;
    const float nx = dy, ny = -dx;
    const float bx = ax + dx * length, by = ay + dy * length;
    const float x0 = std::min(ax, bx) - 0.25f, x1 = std::max(ax, bx) + 0.25f, y0 = std::min(ay, by) - 0.25f,
                y1 = std::max(ay, by) + 0.25f;
    for (const auto& h : holes) {
        // Far from the stretch: nothing to cut (most holes for most walls).
        if (h.hi[0] < x0 || h.lo[0] > x1 || h.hi[1] < y0 || h.lo[1] > y1 || h.hi[2] < bottom - 0.1f ||
            h.lo[2] > top + 0.1f)
            continue;
        float u0 = 1e9f, u1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        size_t near = 0;
        bool front = false, back = false;
        for (const auto& p : h.points) {
            const float rx = p[0] - ax, ry = p[1] - ay;
            const float d = rx * nx + ry * ny, along = rx * dx + ry * dy;
            if (std::abs(d) > 0.25f || p[2] < bottom - 0.1f || p[2] > top + 0.1f)
                continue;
            ++near;
            front = front || d > 0.01f, back = back || d < -0.01f;
            u0 = std::min(u0, along), u1 = std::max(u1, along), z0 = std::min(z0, p[2]), z1 = std::max(z1, p[2]);
        }
        // The object must stand across this wall (frame on both sides) and overlap the stretch.
        if (near < 8 || !front || !back)
            continue;
        u0 = std::max(u0 + kHoleInset, 0.0f), u1 = std::min(u1 - kHoleInset, length);
        // Doors go down to the floor; windows keep a sill.
        z0 = z0 < bottom + 0.05f ? bottom : z0 + kHoleInset;
        z1 = std::min(z1 - kHoleInset, top);
        if (u1 - u0 > 0.05f && z1 - z0 > 0.05f)
            cuts.push_back({u0, u1, z0, z1, size_t(&h - holes.data())});
    }
    return cuts;
}

// The values of a wall mask (docs/buy-build-mac.md 7.6): the TXMT
// "<cutout>_<dx>_<dy>_<level>_<dir>_wallmask" names the mask texture, whose
// alpha is 0 in the hole; its other faces' TXMTs (the wall seen from the
// next tile) use the same texture. nullopt if the object has none there.
// A diagonal stretch (`diagonal`): the tile `dx` east (west) and `dy` north
// (south) of the object's in its own frame, faces "nw"/"se"
// (doorclub2diagonal_1e_1n_0_nw); the other sign of north is tried too.
std::optional<ObjectModel::AlphaMask> wallMaskValues(ResourceIndex& index, const std::string& cutout, int dx, int level,
                                                     LotModelCache* cache, bool diagonal = false, int dy = 0) {
    auto offset = [](int d, const char* plus, const char* minus) {
        return d == 0 ? std::string("0") : std::to_string(std::abs(d)) + (d > 0 ? plus : minus);
    };
    const std::string dxs = offset(dx, "e", "w");
    const std::string key = cutout + "_" + dxs + "_" + std::to_string(level) + (diagonal ? "_d" + std::to_string(dy) : "");
    if (cache)
        if (auto it = cache->wallMasks.find(key); it != cache->wallMasks.end())
            return it->second;
    std::optional<ObjectModel::AlphaMask> out;
    std::vector<std::string> mids; // "<dy>_<level>_<dir>"
    if (!diagonal) {
        for (const char* face : {"0_%d_n", "1s_%d_s", "1n_%d_n", "0_%d_s"}) {
            char mid[32];
            std::snprintf(mid, sizeof mid, face, level);
            mids.push_back(mid);
        }
    } else {
        for (const std::string& dys : {offset(dy, "n", "s"), offset(-dy, "n", "s")})
            for (const char* dir : {"nw", "se"})
                mids.push_back(dys + "_" + std::to_string(level) + "_" + dir);
    }
    for (const std::string& mid : mids) {
        auto loc = index.findByName(cutout + "_" + dxs + "_" + mid + "_wallmask_txmt", kTypeTxmt);
        if (!loc)
            continue;
        try {
            const auto bytes = index.read(*loc);
            Rcol rc(bytes);
            const auto* block = rc.find("cMaterialDefinition");
            if (!block)
                continue;
            const std::string tex = MaterialDefinition::parse(block->body).property("stdMatBaseTextureName");
            auto tloc = tex.empty() ? std::nullopt : index.findByName(tex + "_txtr", kTypeTxtr);
            if (!tloc)
                continue;
            const RgbaImage img = loadTexture(index.read(*tloc), &index);
            if (!img.width || !img.height)
                continue;
            ObjectModel::AlphaMask m;
            m.width = img.width, m.height = img.height;
            m.values.resize(size_t(img.width) * img.height);
            for (size_t i = 0; i < m.values.size(); ++i)
                m.values[i] = img.pixels[i * 4 + 3];
            out = std::move(m);
            break;
        } catch (const std::exception&) {
        }
    }
    if (cache)
        cache->wallMasks[key] = out;
    return out;
}

// Walls as slabs: one face per side with that side's covering, plus a top.
// Each slab is stretched by its half thickness at both ends to close corners;
// texture coordinates follow world position, so overlaps line up. Attic walls
// (gables) rise to the roof above them, half walls under a fence to 1 tile.
void addWalls(const Lot& lot, const PatternCatalog& patterns, const LotRoofs& roofs, const LotViewOptions& view,
              bool roofShown, const std::vector<WallHole>& holes, LotLightMapLayout& lm,
              std::map<std::string, MeshBuilder>& drawn, std::vector<std::string>& warnings,
              std::vector<std::array<float, 5>>* cut, ResourceIndex& index, LotModelCache* cache,
              std::vector<ObjectModel::AlphaMask>& masks, std::map<std::string, int>& maskedParts) {
    // Walls above the view's level are built into this and dropped: their
    // light-map cells stay, so the atlas is the same for every view.
    std::map<std::string, MeshBuilder> hidden;
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.wallPatterns)
        materials[ref] = patterns.wallMaterial(pattern);
    size_t unknownFences = 0;
    for (const auto& w : lot.walls) {
        // The column deck's skirt (walls.txt "wall 4": notRendered) is not drawn.
        if (w.type == Lot::DeckSkirt)
            continue;
        const bool fence = Lot::isFence(w.type);
        if (fence) {
            if (!patterns.fence(w.type))
                ++unknownFences;
            if (w.patterns == std::array<uint16_t, 2>{0xFFFF, 0xFFFF})
                continue; // rails only: see lotFencePieces
        }
        float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        dx /= len, dy /= len;
        const int level = w.a.level;
        const bool attic = w.type == Lot::Attic;
        // Attic walls (the gables) stand on the level over the roof's walls
        // and show with the roof, from the top floor.
        const bool shown = level <= view.level || (attic && roofShown && level == view.level + 1);
        auto& out = shown ? drawn : hidden;
        // The wall mode applies to the view's level (lower levels keep their walls up).
        const bool current = view.level == LotViewOptions::kAllLevels || level == view.level ||
                             (level >= view.lowestCut && level < view.level);
        const float nx = dy, ny = -dx; // right of a -> b
        // Wall modes cut down the walls whose partition style allows it
        // (walls.txt mayCutAway: normal and attic walls; foundations, pool
        // sides and deck skirts say false, and fences default to false as
        // fencearches.txt documents; inferred for fences.txt, which has no
        // key), nTSWorld::PartitionStyleSupportsCutaway.
        bool lowered = false;
        if (current && view.walls != LotViewOptions::Walls::Up && (w.type == Lot::Normal || attic)) {
            if (view.walls == LotViewOptions::Walls::Down) {
                lowered = true;
            } else {
                // cWallCutawayManager::KeepOnlyFrontFacingOutsideWalls
                // (docs/ui-live-mac.md 8c) with the room of each face from
                // the light's room map (3ARY 0x0B), the one nTSSG::cWallManager
                // follows (docs/buy-build-mac.md 7.3: fenced yards are outside
                // there): both faces inside (an inside wall) always cut; one
                // face outside (an outside wall) cut when that face looks at
                // the camera; both outside (a wall alone in the yard) never.
                // The game tests the face against the camera's position; the
                // camera is far, so its view direction stands in for it.
                const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
                const bool right = roomBeside(lot, level, mx, my, nx, ny, true) != 0;
                const bool left = roomBeside(lot, level, mx, my, -nx, -ny, true) != 0;
                if (right && left) {
                    lowered = true;
                } else if (right != left) {
                    // The outside face's normal against the view (into the scene): < 0 looks at the camera.
                    const float s = right ? -1.0f : 1.0f;
                    lowered = s * (nx * view.viewDirection[0] + ny * view.viewDirection[1]) < -1e-3f;
                }
                // usarSim: a wall across the camera-to-Sim line (the world's
                // ray query; here the line in the plan, inferred).
                if (!lowered && view.simLine) {
                    const auto& l = *view.simLine;
                    auto orient = [](float ax, float ay, float bx, float by, float cx, float cy) {
                        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                    };
                    const float d1 = orient(l[0], l[1], l[2], l[3], w.a.x, w.a.y);
                    const float d2 = orient(l[0], l[1], l[2], l[3], w.b.x, w.b.y);
                    const float d3 = orient(w.a.x, w.a.y, w.b.x, w.b.y, l[0], l[1]);
                    const float d4 = orient(w.a.x, w.a.y, w.b.x, w.b.y, l[2], l[3]);
                    lowered = ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
                }
            }
        }
        if (lowered && cut && level <= view.level)
            cut->push_back({w.a.x, w.a.y, w.b.x, w.b.y, float(level)});
        // Bottom and top of the wall at a point along it (grid points or the middle).
        auto bottom = [&](float x, float y) { return lot.elevationAt(level, x, y); };
        auto top = [&](float x, float y, bool cut = true) {
            const float z0 = bottom(x, y);
            float z1 = lot.hasLevel(level + 1) ? lot.elevationAt(level + 1, x, y) : z0 + kStoryHeight;
            if (attic)
                z1 = std::max(roofs.heightAt(x, y, level, z1, kAtticRoofOffset), z0);
            else if (fence)
                z1 = std::min(z1, z0 + kHalfWallHeight);
            if (lowered && cut)
                z1 = std::min(z1, z0 + kWallsDownHeight);
            return z1;
        };
        const float t = kWallHalfThickness;
        // Attic walls follow the roof through their middle too.
        std::vector<std::array<float, 2>> points{{w.a.x - dx * t, w.a.y - dy * t}};
        if (attic)
            points.push_back({(w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2});
        points.push_back({w.b.x + dx * t, w.b.y + dy * t});
        auto clampXY = [&](std::array<float, 2> p) {
            return std::array<float, 2>{std::clamp(p[0], 0.0f, float(lot.width)),
                                        std::clamp(p[1], 0.0f, float(lot.height))};
        };
        std::vector<float> zb, zt, full; // full: the uncut top (the light-map cells)
        for (size_t i = 0; i < points.size(); ++i) {
            // Heights at the wall's own ends, not at the stretched corners.
            const auto q = clampXY(i == 0 ? std::array<float, 2>{w.a.x, w.a.y}
                                   : i + 1 == points.size() ? std::array<float, 2>{w.b.x, w.b.y} : points[i]);
            zb.push_back(bottom(q[0], q[1]));
            zt.push_back(top(q[0], q[1]));
            full.push_back(top(q[0], q[1], false));
        }
        // A partition with no covering on either side has nothing to draw
        // (its top alone floated as dark strips over the lawn; on N002_Lot10
        // those were walls read from the wrong wall graph, fixed in Lot::load).
        bool covered = false;
        for (uint16_t ref : w.patterns)
            if (auto it = materials.find(ref); it != materials.end() && !it->second.empty())
                covered = true;
        if (!covered)
            continue;

        for (size_t seg = 0; seg + 1 < points.size(); ++seg) {
            const float ax = points[seg][0], ay = points[seg][1], bx = points[seg + 1][0], by = points[seg + 1][1];
            const float za0 = zb[seg], zb0 = zb[seg + 1], za1 = zt[seg], zb1 = zt[seg + 1];
            // Doors and windows on this stretch: intervals along it (from
            // (ax, ay)) and heights cut out of both faces.
            const float segLen = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            std::vector<Cut> cuts;
            if (w.type == Lot::Normal && !fence)
                cuts = wallCuts(holes, ax, ay, dx, dy, segLen, std::min(za0, zb0), std::max(za1, zb1));
            // A door or window with a wall mask for this stretch cuts its
            // own shape: the faces get the mask (alpha tested in the wall
            // shader) instead of the box cut. The mask spans the tile the
            // stretch covers, u along the object's +x from the tile's start
            // and v from a story above the wall's floor down to it (u
            // direction and the story inferred from the textures: a two-tile
            // arch's halves are "0" and "1e").
            int maskIndex = -1;
            const float base = std::min(za0, zb0);
            for (auto c = cuts.begin(); c != cuts.end() && maskIndex < 0; ++c) {
                const WallHole& h = holes[c->hole];
                if (h.cutout.empty())
                    continue;
                // Straight: the wall runs along the object's +x. Diagonal:
                // the "...diagonal" models stand square and their wall runs
                // along +x + local north (rotated +90°) (the lots' diagonal
                // windows all do), its faces "nw" and "se"; the stretch is
                // sqrt 2 long and the next tile along is one east and one
                // north of the object's (doorclub2diagonal_1e_1n_0_nw).
                const float along = dx * h.east[0] + dy * h.east[1];
                const bool diag = std::abs(std::abs(along) - 0.7071f) < 0.05f && (h.east[0] == 0 || h.east[1] == 0);
                if (!diag && std::abs(along) < 0.99f)
                    continue;
                std::array<float, 2> axis{h.east[0], h.east[1]};
                if (diag) {
                    const float r = 1.0f / std::sqrt(2.0f);
                    axis = {(h.east[0] - h.east[1]) * r, (h.east[1] + h.east[0]) * r};
                }
                const float span = diag ? std::sqrt(2.0f) : 1.0f;
                const float mx = (w.a.x + w.b.x) / 2 - h.origin[0], my = (w.a.y + w.b.y) / 2 - h.origin[1];
                const int k = int(std::lround((mx * axis[0] + my * axis[1]) / span));
                const int storey = std::max(0, int(std::lround((base - h.origin[2]) / kStoryHeight)));
                auto values = diag ? wallMaskValues(index, h.cutout, k, storey, cache, true, k)
                                   : wallMaskValues(index, h.cutout, k, storey, cache);
                if (!values)
                    continue;
                values->planar = true;
                values->planeU = {axis[0] / span, axis[1] / span, 0,
                                  0.5f - float(k) - (h.origin[0] * axis[0] + h.origin[1] * axis[1]) / span};
                values->planeV = {0, 0, -1.0f / kStoryHeight, (base + kStoryHeight) / kStoryHeight};
                maskIndex = int(masks.size());
                masks.push_back(std::move(*values));
                cuts.erase(c);
                break;
            }
            auto lerp = [&](float a, float b, float f) { return a + (b - a) * f; };
            for (int side = 0; side < 2; ++side) {
                auto it = materials.find(w.patterns[size_t(side)]);
                if (it == materials.end() || it->second.empty())
                    continue;
                std::string key = "wall:" + it->second;
                if (maskIndex >= 0) {
                    key += "#mask" + std::to_string(maskIndex);
                    maskedParts[key] = maskIndex;
                }
                const float s = side == 0 ? 1.0f : -1.0f;
                // A pool wall's face toward the earth (no pool floor on that
                // side) is under the ground: drawn, it showed past the lot's
                // edge as a cyan box under the pool.
                if (w.type == Lot::Pool) {
                    const int tx = int(std::floor((ax + bx) / 2 + nx * s * 0.5f));
                    const int ty = int(std::floor((ay + by) / 2 + ny * s * 0.5f));
                    if (tx < 0 || ty < 0 || tx >= int(lot.width) || ty >= int(lot.height) ||
                        lot.floor(level, uint32_t(tx), uint32_t(ty)) == std::array<uint16_t, 4>{})
                        continue;
                }
                MeshBuilder& b = out[key];
                const Vec3f n{nx * s, ny * s, 0};
                const float ox = nx * t * s, oy = ny * t * s;
                // The face's light-map cell, lit as the room it faces.
                LotLightMapLayout::WallFace face;
                face.origin = {ax + ox, ay + oy};
                face.along = {dx, dy};
                face.normal = {n[0], n[1]};
                face.length = segLen;
                face.base = std::min(za0, zb0);
                face.top = std::max(full[seg], full[seg + 1]);
                face.level = level;
                face.room = roomBeside(lot, level, (ax + bx) / 2, (ay + by) / 2, n[0], n[1], true);
                face.material = lm.materialIndex(it->second);
                const size_t cell = lm.addWall(face);
                auto lmAt = [&](const Vec3f& p) {
                    return lm.wallTexel(cell, (p[0] - face.origin[0]) * dx + (p[1] - face.origin[1]) * dy, p[2]);
                };
                // u runs along the wall in world units (right to left as seen from the face).
                auto u = [&](const Vec3f& p) { return s * (p[0] * dx + p[1] * dy); };
                // v: 0 at the top of a full wall; attic and half walls keep the
                // texture's bottom at their base, as OpenTS2 does.
                auto vv = [&](float z, float base, float top) {
                    return attic || fence ? 1.0f - (z - base) / kStoryHeight : (top - z) / kStoryHeight;
                };
                // Pieces between the cut intervals, each split around the holes over it.
                std::vector<float> breaks{0, segLen};
                for (const auto& c : cuts)
                    breaks.insert(breaks.end(), {c.u0, c.u1});
                std::sort(breaks.begin(), breaks.end());
                for (size_t k = 0; k + 1 < breaks.size(); ++k) {
                    const float s0 = breaks[k], s1 = breaks[k + 1];
                    if (s1 - s0 < 1e-4f)
                        continue;
                    const float f0 = s0 / segLen, f1 = s1 / segLen;
                    const float bot0 = lerp(za0, zb0, f0), bot1 = lerp(za0, zb0, f1), top0 = lerp(za1, zb1, f0),
                                top1 = lerp(za1, zb1, f1);
                    std::vector<std::pair<float, float>> gaps; // z ranges cut out of this piece
                    for (const auto& c : cuts)
                        if (c.u0 <= s0 + 1e-4f && c.u1 >= s1 - 1e-4f)
                            gaps.push_back({c.z0, c.z1});
                    std::sort(gaps.begin(), gaps.end());
                    // Solid bands: from the bottom (or a gap's top) to the next gap's bottom (or the top).
                    std::vector<std::array<float, 4>> bands; // z at s0 low/high, z at s1 low/high
                    std::array<float, 2> low{bot0, bot1};
                    for (const auto& [g0, g1] : gaps) {
                        bands.push_back({low[0], std::min(g0, top0), low[1], std::min(g0, top1)});
                        low = {std::max(g1, bot0), std::max(g1, bot1)};
                    }
                    bands.push_back({low[0], top0, low[1], top1});
                    const float x0 = ax + dx * s0 + ox, y0 = ay + dy * s0 + oy, x1 = ax + dx * s1 + ox,
                                y1 = ay + dy * s1 + oy;
                    for (const auto& band : bands) {
                        if (band[1] - band[0] < 1e-4f && band[3] - band[2] < 1e-4f)
                            continue;
                        const Vec3f pa0{x0, y0, band[0]}, pb0{x1, y1, band[2]}, pb1{x1, y1, band[3]},
                            pa1{x0, y0, band[1]};
                        const uint32_t i0 = b.vertex(pa0, n, u(pa0), vv(band[0], bot0, top0), lmAt(pa0)),
                                       i1 = b.vertex(pb0, n, u(pb0), vv(band[2], bot1, top1), lmAt(pb0)),
                                       i2 = b.vertex(pb1, n, u(pb1), vv(band[3], bot1, top1), lmAt(pb1)),
                                       i3 = b.vertex(pa1, n, u(pa1), vv(band[1], bot0, top0), lmAt(pa1));
                        if (side == 0)
                            b.quad(i0, i1, i2, i3);
                        else
                            b.quad(i1, i0, i3, i2);
                    }
                }
            }
            // The holes' sides through the wall: jambs (where the hole ends
            // inside this stretch), sill and head, in the wall top's material.
            for (const auto& c : cuts) {
                MeshBuilder& rev = out["walltop:wall_top"];
                auto at = [&](float along, float side, float z) {
                    return Vec3f{ax + dx * along + nx * t * side, ay + dy * along + ny * t * side, z};
                };
                auto face = [&](Vec3f p0, Vec3f p1, Vec3f p2, Vec3f p3) {
                    const Vec3f nn = normalOf(p0, p1, p2);
                    auto vtx = [&](const Vec3f& p) {
                        return rev.vertex(p, nn, p[0] + p[2], p[1] + p[2], lm.floorTexel(level, p[0], p[1]));
                    };
                    rev.quad(vtx(p0), vtx(p1), vtx(p2), vtx(p3));
                };
                if (c.u0 > t + 1e-3f) // jamb at u0, facing +along
                    face(at(c.u0, -1, c.z0), at(c.u0, 1, c.z0), at(c.u0, 1, c.z1), at(c.u0, -1, c.z1));
                if (c.u1 < segLen - t - 1e-3f) // jamb at u1, facing -along
                    face(at(c.u1, 1, c.z0), at(c.u1, -1, c.z0), at(c.u1, -1, c.z1), at(c.u1, 1, c.z1));
                face(at(c.u0, 1, c.z0), at(c.u0, -1, c.z0), at(c.u1, -1, c.z0), at(c.u1, 1, c.z0)); // sill, up
                face(at(c.u0, -1, c.z1), at(c.u0, 1, c.z1), at(c.u1, 1, c.z1), at(c.u1, -1, c.z1)); // head, down
            }
            MeshBuilder& cap = out["walltop:wall_top"];
            const Vec3f r0{ax + nx * t, ay + ny * t, za1}, r1{bx + nx * t, by + ny * t, zb1},
                l1{bx - nx * t, by - ny * t, zb1}, l0{ax - nx * t, ay - ny * t, za1};
            const Vec3f up = normalOf(r0, r1, l1);
            // Wall tops take the floor light map of their level (inferred).
            auto vtx = [&](const Vec3f& p) { return cap.vertex(p, up, p[0], p[1], lm.floorTexel(level, p[0], p[1])); };
            cap.quad(vtx(r0), vtx(r1), vtx(l1), vtx(l0));
        }
    }
    if (unknownFences)
        warnings.push_back(std::to_string(unknownFences) + " fence segments of unknown fences");
}

// Doors and windows cut holes in the walls they stand in. The game draws the
// holes with per-tile WallMask textures (TXMTs "<object>_<x>_<y>_<level>_<dir>_wallmask",
// only for some objects); here the hole is the extent of the object's visible
// parts (not its wall shadows) less the frame, for objects whose definition
// is named "Window ..." or "Door ...".
std::vector<WallHole> findWallHoles(const Lot& lot, ResourceIndex& index, std::vector<std::string>& warnings) {
    std::vector<WallHole> holes;
    std::map<std::string, std::optional<std::vector<Vec3f>>> models; // model -> visible vertices
    for (const auto& o : lot.objects) {
        const std::string def = lowerAscii(o.definition);
        if (o.model.empty() || o.person || (o.position[0] == 0 && o.position[1] == 0) ||
            !(def.starts_with("window") || def.starts_with("door")))
            continue;
        auto it = models.find(o.model);
        if (it == models.end()) {
            std::optional<std::vector<Vec3f>> points;
            try {
                const ObjectModel m = loadObjectModel(index, o.model);
                points.emplace();
                for (const auto& part : m.parts) {
                    auto mat = m.materials.find(part.material);
                    if (mat == m.materials.end() || lowerAscii(part.material).find("shadow") != std::string::npos ||
                        !standardMaterial(mat->second).visible)
                        continue;
                    points->insert(points->end(), part.mesh.positions.begin(), part.mesh.positions.end());
                }
            } catch (const std::exception& e) {
                warnings.push_back("wall hole of " + o.model + ": " + e.what());
            }
            it = models.emplace(o.model, std::move(points)).first;
        }
        if (!it->second)
            continue;
        // Rotate by the unit quaternion (x, y, z, w), then move.
        const auto& q = o.rotation;
        WallHole h;
        // The wall masks' base: the object's model name, STR# 0x85 entry
        // (tag 0xCC + 1), the CRES name the lot keeps (docs/buy-build-mac.md 7.6).
        h.cutout = lowerAscii(o.model);
        h.origin = {o.position[0], o.position[1], o.position[2]};
        {
            // +x rotated: 1 - 2(y^2 + z^2), 2(xy + wz), 2(xz - wy); snapped to an axis or diagonal.
            float ex = 1 - 2 * (q[1] * q[1] + q[2] * q[2]), ey = 2 * (q[0] * q[1] + q[3] * q[2]);
            const float len = std::hypot(ex, ey);
            if (len > 1e-4f)
                ex /= len, ey /= len;
            for (float* c : {&ex, &ey})
                *c = std::abs(*c) < 0.2f ? 0.0f : std::abs(*c) > 0.9f ? std::copysign(1.0f, *c) : *c;
            h.east = {ex, ey, 0};
        }
        h.points.reserve(it->second->size());
        for (const auto& v : *it->second) {
            const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
                        tz = 2 * (q[0] * v[1] - q[1] * v[0]);
            h.points.push_back({v[0] + q[3] * tx + (q[1] * tz - q[2] * ty) + o.position[0],
                                v[1] + q[3] * ty + (q[2] * tx - q[0] * tz) + o.position[1],
                                v[2] + q[3] * tz + (q[0] * ty - q[1] * tx) + o.position[2]});
        }
        holes.push_back(std::move(h));
    }
    return holes;
}

// Materials for the parts the game draws with its own shaders: roofs (no
// TXMT) as roof.matShad's RoofTop/RoofEdges/RoofTrim/RoofUnder definitions,
// StandardMaterials lit by the lights like any object (no light map), cull
// none, no specular; the top is darker (0.4, 0.385, 0.38) and bumped.
MaterialDefinition roofMaterial(const std::string& texture, const std::string& bump, bool top) {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatBaseTextureName", texture},
                    {"stdMatDiffCoef", top ? "0.4,0.385,0.38,1" : "0.8,0.8,0.8,1"},
                    {"stdMatSpecPower", "0"},
                    {"stdMatCullMode", "none"}};
    if (!bump.empty())
        m.properties.insert(m.properties.end(), {{"stdMatNormalMapTextureEnabled", "true"},
                                                 {"stdMatNormalMapTextureName", bump}});
    return m;
}

// Pools: SimplifiedWaterSurface of PoolWaterSurfaceMaterial (water.matShad),
// colour (0.2, 0.3, 1.0) at 0.4 alpha, unlit, on the pool water layer (5).
MaterialDefinition poolWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "PoolWaterSurfaceMaterial";
    m.properties = {{"stdMatDiffCoef", "0.2,0.3,1.0,0.4"},
                    {"stdMatUntexturedDiffAlpha", "0.4"},
                    {"stdMatLightingEnabled", "0"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water: TerrainWaterShader (waterOnTerrain) reflects
// "reflectionoutdoorwater-envcube", its alpha from the vertex colour (not in
// the data: colour and 0.8 here are guesses), with a moving bump map left out.
MaterialDefinition terrainWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "TerrainWater";
    m.properties = {{"stdMatDiffCoef", "0.1,0.2,0.55,0.8"},
                    {"stdMatUntexturedDiffAlpha", "0.8"},
                    {"stdMatEnvCubeMode", "reflection"},
                    {"stdMatEnvCubeCoef", "0.35,0.35,0.35"},
                    {"stdMatEnvCubeTextureName", "reflectionoutdoorwater-envcube"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water over every tile whose water level is above the terrain at a
// corner (as OpenTS2 does), and pool water over each pool's floor tiles.
void addWater(const Lot& lot, std::map<std::string, MeshBuilder>& out) {
    MeshBuilder* terrain = nullptr;
    for (uint32_t x = 0; x < lot.width; ++x)
        for (uint32_t y = 0; y < lot.height; ++y) {
            const int ix = int(x), iy = int(y);
            const std::array<std::array<int, 2>, 4> c{{{ix, iy}, {ix + 1, iy}, {ix + 1, iy + 1}, {ix, iy + 1}}};
            bool wet = false;
            std::array<Vec3f, 4> p;
            for (size_t i = 0; i < 4; ++i) {
                const float wl = lot.waterLevel(c[i][0], c[i][1]);
                wet = wet || wl > lot.elevation(0, c[i][0], c[i][1]);
                p[i] = {float(c[i][0]), float(c[i][1]), wl};
            }
            if (!wet)
                continue;
            if (!terrain)
                terrain = &out["water:terrain"];
            const Vec3f up{0, 0, 1};
            auto v = [&](const Vec3f& q) { return terrain->vertex(q, up, q[0] / 4, q[1] / 4); };
            terrain->quad(v(p[0]), v(p[1]), v(p[2]), v(p[3]));
        }
    for (const auto& pool : lot.pools) {
        MeshBuilder& b = out["water:pool"];
        const int x0 = int(std::lround(pool.x)), y0 = int(std::lround(pool.y));
        for (int x = x0; x < x0 + int(pool.width); ++x)
            for (int y = y0; y < y0 + int(pool.height); ++y) {
                if (x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height))
                    continue;
                // Pool tiles have a floor one level down (the pool's bottom) and none at the surface.
                const auto& bottomTile = lot.floor(pool.level - 1, uint32_t(x), uint32_t(y));
                if (bottomTile == std::array<uint16_t, 4>{} ||
                    lot.floor(pool.level, uint32_t(x), uint32_t(y)) != std::array<uint16_t, 4>{})
                    continue;
                const Vec3f up{0, 0, 1};
                auto v = [&](int px, int py) {
                    const Vec3f q{float(px), float(py), lot.elevation(pool.level, px, py) - pool.depth};
                    return b.vertex(q, up, q[0], q[1]);
                };
                b.quad(v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y + 1));
            }
    }
}

} // namespace

PatternCatalog::PatternCatalog(ResourceIndex& index, const std::filesystem::path& resRoot) {
    const auto scripts = resRoot / "Catalog/Scripts";
    floors_ = loadPatternScript(scripts / "floorPatterns.txt", "floorPattern");
    walls_ = loadPatternScript(scripts / "wallPatterns.txt", "wallPattern");
    {
        std::ifstream in(scripts / "fencearches.txt");
        for (std::string line; std::getline(in, line);) {
            std::istringstream ss(line.substr(0, line.find('#')));
            std::string key, value;
            ss >> key;
            if (key == "fenceArch") {
                arches_.emplace_back();
                continue;
            }
            if (arches_.empty())
                continue;
            std::getline(ss >> std::ws, value);
            std::erase(value, '"');
            while (!value.empty() && std::isspace(uint8_t(value.back())))
                value.pop_back();
            if (key == "archCenterModel")
                arches_.back().center = value;
            else if (key == "archEastModel")
                arches_.back().east = value;
            else if (key == "archWestModel")
                arches_.back().west = value;
            else if (key == "archPostModel")
                arches_.back().post = value;
        }
    }
    auto text = [&](const ResourceIndex::Location& loc) {
        const auto bytes = index.read(loc);
        return parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    auto guidOf = [](const std::map<std::string, std::string>& props) -> std::optional<uint32_t> {
        auto it = props.find("guid");
        if (it == props.end())
            return std::nullopt;
        try {
            return uint32_t(std::stoul(it->second));
        } catch (const std::logic_error&) {
            return std::nullopt;
        }
    };
    auto get = [](const std::map<std::string, std::string>& props, const char* key) {
        auto it = props.find(key);
        return it == props.end() ? std::string() : it->second;
    };
    for (const auto& loc : index.ofType(kTypeCatalogRoof)) {
        const auto props = text(loc);
        if (auto guid = guidOf(props))
            roofs_[*guid] = {get(props, "texturetop"), get(props, "texturetopbump"), get(props, "textureedges"),
                             get(props, "texturetrim"), get(props, "textureunder")};
    }
    for (const auto& loc : index.ofType(kTypeCatalogFence)) {
        const auto props = text(loc);
        if (auto guid = guidOf(props))
            fences_[*guid] = {get(props, "rail"), get(props, "diagrail"), get(props, "post")};
    }
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

const PatternCatalog::Roof* PatternCatalog::roof(uint32_t guid) const {
    auto it = roofs_.find(guid);
    return it == roofs_.end() ? nullptr : &it->second;
}

const PatternCatalog::Fence* PatternCatalog::fence(uint32_t guid) const {
    auto it = fences_.find(guid);
    return it == fences_.end() ? nullptr : &it->second;
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

int lotTopLevel(const Lot& lot) {
    int top = lot.minLevel;
    // Not the attic walls: the gables belong to the roof over the level
    // below (a roof drawn in Build mode on a one-storey house puts its
    // gables on level 1, which hid that roof until the lot was reopened
    // with every level shown).
    for (const auto& w : lot.walls)
        if (w.type != Lot::Attic)
            top = std::max(top, w.a.level);
    for (int level = top + 1; level < lot.minLevel + lot.levelCount; ++level)
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y)
                if (lot.floor(level, x, y) != std::array<uint16_t, 4>{})
                    top = level, x = lot.width, y = lot.height;
    return top;
}

// The sun's height map for the light maps (cHeightMapShadow's
// FillFromWalls / FillFromRooves / FillFromTerrain, docs/lighting-mac.md
// 5.3): at each ground texel the top of the walls standing there (full
// height, whatever the view), the roofs and the upper levels' floors.
// Inferred: which parts count (awnings and objects, AddBlobToHeightMap,
// are left out) and the walls' footprint (their thickness).
void fillSunOccluders(const Lot& lot, const LotRoofs& roofs, LotLightMapLayout& lm) {
    constexpr int k = LotLightMapLayout::kTexelsPerTile;
    const long w = long(lot.width) * k, h = long(lot.height) * k;
    lm.occluders.assign(size_t(w * h), -1e9f);
    auto raise = [&](long i, long j, float z) {
        if (i >= 0 && j >= 0 && i < w && j < h)
            lm.occluders[size_t(j * w + i)] = std::max(lm.occluders[size_t(j * w + i)], z);
    };
    for (const auto& wl : lot.walls) {
        if (wl.type == Lot::DeckSkirt)
            continue;
        const bool fence = Lot::isFence(wl.type);
        if (fence && wl.patterns == std::array<uint16_t, 2>{0xFFFF, 0xFFFF})
            continue; // rails
        const int level = wl.a.level;
        const float dx = wl.b.x - wl.a.x, dy = wl.b.y - wl.a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        const int n = int(std::ceil(len * k * 2));
        for (int q = 0; q <= n; ++q) {
            const float t = float(q) / float(n);
            const float x = wl.a.x + dx * t, y = wl.a.y + dy * t;
            const float z0 = lot.elevationAt(level, x, y);
            float z1 = lot.hasLevel(level + 1) ? lot.elevationAt(level + 1, x, y) : z0 + kStoryHeight;
            if (wl.type == Lot::Attic)
                z1 = std::max(roofs.heightAt(x, y, level, z1, kAtticRoofOffset), z0);
            else if (fence)
                z1 = std::min(z1, z0 + kHalfWallHeight);
            // The texels the slab (kWallHalfThickness each side) covers.
            const long i0 = long(std::floor((x - kWallHalfThickness) * k)), i1 = long(std::floor((x + kWallHalfThickness) * k));
            const long j0 = long(std::floor((y - kWallHalfThickness) * k)), j1 = long(std::floor((y + kWallHalfThickness) * k));
            for (long j = j0; j <= j1; ++j)
                for (long i = i0; i <= i1; ++i)
                    raise(i, j, z1);
        }
    }
    const int top = lot.minLevel + int(lot.levelCount);
    for (long j = 0; j < h; ++j)
        for (long i = 0; i < w; ++i) {
            const float x = (float(i) + 0.5f) / k, y = (float(j) + 0.5f) / k;
            for (int level = lot.minLevel + 1; level < top; ++level)
                if (lm.hasFloorAt(level, x, y))
                    raise(i, j, lm.elevationAt(level, x, y));
            if (roofs.count())
                for (int level = lot.minLevel; level < top; ++level)
                    raise(i, j, roofs.heightAt(x, y, level, -1e9f));
        }
}

ObjectModel buildLotModel(const Lot& lot, ResourceIndex& index, const PatternCatalog& patterns,
                          const LotViewOptions& view, LotLightMapLayout* lightMap, LotModelCache* cache) {
    ObjectModel model;
    model.name = "lot";
    model.warnings = lot.warnings;
    LotLightMapLayout lm;
    lm.init(lot);
    auto hasDetail = [&](const std::string& texture) {
        return index.findByName(texture + "_detail_txtr", kTypeTxtr).has_value();
    };

    // Terrain: the base everywhere, then each paint over the tiles its mask
    // touches; none over pools (floors below the ground level), which would
    // hide them, as OpenTS2 leaves out terrain under floors.
    auto open = [&](int x, int y) {
        for (int level = lot.minLevel; level < 0; ++level)
            if (lot.floor(level, uint32_t(x), uint32_t(y)) != std::array<uint16_t, 4>{})
                return false;
        return true;
    };
    if (!lot.terrainBase.empty()) {
        model.materials["terrain:" + lot.terrainBase] = terrainMaterial(lot.terrainBase, false, hasDetail(lot.terrainBase));
        ObjectModel::Part base;
        base.material = "terrain:" + lot.terrainBase;
        base.mesh = terrainMesh(lot, lm, open);
        model.parts.push_back(std::move(base));
        // With a level under the ground (a pool's), the lot's edges get
        // sides of earth down past it: the game's lot sits in the
        // neighborhood's terrain (cLotSkirt) and the pool's bottom and walls
        // never show past the lot's edge; here they showed as a cyan box
        // under the pool (a visual choice, inferred).
        if (lot.minLevel < 0 && lot.hasLevel(lot.minLevel)) {
            const int w = int(lot.width), h = int(lot.height);
            float bottom = 0;
            for (int x = 0; x <= w; ++x)
                for (int y = 0; y <= h; ++y)
                    bottom = std::min(bottom, lot.elevation(lot.minLevel, x, y));
            bottom -= 0.5f;
            MeshBuilder sk;
            auto side = [&](float ax, float ay, float bx, float by, Vec3f n) {
                const float ta = lot.elevation(0, int(ax), int(ay)), tb = lot.elevation(0, int(bx), int(by));
                const float u0 = ax + ay, u1 = bx + by;
                auto v = [&](float x, float y, float z, float u) {
                    return sk.vertex({x, y, z}, n, u, z, lm.floorTexel(0, std::min(x, float(w) - 0.01f), std::min(y, float(h) - 0.01f)));
                };
                sk.quad(v(ax, ay, bottom, u0), v(bx, by, bottom, u1), v(bx, by, tb, u1), v(ax, ay, ta, u0));
            };
            for (int x = 0; x < w; ++x) {
                side(float(x), 0, float(x + 1), 0, {0, -1, 0});
                side(float(x + 1), float(h), float(x), float(h), {0, 1, 0});
            }
            for (int y = 0; y < h; ++y) {
                side(0, float(y + 1), 0, float(y), {-1, 0, 0});
                side(float(w), float(y), float(w), float(y + 1), {1, 0, 0});
            }
            ObjectModel::Part skirt;
            skirt.material = "terrain:" + lot.terrainBase;
            skirt.mesh = std::move(sk.mesh);
            model.parts.push_back(std::move(skirt));
        }
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
        part.mesh = terrainMesh(lot, lm, [&](int x, int y) { return open(x, y) && touched(x, y); });
        part.mask = int(model.masks.size());
        model.masks.push_back(std::move(mask));
        model.materials[part.material] = terrainMaterial(paint.texture, true, hasDetail(paint.texture));
        model.parts.push_back(std::move(part));
    }

    const LotRoofs roofs(lot);
    model.warnings.insert(model.warnings.end(), roofs.warnings.begin(), roofs.warnings.end());
    std::map<std::string, MeshBuilder> built;
    addFloors(lot, patterns, lm, built, model.warnings, view.level);
    std::vector<WallHole> holes;
    if (cache && cache->holes) {
        for (const auto& c : *cache->holes) {
            WallHole h;
            h.points = c.points, h.cutout = c.cutout, h.origin = c.origin, h.east = c.east;
            holes.push_back(std::move(h));
        }
    } else {
        holes = findWallHoles(lot, index, model.warnings);
        if (cache) {
            cache->holes.emplace();
            for (const auto& h : holes)
                cache->holes->push_back({h.points, h.cutout, h.origin, h.east});
        }
    }
    for (auto& h : holes)
        h.bound();
    if (cache)
        cache->cut.clear();
    const bool roofShown =
        roofs.count() && view.walls == LotViewOptions::Walls::Up && view.roof && view.level >= lotTopLevel(lot);
    std::map<std::string, int> maskedParts; // "wall:<material>#mask<n>" -> index into model.masks
    addWalls(lot, patterns, roofs, view, roofShown, holes, lm, built, model.warnings, cache ? &cache->cut : nullptr,
             index, cache, model.masks, maskedParts);
    addWater(lot, built);
    model.materials["water:terrain"] = terrainWaterMaterial();
    model.materials["water:pool"] = poolWaterMaterial();
    for (auto& [key, b] : built)
        if (key.rfind("water:", 0) == 0) {
            ObjectModel::Part part;
            part.material = part.node = key;
            part.mesh = std::move(b.mesh);
            model.parts.push_back(std::move(part));
        }
    std::erase_if(built, [](const auto& kv) { return kv.first.rfind("water:", 0) == 0; });

    if (roofShown) {
        const PatternCatalog::Roof* pattern = patterns.roof(roofs.pattern());
        if (!pattern) {
            model.warnings.push_back("roof pattern " + hexString(roofs.pattern()) + " not in the catalog");
        } else {
            LotRoofs::Meshes meshes = roofs.build();
            const std::pair<MeshGroup*, std::pair<std::string, std::string>> parts[] = {
                {&meshes.top, {pattern->top, pattern->topBump}},
                {&meshes.edges, {pattern->edges, ""}},
                {&meshes.trim, {pattern->trim, ""}},
                {&meshes.under, {pattern->under, ""}}};
            for (const auto& [mesh, tex] : parts) {
                if (mesh->triangles.empty() || tex.first.empty())
                    continue;
                ObjectModel::Part part;
                part.material = part.node = "roof:" + tex.first;
                model.materials[part.material] = roofMaterial(tex.first, tex.second, mesh == &meshes.top);
                part.mesh = std::move(*mesh);
                model.parts.push_back(std::move(part));
            }
        }
    }
    for (auto& [key, b] : built) {
        ObjectModel::Part part;
        part.material = key.substr(key.find(':') + 1);
        if (auto m = maskedParts.find(key); m != maskedParts.end()) {
            part.material.erase(part.material.rfind("#mask"));
            part.mask = m->second;
        }
        part.node = key;
        part.mesh = std::move(b.mesh);
        model.parts.push_back(std::move(part));
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        if (cache)
            if (auto it = cache->materials.find(part.material); it != cache->materials.end()) {
                model.materials.emplace(part.material, it->second);
                continue;
            }
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
        if (cache)
            cache->materials.emplace(part.material, model.materials.at(part.material));
    }

    // The mean colour of each floor and wall material's texture, for the
    // room irradiance (GatherRoomLighting mixes it in by roomSaturation).
    for (size_t i = 1; i < lm.materialNames.size(); ++i) {
        auto m = model.materials.find(lm.materialNames[i]);
        if (m == model.materials.end())
            continue;
        if (cache)
            if (auto c = cache->meanColors.find(lm.materialNames[i]); c != cache->meanColors.end()) {
                if (c->second)
                    lm.materialColors[i] = *c->second;
                continue;
            }
        if (cache)
            cache->meanColors[lm.materialNames[i]] = std::nullopt; // until found below
        const std::string tex = m->second.property("stdMatBaseTextureName");
        if (tex.empty())
            continue;
        try {
            auto loc = index.findByName(tex + "_txtr", kTypeTxtr);
            if (!loc)
                continue;
            const auto bytes = index.read(*loc);
            const TextureLevels t = loadTextureLevels(bytes, &index);
            if (t.levels.empty())
                continue;
            // The smallest level at least 8 texels wide: its mean is the texture's.
            size_t pick = 0;
            for (size_t l = 0; l < t.levels.size(); ++l)
                if (t.levels[l].width >= 8 && t.levels[l].height >= 8 &&
                    t.levels[l].width * t.levels[l].height < t.levels[pick].width * t.levels[pick].height)
                    pick = l;
            const auto& lv = t.levels[pick];
            const RgbaImage img = decodeTexture(t.format, lv.width, lv.height, lv.data);
            double sum[3] = {0, 0, 0};
            const size_t n = size_t(img.width) * img.height;
            for (size_t p = 0; p < n; ++p)
                for (size_t c = 0; c < 3; ++c)
                    sum[c] += img.pixels[p * 4 + c];
            if (n)
            {
                lm.materialColors[i] = {float(sum[0] / (255.0 * double(n))), float(sum[1] / (255.0 * double(n))),
                                        float(sum[2] / (255.0 * double(n)))};
                if (cache)
                    cache->meanColors[lm.materialNames[i]] = lm.materialColors[i];
            }
        } catch (const std::exception& e) {
            model.warnings.push_back("mean colour of " + tex + ": " + e.what());
        }
    }

    // Floor materials map their texture across floorMaterialScaleU x V tiles
    // (floor.matShad: texture matrix scale 1 / floorMaterialScale).
    for (auto& part : model.parts) {
        auto m = model.materials.find(part.material);
        if (part.node.rfind("floor:", 0) != 0 || m == model.materials.end())
            continue;
        auto scale = [&](const char* name) {
            const std::string v = m->second.property(name);
            try {
                const float f = v.empty() ? 1.0f : std::stof(v);
                return f > 0 ? f : 1.0f;
            } catch (const std::logic_error&) {
                return 1.0f;
            }
        };
        const float su = scale("floorMaterialScaleU"), sv = scale("floorMaterialScaleV");
        if (su != 1 || sv != 1)
            for (auto& uv : part.mesh.uvs)
                uv = {uv[0] / su, uv[1] / sv};
    }
    // Light-map coordinates from atlas texels to 0-1, now that every wall has its cell.
    for (auto& part : model.parts)
        for (auto& uv : part.mesh.uvs2)
            uv = {uv[0] / float(lm.width), uv[1] / float(std::max<uint32_t>(lm.height, 1))};
    if (lightMap) {
        fillSunOccluders(lot, roofs, lm);
        *lightMap = std::move(lm);
    }
    return model;
}

std::vector<LotFencePiece> lotFencePieces(const Lot& lot, const PatternCatalog& patterns,
                                          std::vector<std::string>* warnings) {
    std::vector<LotFencePiece> out;
    auto ground = [&](int level, float x, float y) {
        return lot.elevation(level, int(std::lround(x)), int(std::lround(y)));
    };
    // As OpenTS2's FenceCollection: a rail model runs one tile along its +X
    // (a diagonal rail one diagonal), placed at the start, turned towards the
    // end and sheared so its far end sits on the ground there.
    for (const auto& w : lot.walls) {
        if (!Lot::isFence(w.type))
            continue;
        const auto* fence = patterns.fence(w.type);
        if (!fence)
            continue;
        const float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        const bool diagonal = len > 1.1f && !fence->diagonalRail.empty();
        const std::string& model = diagonal ? fence->diagonalRail : fence->rail;
        if (model.empty())
            continue;
        const float scale = diagonal ? len / std::sqrt(2.0f) : len;
        const float z0 = ground(w.a.level, w.a.x, w.a.y), z1 = ground(w.b.level, w.b.x, w.b.y);
        const float c = dx / len, s = dy / len, shear = (z1 - z0) / len;
        out.push_back({model, {c * scale, s * scale, shear * scale, 0, -s, c, 0, 0, 0, 0, 1, 0, w.a.x, w.a.y, z0, 1}});
    }
    size_t unknown = 0;
    for (const auto& p : lot.fencePosts) {
        const auto* fence = patterns.fence(p.fence);
        if (!fence || fence->post.empty()) {
            ++unknown;
            continue;
        }
        out.push_back({fence->post, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.x, p.y, ground(p.level, p.x, p.y), 1}});
    }
    if (unknown && warnings)
        warnings->push_back(std::to_string(unknown) + " fence posts of unknown fences");
    // Fence arches (graph 0x13): the centre over the edge like a rail, a
    // post at each end with the west part on the start's and the east part
    // on the end's (placement inferred from the parts' names).
    if (const auto* arch = patterns.fenceArch())
        for (const auto& f : fenceArches(lot)) {
            const float ax = float(f.ax), ay = float(f.ay), bx = float(f.bx), by = float(f.by);
            const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len == 0)
                continue;
            const float c = dx / len, s = dy / len, za = ground(f.level, ax, ay), zb = ground(f.level, bx, by);
            auto put = [&](const std::string& model, float x, float y, float z) {
                if (!model.empty())
                    out.push_back({model, {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, x, y, z, 1}});
            };
            put(arch->center, ax, ay, za);
            put(arch->post, ax, ay, za);
            put(arch->post, bx, by, zb);
            put(arch->west, ax, ay, za);
            put(arch->east, bx, by, zb);
        }
    return out;
}

} // namespace os2
