#include "app/cinematic_player.hpp"

#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/scenegraph.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

bool debug() { return std::getenv("OS2_DEBUG_CINE") != nullptr; }

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A channel of a light's target ("point1b": diffuse, diffuseScale) at `ticks`.
float lightChannel(const Animation& a, const std::string& target, const char* name, size_t component, float ticks,
                   float fallback) {
    const Animation::Target* t = a.findTarget(target);
    if (!t)
        return fallback;
    for (const auto& c : t->channels)
        if (strcasecmp(c.name.c_str(), name) == 0 && c.components.size() > component &&
            !c.components[component].keys.empty())
            return c.components[component].sample(ticks, c.duration());
    return fallback;
}

simd_float4x4 hidden() {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(0, 0, -1000, 1);
    return m;
}

} // namespace

CinematicPlayer::CinematicPlayer(Renderer& renderer, ResourceIndex& index, Host host, const std::string& scene,
                                 std::vector<int16_t> actors, int16_t origin, bool mirror)
    : renderer_(renderer), index_(index), host_(std::move(host)),
      camera_(std::make_unique<CinematicCamera>(index, scene)), actorIds_(std::move(actors)), originId_(origin),
      mirror_(mirror) {
    actorIds_.resize(camera_->scene().actors.size(), 0);
}

CinematicPlayer::~CinematicPlayer() { finish(); }

void CinematicPlayer::loadRig(const std::string& cresName) {
    const auto loc = index_.findByName(cresName + "_cres", kTypeCres);
    if (!loc)
        throw std::runtime_error("light rig not found: " + cresName);
    const auto bytes = index_.read(*loc);
    const Rcol rc(bytes);
    rigSkeleton_ = Skeleton::fromCres(rc);
    // The light nodes hang under the *_trans nodes: bones too, so that the
    // animation's channels named after the lights ("point1b") move them.
    std::map<uint32_t, size_t> parent;
    for (size_t i = 0; i < rc.blocks.size(); ++i)
        if (rc.blocks[i].className == "cTransformNode")
            for (const auto& c : TransformNode::parse(rc.blocks[i].body).children)
                parent[c.index] = i;
    for (size_t i = 0; i < rc.blocks.size(); ++i) {
        if (rc.blocks[i].className != "cLightRefNode")
            continue;
        const LightRefNode node = LightRefNode::parse(rc.blocks[i].body);
        RigLight l;
        l.name = lower(node.renderable.transform.name());
        Skeleton::Bone b;
        b.name = l.name;
        b.rest = {node.renderable.transform.rotation, node.renderable.transform.translation};
        b.block = i;
        if (auto p = parent.find(uint32_t(i)); p != parent.end())
            for (size_t k = 0; k < rigSkeleton_.bones.size(); ++k)
                if (rigSkeleton_.bones[k].block == p->second)
                    b.parent = int(k);
        l.bone = int(rigSkeleton_.bones.size());
        rigSkeleton_.bones.push_back(b);
        for (char c : l.name)
            if (c >= '1' && c <= '3') {
                l.group = c - '0';
                break;
            }
        if (node.lightLink >= 0 && size_t(node.lightLink) < rc.links.size())
            if (auto lb = index_.read(rc.links[size_t(node.lightLink)])) {
                const Rcol lr(*lb);
                for (const auto& blk : lr.blocks) {
                    if (blk.className != "cPointLight" && blk.className != "cSpotLight")
                        continue;
                    const StandardLight sl = StandardLight::parse(blk.className, blk.body);
                    // DiffuseScale, RelSpecularScale, RGB, FalloffRate, ...
                    // (cStandardLightBase::ReadFrom, Mac BV 0xca2fb4): the
                    // rig's read "0 1 | 1 1 1 | 0 ...": DiffuseScale 0 (the
                    // animation's diffuseScale channel lights them) and no
                    // falloff, so their range is unlimited (docs/lighting-mac.md 6).
                    if (sl.values.size() >= 6) {
                        l.color = {sl.values[2], sl.values[3], sl.values[4]};
                        l.falloff = sl.values[5];
                    }
                    if (debug()) {
                        std::fprintf(stderr, "opensims2: cinematic rig light %s (%s):", l.name.c_str(), blk.className.c_str());
                        for (float v : sl.values)
                            std::fprintf(stderr, " %g", v);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        rigLights_.push_back(l);
    }
}

bool CinematicPlayer::start(double now) {
    const CinematicScene& sc = camera_->scene();
    std::optional<Xform> origin;
    if (sc.originActor < actorIds_.size() && actorIds_[sc.originActor] && host_.placement)
        origin = host_.placement(actorIds_[sc.originActor]);
    if (!origin && host_.placement)
        origin = host_.placement(originId_);
    if (!origin)
        return false;
    camera_->start(*origin, now, mirror_);
    if (debug()) {
        const Vec3 fwd = origin->rotate({0, 1, 0});
        std::fprintf(stderr, "opensims2: cinematic origin (%.2f %.2f %.2f), +Y towards (%.2f %.2f %.2f)\n", origin->t[0],
                     origin->t[1], origin->t[2], fwd[0], fwd[1], fwd[2]);
    }
    started_ = true;
    start_ = now;
    for (size_t i = 0; i < sc.actors.size(); ++i) {
        const auto& a = sc.actors[i];
        const bool rig = lower(a.autoModel).find("lightrig") != std::string::npos ||
                         lower(a.sgObjectName).rfind("lightrig", 0) == 0;
        try {
            if (rig) {
                loadRig(a.autoModel.empty() ? "cineLightRig-template" : a.autoModel);
                for (const auto& n : a.autoAnimations)
                    rigAnims_.push_back(std::make_unique<Animation>(loadAnimation(index_, n)));
                rigActor_ = int(i);
                continue;
            }
            SimActor* actor = nullptr;
            if (a.autoCreate && !a.autoModel.empty()) {
                const ObjectModel model = loadObjectModel(index_, a.autoModel);
                SkinnedModel skin(model.skeleton);
                skin.add(model);
                Created c;
                c.actor = std::make_unique<SimActor>(std::move(skin));
                c.actor->transform = *origin;
                c.actor->model = renderer_.addObject(c.actor->skin.model, index_, true);
                c.instance = renderer_.addInstance(c.actor->model, matrix_identity_float4x4);
                actor = c.actor.get();
                created_.push_back(std::move(c));
            }
            const int16_t id = actorIds_[i];
            double at = now;
            if (guessSims_ && !actor && id && a.autoAnimations.empty() && host_.sim && host_.sim(id)) {
                SimActor* s = host_.sim(id);
                std::string sceneName = sc.name.substr(0, sc.name.rfind("_cine"));
                std::vector<std::string> scenes{sceneName};
                if (sceneName.ends_with("Teen"))
                    scenes.push_back(sceneName.substr(0, sceneName.size() - 4));
                bool found = false;
                for (const auto& sn : scenes)
                    for (const char* ages : {sn.size() < sceneName.size() || sceneName.ends_with("Teen") ? "t2t" : "a2a",
                                             "a2a", "t2t", "a2o", "a2b", "a2c", "t", "a", "c", "e", "p", "b"}) {
                        const std::string n = std::string(ages) + "-cinematic-" + a.name + "-" + sn + "_anim";
                        if (!found && index_.findByName(n, 0xFB00791E)) {
                            const Animation& anim = s->animation(index_, n);
                            s->cue(at, anim, {.fade = 0.1f, .loop = false, .mirror = mirror_});
                            found = true;
                            if (debug())
                                std::fprintf(stderr, "opensims2: cinematic actor %s plays %s\n", a.name.c_str(), n.c_str());
                        }
                    }
            }
            for (const auto& n : a.autoAnimations) {
                if (actor) {
                    const Animation& anim = actor->animation(index_, n);
                    actor->cue(at, anim, {.fade = 0.0f, .loop = false, .mirror = mirror_});
                    at += anim.duration / 1000.0;
                } else if (id) {
                    SimActor* s = host_.sim ? host_.sim(id) : nullptr;
                    if (s) {
                        const Animation& anim = s->animation(index_, n);
                        s->cue(at, anim, {.fade = 0.1f, .loop = false, .mirror = mirror_});
                        at += anim.duration / 1000.0;
                        if (a.autoOrigin && !moved_.count(s)) {
                            moved_[s] = s->transform;
                            s->transform = *origin;
                        }
                    } else if (host_.objectAnimation) {
                        SimActor* o = host_.objectAnimation(id, n, at);
                        if (o && mirror_) // flipped like the rest of the scene
                            for (auto& cue : o->cues)
                                if (std::abs(cue.at - at) < 1e-9)
                                    cue.options.mirror = true;
                        at += loadAnimation(index_, n).duration / 1000.0;
                        if (o && a.autoOrigin && !moved_.count(o)) {
                            moved_[o] = o->transform;
                            o->transform = *origin;
                        }
                    }
                }
            }
            if (debug())
                std::fprintf(stderr, "opensims2: cinematic actor %s: object %d%s, %zu animation(s)\n", a.name.c_str(), id,
                             actor ? " (created)" : "", a.autoAnimations.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: cinematic actor %s: %s\n", a.name.c_str(), e.what());
        }
    }
    update(now);
    return true;
}

void CinematicPlayer::update(double now) {
    if (!started_)
        return;
    for (auto& c : created_) {
        c.actor->update(now);
        renderer_.updateObject(c.actor->model, c.actor->posed);
    }
    for (auto& g : rigOut_)
        g.clear();
    if (rigAnims_.empty() || rigLights_.empty())
        return;
    double t = std::max(0.0, now - start_);
    size_t k = 0;
    while (k + 1 < rigAnims_.size() && t >= rigAnims_[k]->duration / 1000.0) {
        t -= rigAnims_[k]->duration / 1000.0;
        ++k;
    }
    const Animation& anim = *rigAnims_[k];
    const float ticks = float(std::min(t, anim.duration / 1000.0) * 1000.0);
    const Animation::Target* target = anim.findTarget("lightrig");
    const std::vector<Xform> local = target ? samplePose(*target, rigSkeleton_, ticks) : rigSkeleton_.restPose();
    const std::vector<Xform> world = rigSkeleton_.world(local);
    const Xform& origin = camera_->origin();
    for (const auto& l : rigLights_) {
        if (l.group < 1 || l.group > 3)
            continue;
        const float scale = lightChannel(anim, l.name, "diffuseScale", 0, ticks, 0.0f);
        if (scale <= 0.001f)
            continue; // the rig's unused lights stay off
        Light out;
        Vec3 p = world[size_t(l.bone)].t;
        if (mirror_)
            p[0] = -p[0];
        out.position = origin.apply(p);
        for (size_t c = 0; c < 3; ++c)
            out.color[c] = lightChannel(anim, l.name, "diffuse", c, ticks, l.color[c]) * scale;
        out.falloff = l.falloff;
        // cPointLight::OnLightRadiantPropertiesChanged (BV 0xca0e48): the
        // range where I / (1 + C d^2) reaches lightRangeCutoff (0.275), I the
        // length of DiffuseScale x colour; no falloff: 10000.
        const float I = std::sqrt(out.color[0] * out.color[0] + out.color[1] * out.color[1] + out.color[2] * out.color[2]);
        constexpr float kCutoff = 0.275f; // Lighting.txt lightRangeCutoff
        out.range = I / kCutoff <= 1 ? 0.0f : out.falloff <= 0 ? 10000.0f : std::sqrt((I / kCutoff - 1) / out.falloff);
        rigOut_[size_t(l.group)].push_back(out);
    }
}

void CinematicPlayer::finish() {
    for (auto& c : created_)
        if (c.instance >= 0)
            renderer_.setInstanceTransform(c.instance, hidden());
    created_.clear();
    for (auto& [actor, before] : moved_)
        actor->transform = before;
    moved_.clear();
    for (auto& g : rigOut_)
        g.clear();
}

std::vector<const SimActor*> CinematicPlayer::createdActors() const {
    std::vector<const SimActor*> out;
    for (const auto& c : created_)
        out.push_back(c.actor.get());
    return out;
}

std::vector<CinematicPlayer::Light> CinematicPlayer::lightsFor(int16_t id) const {
    std::vector<Light> out;
    if (!id)
        return out;
    const CinematicScene& sc = camera_->scene();
    for (size_t i = 0; i < actorIds_.size() && i < sc.actors.size(); ++i) {
        if (actorIds_[i] != id)
            continue;
        for (int g = 1; g <= 3; ++g)
            if (sc.actors[i].renderGroups & (1u << (g - 1)))
                out.insert(out.end(), rigOut_[size_t(g)].begin(), rigOut_[size_t(g)].end());
    }
    return out;
}

} // namespace os2
