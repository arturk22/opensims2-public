#include "lot_lighting.hpp"
#include "formats/log_once.hpp"

#include "lot_vm.hpp"
#include "vm_objects.hpp"

#include "formats/graphics_options.hpp"
#include "formats/image.hpp"
#include "formats/lot_diffusion.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sim_actor.hpp"
#include "formats/skeleton.hpp"
#include "render/renderer.hpp"
#include "vm/dialog.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

simd_float3 f3(const Vec3& v) { return simd_make_float3(v[0], v[1], v[2]); }


// The day/night phase of the hour (docs/lighting-mac.md 2).
const char* stateOfHour(int hour) { return hour >= 7 && hour <= 18 ? "day" : "night"; }

} // namespace

SimShadowLight simShadowLight(const LightingState& state, float zone, float limit) {
    SimShadowLight out;
    simd_float3& sunTravel = out.sunTravel;
    for (const auto& l : state.lights)
        if (lower(l.name) == "sun") {
            const simd_float3 d = simd_make_float3(l.direction[0], l.direction[1], l.direction[2]);
            if (simd_length(d) > 1e-4f && d.z > 0)
                sunTravel = -simd_normalize(d);
        }
    // The outdoor shadow colour (the lighting manager, Mac BV 0x705f70):
    // the state's light on an upward floor (FindIllumFromLightingState over
    // sun, sky, fill1, fill2) with the sun's weight times max(0, 1 - s x
    // hmSunLightTransitionZone), s = shadowStrengthLimit (1) x
    // projectiveShadowStrength (0.73 by day, 0.4 at night), over the same
    // without the shadow, each clamped to [0, 1] (0x7047e4); white when
    // the transition zone is 0.
    const float strength = state.projectiveShadowStrength >= 0 ? state.projectiveShadowStrength : 0.0f;
    const float sunKept = std::max(0.0f, 1.0f - limit * strength * zone);
    auto upwardLight = [&](float sunFactor) {
        const simd_float3 sky = simd_make_float3(state.skyColor[0], state.skyColor[1], state.skyColor[2]) * state.skyStrength;
        simd_float3 e = 0;
        float w = 0;
        for (const auto& l : state.lights) {
            const std::string name = lower(l.name);
            if (name != "sun" && name != "sky" && name != "fill1" && name != "fill2")
                continue;
            const simd_float3 d = simd_make_float3(l.direction[0], l.direction[1], l.direction[2]);
            if (simd_length(d) <= 0)
                continue;
            const float wi = std::max(simd_normalize(d).z, 0.0f) * l.intensity * (name == "sun" ? sunFactor : 1.0f);
            e += simd_make_float3(l.color[0], l.color[1], l.color[2]) * wi;
            w += wi;
        }
        return simd_clamp(sky * (1 - w) + e, simd_make_float3(0, 0, 0), simd_make_float3(1, 1, 1));
    };
    simd_float3& outdoorShadow = out.outdoorColor;
    if (zone > 1e-6f) {
        const simd_float3 lit = upwardLight(1), shaded = upwardLight(sunKept);
        for (int c = 0; c < 3; ++c)
            if (lit[c] > 1e-6f)
                outdoorShadow[c] = shaded[c] / lit[c];
    }
    return out;
}

LotLighting::LotLighting(Renderer& renderer, ResourceIndex& index, LotSimulation& simulation, VmObjectView& objects,
                         LightingConfig config, LightOverrides overrides)
    : renderer_(renderer), index_(index), sim_(simulation), objects_(objects), config_(std::move(config)),
      overrides_(std::move(overrides)) {}

const std::vector<LotLighting::ModelLight>& LotLighting::modelLights(const std::string& cres) {
    auto it = modelLights_.find(cres);
    if (it != modelLights_.end())
        return it->second;
    std::vector<ModelLight> out;
    try {
        if (auto loc = index_.findByName(cres + "_cres", kTypeCres)) {
            const auto bytes = index_.read(*loc);
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
                const auto lb = index_.read(rc.links[size_t(node.lightLink)]);
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
                    if (auto o = overrides_.find(m.name); o != overrides_.end()) {
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
    return modelLights_[cres] = std::move(out);
}

LightingState LotLighting::blended() const {
    const LightingState* to = config_.state(current_);
    const LightingState* from = config_.state(previous_);
    if (!to)
        return {};
    const float in = std::max(to->transitionIn, 0.0f);
    const float t = from && in > 0 ? float(std::clamp((last_ - changedAt_) / in, 0.0, 1.0)) : 1.0f;
    if (t >= 1 || !from)
        return *to;
    LightingState s = *to;
    for (int k = 0; k < 3; ++k)
        s.skyColor[k] = from->skyColor[k] * from->skyStrength * (1 - t) + to->skyColor[k] * to->skyStrength * t;
    s.skyStrength = 1;
    // Lights by name; one missing from a state has intensity 0 there.
    s.lights.clear();
    auto find = [](const LightingState& st, const std::string& name) -> const LightingState::Light* {
        for (const auto& l : st.lights)
            if (lower(l.name) == lower(name))
                return &l;
        return nullptr;
    };
    std::vector<std::string> names;
    for (const auto* st : {from, to})
        for (const auto& l : st->lights)
            if (std::find(names.begin(), names.end(), lower(l.name)) == names.end())
                names.push_back(lower(l.name));
    for (const auto& name : names) {
        const auto* a = find(*from, name);
        const auto* b = find(*to, name);
        LightingState::Light l = b ? *b : *a;
        for (int k = 0; k < 3; ++k) {
            const float ca = a ? a->color[k] * a->intensity : 0, cb = b ? b->color[k] * b->intensity : 0;
            l.color[k] = ca * (1 - t) + cb * t;
            l.direction[k] = (a ? a->direction[k] : b->direction[k]) * (1 - t) + (b ? b->direction[k] : a->direction[k]) * t;
        }
        l.intensity = 1;
        s.lights.push_back(l);
    }
    for (int k = 0; k < 3; ++k)
        s.environment[k] = from->environment[k] * (1 - t) + to->environment[k] * t;
    if (from->projectiveShadowStrength >= 0 && to->projectiveShadowStrength >= 0)
        s.projectiveShadowStrength = from->projectiveShadowStrength * (1 - t) + to->projectiveShadowStrength * t;
    for (auto& [name, m] : s.modifiers)
        if (auto f = from->modifiers.find(name); f != from->modifiers.end()) {
            m.practical = f->second.practical * (1 - t) + m.practical * t;
            m.practicalFalloff = f->second.practicalFalloff * (1 - t) + m.practicalFalloff * t;
        }
    return s;
}

// A room-lighting global ("roomOverallStrength"), with its "sim:"/"object:"
// variant first.
float LotLighting::global(const std::string& group, const std::string& name, float fallback) const {
    for (const std::string& key : {lower(group + ":" + name), lower(name)})
        if (auto it = config_.globals.find(key); it != config_.globals.end())
            try {
                return std::stof(it->second);
            } catch (const std::logic_error& err) { // not a number (std::stof): the next key, else the fallback
                os2::logOnce("lighting value", key, err);
            }
    return fallback;
}

std::optional<Vec3> LotLighting::roomCentre(int16_t room, int level) {
    const auto key = std::make_pair(level, room);
    if (auto it = centres_.find(key); it != centres_.end())
        return it->second;
    std::optional<Vec3> out;
    const auto& world = sim_.machine().world;
    if (world) {
        double sx = 0, sy = 0;
        size_t n = 0;
        for (int x = 0; x < int(world->width()); ++x)
            for (int y = 0; y < int(world->height()); ++y)
                if (world->room(vm::TilePos{x, y, level}) == room)
                    sx += x + 0.5, sy += y + 0.5, ++n;
        if (n) {
            const float cx = float(sx / double(n)), cy = float(sy / double(n));
            out = Vec3{cx, cy, sim_.floorHeight(level, cx, cy) + 1.5f}; // about a Sim's chest (inferred)
        }
    }
    return centres_[key] = out;
}

int16_t LotLighting::lightingRoom(int vmLevel, float x, float y) {
    // With the light maps, the light's room (3ARY 0x0B, room definition 0)
    // of the quarter tile under the point: the rooms the lighting manager
    // gathers and gives lights to (cRoomLightingDesc, docs/lighting-mac.md
    // 4.1, 11.1); outdoors is 0. The VM's rooms (0x09) are split by fences
    // and number the rooms their own way.
    if (layout_)
        return int16_t(lightRoomAt(*layout_, vmLevel - 1, x, y));
    const auto& w = sim_.machine().world;
    return w ? w->room(vm::TilePos{int(std::floor(x)), int(std::floor(y)), vmLevel}) : int16_t(0);
}

bool LotLighting::outside(int16_t room) {
    if (!room)
        return true;
    if (layout_)
        return false; // a light room (lightingRoom): outdoors is 0
    if (!outdoorRooms_) {
        outdoorRooms_.emplace();
        if (const auto& w = sim_.machine().world) {
            const int level = w->firstLevel();
            auto add = [&](int x, int y) {
                const int16_t r = w->room(vm::TilePos{x, y, level});
                if (r && std::find(outdoorRooms_->begin(), outdoorRooms_->end(), r) == outdoorRooms_->end())
                    outdoorRooms_->push_back(r);
            };
            for (int x = 0; x < int(w->width()); ++x)
                add(x, 0), add(x, int(w->height()) - 1);
            for (int y = 0; y < int(w->height()); ++y)
                add(0, y), add(int(w->width()) - 1, y);
        }
    }
    return std::find(outdoorRooms_->begin(), outdoorRooms_->end(), room) != outdoorRooms_->end();
}

// The lot's light maps (formats/lot_lightmap) for the lamps at their levels
// and a lighting state, cached by both: recomputed when Change Light moves a
// lamp, crossfaded from the previous state's over the transition.
std::shared_ptr<const LotLightMap> LotLighting::lightMap(const std::string& stateName) {
    std::string key = stateName;
    std::vector<LightMapLight> lights;
    for (const auto& l : lamps_) {
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
        key += "|" + std::to_string(l.id) + ":" + l.light + ":" + std::to_string(l.level);
    }
    for (const auto& [k, map] : lightMaps_)
        if (k == key)
            return map;
    const LightingState* state = config_.state(stateName);
    if (!state)
        return nullptr;
    auto map = std::make_shared<const LotLightMap>(computeLotLightMap(*layout_, *state, lights, LightMapGlobals::from(config_)));
    lightMaps_.push_back({key, map});
    if (lightMaps_.size() > 4)
        lightMaps_.erase(lightMaps_.begin());
    return map;
}

void LotLighting::updateLightMaps(double seconds) {
    if (!layout_)
        return;
    const auto to = lightMap(current_);
    const auto from = previous_.empty() ? to : lightMap(previous_);
    if (!to)
        return;
    if (to != shownTo_) {
        // OS2_LIGHTMAP_DUMP=<file.png>: writes the light-map atlas (debugging aid).
        if (const char* dump = std::getenv("OS2_LIGHTMAP_DUMP"))
            writePng(dump, to->image);
        renderer_.setLightMap(1, to->image);
        shownTo_ = to;
    }
    if (from && from != shownFrom_) {
        renderer_.setLightMap(0, from->image);
        shownFrom_ = from;
    }
    const LightingState* target = config_.state(current_);
    const float in = target ? std::max(target->transitionIn, 0.0f) : 0.0f;
    lightMapMix_ = in > 0 ? float(std::clamp((seconds - changedAt_) / in, 0.0, 1.0)) : 1.0f;
    renderer_.setLightMapMix(lightMapMix_);
}

void LotLighting::update(double seconds) {
    last_ = seconds;
    vm::VM& m = sim_.machine();
    const std::string want = forced_.empty() ? stateOfHour(m.clock.hour) : forced_;
    if (want != current_) {
        previous_ = current_;
        current_ = want;
        // A forced state (the Buy/Build day/night button) snaps: the mode pauses the
        // scene clock the blend runs on, so a fade would never advance.
        changedAt_ = forced_.empty() ? seconds : seconds - 1.0e6;
    }
    const LightingState state = blended();
    renderer_.setEnvironmentLight(simd_make_float3(state.environment[0], state.environment[1], state.environment[2]));

    // The lamps: every light of every drawn object's model, at its level.
    lamps_.clear();
    objects_.forEachDrawn([&](int16_t id, int, SimActor*) {
        const vm::Object* o = m.object(id);
        if (!o)
            return;
        const auto& lights = modelLights(objects_.cresOf(id));
        if (lights.empty())
            return;
        const auto at = objects_.placement(id);
        if (!at)
            return;
        const std::string objd = o->definition ? lower(o->definition->fileName) : std::string();
        const bool portal = objd.starts_with("window") || objd.starts_with("door");
        // Doors and arches are OBJD type 8 (the portal builder's +0x3c, docs/lighting-mac.md 11.4).
        const bool doorOrArch = o->definition && o->definition->objectType() == 8;
        for (const auto& l : lights) {
            int level = vm::lightLevel(m, *o, l.name);
            if (portal)
                level = 100; // the outside's light, not a lamp
            else if (level < 0) // not changed since the lot loaded: the lamps keep on/off in semi-attribute 2 (inferred)
                level = defaultLevel_ >= 0 ? defaultLevel_ : o->semiAttributes.size() > 2 && o->semiAttributes[2] ? 100 : 0;
            Lamp lamp;
            lamp.id = id;
            lamp.light = l.name;
            lamp.position = at->apply(l.position);
            lamp.color = {l.color[0] * l.intensity, l.color[1] * l.intensity, l.color[2] * l.intensity};
            lamp.falloff = l.falloff;
            lamp.spot = l.spot, lamp.cone = l.cone, lamp.penumbra = l.penumbra;
            lamp.direction = at->rotate(l.direction);
            lamp.level = std::clamp(level, 0, 100);
            lamp.portal = portal;
            lamp.doorOrArch = portal && doorOrArch;
            lamp.objectLevel = o->position.level - 1; // the VM numbers levels from 1 (lot level + 1)
            for (int c = 0; c < 3; ++c)
                lamp.diffusionColor[size_t(c)] = l.diffusionColor[size_t(c)] * l.diffusionIntensity;
            lamp.diffusionFalloff = l.diffusionFalloff;
            lamp.floorScale = l.floorScale;
            lamp.wallScale = l.wallScale;
            lamp.interFloor = l.interFloor;
            lamp.directional = l.directional, lamp.directionalFalloff = l.directionalFalloff;
            lamp.room = lightingRoom(o->position.level, lamp.position[0], lamp.position[1]);
            lamps_.push_back(lamp);
        }
    });

    if (std::getenv("OS2_DEBUG_LIGHTS")) {
        static size_t printed = 0;
        if (printed++ < 1)
            for (const auto& l : lamps_)
                std::fprintf(stderr, "opensims2: lamp %d %s at (%.2f, %.2f, %.2f) floor %d room %d level %d colour (%.2f %.2f %.2f) falloff %.2f%s cone %.3f penumbra %.3f dir (%.2f %.2f %.2f); state %s\n",
                             l.id, l.light.c_str(), l.position[0], l.position[1], l.position[2], l.objectLevel, l.room, l.level,
                             l.color[0], l.color[1], l.color[2], l.falloff, l.spot ? " spot" : "", l.cone, l.penumbra,
                             l.direction[0], l.direction[1], l.direction[2], current_.c_str());
    }
    updateLightMaps(seconds);

    // The lamps' practical lights (docs/lighting-mac.md 4.1, 6): the model's
    // point light at its Change Light level, intensity x the modifier's
    // practical strength and FalloffRate x its practical falloff
    // (insideObjects in a room, outsideObjects outside), out to the range
    // where I / (1 + C d^2) reaches lightRangeCutoff; they light the
    // objects and Sims of the lamp's room (the outside's, for lamps outside).
    std::map<std::pair<int, int16_t>, std::vector<Renderer::LightSet::Point>> practical; // by (VM level, room); outside: (0, 0)
    {
        auto modifier = [&](const char* name) {
            auto it = state.modifiers.find(name);
            return it == state.modifiers.end() ? LightingState::Modifier{} : it->second;
        };
        const auto inside = modifier("insideobjects"), outsideMod = modifier("outsideobjects");
        const float cutoff = global("", "lightRangeCutoff", 0.5f);
        for (const auto& l : lamps_) {
            if (l.portal || l.level <= 0)
                continue;
            const bool out = outside(l.room);
            const auto& mod = out ? outsideMod : inside;
            const float k = float(l.level) / 100.0f * mod.practical;
            Renderer::LightSet::Point p;
            p.position = f3(l.position);
            p.color = f3(l.color) * k;
            p.falloff = l.falloff * mod.practicalFalloff;
            if (l.spot) {
                const simd_float2 c = Renderer::spotCosines(l.cone, l.penumbra);
                p.spotDirection = f3(l.direction), p.cosOuter = c.x, p.cosInner = c.y;
            }
            const float I = simd_length(p.color);
            if (cutoff <= 0 || I / cutoff <= 1)
                continue;
            p.range = p.falloff <= 0 ? 10000.0f : std::sqrt((I / cutoff - 1) / p.falloff);
            practical[out ? std::make_pair(0, int16_t(0)) : std::make_pair(l.objectLevel + 1, l.room)].push_back(p);
        }
        // The windows' and doors' practical lights (SetAndAddPortalLights,
        // docs/lighting-mac.md 11.4), from the light map's portals: the
        // portal's own spot ("LM_PortalDirect", spots only) and a point light
        // in front of the portal's plane ("LM_PortalSpill"), for the room it
        // shines into.
        if (shownTo_)
            for (const auto& pl : shownTo_->portalLights) {
                const Lamp* lamp = nullptr;
                for (const auto& l : lamps_)
                    if (l.portal && l.id == pl.object && l.light == pl.name)
                        lamp = &l;
                if (!lamp || (!pl.spill && !lamp->spot))
                    continue;
                Renderer::LightSet::Point p;
                p.position = f3(lamp->position);
                p.color = f3(lamp->color) * simd_make_float3(pl.scale[0], pl.scale[1], pl.scale[2]);
                p.falloff = lamp->falloff * pl.falloffScale;
                if (pl.spill) {
                    const simd_float3 n = simd_make_float3(pl.planeNormal[0], pl.planeNormal[1], pl.planeNormal[2]);
                    p.plane = simd_make_float4(n, -simd_dot(n, simd_make_float3(pl.planePoint[0], pl.planePoint[1], 0)));
                } else {
                    const simd_float2 c = Renderer::spotCosines(lamp->cone, lamp->penumbra);
                    p.spotDirection = f3(lamp->direction), p.cosOuter = c.x, p.cosInner = c.y;
                }
                const float I = simd_length(p.color);
                if (cutoff <= 0 || I / cutoff <= 1)
                    continue;
                p.range = p.falloff <= 0 ? 10000.0f : std::sqrt((I / cutoff - 1) / p.falloff);
                practical[pl.room ? std::make_pair(pl.level + 1, int16_t(pl.room)) : std::make_pair(0, int16_t(0))].push_back(p);
            }
        for (auto& [key, points] : practical) {
            // The brightest first, as many as a light set holds next to the 4 Deltas.
            std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
                return simd_length(a.color) > simd_length(b.color);
            });
            if (points.size() > 12)
                points.resize(12);
        }
    }

    // Light sets: 0/1 outdoor objects/Sims, then 2 per room.
    std::vector<Renderer::LightSet> sets{Renderer::outdoorLights(state, false), Renderer::outdoorLights(state, true)};
    if (auto it = practical.find({0, 0}); it != practical.end())
        for (auto& set : sets)
            set.points = it->second;
    std::map<std::pair<int, int16_t>, int> roomSets; // (level, room) -> objects' set; +1 Sims'
    auto roomSet = [&](int level, int16_t room) -> int {
        if (outside(room))
            return 0;
        const auto key = std::make_pair(level, room);
        if (auto it = roomSets.find(key); it != roomSets.end())
            return it->second;
        if (sets.size() + 2 > Renderer::kMaxLightSets)
            return 0;
        // The room's irradiance by the direction the light travels (+X, -X,
        // +Y, -Y, up from the floor, down from the ceiling), gathered from
        // the light maps (GatherRoomLighting, docs/lighting-mac.md 4.1),
        // crossfading with the light maps.
        std::array<simd_float3, 6> irr{};
        bool found = false;
        auto gather = [&](const LotLightMap* lm, float w) {
            if (!lm)
                return;
            if (auto it = lm->rooms.find({level - 1, uint16_t(room)}); it != lm->rooms.end()) {
                found = true;
                for (size_t d = 0; d < 6; ++d)
                    irr[d] += f3(it->second[d]) * w;
            }
        };
        gather(shownTo_.get(), lightMapMix_);
        gather(shownFrom_.get(), 1 - lightMapMix_);
        if (!found) {
            if (std::getenv("OS2_DEBUG_LIGHTS"))
                std::fprintf(stderr, "opensims2: room %d on level %d has no light-map irradiance: outdoor lights\n", room, level);
            return roomSets[key] = 0; // a room the light maps do not know (no floor on that level)
        }
        const int first = int(sets.size());
        std::array<std::array<float, 3>, 6> irradiance{};
        for (size_t d = 0; d < 6; ++d)
            irradiance[d] = {irr[d].x, irr[d].y, irr[d].z};
        for (const char* group : {"object", "sim"}) {
            RoomLightTuning t;
            t.type = int(global(group, "roomLightingType", 4));
            t.overall = global(group, "roomOverallStrength", 0.4f);
            t.side = global(group, "roomSideStrength", 1.2f), t.sideCrossover = global(group, "roomSideCrossover", 1);
            t.bounce = global(group, "roomBounceStrength", 0.25f), t.bounceCrossover = global(group, "roomBounceCrossover", 1);
            t.sideSlope = global(group, "roomSideSlope", 0.75f), t.directionalBoost = global(group, "roomDirectionalBoost", 1);
            t.specular = global(group, "roomSpecular", 0.8f);
            // The ambient and the "Delta" directionals (FUN_0070daa8, signed;
            // dark rooms' irradiance is already brightened with the light maps).
            const RoomLights rl = roomLights(irradiance, t);
            if (std::getenv("OS2_DEBUG_LIGHTS") && std::string(group) == "object") {
                std::fprintf(stderr, "opensims2: room %d level %d irradiance", room, level);
                for (const auto& v : irradiance)
                    std::fprintf(stderr, " (%.3f %.3f %.3f)", v[0], v[1], v[2]);
                std::fprintf(stderr, "; ambient (%.3f %.3f %.3f), Deltas", rl.ambient[0], rl.ambient[1], rl.ambient[2]);
                for (const auto& d : rl.deltas)
                    std::fprintf(stderr, " [to (%.2f %.2f %.2f) (%.3f %.3f %.3f)]", d.towardLight[0], d.towardLight[1],
                                 d.towardLight[2], d.color[0], d.color[1], d.color[2]);
                std::fprintf(stderr, "\n");
            }
            Renderer::LightSet set;
            set.ambient = simd_make_float3(rl.ambient[0], rl.ambient[1], rl.ambient[2]);
            for (const auto& d : rl.deltas)
                set.lights.push_back({simd_make_float3(d.towardLight[0], d.towardLight[1], d.towardLight[2]),
                                      simd_make_float3(d.color[0], d.color[1], d.color[2]), d.specular});
            if (auto it = practical.find(key); it != practical.end())
                set.points = it->second;
            sets.push_back(set);
        }
        return roomSets[key] = first;
    };

    // Each object and Sim gets the set of the room it is in.
    auto roomOf = [&](const vm::Object& o) -> int16_t {
        if (!o.position.inWorld())
            return 0;
        return lightingRoom(o.position.level, o.position.x, o.position.y);
    };
    // A copy of a set with an object's extra lights after its own.
    auto withExtras = [&](int16_t id, int set) {
        if (!extraLights || sets.size() + 1 > Renderer::kMaxLightSets)
            return set;
        const auto extra = extraLights(id);
        if (extra.empty())
            return set;
        Renderer::LightSet s = sets[size_t(set)];
        for (const auto& e : extra) {
            Renderer::LightSet::Point p{f3(e.position), f3(e.color), e.falloff, e.range, 1.0f};
            if (e.spot) {
                const simd_float2 c = Renderer::spotCosines(e.cone, e.penumbra);
                p.spotDirection = f3(e.direction), p.cosOuter = c.x, p.cosInner = c.y;
            }
            s.points.push_back(p);
        }
        sets.push_back(std::move(s));
        return int(sets.size() - 1);
    };
    objects_.forEachDrawn([&](int16_t id, int instance, SimActor* actor) {
        const vm::Object* o = m.object(id);
        if (!o)
            return;
        const int set = withExtras(id, roomSet(o->position.level, roomOf(*o)));
        renderer_.setInstanceLightSet(actor ? renderer_.instanceOf(actor->model) : instance, set);
    });
    // The Sims' shadows (nTSSG::cShadow style 1, docs/lighting-mac.md 9):
    // inside, straight down; outside, along the state's sun (where its
    // light travels) in the outdoor shadow colour.
    std::vector<Renderer::ShadowCaster> casters;
    const SimShadowLight shadowLight =
        simShadowLight(state, global("", "hmSunLightTransitionZone", 1.0f), global("", "shadowStrengthLimit", 1.0f));
    const simd_float3 sunTravel = shadowLight.sunTravel, outdoorShadow = shadowLight.outdoorColor;
    for (const auto& [id, actor] : sims_) {
        const vm::Object* o = m.object(id);
        if (!o || !actor)
            continue;
        const int16_t room = roomOf(*o);
        const int set = withExtras(id, roomSet(o->position.level, room) + 1);
        const int instance = renderer_.instanceOf(actor->model);
        renderer_.setInstanceLightSet(instance, set);
        if (!graphicsOptions().simShadows || instance < 0 || !o->position.inWorld())
            continue;
        // The Sim's vertices are posed in world space: its placement is the actor's.
        const Xform at = actor->placement();
        const Vec3 feet = at.apply({0, 0, 0}), side = at.apply({1, 0, 0});
        Renderer::ShadowCaster c;
        c.instance = instance;
        c.feet = f3(feet);
        c.heading = std::atan2(side[1] - feet[1], side[0] - feet[0]);
        c.outside = outside(room);
        c.direction = c.outside ? sunTravel : simd_make_float3(0, 0, -1);
        c.color = outdoorShadow;
        if (std::getenv("OS2_DEBUG_SHADOWS"))
            std::fprintf(stderr, "opensims2: shadow of %d at (%.2f, %.2f, %.2f) heading %.2f %s, outdoor colour (%.3f %.3f %.3f)\n", id,
                         c.feet.x, c.feet.y, c.feet.z, c.heading, c.outside ? "outside" : "inside", c.color.x, c.color.y, c.color.z);
        casters.push_back(c);
    }
    renderer_.setShadowCasters(std::move(casters));
    renderer_.setLightSets(std::move(sets));
}

} // namespace os2
