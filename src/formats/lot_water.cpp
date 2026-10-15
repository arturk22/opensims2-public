// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "lot_water.hpp"

#include "lot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace os2 {

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r");
    const auto e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

} // namespace

// FUN_00c42638 (BV): reads [WaterRenderParameters] of Terrain.ini, each value
// falling back to the binary's default.
LotWaterParams LotWaterParams::parse(const std::string& terrainIni) {
    LotWaterParams p;
    std::istringstream in(terrainIni);
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (const auto c = line.find(';'); c != std::string::npos)
            line.resize(c);
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        const auto eq = line.find('=');
        if (section != "WaterRenderParameters" || eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        const float v = std::strtof(value.c_str(), nullptr);
        if (key == "AlphaVsDepthCurve") {
            std::vector<float> f;
            std::string t = value;
            std::replace(t.begin(), t.end(), ',', ' ');
            std::istringstream ns(t);
            for (float x; ns >> x;)
                f.push_back(x);
            if (f.size() >= 2) {
                p.alphaVsDepth.clear();
                for (size_t i = 0; i + 1 < f.size(); i += 2)
                    p.alphaVsDepth.push_back({f[i], f[i + 1]});
            }
        } else if (key == "ShallowPaintDepthMin") {
            p.shallowMin = v;
        } else if (key == "ShallowPaintDepthMax") {
            p.shallowMax = v;
        } else if (key == "DeepPaintDepthMin") {
            p.deepMin = v;
        } else if (key == "DeepPaintDepthMax") {
            p.deepMax = v;
        } else if (key == "BeachPaintStartDepth") {
            p.beachStart = v;
        } else if (key == "BeachPaintPeakDepth") {
            p.beachPeak = v;
        } else if (key == "BeachPaintEndDepth") {
            p.beachEnd = v;
        } else if (key == "MaxBeachWidth") {
            p.maxBeachWidth = v;
        } else if (key == "BeachIntensityFactor") {
            p.beachIntensity = v;
        }
    }
    if (p.maxBeachWidth <= 0)
        p.maxBeachWidth = 0.8f;
    return p;
}

float LotWaterParams::alpha(float depth) const {
    if (alphaVsDepth.empty())
        return 255;
    if (depth <= alphaVsDepth.front()[0])
        return alphaVsDepth.front()[1];
    for (size_t i = 1; i < alphaVsDepth.size(); ++i)
        if (depth <= alphaVsDepth[i][0]) {
            const auto& a = alphaVsDepth[i - 1];
            const auto& b = alphaVsDepth[i];
            const float t = b[0] > a[0] ? (depth - a[0]) / (b[0] - a[0]) : 1.0f;
            return a[1] + (b[1] - a[1]) * t;
        }
    return alphaVsDepth.back()[1];
}

float lotWaterDepth(const Lot& lot, int x, int y) {
    const float w = lot.waterLevel(x, y);
    if (!std::isfinite(w))
        return -std::numeric_limits<float>::infinity();
    return w - lot.elevation(0, x, y);
}

// cWaterGeometryBuilder::PaintUnderWaterTerrain (BV FUN_00c44670): each paint
// texel takes the depth interpolated between its tile's four corners, and
// - the shallow lake bed rises from 0 at ShallowPaintDepthMin to 254 at
//   DeepPaintDepthMin and falls back to 0 at ShallowPaintDepthMax;
// - the deep lake bed rises from DeepPaintDepthMin to 254 at
//   DeepPaintDepthMax, 255 past it;
// - the beach rises from BeachPaintStartDepth to 255 at BeachPaintPeakDepth
//   and falls to 0 at BeachPaintEndDepth; above the water (depth < 0) it is
//   also scaled by 1 - d / MaxBeachWidth (d: distance to the nearest grid
//   point under water, none farther than that) and BeachIntensityFactor.
// The game paints the tiles whose depths changed (FUN_00c44be4), which on a
// loaded lot is every tile with water data.
LotWaterPaints lotWaterPaints(const Lot& lot, const LotWaterParams& p, uint32_t mw, uint32_t mh, float spacing) {
    LotWaterPaints out;
    const size_t n = size_t(mw) * mh;
    out.shallow.assign(n, 0), out.deep.assign(n, 0), out.beach.assign(n, 0);
    const int w = int(lot.width), h = int(lot.height);
    if (lot.water.size() != size_t(w + 1) * size_t(h + 1) || mw == 0 || mh == 0 || spacing <= 0)
        return out;
    std::vector<float> depth(size_t(w + 1) * size_t(h + 1));
    std::vector<std::array<float, 2>> wet; // grid points under water
    for (int y = 0; y <= h; ++y)
        for (int x = 0; x <= w; ++x) {
            const float d = lotWaterDepth(lot, x, y);
            depth[size_t(y) * size_t(w + 1) + size_t(x)] = d;
            if (d > 0)
                wet.push_back({float(x), float(y)});
        }
    if (wet.empty())
        return out;
    const float kShallowUp = 254.0f / (p.deepMin - p.shallowMin), kShallowDown = 254.0f / (p.shallowMax - p.deepMin),
                kDeep = 254.0f / (p.deepMax - p.deepMin), kBeachUp = 255.0f / (p.beachPeak - p.beachStart),
                kBeachDown = 255.0f / (p.beachEnd - p.beachPeak);
    auto at = [&](int x, int y) { return depth[size_t(y) * size_t(w + 1) + size_t(x)]; };
    for (uint32_t j = 0; j < mh; ++j)
        for (uint32_t i = 0; i < mw; ++i) {
            const float px = float(i) * spacing, py = float(j) * spacing;
            const int tx = std::clamp(int(std::floor(px)), 0, std::max(w - 1, 0)),
                      ty = std::clamp(int(std::floor(py)), 0, std::max(h - 1, 0));
            const float u = std::clamp(px - float(tx), 0.0f, 1.0f), v = std::clamp(py - float(ty), 0.0f, 1.0f);
            const float d = ((1 - u) * at(tx, ty) + u * at(tx + 1, ty)) * (1 - v) +
                            v * ((1 - u) * at(tx, ty + 1) + u * at(tx + 1, ty + 1));
            const size_t k = size_t(j) * mw + i;
            if (p.shallowMin < d && d < p.shallowMax) {
                const uint8_t s = d >= p.deepMin ? uint8_t(255 - (uint32_t(int((d - p.deepMin) * kShallowDown)) & 0xFF))
                                                 : uint8_t(uint32_t(int(d * kShallowUp)) & 0xFF);
                out.shallow[k] = s;
            }
            if (p.deepMin < d)
                out.deep[k] = d < p.deepMax ? uint8_t(int((d - p.deepMin) * kDeep)) : 255;
            if (p.beachStart < d && d < p.beachEnd) {
                const float b = d > p.beachPeak ? 255.0f - (d - p.beachPeak) * kBeachDown : (d - p.beachStart) * kBeachUp;
                if (d >= 0) {
                    out.beach[k] = uint8_t(int(b));
                } else {
                    float best = std::numeric_limits<float>::max();
                    for (const auto& q : wet)
                        best = std::min(best, (px - q[0]) * (px - q[0]) + (py - q[1]) * (py - q[1]));
                    const float dist = std::sqrt(best);
                    if (dist < p.maxBeachWidth) {
                        const float f = b * (1 - dist / p.maxBeachWidth) * p.beachIntensity;
                        out.beach[k] = f >= 255 ? 255 : uint8_t(int(f));
                    }
                }
            }
        }
    return out;
}

} // namespace os2
