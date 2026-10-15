// Neighborhood view data (formats/hood_terrain.hpp, hood_scene.hpp) on
// hand-built resources: terrain, occupants, the ini, paint weights.
#include "formats/hood_scene.hpp"
#include "formats/hood_terrain.hpp"
#include "formats/lighting_config.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace os2;
using namespace os2::hood;

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Writer {
    std::vector<uint8_t> b;
    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& u16(uint16_t v) { return u8(uint8_t(v)).u8(uint8_t(v >> 8)); }
    Writer& u32(uint32_t v) { return u16(uint16_t(v)).u16(uint16_t(v >> 16)); }
    Writer& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Writer& str32(const std::string& s) { u32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); return *this; }
};

// A 2x2-cell terrain: heights 0..8 row by row, sea level 1.5.
static std::vector<uint8_t> terrain(bool withBytes) {
    Writer w;
    w.u32(hoodtype::Terrain).u32(4).u32(2).u32(2).f32(1.5f).str32("Temperate");
    w.u32(0x6B943B43).u32(1).str32("c2DArray").u32(3).u32(3);
    for (int i = 0; i < 9; ++i)
        w.f32(float(i));
    if (withBytes) {
        w.u32(0x6B943B43).u32(1).str32("c2DArray").u32(3).u32(3);
        for (int i = 0; i < 9; ++i)
            w.u8(0);
    }
    return w.b;
}

static void position(Writer& w, float x, float y, float z) {
    w.u8(2).f32(x).f32(y).f32(z).f32(0).f32(0).f32(1).f32(1);
}

static void testTerrain() {
    const Terrain t = Terrain::parse(terrain(true));
    check(t.width == 2 && t.gridWidth == 3 && t.heights.size() == 9, "terrain size");
    check(t.type == "Temperate" && t.seaLevel == 1.5f && t.vertexBytes.size() == 9, "terrain header");
    check(t.vertex(2, 1) == 5 && t.vertex(9, 9) == 8, "terrain vertex (clamped)");
    // GetTerrainHeight: tile split along (0,0)-(1,1); fy < fx uses h10.
    check(std::abs(t.heightAt(5, 0) - 0.5f) < 1e-5f, "height along x");
    check(std::abs(t.heightAt(0, 5) - 1.5f) < 1e-5f, "height along y");
    check(std::abs(t.heightAt(5, 5) - 2.0f) < 1e-5f, "height on the diagonal");
    check(t.heightAt(-1, 5) == 0, "height outside");
    check(Terrain::parse(terrain(false)).vertexBytes.empty(), "terrain without the byte layer");
}

static void testOccupants() {
    Writer w;
    w.u32(111);
    w.u16(8).u32(1); // flora
    position(w, 1, 2, 3);
    w.u8(8).f32(45).u32(0x41);
    w.u16(3).u32(1); // roads
    position(w, 15, 15, 0);
    w.u8(3);
    for (int k = 0; k < 4; ++k)
        w.f32(float(10 + 10 * (k & 1))).f32(float(10 + 10 * (k >> 1))).f32(0).f32(float(k & 1)).f32(float(k >> 1));
    w.u32(0x4b00).u32(0).u8(1).u8(5).u32(0);
    w.u16(3).u32(0); // bridges
    w.u16(8).u32(1); // props
    position(w, 4, 5, 6);
    w.u8(8).u32(0x0C).f32(90);
    const Occupants o = Occupants::parse(w.b);
    check(o.flora.size() == 1 && o.flora[0].guid == 0x41 && o.flora[0].rotation == 45, "flora");
    check(o.flora[0].at.position[2] == 3, "flora position");
    check(o.roads.size() == 1 && o.roads[0].piece == 0x4b00 && o.roads[0].uvs[3][0] == 1, "road");
    check(o.props.size() == 1 && o.props[0].guid == 0x0C && o.props[0].rotation == 90, "prop");
}

static void testSettings() {
    const std::string ini = "[PaintInfos]\n0x10=1.0,nh-test-cliff,1, terrain-cliff, 0,1, 24,0\n"
                            "0x11=1.0,nh-test-beach,1, terrain-beach, 0,1, 24,0\n"
                            "0x01=0.9,nh-temperate-wet,2, lot, 0,1, 26,0 ; comment\n"
                            "[TerrainTypeInfos]\nTemperate=0x10,0x11\n"
                            "[PaintMapTable_Temperate]\n00=0x01,0x01,\n01=0x01,0x01,\n"
                            "[NHoodTerrainTunables]\nCliffAngleMin=52\nMaxRoadProximityValue=20\n";
    const TerrainSettings s = TerrainSettings::parse(ini, "[ImposterDisplayTunables]\nImposterZOffset=0.5\n");
    check(s.paints.size() == 3 && s.paints.at(1).variants == 2 && s.paints.at(1).maxStrength == 0.9f, "paint infos");
    check(s.paints.at(1).variantTexture(1) == "nh-temperate-wet-01_txtr", "variant texture");
    check(s.paints.at(0x10).variantTexture(0) == "nh-test-cliff_txtr", "single variant texture");
    check(std::abs(s.paints.at(1).spreadAt(13) - 0.5f) < 1e-5f, "spread curve");
    check(s.types.at("Temperate").beach == 0x11, "terrain types");
    check(s.paintMaps.at("Temperate").size() == 2 && s.paintMaps.at("Temperate")[1].size() == 2, "paint map");
    check(s.imposterZOffset == 0.5f, "imposter offset");

    // Weights on the flat 2x2 terrain far above the sea: one paint, weight 1.
    Terrain t = Terrain::parse(terrain(true));
    t.heights.assign(9, 10.0f);
    t.seaLevel = 0;
    const PaintWeights pw = computePaintWeights(t, Occupants{}, s);
    check(pw.paints.size() == 1 && pw.paints[0] == 1, "one paint");
    // NormalizePaintWeights: weight x 250 / sum, so one paint alone is 250.
    check(pw.alpha[0][4] == 250, "alpha byte of a lone paint");

    // The terrain model: a black canvas, then the paint's part, added, with
    // its alpha map one value per vertex at the tile's middle.
    const TerrainVariants variants = TerrainVariants::make(t, s, 7);
    const ObjectModel m = buildHoodTerrainModel(t, Occupants{}, s, variants);
    check(m.parts.size() >= 2 && m.parts[0].shape == "canvas" && m.parts[0].mesh.triangles.size() == 4 * 6,
          "canvas part over the 4 tiles");
    size_t paintTris = 0;
    for (size_t i = 1; i < m.parts.size(); ++i)
        paintTris += m.parts[i].mesh.triangles.size();
    check(paintTris == 4 * 6, "each tile drawn once per paint (one variant each)");
    check(m.masks.size() == 1 && m.masks[0].width == 3 && m.masks[0].spacing == 10 && m.masks[0].originX == 5,
          "alpha map: a value per vertex, at the tile's middle");
    check(m.materials.at(m.parts[1].material).property("stdMatAlphaBlendMode") == "additive",
          "the paint is added over the canvas (srcAlpha + one)");
}

// The game's random numbers and the texture variant cells (InitPaintVariants).
static void testVariants() {
    GameRandom r{12345};
    const uint32_t a = r.next(256), b = r.next(256), c = r.next(256), d = r.next(256);
    check(a == 12 && b == 254 && c == 110 && d == 192, "FUN_00171674 sequence");
    check(GameRandom{1}.next(0) == 0, "next(0) is 0");
    Terrain t;
    t.width = t.height = 128;
    t.gridWidth = t.gridHeight = 129;
    TerrainSettings s;
    s.textureVariantGridSize = 13;
    const TerrainVariants v = TerrainVariants::make(t, s, 12345);
    check(v.cellsX == 10 && v.cellsY == 10 && v.bytes.size() == 100 && v.bytes[0] == 12 && v.bytes[1] == 254,
          "a byte per 13-tile cell, row by row");
    check(v.variant(12, 0, 2) == 0 && v.variant(13, 0, 2) == 0 && v.variant(13, 0, 3) == 254 % 3,
          "a tile's variant is its cell's byte mod the variants");
    check(v.variant(0, 0, 1) == 0, "one texture: variant 0");
    check(TerrainSettings{}.textureVariantGridSize == 4, "grid 4 without the ini key");
}

// cTSNHoodTerrain's normals: the six faces round a vertex.
static void testNormals() {
    Terrain t = Terrain::parse(terrain(true));
    t.heights.assign(9, 3.0f);
    auto n = terrainNormals(t);
    check(n.size() == 9 && std::abs(n[4][2] - 1) < 1e-6f, "flat: straight up");
    // A slope along x: 1 unit up per tile.
    for (size_t j = 0; j < 3; ++j)
        for (size_t i = 0; i < 3; ++i)
            t.heights[j * 3 + i] = float(i);
    n = terrainNormals(t);
    const float e = 1 / std::sqrt(1.0f + 100.0f);
    check(std::abs(n[4][0] + e) < 1e-5f && std::abs(n[4][1]) < 1e-5f, "slope: tilted against x");
}

// The "Ver Amigos" arch: a tube round a Bezier from one lot to the other,
// rising at 45 degrees (c3DUIManager::AddArch).
static void testRelationshipArch() {
    const auto m = os2::hood::relationshipArchMesh({0, 0, 10}, {100, 0, 10});
    check(m.positions.size() == 32 * 8 && m.triangles.size() == 31 * 7 * 6, "32 rings of 8, closed round");
    float top = -1e9f, first = 0, last = 0;
    for (size_t i = 0; i < 8; ++i)
        first += m.positions[i][0] / 8, last += m.positions[m.positions.size() - 1 - i][0] / 8;
    for (const auto& p : m.positions)
        top = std::max(top, p[2]);
    // Control points at 10 + tan(45) x 0.25 x 100 = 35: the curve's middle at
    // 10 + 0.75 x 25 = 28.75, the tube's top one radius above.
    check(std::fabs(top - 29.75f) < 0.1f, "the arch's top: 3/4 of the control height, plus the radius");
    check(std::fabs(first) < 0.2f && std::fabs(last - 100) < 0.2f, "it runs from one end to the other");
}

int main() {
    testRelationshipArch();
    testTerrain();
    testOccupants();
    testSettings();
    testVariants();
    testNormals();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("hood_terrain: all tests passed\n");
    return failures ? 1 : 0;
}
