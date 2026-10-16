#include "lot_model.hpp"
#include "formats/log_once.hpp"
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
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C, kTypeCatalogPattern = 0xCCA8E925, kTypeCatalogRoof = 0xACA8EA06,
                   kTypeCatalogFence = 0x2CB230B8;

// Ground floors sit on the terrain; lift them so they win the depth test
// against the terrain paints drawn after them.
constexpr float kGroundFloorLift = 0.005f;
// terrain.matShad: the canvas texture spans 20 tiles (canvasBaseTextureScale,
// paintTextureScale), its "_detail" texture 4 (canvasDetailTextureScale,
// paintDetailTextureScale); terrainMatDiffCoef 0.9.
constexpr float kTerrainTextureTiles = 20.0f, kTerrainDetailTiles = 4.0f;
constexpr float kStoryHeight = 3.0f;
// Height of walls cut down by the wall modes (cWallGeometryNode's
// SetWallDownHeight default, Mac BV 0x74ab40).
constexpr float kWallsDownHeight = 0.2f;
// The wall tops' material: wall_top's texture twice over, unlit (their
// subset has no light-map page, docs/walls-floors-mac.md §2.2; built in
// buildLotModel from the wall_top TXMT).
constexpr const char* kWallTopMaterial = "wall_top 2x";

// The wall geometry nodes (nTSSG::cWallManager::CreateWallNodesOnLevel, Mac
// BV 0x6c2744; docs/walls-floors-mac.md §1): each level has one per
// tWallTypeIndex, and AddWallImpl (0x6c64c8) puts a partition in the one of
// its vertical span. The node's thickness, height offset (all z), span and
// whether the wall modes reach it; and the style's requiresFlatTop
// (walls.txt / fences.txt), which hangs the wallpaper from the wall's top.
// Walls mitre only with the walls of their own node (its wall graph).
// Vertical spans (cITSProductPartition::tVerticalSpanType, the verticalSpan
// key, table 0x28660e0).
constexpr int kSpanFixedHeight = 0, kSpanFloorToFloor = 1, kSpanFloorToRoof = 2, kSpanFloorToFloorBasedCeiling = 4;
struct WallNode {
    int index = 0;
    float thickness = 0.15f, offset = -0.01f;
    int span = kSpanFloorToFloor;
    bool flatTop = true;
    bool followsMode = true;
};
// Deck skirts (walls.txt walls 16 and 24, "wallThickness none",
// floorToFloorBasedCeiling).
constexpr uint32_t kDeckSkirtRedwood = 16, kDeckSkirtMinimal = 24;

// The deck/foundation style that raises fences, half walls and their posts
// 0.75 (docs/roof-fence-water-mac.md §2.1): the raw marker 4, not a product
// type. No base-game product has that ID (deckfoundations.txt: 0-3; the BV
// parser's fourth keyword is "stage": an expansion's stage, inferred).
constexpr int32_t kRaisingDeckFoundationStyle = 4;
constexpr float kRaisedByStyle = 0.75f;
// FUN_00164a7c (Mac BV): whether grid point (x, y) of `level` touches a
// quarter tile marked kRaisingDeckFoundationStyle (cWorldDB::
// DeckFoundationStyle, cIWorldDB slot 0xc8): the four tiles round the point
// on the lot, each with the two quarters that touch it (table 0x27324e0).
bool touchesRaisingStyle(const Lot& lot, int level, int x, int y) {
    struct Round {
        int dx, dy, quarters; // bits: 1 west, 2 north, 4 east, 8 south
    };
    static constexpr Round kRound[] = {{-1, -1, 6}, {0, -1, 3}, {0, 0, 9}, {-1, 0, 12}};
    for (const auto& t : kRound)
        for (int q = 0; q < 4; ++q)
            if ((t.quarters >> q & 1) && lot.deckFoundationStyle(level, x + t.dx, y + t.dy, q) == kRaisingDeckFoundationStyle)
                return true;
    return false;
}
// FUN_00166414: both ends of a partition touch it.
bool bothEndsRaised(const Lot& lot, const Lot::Wall& w) {
    return touchesRaisingStyle(lot, w.a.level, int(std::lround(w.a.x)), int(std::lround(w.a.y))) &&
           touchesRaisingStyle(lot, w.b.level, int(std::lround(w.b.x)), int(std::lround(w.b.y)));
}

// nTSWorld::PartitionStyleIsWall (Mac BV 0x8b566c): PartitionType 0 (the
// walls.txt walls) or 3 (half walls). AddWallImpl (0x6c64c8) builds nothing
// for any other partition: a fence, covered or not, is only its rails and
// posts (lotFencePieces). docs/walls-floors-mac.md §6.4.
bool partitionIsWall(const Lot::Wall& w, const PatternCatalog& patterns) {
    return !Lot::isFence(w.type) || patterns.isHalfWall(w.type);
}

// Called for walls only (partitionIsWall).
WallNode wallNode(const Lot& lot, const Lot::Wall& w, const PatternCatalog& patterns) {
    // A half wall (PartitionStyleIsHalfWall: PartitionType 3): node 3
    // (fixedHeight, not flat-topped), or node 5, 0.1 thick and raised 0.75,
    // when both ends touch the raising deck/foundation style (AddWallImpl
    // with FUN_00166414, docs/walls-floors-mac.md §1).
    if (patterns.isHalfWall(w.type))
        return bothEndsRaised(lot, w) ? WallNode{5, 0.1f, kRaisedByStyle, kSpanFixedHeight, false, true}
                                      : WallNode{3, 0.15f, -0.01f, kSpanFixedHeight, false, true};
    if (w.type == Lot::Attic)
        return {2, 0.15f, -0.01f, kSpanFloorToRoof, false, true};
    if (w.type == kDeckSkirtRedwood || w.type == kDeckSkirtMinimal)
        return {1, 0.0f, 0.0f, kSpanFloorToFloorBasedCeiling, true, false};
    return {}; // floorToFloor: walls, foundations, pool walls
}
// nTSSG::WallTopElevation (Mac BV 0x743ab0): the top of a wall of `span` at
// (x, y) of `level`. Fixed height: 1.025 over the floor; floor to floor: the
// next level's floor (on the top level the floor plus a story); floor to
// roof: the roofs of the level over the point (RoofHeightAtLocation, the
// next floor without one) less 0.25, not under the floor; based ceiling:
// the next floor less 0.15 (the floor's thickness).
float wallTopElevation(const Lot& lot, const LotRoofs& roofs, int level, float x, float y, int span) {
    x = std::clamp(x, 0.0f, float(lot.width)), y = std::clamp(y, 0.0f, float(lot.height));
    const float floor = lot.elevationAt(level, x, y);
    const float above = lot.hasLevel(level + 1) ? lot.elevationAt(level + 1, x, y) : floor + kStoryHeight;
    switch (span) {
    case kSpanFixedHeight:
        return floor + 1.025f;
    case kSpanFloorToRoof:
        return std::max(roofs.heightAt(x, y, level, above) - 0.25f, floor);
    case kSpanFloorToFloorBasedCeiling:
        return above - 0.15f;
    default:
        return above;
    }
}

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

// The floors' thickness (nTSSG::cFloorThicknessGeometryNode; docs/walls-floors-mac.md
// §6.1): the FloorThickness material (Materials.package: a StandardMaterial,
// diffuse 0.5, no specular, texture floor-edge), lit like an object.
constexpr const char* kFloorThicknessMaterial = "FloorThickness";
constexpr float kFloorThickness = 0.15f; // Mac BV 0x274b638
MaterialDefinition floorThicknessMaterial() {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatBaseTextureName", "floor-edge"},
                    {"stdMatBaseTextureEnabled", "true"},
                    {"stdMatDiffCoef", "0.5,0.5,0.5,1"},
                    {"stdMatSpecPower", "0"}};
    return m;
}

// Quarter-tile triangles (west, north, east, south) of each floor tile, with
// the tile's own texture coordinates, one mesh per material; and the floors'
// thickness on levels 1 and up.
// Levels above `maxLevel` are not drawn (their light-map materials are still set);
// nor, with `lowestCut` below it, the tiles of `maxLevel` a story over the
// ground (storyOverGround).
void addFloors(const Lot& lot, const PatternCatalog& patterns, LotLightMapLayout& lm,
               std::map<std::string, MeshBuilder>& drawn, std::vector<std::string>& warnings, int maxLevel,
               int lowestCut) {
    std::map<std::string, MeshBuilder> hidden;
    // cFloorThicknessGeometryNode (docs/walls-floors-mac.md §6.1): on levels
    // 1 and up (cFloor gives every floor subset that is not a grid one a
    // FloorThickness subset; none on the ground or below it) each edge of
    // each quarter's triangle toggles its ordered locator in a set
    // (UpdateOrderedLocatorSet: in if absent, out if there), leaving the
    // edges round the level's floor; each gets a quad from the floor down
    // 0.15 (cFloorTilePiece, Mac BV 0x7ab07c), normal Perp(b - a) (out of
    // the triangle), u 0 to 1 from a to b, v 0 at the top to 1 at the
    // bottom. Kept with the quarter it came from (drawn or hidden as its
    // floor triangle).
    struct Edge {
        Vec3f a, b;
        bool shown = true;
    };
    using EdgeKey = std::array<long, 4>; // the ends in half tiles, the lesser first
    const std::string thicknessKey = std::string("floorthickness:") + kFloorThicknessMaterial;
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.floorPatterns) {
        const std::string m = patterns.floorMaterial(pattern);
        if (m.empty())
            warnings.push_back("floor pattern " + pattern + " has no material");
        materials[ref] = m;
    }
    for (int level = lot.minLevel; level < lot.minLevel + lot.levelCount; ++level) {
        const float lift = level == 0 ? kGroundFloorLift : 0.0f;
        const bool withBelow = level == maxLevel && lowestCut < maxLevel;
        std::map<EdgeKey, Edge> edgeSet;
        auto toggle = [&](const Vec3f& a, const Vec3f& b, bool shown) {
            const std::array<long, 2> ka{std::lround(a[0] * 2), std::lround(a[1] * 2)},
                kb{std::lround(b[0] * 2), std::lround(b[1] * 2)};
            const EdgeKey key = ka < kb ? EdgeKey{ka[0], ka[1], kb[0], kb[1]} : EdgeKey{kb[0], kb[1], ka[0], ka[1]};
            if (auto it = edgeSet.find(key); it != edgeSet.end())
                edgeSet.erase(it);
            else
                edgeSet.emplace(key, Edge{a, b, shown});
        };
        for (uint32_t x = 0; x < lot.width; ++x)
            for (uint32_t y = 0; y < lot.height; ++y) {
                const auto& quarters = lot.floor(level, x, y);
                if (quarters == std::array<uint16_t, 4>{})
                    continue;
                auto& out = level > maxLevel || (withBelow && storyOverGround(lot, level, int(x), int(y)))
                                ? hidden
                                : drawn;
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
                    // cFloorGeometryNode::SetTileQuadrantSubset (Mac BV
                    // 0x75fa6c, docs/walls-floors-mac.md §4): one triangle a
                    // quarter, its edge's corners and the centre, normal by
                    // their cross product; the texture coordinates are (y, x)
                    // in the world of the corners of the quarter turned by the
                    // tile's saved rotation (WorldLocationsForPatternRotation,
                    // 0x75d004: 2 -> the quarter + 3, 4 -> + 2, 8 -> + 1, any
                    // other value: the quarter itself). So the texture's top
                    // is the west edge and its right the north (the road
                    // tiles' edge lines, road_sidelines_01 at the west curb,
                    // run along a north-south road), they run on across tiles
                    // (floorMaterialScaleU/V spans several), and a turn of
                    // the quarters is one of the texture about the centre.
                    int turns = 0;
                    if (at < lot.floorRotations.size())
                        switch (lot.floorRotations[at][q]) {
                        case 2: turns = 1; break;
                        case 4: turns = 2; break;
                        case 8: turns = 3; break;
                        default: break;
                        }
                    auto v = [&](const Vec3f& p) {
                        float lu = p[1] - fy - 0.5f, lv = p[0] - fx - 0.5f;
                        for (int k = 0; k < turns; ++k)
                            std::swap(lu, lv), lv = -lv;
                        return b.vertex(p, n, fy + 0.5f + lu, fx + 0.5f + lv, lm.floorTexel(level, p[0], p[1]));
                    };
                    b.triangle(v(a), v(e), v(c));
                    if (level >= 1)
                        for (const auto& [from, to] : {std::pair{a, e}, std::pair{e, c}, std::pair{c, a}})
                            toggle(from, to, &out == &drawn);
                }
            }
        for (const auto& [key, edge] : edgeSet) {
            MeshBuilder& t = (edge.shown ? drawn : hidden)[thicknessKey];
            const float dx = edge.b[0] - edge.a[0], dy = edge.b[1] - edge.a[1], len = std::hypot(dx, dy);
            const Vec3f n{dy / len, -dx / len, 0};
            const uint32_t v0 = t.vertex(edge.a, n, 0, 0), v1 = t.vertex(edge.b, n, 1, 0),
                           v2 = t.vertex({edge.a[0], edge.a[1], edge.a[2] - kFloorThickness}, n, 0, 1),
                           v3 = t.vertex({edge.b[0], edge.b[1], edge.b[2] - kFloorThickness}, n, 1, 1);
            t.triangle(v0, v2, v1);
            t.triangle(v1, v2, v3);
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
        } catch (const std::exception& err) {
            os2::logOnce("wall mask", mid, err);
        }
    }
    if (cache)
        cache->wallMasks[key] = out;
    return out;
}

bool wallModeLevel(const LotViewOptions& view, int level) { return lotWallModeLevel(view, level); }


// Walls as the game's cWallGeometryNode builds them (docs/walls-floors-mac.md
// §2-3): each side of a wall is a face of three columns (start, middle, end:
// cWallPiece::ComputeSegmentVertices, Mac BV 0x744520) whose end corners are
// mitred by the walls of the same node meeting there (ComputeMetaData,
// 0x749138), an end with no other wall gets a cap, each wall a top of six
// points, and each vertex where three or more walls meet a polygon over the
// junction (cWallVertexPiece, 0x74a40c). Walls cut down by the wall modes
// lower the top of their columns to kWallsDownHeight; in cutaway by end
// points, as cWallCutawayManager::UpdateSceneGraph (0xc8cd90) does.
void addWalls(const Lot& lot, const PatternCatalog& patterns, const LotRoofs& roofs, const LotViewOptions& view,
              bool roofShown, const std::vector<WallHole>& holes, LotLightMapLayout& lm,
              std::map<std::string, MeshBuilder>& drawn, std::vector<std::string>& warnings,
              std::vector<std::array<float, 5>>* cut, ResourceIndex& index, LotModelCache* cache,
              std::vector<ObjectModel::AlphaMask>& masks, std::map<std::string, int>& maskedParts) {
    using V2 = std::array<float, 2>;
    // Walls above the view's level are built into this and dropped: their
    // light-map cells stay, so the atlas is the same for every view.
    std::map<std::string, MeshBuilder> hidden;
    std::map<uint16_t, std::string> materials;
    for (const auto& [ref, pattern] : lot.wallPatterns)
        materials[ref] = patterns.wallMaterial(pattern);
    size_t unknownFences = 0;
    const std::vector<bool> cutaway = lotCutawayWalls(lot, view);
    auto keyOf = [](int level, float x, float y) {
        return std::make_tuple(level, std::lround(x * 8), std::lround(y * 8));
    };
    using VKey = std::tuple<int, long, long>;

    // The walls drawn, with their node.
    struct W {
        const Lot::Wall* wall = nullptr;
        WallNode node;
        bool shown = false, offFoundation = false;
        std::array<bool, 3> down{}; // columns start, middle, end
        // Filled below: the ends' corner offsets (A: right of a -> b, B: left)
        // and whether an end has no other wall of the node (a cap).
        V2 a0{}, b0{}, a1{}, b1{};
        bool capA = false, capB = false;
    };
    std::vector<W> ws;
    for (const auto& w : lot.walls) {
        // The column deck's skirt (walls.txt "wall 4": notRendered) is not drawn.
        if (w.type == Lot::DeckSkirt)
            continue;
        if (Lot::isFence(w.type) && !patterns.fence(w.type))
            ++unknownFences;
        if (!partitionIsWall(w, patterns))
            continue; // rails and posts only: see lotFencePieces
        if (w.a.x == w.b.x && w.a.y == w.b.y)
            continue;
        // AddWallImpl: nothing either when neither tile beside the wall is on
        // the lot (cWorldDB slot 0xf8 on the locator's two tiles).
        {
            const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
            const float len = std::hypot(w.b.x - w.a.x, w.b.y - w.a.y);
            const float nx = (w.b.y - w.a.y) / len, ny = -(w.b.x - w.a.x) / len;
            auto onLot = [&](float s) {
                const float x = std::floor(mx + s * nx * 0.5f), y = std::floor(my + s * ny * 0.5f);
                return x >= 0 && y >= 0 && x < float(lot.width) && y < float(lot.height);
            };
            if (!onLot(1) && !onLot(-1))
                continue;
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
        W r;
        r.wall = &w;
        r.node = wallNode(lot, w, patterns);
        ws.push_back(r);
    }

    // Which columns are down. Walls down: every wall of the level's nodes
    // the mode reaches (SetLevelUpDownState 0x6c4e10: nodes 0, 2, 3 and 5;
    // foundations and pool walls, in node 0, inferred kept up: which levels
    // the game lowers was not read). Cutaway: the end points of the walls cut
    // (lotCutawayWalls) are down; in every wall of nodes 0 and 2 an end at a
    // point down is down (SetVertexUpDownState 0x7487d8) and so is its middle
    // (SetSegmentUpDownState 0x748668), so a wall with both ends down is down
    // whole and one with a single end down ramps down to it.
    std::set<VKey> downPoints;
    if (view.walls == LotViewOptions::Walls::Cutaway)
        for (size_t i = 0; i < lot.walls.size(); ++i)
            if (cutaway[i]) {
                const auto& w = lot.walls[i];
                downPoints.insert(keyOf(w.a.level, w.a.x, w.a.y));
                downPoints.insert(keyOf(w.a.level, w.b.x, w.b.y));
            }
    for (auto& r : ws) {
        const auto& w = *r.wall;
        const int level = w.a.level;
        if (!r.node.followsMode || !wallModeLevel(view, level))
            continue;
        if (view.walls == LotViewOptions::Walls::Down) {
            const bool lowers = r.node.index == 2 || r.node.index == 3 || r.node.index == 5 || (r.node.index == 0 && w.type == Lot::Normal);
            r.down = {lowers, lowers, lowers};
        } else if (view.walls == LotViewOptions::Walls::Cutaway && (r.node.index == 0 || r.node.index == 2)) {
            const bool da = downPoints.count(keyOf(level, w.a.x, w.a.y)) > 0;
            const bool db = downPoints.count(keyOf(level, w.b.x, w.b.y)) > 0;
            r.down = {da, da || db, db};
        }
    }

    // Visibility. Attic walls (the gables) stand on the level over the
    // roof's walls and show with the roof, from the top floor. A view showing
    // its level with the one below (a house on foundations) leaves out that
    // level's walls of the next story of a building on the ground
    // (storyOverGround; attic walls also over a dip: the sawtooth gables
    // across N002_Lot31's pool pit). Not the fences: railings stand on decks
    // over any ground.
    for (auto& r : ws) {
        const auto& w = *r.wall;
        const int level = w.a.level;
        const bool attic = w.type == Lot::Attic, fence = Lot::isFence(w.type);
        if (level == view.level && view.lowestCut < view.level && !fence && !(attic && roofShown)) {
            const float len = std::hypot(w.b.x - w.a.x, w.b.y - w.a.y);
            const float nx = (w.b.y - w.a.y) / len, ny = -(w.b.x - w.a.x) / len;
            const float mx = std::clamp((w.a.x + w.b.x) / 2, 0.0f, float(lot.width)),
                        my = std::clamp((w.a.y + w.b.y) / 2, 0.0f, float(lot.height));
            auto side = [&](float s) {
                return storyOverGround(lot, level, int(std::floor((w.a.x + w.b.x) / 2 + s * nx * 0.5f)),
                                       int(std::floor((w.a.y + w.b.y) / 2 + s * ny * 0.5f)));
            };
            r.offFoundation = (side(1) && side(-1)) ||
                              (attic && lot.elevationAt(level, mx, my) - lot.elevationAt(level - 1, mx, my) > kStoryHeight);
        }
        r.shown = (level <= view.level && !r.offFoundation) || (attic && roofShown && level == view.level + 1);
        // (A wall of the next story left out too: what hangs on it goes with it.)
        if (cut && level <= view.level && ((r.down[0] && r.down[2]) || r.offFoundation))
            cut->push_back({w.a.x, w.a.y, w.b.x, w.b.y, float(level)});
    }

    // The mitred corners (ComputeMetaData): at each end, the face on each
    // side of the wall meets the wall of its node turning first toward that
    // side, whose face looks at the same angle; the corner moves to
    // h m / (m . n) with m = normalize(n + n') (n, n' the two faces'
    // normals, h half the thickness). No other wall there: h n, and a cap.
    std::map<std::pair<int, VKey>, std::vector<std::pair<size_t, int>>> atPoint; // (node, point) -> (wall, end)
    for (size_t i = 0; i < ws.size(); ++i) {
        const auto& w = *ws[i].wall;
        atPoint[{ws[i].node.index, keyOf(w.a.level, w.a.x, w.a.y)}].push_back({i, 0});
        atPoint[{ws[i].node.index, keyOf(w.a.level, w.b.x, w.b.y)}].push_back({i, 1});
    }
    auto unit = [](V2 v) {
        const float n = std::hypot(v[0], v[1]);
        return n > 0 ? V2{v[0] / n, v[1] / n} : V2{1, 0};
    };
    auto rightOf = [](V2 d) { return V2{d[1], -d[0]}; };
    // The way out of wall i's end e.
    auto outOf = [&](size_t i, int e) {
        const auto& w = *ws[i].wall;
        return e == 0 ? unit({w.b.x - w.a.x, w.b.y - w.a.y}) : unit({w.a.x - w.b.x, w.a.y - w.b.y});
    };
    constexpr float kTwoPi = 6.28318530718f;
    // Clockwise angle from u to v in (0, 2 pi]: the way rightOf turns.
    auto clockwise = [&](V2 u, V2 v) {
        float a = -std::atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]);
        if (a <= 1e-5f)
            a += kTwoPi;
        return a;
    };
    for (auto& [key, ends] : atPoint)
        for (const auto& [i, e] : ends) {
            const V2 out = outOf(i, e);
            const float h = ws[i].node.thickness / 2;
            // right: the face right of the way out; left: the other.
            V2 offset[2];
            for (int side = 0; side < 2; ++side) {
                const V2 nf = side == 0 ? rightOf(out) : V2{-rightOf(out)[0], -rightOf(out)[1]};
                offset[side] = {nf[0] * h, nf[1] * h};
                float best = 1e9f;
                V2 ng{};
                for (const auto& [j, f] : ends) {
                    if (j == i)
                        continue;
                    const V2 dj = outOf(j, f);
                    const float a = side == 0 ? clockwise(out, dj) : clockwise(dj, out);
                    if (a < best) {
                        best = a;
                        // The neighbour's face toward the angle between them.
                        ng = side == 0 ? V2{-rightOf(dj)[0], -rightOf(dj)[1]} : rightOf(dj);
                    }
                }
                if (best > 1e8f)
                    continue;
                const V2 m{nf[0] + ng[0], nf[1] + ng[1]};
                const float len = std::hypot(m[0], m[1]);
                if (len <= 1e-6f)
                    continue;
                const V2 mu{m[0] / len, m[1] / len};
                const float dot = mu[0] * nf[0] + mu[1] * nf[1];
                if (dot > 1e-4f)
                    offset[side] = {h * mu[0] / dot, h * mu[1] / dot};
            }
            W& r = ws[i];
            // At the start the way out is a -> b: its right is side A; at the
            // end it is b -> a, whose right is side B.
            if (e == 0)
                r.a0 = offset[0], r.b0 = offset[1], r.capA = ends.size() == 1;
            else
                r.b1 = offset[0], r.a1 = offset[1], r.capB = ends.size() == 1;
        }

    // The top of each wall's columns (start, middle, end), as drawn.
    struct Built {
        std::array<float, 3> floorZ{}, fullTop{}, top{}; // floor, uncut top and drawn top per column
        std::array<V2, 6> q{};                            // Q0..Q5 (ComputeSegmentVertices)
    };
    std::vector<Built> built(ws.size());
    for (size_t i = 0; i < ws.size(); ++i) {
        const auto& r = ws[i];
        const auto& w = *r.wall;
        const int level = w.a.level;
        const V2 pa{w.a.x, w.a.y}, pb{w.b.x, w.b.y}, pm{(w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2};
        const V2 d = unit({pb[0] - pa[0], pb[1] - pa[1]}), n = rightOf(d);
        const float h = r.node.thickness / 2;
        Built& b = built[i];
        const std::array<V2, 3> col{pa, pm, pb};
        for (size_t c = 0; c < 3; ++c) {
            const float x = std::clamp(col[c][0], 0.0f, float(lot.width)), y = std::clamp(col[c][1], 0.0f, float(lot.height));
            b.floorZ[c] = lot.elevationAt(level, x, y);
            b.fullTop[c] = wallTopElevation(lot, roofs, level, x, y, r.node.span);
            const float height = r.down[c] ? kWallsDownHeight : b.fullTop[c] - b.floorZ[c];
            b.top[c] = b.floorZ[c] + height;
        }
        b.q = {V2{pa[0] + r.a0[0], pa[1] + r.a0[1]}, V2{pm[0] + n[0] * h, pm[1] + n[1] * h},
               V2{pb[0] + r.a1[0], pb[1] + r.a1[1]}, V2{pb[0] + r.b1[0], pb[1] + r.b1[1]},
               V2{pm[0] - n[0] * h, pm[1] - n[1] * h}, V2{pa[0] + r.b0[0], pa[1] + r.b0[1]}};
    }

    const std::string topKey = "walltop:" + std::string(kWallTopMaterial);
    for (size_t i = 0; i < ws.size(); ++i) {
        const auto& r = ws[i];
        const auto& w = *r.wall;
        const Built& b = built[i];
        const int level = w.a.level;
        const bool attic = w.type == Lot::Attic, fence = Lot::isFence(w.type);
        auto& out = r.shown ? drawn : hidden;
        const float off = r.node.offset;
        const float len = std::hypot(w.b.x - w.a.x, w.b.y - w.a.y);
        const V2 d = unit({w.b.x - w.a.x, w.b.y - w.a.y}), n = rightOf(d);
        const float h = r.node.thickness / 2;

        // Doors and windows on this wall: intervals along it (from a) and
        // heights cut out of both faces.
        std::vector<Cut> cuts;
        // Attic walls too: the sawtooth roofs of N002_Lot31 have their row
        // of windows in the attic walls between the roofs.
        if ((w.type == Lot::Normal || attic) && !fence)
            cuts = wallCuts(holes, w.a.x, w.a.y, d[0], d[1], len, std::min(b.floorZ[0], b.floorZ[2]),
                            std::max({b.top[0], b.top[1], b.top[2]}));
        // A door or window with a wall mask for this wall cuts its own shape:
        // the faces get the mask (alpha tested in the wall shader) instead of
        // the box cut. The mask spans the tile the wall covers, u along the
        // object's -x (u = 0 on the tile's edge toward the object's +x) and v
        // from a story above the wall's floor down to it, as the game's UV1
        // (inferred from the textures: a two-tile arch's halves "0" and "1e",
        // the second one tile along +x, are open up to their shared edge only
        // with u running along -x; u along +x opened them at the outer edges
        // and left a strip of wall in the middle of N001_Lot82's front
        // arches).
        int maskIndex = -1;
        const float base = std::min(b.floorZ[0], b.floorZ[2]);
        for (auto c = cuts.begin(); c != cuts.end() && maskIndex < 0; ++c) {
            const WallHole& hole = holes[c->hole];
            if (hole.cutout.empty())
                continue;
            // Straight: the wall runs along the object's +x. Diagonal: the
            // "...diagonal" models stand square and their wall runs along
            // +x + local north (rotated +90°) (the lots' diagonal windows all
            // do), its faces "nw" and "se"; the wall is sqrt 2 long and the
            // next tile along is one east and one north of the object's
            // (doorclub2diagonal_1e_1n_0_nw).
            const float along = d[0] * hole.east[0] + d[1] * hole.east[1];
            const bool diag = std::abs(std::abs(along) - 0.7071f) < 0.05f && (hole.east[0] == 0 || hole.east[1] == 0);
            if (!diag && std::abs(along) < 0.99f)
                continue;
            std::array<float, 2> axis{hole.east[0], hole.east[1]};
            if (diag) {
                const float rr = 1.0f / std::sqrt(2.0f);
                axis = {(hole.east[0] - hole.east[1]) * rr, (hole.east[1] + hole.east[0]) * rr};
            }
            const float span = diag ? std::sqrt(2.0f) : 1.0f;
            const float mx = (w.a.x + w.b.x) / 2 - hole.origin[0], my = (w.a.y + w.b.y) / 2 - hole.origin[1];
            const int k = int(std::lround((mx * axis[0] + my * axis[1]) / span));
            const int storey = std::max(0, int(std::lround((base - hole.origin[2]) / kStoryHeight)));
            auto values = diag ? wallMaskValues(index, hole.cutout, k, storey, cache, true, k)
                               : wallMaskValues(index, hole.cutout, k, storey, cache);
            if (!values)
                continue;
            values->planar = true;
            values->planeU = {-axis[0] / span, -axis[1] / span, 0,
                              0.5f + float(k) + (hole.origin[0] * axis[0] + hole.origin[1] * axis[1]) / span};
            values->planeV = {0, 0, -1.0f / kStoryHeight, (base + kStoryHeight) / kStoryHeight};
            maskIndex = int(masks.size());
            masks.push_back(std::move(*values));
            cuts.erase(c);
            break;
        }

        // The two faces: side 0 is a -> b's (A, the right: patterns[0]),
        // side 1 the opposite locator's (B). Each face runs from its own
        // locator's start: columns start, middle, end.
        for (int side = 0; side < 2; ++side) {
            auto it = materials.find(w.patterns[size_t(side)]);
            if (it == materials.end() || it->second.empty())
                continue;
            const float s = side == 0 ? 1.0f : -1.0f;
            // A pool wall's face toward the earth (no pool floor on that side)
            // is under the ground: drawn, it showed past the lot's edge as a
            // cyan box under the pool.
            if (w.type == Lot::Pool) {
                const int tx = int(std::floor((w.a.x + w.b.x) / 2 + n[0] * s * 0.5f));
                const int ty = int(std::floor((w.a.y + w.b.y) / 2 + n[1] * s * 0.5f));
                if (tx < 0 || ty < 0 || tx >= int(lot.width) || ty >= int(lot.height) ||
                    lot.floor(level, uint32_t(tx), uint32_t(ty)) == std::array<uint16_t, 4>{})
                    continue;
            }
            std::string key = "wall:" + it->second;
            if (maskIndex >= 0) {
                key += "#mask" + std::to_string(maskIndex);
                maskedParts[key] = maskIndex;
            }
            MeshBuilder& mb = out[key];
            const Vec3f nrm{n[0] * s, n[1] * s, 0};
            // The face's columns in its locator's order, `t` along a -> b from a.
            struct Col {
                float t, z0, z1, full;
                V2 p;
                float u;
            };
            const std::array<int, 3> order = side == 0 ? std::array<int, 3>{0, 1, 2} : std::array<int, 3>{2, 1, 0};
            const std::array<V2, 3> face = side == 0 ? std::array<V2, 3>{b.q[0], b.q[1], b.q[2]}
                                                     : std::array<V2, 3>{b.q[3], b.q[4], b.q[5]};
            std::array<Col, 3> cols;
            for (int c = 0; c < 3; ++c) {
                const int k = order[size_t(c)];
                const V2 p = face[size_t(c)];
                cols[size_t(c)] = {(p[0] - w.a.x) * d[0] + (p[1] - w.a.y) * d[1], b.floorZ[size_t(k)], b.top[size_t(k)],
                                   b.fullTop[size_t(k)], p, 0.5f * float(c)};
            }
            // The face's light-map cell, lit as the room it faces: the face's
            // line from its least to its farthest point along a -> b.
            const float t0 = std::min({0.0f, cols[0].t, cols[2].t}), t1 = std::max({len, cols[0].t, cols[2].t});
            LotLightMapLayout::WallFace lf;
            lf.origin = {w.a.x + n[0] * h * s + d[0] * t0, w.a.y + n[1] * h * s + d[1] * t0};
            lf.along = {d[0], d[1]};
            lf.normal = {nrm[0], nrm[1]};
            lf.length = t1 - t0;
            lf.base = std::min(b.floorZ[0], b.floorZ[2]);
            lf.top = std::max({b.fullTop[0], b.fullTop[1], b.fullTop[2]});
            lf.level = level;
            lf.wallA = {w.a.x, w.a.y}, lf.wallB = {w.b.x, w.b.y};
            lf.room = roomBeside(lot, level, (w.a.x + w.b.x) / 2, (w.a.y + w.b.y) / 2, nrm[0], nrm[1], true);
            // An attic wall standing over the roof on its side (the tall face
            // of a sawtooth roof, N002_Lot31) is outside there, though the
            // room map puts the attic under that roof beside it: lit as
            // outside (inferred; it was drawn dark).
            if (attic && lf.room) {
                const float mx = (w.a.x + w.b.x) / 2 + nrm[0] * 0.05f, my = (w.a.y + w.b.y) / 2 + nrm[1] * 0.05f;
                const float under = roofs.heightAt(std::clamp(mx, 0.0f, float(lot.width)),
                                                   std::clamp(my, 0.0f, float(lot.height)), level, -1e9f);
                if (under > -1e8f && under < lf.top - 0.5f)
                    lf.room = 0;
            }
            lf.material = lm.materialIndex(it->second);
            const size_t cell = lm.addWall(lf);
            auto lmAt = [&](const Vec3f& p) {
                return lm.wallTexel(cell, (p[0] - lf.origin[0]) * d[0] + (p[1] - lf.origin[1]) * d[1], p[2]);
            };
            // Wallpaper (UV0): u 0, 0.5, 1 over the three columns, whatever
            // the mitre; v from the wall's top down, a story to the texture
            // (D / 3 at the floor, D the full height, so a wall cut down
            // shows the foot of its paper), or from 1 at the floor for the
            // styles that do not require a flat top (attic and half walls);
            // + 0.05 on deck skirts (span 4).
            auto vFloor = [&](const Col& c) {
                float v = r.node.flatTop ? (c.full - c.z0) / kStoryHeight : 1.0f;
                if (r.node.flatTop && r.node.span == kSpanFloorToFloorBasedCeiling)
                    v += 0.05f;
                return v;
            };
            // Pieces between the columns and the holes' edges, each split
            // around the holes over it.
            std::vector<float> breaks{cols[0].t, cols[1].t, cols[2].t};
            const bool rising = cols[2].t >= cols[0].t;
            for (const auto& c : cuts)
                for (float x : {c.u0, c.u1})
                    if (rising ? (x > cols[0].t && x < cols[2].t) : (x < cols[0].t && x > cols[2].t))
                        breaks.push_back(x);
            std::sort(breaks.begin(), breaks.end());
            if (!rising)
                std::reverse(breaks.begin(), breaks.end());
            // A point of the face at `t` (interpolated between its columns).
            struct At {
                V2 p;
                float z0, z1, u, v0;
            };
            auto at = [&](float t) {
                const bool first = rising ? t <= cols[1].t : t >= cols[1].t;
                const Col& c0 = first ? cols[0] : cols[1];
                const Col& c1 = first ? cols[1] : cols[2];
                const float f = std::abs(c1.t - c0.t) > 1e-6f ? std::clamp((t - c0.t) / (c1.t - c0.t), 0.0f, 1.0f) : 0.0f;
                auto lerp = [&](float x, float y) { return x + (y - x) * f; };
                return At{V2{lerp(c0.p[0], c1.p[0]), lerp(c0.p[1], c1.p[1])}, lerp(c0.z0, c1.z0), lerp(c0.z1, c1.z1),
                          lerp(c0.u, c1.u), lerp(vFloor(c0), vFloor(c1))};
            };
            for (size_t k = 0; k + 1 < breaks.size(); ++k) {
                const float s0 = breaks[k], s1 = breaks[k + 1];
                if (std::abs(s1 - s0) < 1e-4f)
                    continue;
                const At p0 = at(s0), p1 = at(s1);
                std::vector<std::pair<float, float>> gaps; // z ranges cut out of this piece
                for (const auto& c : cuts)
                    if (c.u0 <= std::min(s0, s1) + 1e-4f && c.u1 >= std::max(s0, s1) - 1e-4f)
                        gaps.push_back({c.z0, c.z1});
                std::sort(gaps.begin(), gaps.end());
                // Solid bands: from the bottom (or a gap's top) to the next gap's bottom (or the top).
                std::vector<std::array<float, 4>> bands; // z at s0 low/high, z at s1 low/high
                std::array<float, 2> low{p0.z0, p1.z0};
                for (const auto& [g0, g1] : gaps) {
                    bands.push_back({low[0], std::min(g0, p0.z1), low[1], std::min(g0, p1.z1)});
                    low = {std::max(g1, p0.z0), std::max(g1, p1.z0)};
                }
                bands.push_back({low[0], p0.z1, low[1], p1.z1});
                for (const auto& band : bands) {
                    if (band[1] - band[0] < 1e-4f && band[3] - band[2] < 1e-4f)
                        continue;
                    // v runs down a story per 3 up from the column's floor.
                    auto vtx = [&](const At& p, float z) {
                        const Vec3f pos{p.p[0], p.p[1], z + off};
                        return mb.vertex(pos, nrm, p.u, p.v0 - (z - p.z0) / kStoryHeight, lmAt({p.p[0], p.p[1], z}));
                    };
                    const uint32_t i0 = vtx(p0, band[0]), i1 = vtx(p1, band[2]), i2 = vtx(p1, band[3]), i3 = vtx(p0, band[1]);
                    // Counter-clockwise seen from the face's side (each
                    // face runs along its own locator).
                    mb.quad(i0, i1, i2, i3);
                }
            }
            // The cap of the face's locator's start when no other wall of the
            // node meets it (cWallSide::InsertVertices 0x745ec4): from this
            // face's corner across the wall to the other face's, normal back
            // along the wall, the paper from u = 1 - thickness to 1.
            if (side == 0 ? r.capA : r.capB) {
                const int k = side == 0 ? 0 : 2;
                const V2 qa = side == 0 ? b.q[0] : b.q[3], qb = side == 0 ? b.q[5] : b.q[2];
                const Vec3f cn{-d[0] * s, -d[1] * s, 0};
                const float z0 = b.floorZ[size_t(k)], z1 = b.top[size_t(k)];
                const Col& c = cols[0];
                const float v0 = vFloor(c), v1 = v0 - (z1 - z0) / kStoryHeight;
                // Light map: the first `thickness` of the face's cell.
                auto lmCap = [&](float t, float z) {
                    return lm.wallTexel(cell, (c.p[0] - lf.origin[0]) * d[0] + (c.p[1] - lf.origin[1]) * d[1] + t * s, z);
                };
                const uint32_t i0 = mb.vertex({qb[0], qb[1], z0 + off}, cn, 1 - r.node.thickness, v0, lmCap(r.node.thickness, z0)),
                               i1 = mb.vertex({qa[0], qa[1], z0 + off}, cn, 1, v0, lmCap(0, z0)),
                               i2 = mb.vertex({qa[0], qa[1], z1 + off}, cn, 1, v1, lmCap(0, z1)),
                               i3 = mb.vertex({qb[0], qb[1], z1 + off}, cn, 1 - r.node.thickness, v1, lmCap(r.node.thickness, z1));
                // Facing away from the face's locator: counter-clockwise from there.
                mb.quad(i0, i1, i2, i3);
            }
        }
        // The holes' sides through the wall: jambs (where the hole ends
        // inside the wall), sill and head, in the wall top's material (the
        // game has none: its masks cut the faces and the frames cover the
        // edges; these close the box cuts of objects without a mask).
        for (const auto& c : cuts) {
            MeshBuilder& rev = out[topKey];
            auto pt = [&](float along, float sd, float z) {
                return Vec3f{w.a.x + d[0] * along + n[0] * h * sd, w.a.y + d[1] * along + n[1] * h * sd, z};
            };
            auto quadFace = [&](Vec3f p0, Vec3f p1, Vec3f p2, Vec3f p3) {
                const Vec3f nn = normalOf(p0, p1, p2);
                auto vtx = [&](const Vec3f& p) { return rev.vertex(p, nn, p[0] + p[2], p[1] + p[2]); };
                rev.quad(vtx(p0), vtx(p1), vtx(p2), vtx(p3));
            };
            if (c.u0 > 1e-3f) // jamb at u0, facing +along
                quadFace(pt(c.u0, -1, c.z0), pt(c.u0, 1, c.z0), pt(c.u0, 1, c.z1), pt(c.u0, -1, c.z1));
            if (c.u1 < len - 1e-3f) // jamb at u1, facing -along
                quadFace(pt(c.u1, 1, c.z0), pt(c.u1, -1, c.z0), pt(c.u1, -1, c.z1), pt(c.u1, 1, c.z1));
            quadFace(pt(c.u0, 1, c.z0), pt(c.u0, -1, c.z0), pt(c.u1, -1, c.z0), pt(c.u1, 1, c.z0)); // sill, up
            quadFace(pt(c.u0, -1, c.z1), pt(c.u0, 1, c.z1), pt(c.u1, 1, c.z1), pt(c.u1, -1, c.z1)); // head, down
        }
        // The top (cWallTop::InsertVertices 0x745cfc): Q0..Q5 at the top of
        // their columns, normal up, the texture by the point's (x, y) in the
        // world; no light map (its subset has no light-map page: 2 x the
        // texture, kWallTopMaterial).
        {
            MeshBuilder& top = out[topKey];
            const Vec3f up{0, 0, 1};
            const std::array<size_t, 6> colOf{0, 1, 2, 2, 1, 0};
            std::array<uint32_t, 6> id{};
            for (size_t k = 0; k < 6; ++k)
                id[k] = top.vertex({b.q[k][0], b.q[k][1], b.top[colOf[k]] + off}, up, b.q[k][0], b.q[k][1]);
            top.triangle(id[0], id[1], id[4]);
            top.triangle(id[4], id[5], id[0]);
            top.triangle(id[1], id[2], id[3]);
            top.triangle(id[3], id[4], id[1]);
        }
    }

    // Where three or more walls of a node meet (cWallVertexPiece), the
    // polygon of their corners: of each wall the top corner right of its way
    // out of the point (Q0 if it starts there, Q3 if it ends there), going
    // round the point; normal up, the texture by (x, y), at the wall's top
    // there (down with the point).
    for (const auto& [key, ends] : atPoint) {
        if (ends.size() < 3)
            continue;
        std::vector<std::pair<float, std::array<float, 3>>> corners;
        bool shown = false;
        for (const auto& [i, e] : ends) {
            const V2 out = outOf(i, e);
            const Built& b = built[i];
            const V2 q = e == 0 ? b.q[0] : b.q[3];
            const float z = (e == 0 ? b.top[0] : b.top[2]) + ws[i].node.offset;
            corners.push_back({std::atan2(out[1], out[0]), {q[0], q[1], z}});
            shown = shown || ws[i].shown;
        }
        std::sort(corners.begin(), corners.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        MeshBuilder& poly = (shown ? drawn : hidden)[topKey];
        const Vec3f up{0, 0, 1};
        std::vector<uint32_t> id;
        for (const auto& [angle, p] : corners)
            id.push_back(poly.vertex(p, up, p[0], p[1]));
        for (size_t k = 1; k + 1 < id.size(); ++k)
            poly.triangle(id[0], id[k], id[k + 1]);
    }
    if (unknownFences)
        warnings.push_back(std::to_string(unknownFences) + " fence segments of unknown fences (drawn as the default fence)");
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

// Pools (cPoolGeometryNode::CreatePoolWater, docs/roof-fence-water-mac.md
// 3.2): the surface "poolwater-1" (water.matShad poolWater-1: on the Mac's
// DX8 path RegularWavesHLSL, with swimming_pool-envcube and its waves), and
// three depth layers "poolwaterlayer-1..3" 0.75 apart below it
// (PoolDepthLayerMaterial: swimming_pool_deeplayer x poolLayerColour (0,
// 0.5, 1), alpha x 0.9, blended, unlit), on the pool water layer (5).
MaterialDefinition poolWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "PoolWaterSurface";
    m.properties = {{"stdMatEnvCubeTextureName", "swimming_pool-envcube"},
                    {"wmXSpeed", "3"},
                    {"wmXRepeat", "5"},
                    {"wmXWaveHeight", "0.01"},
                    {"wmYRepeat", "6"},
                    {"stdMatCullMode", "none"}};
    return m;
}
MaterialDefinition poolLayerMaterial() {
    MaterialDefinition m;
    m.materialType = "PoolWaterLayer";
    m.properties = {{"stdMatBaseTextureName", "swimming_pool_deeplayer"},
                    {"stdMatDiffCoef", "0,0.5,1,1"},
                    {"stdMatAlphaMultiplier", "0.9"},
                    {"stdMatAlphaBlendMode", "blend"},
                    {"stdMatLightingEnabled", "false"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water: TerrainWaterShader (waterOnTerrain, docs/shaders-mac.md 9):
// the reflection of "reflectionoutdoorwater-envcube" x colorScalar (1, 1, 1),
// alpha from the vertex colour (cWaterGeometryBuilder sets it from the
// depth, docs/roof-fence-water-mac.md 3).
MaterialDefinition terrainWaterMaterial() {
    MaterialDefinition m;
    m.materialType = "TerrainWater";
    m.properties = {{"stdMatEnvCubeMode", "reflection"},
                    {"stdMatEnvCubeCoef", "1,1,1"},
                    {"stdMatEnvCubeTextureName", "reflectionoutdoorwater-envcube"},
                    {"stdMatCullMode", "none"}};
    return m;
}

// Terrain water over every tile whose water level is above the terrain at a
// corner (as OpenTS2 does; the game shows each quarter triangle of a tile
// with a corner or the centre under water, IsWaterTilePartUnderTerrain
// FUN_00c41488), each vertex white with the alpha of its depth
// (FUN_00c41644: Terrain.ini AlphaVsDepthCurve); and pool water over each
// pool's floor tiles.
void addWater(const Lot& lot, const LotWaterParams& params, std::map<std::string, MeshBuilder>& out) {
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
            auto v = [&](size_t i) {
                const uint32_t k = terrain->vertex(p[i], up, p[i][0] / 4, p[i][1] / 4);
                const float a = params.alpha(lotWaterDepth(lot, c[i][0], c[i][1]));
                terrain->mesh.colors.resize(terrain->mesh.positions.size(), 0xFFFFFFFFu);
                terrain->mesh.colors[k] = (uint32_t(std::clamp(a, 0.0f, 255.0f) + 0.5f) << 24) | 0xFFFFFFu;
                return k;
            };
            terrain->quad(v(0), v(1), v(2), v(3));
        }
    // Each pool tile: corners, edge midpoints and centre (the game's quarter
    // triangles split at their midpoints, AddFloorTileGeometry). The depth
    // layers fade out at the pool's walls: AddPoolDepthLayerColors gives the
    // vertices on the walls other texture coordinates than the rest (their
    // values are set at run time, not read: here the texture's transparent
    // corner and its centre, inferred).
    for (const auto& pool : lot.pools) {
        const int x0 = int(std::lround(pool.x)), y0 = int(std::lround(pool.y));
        auto isPool = [&](int x, int y) {
            if (x < x0 || y < y0 || x >= x0 + int(pool.width) || y >= y0 + int(pool.height) || x < 0 || y < 0 ||
                x >= int(lot.width) || y >= int(lot.height))
                return false;
            // Pool tiles have a floor one level down (the pool's bottom) and none at the surface.
            return lot.floor(pool.level - 1, uint32_t(x), uint32_t(y)) != std::array<uint16_t, 4>{} &&
                   lot.floor(pool.level, uint32_t(x), uint32_t(y)) == std::array<uint16_t, 4>{};
        };
        // A point (in half tiles) is on the wall when a tile it touches is not pool.
        auto onWall = [&](int hx, int hy) {
            for (int tx = (hx - 1) / 2 - 1; tx <= hx / 2; ++tx)
                for (int ty = (hy - 1) / 2 - 1; ty <= hy / 2; ++ty)
                    if (2 * tx <= hx && hx <= 2 * tx + 2 && 2 * ty <= hy && hy <= 2 * ty + 2 && !isPool(tx, ty))
                        return true;
            return false;
        };
        for (int x = x0; x < x0 + int(pool.width); ++x)
            for (int y = y0; y < y0 + int(pool.height); ++y) {
                if (!isPool(x, y))
                    continue;
                const Vec3f up{0, 0, 1};
                for (int layer = 0; layer <= 3; ++layer) {
                    MeshBuilder& b = out[layer == 0 ? std::string("water:pool") : "water:poollayer" + std::to_string(layer)];
                    std::array<uint32_t, 9> k{};
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) {
                            const float px = float(x) + 0.5f * float(i), py = float(y) + 0.5f * float(j);
                            const Vec3f q{px, py, lot.elevationAt(pool.level, px, py) - pool.depth - 0.75f * float(layer)};
                            const bool wall = layer > 0 && onWall(2 * x + i, 2 * y + j);
                            k[size_t(i * 3 + j)] = layer == 0 ? b.vertex(q, up, px, py)
                                                              : b.vertex(q, up, wall ? 0.0f : 0.5f, wall ? 0.0f : 0.5f);
                        }
                    for (int i = 0; i < 2; ++i)
                        for (int j = 0; j < 2; ++j)
                            b.quad(k[size_t(i * 3 + j)], k[size_t((i + 1) * 3 + j)], k[size_t((i + 1) * 3 + j + 1)],
                                   k[size_t(i * 3 + j + 1)]);
                }
            }
    }
}

} // namespace

PatternCatalog::PatternCatalog(ResourceIndex& index, const std::filesystem::path& resRoot) {
    const auto scripts = resRoot / "Catalog/Scripts";
    {
        std::ifstream in(resRoot.parent_path() / "Sys" / "Terrain.ini");
        std::stringstream text;
        text << in.rdbuf();
        water_ = LotWaterParams::parse(text.str());
    }
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
                ss >> arches_.back().style;
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
    {
        // fences.txt: the product flagged `default` (the picket fence 2) is
        // the one cTSCatalog::DefaultFenceProduct gives for a style with no
        // fence of its own.
        std::ifstream in(scripts / "fences.txt");
        std::optional<Fence> block;
        bool isDefault = false;
        auto close = [&] {
            if (block && isDefault)
                defaultFence_ = block;
            block.reset(), isDefault = false;
        };
        for (std::string line; std::getline(in, line);) {
            std::istringstream ss(line.substr(0, line.find('#')));
            std::string key, value;
            ss >> key;
            if (key == "fence") {
                close();
                block.emplace();
                continue;
            }
            if (!block)
                continue;
            std::getline(ss >> std::ws, value);
            std::erase(value, '"');
            while (!value.empty() && std::isspace(uint8_t(value.back())))
                value.pop_back();
            if (key == "railModel")
                block->rail = value;
            else if (key == "diagRailModel")
                block->diagonalRail = value;
            else if (key == "postModel")
                block->post = value;
            else if (key == "default")
                isDefault = true;
        }
        close();
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
        if (auto guid = guidOf(props)) {
            const std::string half = lowerAscii(get(props, "ishalfwall"));
            fences_[*guid] = {get(props, "rail"), get(props, "diagrail"), get(props, "post"),
                              half == "true" || half == "1"};
        }
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

const PatternCatalog::Fence* PatternCatalog::fenceOrDefault(uint32_t style) const {
    if (const Fence* f = fence(style))
        return f;
    return defaultFence_ ? &*defaultFence_ : nullptr;
}

bool PatternCatalog::isFenceArchStyle(uint32_t style) const {
    return std::any_of(arches_.begin(), arches_.end(), [&](const FenceArch& a) { return a.style == style; });
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

bool lotWallModeLevel(const LotViewOptions& view, int level) {
    return view.level == LotViewOptions::kAllLevels || level == view.level ||
           (level >= view.lowestCut && level < view.level);
}

bool storyOverGround(const Lot& lot, int level, int x, int y) {
    if (!lot.hasLevel(level) || !lot.hasLevel(level - 1))
        return false;
    if (x < 0 || y < 0 || x >= int(lot.width) || y >= int(lot.height))
        return true;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (std::abs(lot.elevation(level, x + i, y + j) - lot.elevation(level - 1, x + i, y + j) - kStoryHeight) >= 0.05f)
                return false;
    return true;
}

std::vector<bool> lotCutawayWalls(const Lot& lot, const LotViewOptions& view) {
    std::vector<bool> cut(lot.walls.size(), false);
    if (view.walls != LotViewOptions::Walls::Cutaway)
        return cut;
    // The eligible walls (the manager's whole set, +0x3c) and those not cut
    // yet (its working set, +0x84, which SelectRoomBlockers goes through).
    std::vector<bool> eligible(lot.walls.size(), false);
    for (size_t i = 0; i < lot.walls.size(); ++i) {
        const auto& w = lot.walls[i];
        const int level = w.a.level;
        // CollectWallsEligibleForCutaway: the level's walls whose partition
        // style allows cutaway (walls.txt mayCutAway: normal and attic
        // walls; foundations, pool sides and deck skirts say false, and
        // fences default to false as fencearches.txt documents; inferred for
        // fences.txt, which has no key), nTSWorld::PartitionStyleSupportsCutaway.
        if (!wallModeLevel(view, level) || (w.type != Lot::Normal && w.type != Lot::Attic))
            continue;
        float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        dx /= len, dy /= len;
        const float nx = dy, ny = -dx; // right of a -> b
        // KeepOnlyFrontFacingOutsideWalls (BV 0xc8c868, docs/ui-live-mac.md
        // 8c-8d) with the room of each face from the light's room map (3ARY
        // 0x0B), the one nTSSG::cWallManager follows (docs/buy-build-mac.md
        // 7.3: fenced yards are outside there): both faces inside (an inside
        // wall) always cut; one face outside (an outside wall) cut when that
        // face looks at the camera's position (the camera on the outside of
        // the wall's line: the game's 2D cross product < 0, strict); both
        // outside (a wall alone in the yard) never.
        const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
        const bool right = roomBeside(lot, level, mx, my, nx, ny, true) != 0;
        const bool left = roomBeside(lot, level, mx, my, -nx, -ny, true) != 0;
        eligible[i] = true;
        if (view.cinematics)
            continue; // Expose instead (below)
        if (right && left) {
            cut[i] = true;
        } else if (right != left) {
            const float s = right ? -1.0f : 1.0f; // the outside face's normal is s * (nx, ny)
            if (view.camera)
                cut[i] = s * (nx * ((*view.camera)[0] - w.a.x) + ny * ((*view.camera)[1] - w.a.y)) > 0;
            else // no camera: along the view direction (into the scene)
                cut[i] = s * (nx * view.viewDirection[0] + ny * view.viewDirection[1]) < -1e-3f;
        }
    }
    // SelectWallsBlockingPoints: the eligible walls of the view's level
    // between `points` and the camera (the locator or its opposite face).
    auto cutBlocking = [&](const std::vector<std::array<float, 3>>& points) {
        if (points.empty() || !view.camera || view.level == LotViewOptions::kAllLevels)
            return;
        const auto edges = lotWallsBetweenPointsAndCamera(lot, points, *view.camera);
        for (size_t i = 0; i < lot.walls.size(); ++i) {
            const auto& w = lot.walls[i];
            if (cut[i] || !eligible[i] || w.a.level != view.level)
                continue;
            // The wall's tile edges (walls run along whole tiles, straight or diagonal).
            int x = int(std::lround(w.a.x)), y = int(std::lround(w.a.y));
            const int bx = int(std::lround(w.b.x)), by = int(std::lround(w.b.y));
            const int sx = (bx > x) - (bx < x), sy = (by > y) - (by < y);
            for (int k = 0; k < 64 && (x != bx || y != by) && !cut[i]; ++k) {
                std::array<int, 4> e{x, y, x + sx, y + sy};
                if (std::make_pair(e[2], e[3]) < std::make_pair(e[0], e[1]))
                    e = {e[2], e[3], e[0], e[1]};
                cut[i] = std::binary_search(edges.begin(), edges.end(), e);
                x += sx, y += sy;
            }
        }
    };
    if (!view.cinematics) {
        // usarSim (KeepOnlyFrontFacingOutsideWalls with cutawayForSelectedSim).
        if (view.simPoint)
            cutBlocking({*view.simPoint});
        return cut;
    }
    if (!view.camera || view.level == LotViewOptions::kAllLevels)
        return cut;
    const std::array<float, 3> cam = *view.camera;
    // Expose (BV 0xc8de78). Targets outdoors (cutawayOutside, true in
    // globalProps.xml): SelectNearSemicircleBlockers(2.0) (BV 0xc8c300).
    constexpr float kSemicircle = 2.0f;
    for (const auto& t : view.exposeOutside) {
        const float vx = t[0] - cam[0], vy = t[1] - cam[1], vz = t[2] - cam[2];
        const float dist = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (dist <= 0)
            continue;
        const float dirx = vx / dist, diry = vy / dist, dirz = vz / dist;
        const int tx = int(std::floor(t[0])), ty = int(std::floor(t[1]));
        const int r = int(std::ceil(kSemicircle));
        const int x0 = std::clamp(tx - r, 0, int(lot.width)), x1 = std::clamp(tx + r, 0, int(lot.width));
        const int y0 = std::clamp(ty - r, 0, int(lot.height)), y1 = std::clamp(ty + r, 0, int(lot.height));
        std::vector<std::array<float, 3>> points;
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                // Only vertices touching no room (the tiles around it, inferred).
                bool room = false;
                for (int ax = x - 1; ax <= x && !room; ++ax)
                    for (int ay = y - 1; ay <= y && !room; ++ay)
                        if (ax >= 0 && ay >= 0 && ax < int(lot.width) && ay < int(lot.height))
                            for (const float qx : {0.25f, 0.75f})
                                for (const float qy : {0.25f, 0.75f})
                                    room = room || lotRoomAt(lot, view.level, float(ax) + qx, float(ay) + qy) != 0;
                if (room)
                    continue;
                const float pz = lot.elevationAt(view.level, float(x), float(y));
                const float ox = float(x) - t[0], oy = float(y) - t[1], oz = pz - t[2];
                if (std::sqrt(ox * ox + oy * oy + oz * oz) > kSemicircle)
                    continue;
                // The near half: not farther than the target along the view to it.
                if ((float(x) - cam[0]) * dirx + (float(y) - cam[1]) * diry + (pz - cam[2]) * dirz > dist)
                    continue;
                points.push_back({float(x), float(y), pz});
            }
        cutBlocking(points);
    }
    // SelectRoomBlockers (BV 0xc8bf10) for each remembered room: its walls
    // with the other face to the camera (and those with the room on both
    // faces), then what blocks them from the camera (their ends).
    for (const uint16_t room : view.exposeRooms) {
        std::vector<std::array<float, 3>> ends;
        for (size_t i = 0; i < lot.walls.size(); ++i) {
            const auto& w = lot.walls[i];
            if (!eligible[i] || cut[i] || w.a.level != view.level)
                continue;
            float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len == 0)
                continue;
            dx /= len, dy /= len;
            const float nx = dy, ny = -dx;
            const float mx = (w.a.x + w.b.x) / 2, my = (w.a.y + w.b.y) / 2;
            const bool right = roomBeside(lot, w.a.level, mx, my, nx, ny, true) == room;
            const bool left = roomBeside(lot, w.a.level, mx, my, -nx, -ny, true) == room;
            bool take = right && left;
            if (right != left) {
                const float s = right ? -1.0f : 1.0f; // the face out of the room
                take = s * (nx * (cam[0] - w.a.x) + ny * (cam[1] - w.a.y)) > 0;
            }
            if (!take)
                continue;
            cut[i] = true;
            ends.push_back({w.a.x, w.a.y, lot.elevationAt(w.a.level, w.a.x, w.a.y)});
            ends.push_back({w.b.x, w.b.y, lot.elevationAt(w.a.level, w.b.x, w.b.y)});
        }
        cutBlocking(ends);
    }
    return cut;
}

std::vector<std::array<int, 4>> lotWallsBetweenPointsAndCamera(const Lot& lot,
                                                               const std::vector<std::array<float, 3>>& points,
                                                               std::array<float, 3> camera) {
    const int w = int(lot.width), h = int(lot.height);
    std::vector<bool> marked(size_t(std::max(w, 0)) * size_t(std::max(h, 0)), false);
    for (const auto& p : points) {
        // From the point toward the camera, stopped 3.0 over the point (0x0274b48c).
        float d[3] = {camera[0] - p[0], camera[1] - p[1], camera[2] - p[2]};
        float e[3] = {camera[0], camera[1], camera[2]};
        if (d[2] > 3.0f) {
            const float f = 3.0f / d[2];
            for (int k = 0; k < 3; ++k)
                d[k] *= f, e[k] = p[k] + d[k];
        }
        const int signX = d[0] > 0 ? 1 : d[0] < 0 ? -1 : 0, signY = d[1] > 0 ? 1 : d[1] < 0 ? -1 : 0;
        // The point's tile, one back on each axis the camera is on the negative side of.
        int cur[2] = {int(std::floor(p[0])) - (signX < 0), int(std::floor(p[1])) - (signY < 0)};
        const int end[2] = {int(std::floor(e[0])), int(std::floor(e[1]))};
        const float ax = std::abs(d[0]), ay = std::abs(d[1]);
        const int major = ax > ay ? 0 : 1, minor = 1 - major;
        const float big = major == 0 ? ax : ay, small = major == 0 ? ay : ax;
        const int stepMajor = major == 0 ? signX : signY, stepMinor = major == 0 ? signY : signX;
        float err = small;
        // Until the major axis reaches the end's tile (which is not marked).
        for (int guard = 0; guard < 4 * (w + h) + 8; ++guard) {
            if (stepMajor > 0 ? !(end[major] > cur[major]) : !(end[major] < cur[major]))
                break;
            if (cur[0] >= 0 && cur[0] < w && cur[1] >= 0 && cur[1] < h)
                marked[size_t(cur[0]) * size_t(h) + size_t(cur[1])] = true;
            if (big > err)
                cur[major] += stepMajor, err += small;
            else
                cur[minor] += stepMinor, err -= big;
        }
    }
    // The marked tiles' walls: the diagonals always, the edge on the low x
    // (y) side when the tile across it is marked too or it is the lot's edge.
    std::vector<std::array<int, 4>> out;
    auto at = [&](int x, int y) { return marked[size_t(x) * size_t(h) + size_t(y)]; };
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) {
            if (!at(x, y))
                continue;
            out.push_back({x, y + 1, x + 1, y}); // mask 8
            out.push_back({x, y, x + 1, y + 1}); // mask 4
            if (x == 0 || at(x - 1, y))
                out.push_back({x, y, x, y + 1}); // mask 1
            if (y == 0 || at(x, y - 1))
                out.push_back({x, y, x + 1, y}); // mask 2
        }
    std::sort(out.begin(), out.end());
    return out;
}

uint16_t lotRoomAt(const Lot& lot, int level, float x, float y) {
    const auto it = lot.roomLayers.find(0x0B);
    const auto& rooms = it != lot.roomLayers.end() && it->second.size() == lot.rooms.size() ? it->second : lot.rooms;
    if (rooms.empty() || !lot.hasLevel(level) || x < 0 || y < 0 || x >= float(lot.width) || y >= float(lot.height))
        return 0;
    const uint32_t tx = uint32_t(x), ty = uint32_t(y);
    const auto& q = rooms[(size_t(level - lot.minLevel) * lot.width + tx) * lot.height + ty];
    // The quarters west, north, east, south (roomBeside's order): the one
    // the point's offset from the tile's centre leans to.
    const float fx = x - float(tx) - 0.5f, fy = y - float(ty) - 0.5f;
    const size_t quarter = std::abs(fx) > std::abs(fy) ? (fx < 0 ? 0 : 2) : (fy > 0 ? 1 : 3);
    return q[quarter];
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

// The sun's height map for the light maps (cHeightMapShadow,
// docs/lighting-mac.md 5.3; Mac BV: FillFromTerrain 0xd7661c, FillFromWalls
// 0xd76f40, FillFromRooves 0xd77b54). FillFromTerrain puts the highest floor
// over each texel in the grid (here: the upper levels' floors; the terrain
// is added when the light maps are made); FillFromRooves the roofs' height
// over their rectangles; FillFromWalls each wall's top at its two ends,
// drawn along the wall for the sun's direction (computeLotLightMap). A
// wall's top: the drawn one (wallTopElevation: on the top level a story
// over the floor, below it the next level's floor, attic walls the roof
// less 0.25, half walls 1.025 over their floor; that FillFromWalls takes the
// same is inferred; rails cast none, FenceStyleHasNoShadow).
void fillSunOccluders(const Lot& lot, const PatternCatalog& patterns, const LotRoofs& roofs, LotLightMapLayout& lm) {
    constexpr int k = LotLightMapLayout::kTexelsPerTile;
    const long w = long(lot.width) * k, h = long(lot.height) * k;
    lm.occluders.assign(size_t(w * h), -1e9f);
    lm.sunWalls.clear();
    auto raise = [&](long i, long j, float z) {
        if (i >= 0 && j >= 0 && i < w && j < h)
            lm.occluders[size_t(j * w + i)] = std::max(lm.occluders[size_t(j * w + i)], z);
    };
    for (const auto& wl : lot.walls) {
        if (wl.type == Lot::DeckSkirt)
            continue;
        if (!partitionIsWall(wl, patterns))
            continue; // rails
        const int level = wl.a.level;
        if (wl.a.x == wl.b.x && wl.a.y == wl.b.y)
            continue;
        // The wall's top as drawn (WallTopElevation, the node's span).
        const int span = wallNode(lot, wl, patterns).span;
        auto topAt = [&](float x, float y) { return wallTopElevation(lot, roofs, level, x, y, span); };
        LotLightMapLayout::SunWall sw;
        sw.a = {wl.a.x, wl.a.y};
        sw.b = {wl.b.x, wl.b.y};
        sw.topA = topAt(wl.a.x, wl.a.y);
        sw.topB = topAt(wl.b.x, wl.b.y);
        lm.sunWalls.push_back(sw);
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

float lotSkirtBottom(const Lot& lot) {
    const int w = int(lot.width), h = int(lot.height);
    float terrain = 0, under = 0;
    bool any = false;
    for (int x = 0; x <= w; ++x)
        for (int y = 0; y <= h; ++y) {
            const float e = lot.hasLevel(0) ? lot.elevation(0, x, y) : 0.0f;
            terrain = any ? std::min(terrain, e) : e;
            any = true;
            if (lot.minLevel < 0 && lot.hasLevel(lot.minLevel))
                under = std::min(under, lot.elevation(lot.minLevel, x, y));
        }
    return std::min(terrain - kSkirtDepth, under - 0.5f);
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
        // The lot's edges get sides of earth down to lotSkirtBottom: the
        // game's lot sits in the neighborhood's terrain (cLotSkirt), so
        // nothing under the ground ever shows past the lot's edge (a pool's
        // bottom and walls, the deck supports' 22-tile shafts, stairs Maxis
        // buried); the host clips what goes deeper (Renderer::setGroundClip).
        // A visual choice, inferred.
        if (lot.width && lot.height) {
            const int w = int(lot.width), h = int(lot.height);
            const float bottom = lotSkirtBottom(lot);
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
    // A lot without paints has no paint grid: four points a tile (inferred).
    const uint32_t mw = lot.maskWidth ? lot.maskWidth : lot.width * 4 + 1,
                   mh = lot.maskWidth ? lot.maskHeight : lot.height * 4 + 1;
    const float spacing = lot.width ? float(lot.width) / float(std::max(mw, 2u) - 1) : 1.0f;
    // The water's paints (lake bed and beach), after the lot's own (their
    // order among the terrain's paint layers is inferred).
    std::vector<Lot::TerrainPaint> paints = lot.paints;
    {
        LotWaterPaints wp = lotWaterPaints(lot, patterns.water(), mw, mh, spacing);
        for (auto& [texture, mask] : {std::pair{LotWaterPaints::kShallow, &wp.shallow},
                                      std::pair{LotWaterPaints::kDeep, &wp.deep},
                                      std::pair{LotWaterPaints::kBeach, &wp.beach}})
            paints.push_back({texture, std::move(*mask)});
    }
    for (const auto& paint : paints) {
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
    addFloors(lot, patterns, lm, built, model.warnings, view.level, view.lowestCut);
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
    addWater(lot, patterns.water(), built);
    model.materials["water:terrain"] = terrainWaterMaterial();
    model.materials["water:pool"] = poolWaterMaterial();
    for (int layer = 1; layer <= 3; ++layer)
        model.materials["water:poollayer" + std::to_string(layer)] = poolLayerMaterial();
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

    // The wall tops: CombinedWallMaterial("wall_top", "wallSolid_mask") with
    // no light-map page, which the WallpaperAndMask shader draws as 2 x the
    // wallpaper (docs/shaders-mac.md §5): an unlit material with wall_top's
    // texture and a diffuse of 2.
    if (std::any_of(model.parts.begin(), model.parts.end(),
                    [](const ObjectModel::Part& p) { return p.material == kWallTopMaterial; })) {
        std::string texture = "wall-top";
        try {
            if (auto loc = index.findByName("wall_top_txmt", kTypeTxmt)) {
                const auto bytes = index.read(*loc);
                Rcol rc(bytes);
                if (const auto* block = rc.find("cMaterialDefinition"))
                    if (auto t = MaterialDefinition::parse(block->body).property("stdMatBaseTextureName"); !t.empty())
                        texture = t;
            }
        } catch (const std::exception& e) {
            model.warnings.push_back(std::string("wall_top: ") + e.what());
        }
        MaterialDefinition m;
        m.materialType = "StandardMaterial";
        m.properties = {{"stdMatBaseTextureName", texture},
                        {"stdMatDiffCoef", "2,2,2,1"},
                        {"stdMatLightingEnabled", "false"},
                        {"stdMatSpecPower", "0"}};
        model.materials[kWallTopMaterial] = m;
    }
    if (std::any_of(model.parts.begin(), model.parts.end(),
                    [](const ObjectModel::Part& p) { return p.material == kFloorThicknessMaterial; }))
        model.materials[kFloorThicknessMaterial] = floorThicknessMaterial();
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
        fillSunOccluders(lot, patterns, roofs, lm);
        *lightMap = std::move(lm);
    }
    return model;
}

std::vector<LotFencePiece> lotFencePieces(const Lot& lot, const PatternCatalog& patterns,
                                          std::vector<std::string>* warnings) {
    std::vector<LotFencePiece> out;
    // SampledFloorElevation of a grid point (the ends, posts and arches all
    // stand on one).
    auto ground = [&](int level, float x, float y) {
        return lot.elevation(level, int(std::lround(x)), int(std::lround(y)));
    };
    auto raised = [&](int level, float x, float y) {
        return touchesRaisingStyle(lot, level, int(std::lround(x)), int(std::lround(y)));
    };
    // Rails (cFenceGeometryNode::AddRailImpl, Mac BV 0x759e38;
    // docs/roof-fence-water-mac.md §2.1): the rail model (a diagonal edge's
    // DiagRail, no straight one in its place) runs one edge along its +X,
    // translated to the start, turned by atan2(dy, dx) and sheared so its far
    // end sits on the floor there (z += dz / horizontal length * x), scale 1;
    // 0.75 higher when both ends touch the raising deck/foundation style. A
    // style without a fence of its own takes the default fence's models.
    for (const auto& w : lot.walls) {
        if (!Lot::isFence(w.type))
            continue;
        const auto* fence = patterns.fenceOrDefault(w.type);
        if (!fence)
            continue;
        const float dx = w.b.x - w.a.x, dy = w.b.y - w.a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len == 0)
            continue;
        const bool diagonal = std::abs(dx) > 0.5f && std::abs(dy) > 0.5f; // the locator's direction bits 0x30
        const std::string& model = diagonal ? fence->diagonalRail : fence->rail;
        if (model.empty())
            continue;
        const float z0 = ground(w.a.level, w.a.x, w.a.y), z1 = ground(w.b.level, w.b.x, w.b.y);
        const float c = dx / len, s = dy / len, shear = (z1 - z0) / len;
        const float lift = bothEndsRaised(lot, w) ? kRaisedByStyle : 0.0f;
        out.push_back({model, {c, s, shear, 0, -s, c, 0, 0, 0, 0, 1, 0, w.a.x, w.a.y, z0 + lift, 1}, w.a.level,
                       patterns.isHalfWall(w.type)});
    }
    // Posts (AddPostImpl, 0x75a4c0), from the lot's post layer: the style's
    // post at the grid point, not turned, scale 1, 0.75 higher when that one
    // point touches the raising style and, for a half wall's post, a half
    // wall of the same style ends there (FUN_00a7098c: graph 0xb, the half
    // walls). An arch's posts are drawn with the arch below.
    size_t unknown = 0;
    for (const auto& p : lot.fencePosts) {
        if (patterns.isFenceArchStyle(p.fence))
            continue;
        if (!patterns.fence(p.fence))
            ++unknown;
        const auto* fence = patterns.fenceOrDefault(p.fence);
        if (!fence || fence->post.empty())
            continue; // (the rock walls have no post)
        const bool half = patterns.isHalfWall(p.fence);
        auto halfWallThere = [&] {
            return std::any_of(lot.walls.begin(), lot.walls.end(), [&](const Lot::Wall& w) {
                return w.type == p.fence && w.a.level == p.level &&
                       ((w.a.x == p.x && w.a.y == p.y) || (w.b.x == p.x && w.b.y == p.y));
            });
        };
        const float lift = raised(p.level, p.x, p.y) && (!half || halfWallThere()) ? kRaisedByStyle : 0.0f;
        out.push_back({fence->post, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p.x, p.y, ground(p.level, p.x, p.y) + lift, 1},
                       p.level, half});
    }
    if (unknown && warnings)
        warnings->push_back(std::to_string(unknown) + " fence posts of unknown fences (drawn as the default fence)");
    // Fence arches (graph 0x13; AddArchCenterImpl, 0x75b8a8): the centre at
    // the start, turned toward the end, not sheared; the west part at the
    // start unless another arch goes on from there along the line (a - d,
    // cPartitionManager::FenceArchExists), the east part at the end unless
    // one goes on past it (b + d); both turned with the arch; no +0.75. The
    // posts: the game takes them from the post layer (an arch-style post,
    // part 3, not turned); here one at each end (inferred: the app's Build
    // mode writes no arch posts).
    if (const auto* arch = patterns.fenceArch()) {
        const auto arches = fenceArches(lot);
        auto archAt = [&](int level, int ax, int ay, int bx, int by) {
            return std::any_of(arches.begin(), arches.end(), [&](const auto& f) {
                return f.level == level && ((f.ax == ax && f.ay == ay && f.bx == bx && f.by == by) ||
                                            (f.ax == bx && f.ay == by && f.bx == ax && f.by == ay));
            });
        };
        for (const auto& f : arches) {
            const float ax = float(f.ax), ay = float(f.ay), bx = float(f.bx), by = float(f.by);
            const float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
            if (len == 0)
                continue;
            const float c = dx / len, s = dy / len, za = ground(f.level, ax, ay), zb = ground(f.level, bx, by);
            auto put = [&](const std::string& model, float x, float y, float z, bool turned = true) {
                if (!model.empty())
                    out.push_back({model,
                                   turned ? std::array<float, 16>{c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, x, y, z, 1}
                                          : std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1},
                                   f.level});
            };
            const int ddx = f.bx - f.ax, ddy = f.by - f.ay;
            put(arch->center, ax, ay, za);
            put(arch->post, ax, ay, za, false);
            put(arch->post, bx, by, zb, false);
            if (!archAt(f.level, f.ax, f.ay, f.ax - ddx, f.ay - ddy))
                put(arch->west, ax, ay, za);
            if (!archAt(f.level, f.bx, f.by, f.bx + ddx, f.by + ddy))
                put(arch->east, bx, by, zb);
        }
    }
    return out;
}

} // namespace os2
