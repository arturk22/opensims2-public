#include "lot_lamps.hpp"

#include "rcol.hpp"
#include "resindex.hpp"
#include "scenegraph.hpp"
#include "skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<ModelLight> loadModelLights(ResourceIndex& index, const std::string& cres, const LightOverrides& overrides) {
    std::vector<ModelLight> out;
    try {
        if (auto loc = index.findByName(cres + "_cres", kTypeCres)) {
            const auto bytes = index.read(*loc);
            const Rcol rc(bytes);
            // Parents of blocks, by the transform nodes' children.
            std::map<uint32_t, size_t> parent;
            for (size_t i = 0; i < rc.blocks.size(); ++i)
                if (rc.blocks[i].className == "cTransformNode")
                    for (const auto& c : TransformNode::parse(rc.blocks[i].body).children)
                        parent[c.index] = i;
            const Skeleton sk = Skeleton::fromCres(rc);
            const auto world = sk.world(sk.restPose());
            for (size_t i = 0; i < rc.blocks.size(); ++i) {
                if (rc.blocks[i].className != "cLightRefNode")
                    continue;
                const LightRefNode node = LightRefNode::parse(rc.blocks[i].body);
                if (node.lightLink < 0 || size_t(node.lightLink) >= rc.links.size())
                    continue;
                const auto lb = index.read(rc.links[size_t(node.lightLink)]);
                if (!lb)
                    continue;
                const Rcol lr(*lb);
                for (const auto& blk : lr.blocks) {
                    if (blk.className != "cPointLight" && blk.className != "cSpotLight")
                        continue;
                    const StandardLight l = StandardLight::parse(blk.className, blk.body);
                    if (l.values.size() < 6)
                        continue;
                    // cStandardLightBase::ReadFrom (Mac BV 0xca2fb4): DiffuseScale (the
                    // intensity), RelSpecularScale, RGB; a point light then FalloffRate, range.
                    ModelLight m;
                    m.name = lower(l.name.empty() ? node.renderable.transform.name() : l.name);
                    m.intensity = l.values[0];
                    m.color = {l.values[2], l.values[3], l.values[4]};
                    m.falloff = l.values[5];
                    // cSpotLight::ReadFrom (Mac BV 0xca3208): ..., cone, penumbra, range.
                    if (blk.className == "cSpotLight" && l.values.size() >= 8) {
                        m.spot = true;
                        m.cone = l.values[6], m.penumbra = l.values[7];
                    }
                    const auto& t = node.renderable.transform;
                    Xform at{t.rotation, t.translation};
                    if (auto p = parent.find(uint32_t(i)); p != parent.end())
                        for (size_t b = 0; b < sk.bones.size(); ++b)
                            if (sk.bones[b].block == p->second)
                                at = world[b].then(at);
                    m.position = at.t;
                    m.direction = at.rotate({0, 0, -1});
                    // LotLights.txt's overrides, by light name. A light is a
                    // diffusion source only with a cDiffusionLightExtension
                    // (BuildLightFromObjectLight, Mac BV 0xd7d404: no
                    // extension, black, no source), which the game's lights
                    // get from their diffusionSource* overrides (the models
                    // have none); its fields default to the extension's
                    // (constructor 0x79f024): intensity 0, colour white,
                    // falloff 1, wall and floor scales 1.
                    bool diffuses = false;
                    if (auto o = overrides.find(m.name); o != overrides.end()) {
                        auto one = [&](const char* name, float& v) {
                            if (auto it = o->second.find(name); it != o->second.end() && !it->second.empty())
                                v = it->second[0];
                        };
                        for (const auto& [key, value] : o->second)
                            diffuses = diffuses || key.starts_with("diffusionsource");
                        one("diffusionsourceintensity", m.diffusionIntensity);
                        one("diffusionsourcefalloff", m.diffusionFalloff);
                        one("diffusionsourcefloorintensityscale", m.floorScale);
                        one("diffusionsourcewallintensityscale", m.wallScale);
                        one("diffusionsourceinterfloorfalloff", m.interFloor);
                        one("diffusionsourcedirectionalfalloff", m.directionalFalloff);
                        float directional = 0;
                        one("diffusionsourceisdirectional", directional);
                        m.directional = directional != 0;
                        if (auto v = o->second.find("diffusionsourcecolor"); v != o->second.end() && v->second.size() >= 3)
                            m.diffusionColor = {v->second[0], v->second[1], v->second[2]};
                        if (auto v = o->second.find("intensity"); v != o->second.end() && !v->second.empty())
                            m.intensity = v->second[0];
                        if (auto v = o->second.find("falloffrate"); v != o->second.end() && !v->second.empty())
                            m.falloff = v->second[0];
                        if (auto v = o->second.find("color"); v != o->second.end() && v->second.size() >= 3)
                            m.color = {v->second[0], v->second[1], v->second[2]};
                    }
                    if (!diffuses)
                        m.diffusionIntensity = 0;
                    out.push_back(m);
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: lights of %s: %s\n", cres.c_str(), e.what());
    }
    return out;
}

LotLamp placedLamp(int16_t id, const ModelLight& l, const Xform& at, int level, int objectLevel, bool portal,
                   bool doorOrArch) {
    LotLamp lamp;
    lamp.id = id;
    lamp.light = l.name;
    lamp.position = at.apply(l.position);
    lamp.color = {l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity};
    lamp.falloff = l.falloff;
    lamp.spot = l.spot, lamp.cone = l.cone, lamp.penumbra = l.penumbra;
    lamp.direction = at.rotate(l.direction);
    lamp.level = std::clamp(level, 0, 100);
    lamp.portal = portal;
    lamp.doorOrArch = portal && doorOrArch;
    lamp.objectLevel = objectLevel;
    for (int c = 0; c < 3; ++c)
        lamp.diffusionColor[size_t(c)] = l.diffusionColor[size_t(c)] * l.diffusionIntensity;
    lamp.diffusionFalloff = l.diffusionFalloff;
    lamp.floorScale = l.floorScale;
    lamp.wallScale = l.wallScale;
    lamp.interFloor = l.interFloor;
    lamp.directional = l.directional, lamp.directionalFalloff = l.directionalFalloff;
    return lamp;
}

std::vector<LightMapLight> lightMapLights(const std::vector<LotLamp>& lamps, const std::string& state, std::string* key) {
    if (key)
        *key = state;
    std::vector<LightMapLight> lights;
    for (const auto& l : lamps) {
        if (!l.level)
            continue;
        LightMapLight m;
        m.position = l.position;
        const float k = float(l.level) / 100.0f;
        m.color = {l.diffusionColor[0] * k, l.diffusionColor[1] * k, l.diffusionColor[2] * k};
        m.falloff = l.diffusionFalloff;
        m.floorScale = l.floorScale;
        m.wallScale = l.wallScale;
        m.interFloor = l.interFloor;
        m.directional = l.directional, m.directionalFalloff = l.directionalFalloff;
        m.direction = {l.direction[0], l.direction[1], l.direction[2]};
        m.level = l.objectLevel;
        m.portal = l.portal;
        m.doorOrArch = l.doorOrArch;
        m.object = l.id;
        m.name = l.light;
        lights.push_back(m);
        if (key)
            *key += "|" + std::to_string(l.id) + ":" + l.light + ":" + std::to_string(l.level);
    }
    return lights;
}

const char* lightingStateOfHour(int hour) { return hour >= 7 && hour <= 18 ? "day" : "night"; }

std::optional<float> floorIlluminationAt(const LotLightMapLayout& layout, const LotLightMap* from,
                                         const LotLightMap* to, float mix, int vmLevel, float x, float y) {
    if (!to)
        return std::nullopt;
    const int level = vmLevel - 1; // the lot's levels
    // LookupFloorLightMap (Mac BV 0x6f6a24): the texel (frac(x) 8, frac(y) 8)
    // of the tile's page; off the pages, black.
    if (level < layout.minLevel || level >= layout.minLevel + layout.levels || x < 0 || y < 0 ||
        x >= float(layout.lotWidth) || y >= float(layout.lotHeight))
        return 0.0f;
    const auto uv = layout.floorTexel(level, x, y);
    const auto texel = [&](const LotLightMap* map, int c) -> float {
        if (!map)
            return 0.0f;
        const RgbaImage& im = map->image;
        const auto u = uint32_t(std::floor(uv[0])), v = uint32_t(std::floor(uv[1]));
        if (u >= im.width || v >= im.height)
            return 0.0f;
        return float(im.pixels[(size_t(v) * im.width + u) * 4 + size_t(c)]) / 255.0f;
    };
    std::array<float, 3> rgb{};
    if (!from)
        from = to;
    for (int c = 0; c < 3; ++c) {
        const float a = texel(from, c), b = texel(to, c);
        rgb[size_t(c)] = (a + (b - a) * mix) * 2.0f; // cLightMapManager +0x70
    }
    // ColourToIlluminance (Mac BV 0x1808dd4).
    return std::fmaf(rgb[2], 0.11f, std::fmaf(rgb[0], 0.3f, rgb[1] * 0.59f));
}

} // namespace os2
