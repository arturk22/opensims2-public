#include "lot_lighting.hpp"

#include "lot_vm.hpp"
#include "vm_objects.hpp"

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
constexpr float kLightRangeCutoff = 0.275f; // Lighting.txt's lightRangeCutoff

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

simd_float3 f3(const Vec3& v) { return simd_make_float3(v[0], v[1], v[2]); }

float luminance(simd_float3 c) { return 0.299f * c.x + 0.587f * c.y + 0.114f * c.z; }

// The day/night phase of the hour (docs/lighting-mac.md 2).
const char* stateOfHour(int hour) { return hour >= 7 && hour <= 18 ? "day" : "night"; }

} // namespace

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
                    // Point light values: intensity, (unknown), RGB, falloff rate, range (inferred).
                    ModelLight m;
                    m.name = lower(l.name.empty() ? node.renderable.transform.name() : l.name);
                    m.intensity = l.values[0];
                    m.color = {l.values[2], l.values[3], l.values[4]};
                    m.falloff = l.values[5];
                    const auto& t = node.renderable.transform;
                    Xform at{t.rotation, t.translation};
                    if (auto p = parent.find(uint32_t(i)); p != parent.end())
                        for (size_t b = 0; b < sk.bones.size(); ++b)
                            if (sk.bones[b].block == p->second)
                                at = world[b].then(at);
                    m.position = at.t;
                    // LotLights.txt's overrides, by light name.
                    if (auto o = overrides_.find(m.name); o != overrides_.end()) {
                        if (auto v = o->second.find("intensity"); v != o->second.end() && !v->second.empty())
                            m.intensity = v->second[0];
                        if (auto v = o->second.find("falloffrate"); v != o->second.end() && !v->second.empty())
                            m.falloff = v->second[0];
                        if (auto v = o->second.find("color"); v != o->second.end() && v->second.size() >= 3)
                            m.color = {v->second[0], v->second[1], v->second[2]};
                    }
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
            } catch (const std::exception&) {
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

bool LotLighting::outside(int16_t room) {
    if (!room)
        return true;
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

void LotLighting::update(double seconds) {
    last_ = seconds;
    vm::VM& m = sim_.machine();
    const std::string want = forced_.empty() ? stateOfHour(m.clock.hour) : forced_;
    if (want != current_) {
        previous_ = current_;
        current_ = want;
        changedAt_ = seconds;
    }
    const LightingState state = blended();
    auto modifier = [&](const char* name) {
        auto it = state.modifiers.find(name);
        return it == state.modifiers.end() ? LightingState::Modifier{} : it->second;
    };
    const auto inside = modifier("insideobjects"), outside_ = modifier("outsideobjects");

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
            lamp.level = std::clamp(level, 0, 100);
            lamp.portal = portal;
            if (m.world) {
                const vm::TilePos t{int(std::floor(lamp.position[0])), int(std::floor(lamp.position[1])), o->position.level};
                lamp.room = m.world->room(t);
            }
            lamps_.push_back(lamp);
        }
    });

    if (std::getenv("OS2_DEBUG_LIGHTS")) {
        static size_t printed = 0;
        if (printed++ < 1)
            for (const auto& l : lamps_)
                std::fprintf(stderr, "opensims2: lamp %d %s at (%.2f, %.2f, %.2f) room %d level %d colour (%.2f %.2f %.2f) falloff %.2f; state %s\n",
                             l.id, l.light.c_str(), l.position[0], l.position[1], l.position[2], l.room, l.level,
                             l.color[0], l.color[1], l.color[2], l.falloff, current_.c_str());
    }
    // Point lights for the light-mapped surfaces, strongest first.
    std::vector<Renderer::PointLight> points;
    const auto insidePortals = modifier("insideportals");
    for (const auto& l : lamps_) {
        if (!l.level)
            continue;
        if (l.portal)
            continue; // the surfaces already get the outdoor lights (no light maps yet)
        const auto& mod = outside(l.room) ? outside_ : inside;
        const float strength = mod.practical * float(l.level) / 100.0f;
        if (strength <= 0)
            continue;
        Renderer::PointLight p;
        p.position = f3(l.position);
        p.color = f3(l.color) * strength;
        p.falloff = std::max(0.01f, l.falloff * mod.practicalFalloff);
        p.range = std::sqrt((1 / kLightRangeCutoff - 1) / p.falloff);
        points.push_back(p);
    }
    std::sort(points.begin(), points.end(),
              [](const auto& a, const auto& b) { return luminance(a.color) > luminance(b.color); });
    if (points.size() > Renderer::kMaxPointLights)
        points.resize(Renderer::kMaxPointLights);
    renderer_.setPointLights(std::move(points));

    // Light sets: 0/1 outdoor objects/Sims, then 2 per room.
    std::vector<Renderer::LightSet> sets{Renderer::outdoorLights(state, false), Renderer::outdoorLights(state, true)};
    std::map<std::pair<int, int16_t>, int> roomSets; // (level, room) -> objects' set; +1 Sims'
    auto roomSet = [&](int level, int16_t room) -> int {
        if (outside(room))
            return 0;
        const auto key = std::make_pair(level, room);
        if (auto it = roomSets.find(key); it != roomSets.end())
            return it->second;
        if (sets.size() + 2 > Renderer::kMaxLightSets)
            return 0;
        const auto centre = roomCentre(room, level);
        // Irradiance from the 6 directions: +X, -X, +Y, -Y, up, down.
        std::array<simd_float3, 6> irr{};
        for (const auto& l : lamps_) {
            if (l.room != room || !l.level || !centre)
                continue;
            const simd_float3 d = f3(l.position) - f3(*centre);
            const float r2 = simd_length_squared(d);
            const auto& mod = l.portal ? insidePortals : inside;
            const float falloff = std::max(0.01f, l.falloff * mod.practicalFalloff);
            const simd_float3 c = f3(l.color) * (mod.practical * float(l.level) / 100.0f / (1 + falloff * r2));
            const simd_float3 u = r2 > 1e-6f ? d / std::sqrt(r2) : simd_make_float3(0, 0, 1);
            irr[0] += c * std::max(u.x, 0.0f), irr[1] += c * std::max(-u.x, 0.0f);
            irr[2] += c * std::max(u.y, 0.0f), irr[3] += c * std::max(-u.y, 0.0f);
            irr[4] += c * std::max(u.z, 0.0f), irr[5] += c * std::max(-u.z, 0.0f);
        }
        const int first = int(sets.size());
        for (const char* group : {"object", "sim"}) {
            const float overall = global(group, "roomOverallStrength", 0.4f);
            const float side = global(group, "roomSideStrength", 1.2f), sideX = global(group, "roomSideCrossover", 1);
            const float bounce = global(group, "roomBounceStrength", 0.25f), bounceX = global(group, "roomBounceCrossover", 1);
            const float slope = global(group, "roomSideSlope", 0.75f), boost = global(group, "roomDirectionalBoost", 1);
            const simd_float3 sum = irr[0] + irr[1] + irr[2] + irr[3];
            std::array<simd_float3, 6> L{};
            for (int i = 0; i < 4; ++i)
                L[size_t(i)] = (irr[size_t(i)] + sideX * irr[4]) * side * overall;
            for (int j = 4; j < 6; ++j)
                L[size_t(j)] = (irr[size_t(j)] + bounceX * sum) * bounce * overall;
            simd_float3 a = 0;
            for (const auto& v : L)
                a += v;
            a /= 6;
            // A dark room gets the state's unlitRoomColor (the first whose
            // threshold its brightness is under; inferred).
            Renderer::LightSet set;
            set.ambient = a;
            const float bright = luminance(a);
            for (const auto& u : state.unlitRooms)
                if (bright < u.threshold)
                    set.ambient = simd_max(set.ambient, f3(u.color));
            for (auto& v : L)
                v -= a;
            static const simd_float3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (int k = 0; k < 4; ++k) {
                int best = -1;
                float bestValue = 0;
                for (int d = 0; d < 6; ++d) {
                    const float v = std::max({L[size_t(d)].x, L[size_t(d)].y, L[size_t(d)].z});
                    if (v > bestValue)
                        best = d, bestValue = v;
                }
                if (best < 0)
                    break;
                simd_float3 dir = dirs[best];
                if (best < 4)
                    dir += simd_make_float3(0, 0, slope); // towards a light above the side (inferred)
                set.lights.push_back({simd_normalize(dir), L[size_t(best)] * boost});
                L[size_t(best)] = 0;
            }
            sets.push_back(set);
        }
        return roomSets[key] = first;
    };

    // Each object and Sim gets the set of the room it is in.
    auto roomOf = [&](const vm::Object& o) -> int16_t {
        if (!m.world || !o.position.inWorld())
            return 0;
        return m.world->room(vm::VM::tileOf(o));
    };
    objects_.forEachDrawn([&](int16_t id, int instance, SimActor* actor) {
        const vm::Object* o = m.object(id);
        if (!o)
            return;
        const int set = roomSet(o->position.level, roomOf(*o));
        renderer_.setInstanceLightSet(actor ? renderer_.instanceOf(actor->model) : instance, set);
    });
    for (const auto& [id, actor] : sims_) {
        const vm::Object* o = m.object(id);
        if (!o || !actor)
            continue;
        const int set = roomSet(o->position.level, roomOf(*o));
        renderer_.setInstanceLightSet(renderer_.instanceOf(actor->model), set + 1);
    }
    renderer_.setLightSets(std::move(sets));
}

} // namespace os2
