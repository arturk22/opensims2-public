#include "hood_terrain.hpp"

#include "binreader.hpp"
#include "config_text.hpp"
#include "property_set.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace os2::hood {

namespace {

std::string str32(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > r.remaining())
        throw FormatError("bad string length " + std::to_string(n));
    auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), n);
}

std::array<float, 3> vec3(BinReader& r) {
    std::array<float, 3> v;
    for (auto& x : v)
        x = r.f32();
    return v;
}

std::array<float, 2> vec2(BinReader& r) {
    return {r.f32(), r.f32()};
}

OccupantPosition position(BinReader& r) {
    OccupantPosition p;
    r.u8(); // version (< 2: no Y bounds, OpenTS2)
    p.position = vec3(r);
    p.boundsMin = vec2(r);
    p.boundsMax = vec2(r);
    return p;
}

Road road(BinReader& r) {
    Road road;
    road.at = position(r);
    const uint8_t v = r.u8();
    if (v < 3)
        throw FormatError("road: unsupported version " + std::to_string(v));
    for (size_t k = 0; k < 4; ++k) {
        road.corners[k] = vec3(r);
        road.uvs[k] = vec2(r);
    }
    road.piece = r.u32();
    road.underTexture = r.u32();
    road.flags = r.u8();
    road.connections = r.u8();
    const uint32_t addons = r.u32();
    if (addons > r.remaining() / 24)
        throw FormatError("road: bad addon count");
    for (uint32_t i = 0; i < addons; ++i) {
        Road::Addon a;
        a.id = r.u32();
        a.a = vec3(r);
        a.b = vec2(r);
        road.addons.push_back(a);
    }
    return road;
}

uint32_t count(BinReader& r, size_t minBytes, const char* what) {
    const uint32_t n = r.u32();
    if (n > r.remaining() / minBytes)
        throw FormatError(std::string(what) + ": bad count " + std::to_string(n));
    return n;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(uint8_t(s[a])))
        ++a;
    while (b > a && std::isspace(uint8_t(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> splitCommas(std::string_view s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string_view::npos)
            c = s.size();
        out.push_back(trim(s.substr(pos, c - pos)));
        pos = c + 1;
    }
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

float number(const std::string& s, float fallback) {
    try {
        size_t used = 0;
        const float v = std::stof(s, &used);
        return used ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

uint32_t hexOrDec(const std::string& s) {
    try {
        return uint32_t(std::stoul(s, nullptr, 0));
    } catch (...) {
        return 0;
    }
}

// INI sections: name -> ordered (key, value) pairs; ';' starts a comment.
using Ini = std::map<std::string, std::vector<std::pair<std::string, std::string>>>;
Ini parseIni(const std::string& text) {
    Ini ini;
    std::string section;
    forEachConfigLine(text, [&](std::string_view raw) {
        std::string line = trim(raw.substr(0, raw.find(';')));
        if (line.empty())
            return;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            return;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return;
        ini[section].emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    });
    return ini;
}

const std::string* iniValue(const Ini& ini, const std::string& section, const std::string& key) {
    auto s = ini.find(section);
    if (s == ini.end())
        return nullptr;
    for (const auto& [k, v] : s->second)
        if (k == key)
            return &v;
    return nullptr;
}

void setNumber(const Ini& ini, const std::string& section, const std::string& key, float& out) {
    if (const auto* v = iniValue(ini, section, key))
        out = number(*v, out);
}

} // namespace

Terrain Terrain::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Terrain t;
    if (r.u32() != hoodtype::Terrain)
        throw FormatError("neighborhood terrain: bad class id");
    t.version = r.u32();
    if (t.version <= 2)
        throw FormatError("neighborhood terrain: version " + std::to_string(t.version) + " has no heights");
    t.width = r.u32();
    t.height = r.u32();
    t.seaLevel = r.f32();
    t.type = str32(r);
    // c2DArray (0x6B943B43, version 1): width, height, floats x fastest.
    if (r.u32() != 0x6B943B43)
        throw FormatError("neighborhood terrain: bad height array id");
    if (r.u32() != 1)
        throw FormatError("neighborhood terrain: bad height array version");
    if (str32(r) != "c2DArray")
        throw FormatError("neighborhood terrain: bad height array name");
    t.gridWidth = r.u32();
    t.gridHeight = r.u32();
    if (uint64_t(t.gridWidth) * t.gridHeight * 4 > r.remaining())
        throw FormatError("neighborhood terrain: height array too large");
    t.heights.resize(size_t(t.gridWidth) * t.gridHeight);
    for (auto& h : t.heights)
        h = r.f32();
    // Version 4: a second c2DArray, of one byte per vertex (all zero in the
    // base game's neighborhoods; meaning unknown).
    if (!r.atEnd()) {
        if (r.u32() != 0x6B943B43 || r.u32() != 1 || str32(r) != "c2DArray")
            throw FormatError("neighborhood terrain: bad second array");
        const uint32_t w = r.u32(), h = r.u32();
        if (uint64_t(w) * h != r.remaining())
            throw FormatError("neighborhood terrain: bad second array size");
        auto b = r.bytes(r.remaining());
        t.vertexBytes.assign(b.begin(), b.end());
    }
    r.expectEnd("neighborhood terrain");
    return t;
}

float Terrain::vertex(int i, int j) const {
    if (heights.empty())
        return 0;
    i = std::clamp(i, 0, int(gridWidth) - 1);
    j = std::clamp(j, 0, int(gridHeight) - 1);
    return heights[size_t(j) * gridWidth + size_t(i)];
}

float Terrain::heightAt(float x, float y) const {
    if (x < 0 || y < 0 || x > worldWidth() || y > worldHeight() || heights.empty())
        return 0;
    const int i = std::min(int(x / kHoodCellSize), int(width) - 1), j = std::min(int(y / kHoodCellSize), int(height) - 1);
    const float fx = x - float(i) * kHoodCellSize, fy = y - float(j) * kHoodCellSize;
    const float h00 = vertex(i, j), h10 = vertex(i + 1, j), h01 = vertex(i, j + 1), h11 = vertex(i + 1, j + 1);
    if (fy < fx)
        return h00 + (fx * (h10 - h00) + fy * (h11 - h10)) / kHoodCellSize;
    return h00 + (fy * (h01 - h00) + fx * (h11 - h01)) / kHoodCellSize;
}

Occupants Occupants::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Occupants o;
    o.version = r.u32();
    if (o.version < 108)
        throw FormatError("neighborhood occupants: unsupported version " + std::to_string(o.version));
    // Trees and other flora.
    o.floraVersion = r.u16();
    if (o.floraVersion <= 7)
        throw FormatError("flora: unsupported version");
    for (uint32_t i = 0, n = count(r, 30, "flora"); i < n; ++i) {
        Flora f;
        f.at = position(r);
        if (r.u8() <= 7)
            throw FormatError("flora: unsupported object version");
        f.rotation = r.f32();
        f.guid = r.u32();
        o.flora.push_back(f);
    }
    o.roadVersion = r.u16();
    if (o.roadVersion <= 2)
        throw FormatError("roads: unsupported version");
    for (uint32_t i = 0, n = count(r, 30, "roads"); i < n; ++i)
        o.roads.push_back(road(r));
    o.bridgeVersion = r.u16();
    if (o.bridgeVersion <= 2)
        throw FormatError("bridges: unsupported version");
    for (uint32_t i = 0, n = count(r, 30, "bridges"); i < n; ++i) {
        Bridge b;
        b.road = road(r);
        const uint8_t v = r.u8();
        b.offset = vec3(r);
        for (auto& q : b.orientation)
            q = r.f32();
        b.unknown = vec3(r);
        if (v < 2) {
            b.offset[0] *= 1.25f, b.offset[1] *= 1.25f;
            b.unknown[0] *= 1.25f, b.unknown[1] *= 1.25f;
        }
        o.bridges.push_back(b);
    }
    o.propVersion = r.u16();
    if (o.propVersion < 7)
        throw FormatError("props: unsupported version");
    for (uint32_t i = 0, n = count(r, 30, "props"); i < n; ++i) {
        Prop p;
        p.at = position(r);
        if (r.u8() <= 7)
            throw FormatError("props: unsupported object version");
        p.guid = r.u32();
        p.rotation = r.f32();
        o.props.push_back(p);
    }
    r.expectEnd("neighborhood occupants");
    return o;
}

std::map<uint32_t, std::string> loadNeighborhoodObjects(ResourceIndex& index) {
    std::map<uint32_t, std::string> out;
    for (const auto& loc : index.ofType(hoodtype::ObjectEntry)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto g = props.find("guid");
        auto m = props.find("modelname");
        if (g == props.end() || m == props.end() || m->second.empty())
            continue;
        std::string model = m->second;
        // "the game just tacks on a _cres" (OpenTS2).
        if (model.size() < 5 || model.compare(model.size() - 5, 5, "_cres") != 0)
            model += "_cres";
        out[uint32_t(std::stoul(g->second))] = model;
    }
    return out;
}

bool SavedCamera::parse(std::span<const uint8_t> data, SavedCamera& out) {
    if (data.size() < 0x65)
        return false;
    BinReader r(data);
    r.seek(0x48);
    out.distance = r.f32();
    r.seek(0x51);
    out.heading = r.f32();
    out.pitch = r.f32();
    out.pivot = vec3(r);
    return std::isfinite(out.distance) && out.distance > 0 && std::isfinite(out.pivot[0]) && std::isfinite(out.pivot[1]);
}

float TerrainSettings::Paint::spreadAt(float d) const {
    if (spread.empty())
        return d <= 0 ? 1.0f : 0.0f;
    if (d <= spread.front()[0])
        return spread.front()[1];
    for (size_t i = 1; i < spread.size(); ++i)
        if (d <= spread[i][0]) {
            const float span = spread[i][0] - spread[i - 1][0];
            const float t = span > 0 ? (d - spread[i - 1][0]) / span : 1.0f;
            return spread[i - 1][1] + (spread[i][1] - spread[i - 1][1]) * t;
        }
    return spread.back()[1];
}

std::string TerrainSettings::Paint::variantTexture(int v) const {
    if (variants < 2) // InitPaintDisplay: "%s-%02d" only with 2 or more variants
        return texture + "_txtr";
    char buf[8];
    std::snprintf(buf, sizeof buf, "-%02d", v);
    return texture + buf + "_txtr";
}

TerrainSettings TerrainSettings::parse(const std::string& terrainIni, const std::string& neighborhoodIni,
                                       const std::string& lotTerrainIni) {
    TerrainSettings s;
    const Ini ini = parseIni(terrainIni);
    if (auto it = ini.find("PaintInfos"); it != ini.end())
        for (const auto& [key, value] : it->second) {
            const auto f = splitCommas(value);
            if (f.size() < 4)
                continue;
            Paint p;
            p.maxStrength = number(f[0], 1);
            p.texture = f[1];
            p.variants = std::max(1, int(number(f[2], 1)));
            p.lotPaint = f[3];
            for (size_t i = 4; i + 1 < f.size(); i += 2)
                p.spread.push_back({number(f[i], 0), number(f[i + 1], 0)});
            s.paints[hexOrDec(key)] = p;
        }
    if (auto it = ini.find("TerrainTypeInfos"); it != ini.end())
        for (const auto& [key, value] : it->second) {
            const auto f = splitCommas(value);
            if (f.size() >= 2)
                s.types[key] = {hexOrDec(f[0]), hexOrDec(f[1])};
        }
    for (const auto& [section, rows] : ini) {
        static const std::string prefix = "PaintMapTable_";
        if (section.rfind(prefix, 0) != 0)
            continue;
        auto& table = s.paintMaps[section.substr(prefix.size())];
        for (const auto& [key, value] : rows) {
            const size_t row = size_t(number(key, -1));
            if (row > 255)
                continue;
            if (table.size() <= row)
                table.resize(row + 1);
            for (const auto& c : splitCommas(value))
                table[row].push_back(hexOrDec(c));
        }
    }
    const std::string t = "NHoodTerrainTunables";
    setNumber(ini, t, "MaxBeachAltitude", s.maxBeachAltitude);
    setNumber(ini, t, "MaxBeachWidthInCells", s.maxBeachWidthInCells);
    setNumber(ini, t, "HeightRangeForTemperatureDistribution", s.heightRangeForTemperature);
    setNumber(ini, t, "CliffAngleMin", s.cliffAngleMin);
    setNumber(ini, t, "CliffAngleMax", s.cliffAngleMax);
    setNumber(ini, t, "WaterDepthThresholdForPaint", s.waterDepthThresholdForPaint);
    setNumber(ini, t, "MaxRoadProximityValue", s.maxRoadProximity);
    setNumber(ini, "NHoodTerrainRenderTunables", "TextureVariantGridSize", s.textureVariantGridSize);
    if (!neighborhoodIni.empty())
        setNumber(parseIni(neighborhoodIni), "ImposterDisplayTunables", "ImposterZOffset", s.imposterZOffset);
    if (!lotTerrainIni.empty()) {
        const Ini lot = parseIni(lotTerrainIni);
        if (const auto* v = iniValue(lot, "WaterRenderParameters", "AlphaVsDepthCurve")) {
            const auto f = splitCommas(*v);
            for (size_t i = 0; i + 1 < f.size(); i += 2)
                s.waterAlphaVsDepth.push_back({number(f[i], 0), number(f[i + 1], 0)});
        }
    }
    return s;
}

TerrainSettings TerrainSettings::load(const std::filesystem::path& sysDir) {
    auto read = [&](const char* name) {
        const auto p = sysDir / name;
        return std::filesystem::exists(p) ? readTextFile(p) : std::string();
    };
    const std::string terrain = read("NeighborhoodTerrain.ini");
    if (terrain.empty())
        throw std::runtime_error("no NeighborhoodTerrain.ini in " + sysDir.string());
    return parse(terrain, read("Neighborhood.ini"), read("Terrain.ini"));
}

} // namespace os2::hood
