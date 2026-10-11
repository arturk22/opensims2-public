#include "render/particle_system.hpp"

#include <algorithm>
#include <cmath>

namespace os2::fx {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kMaxStep = 0.1f;      // physics sub-step (seconds)
constexpr float kMaxPreRoll = 12.0f;  // pre-roll simulated on start (seconds)
constexpr size_t kMaxParticles = 512; // per emitter

// Particle flag bits (Particles::flags), see docs/effects-mac.md.
constexpr uint64_t kLocalSpace = 0x3;     // bits 0/1: particles follow the effect
constexpr uint64_t kEllipsoid = 1u << 8;  // emit volume is an ellipsoid (OpenTS2)
constexpr uint64_t kRandomWalk = 1u << 20; // set on every effect with a real random walk
constexpr uint64_t kIcon = 1u << 24;      // balloon icon: the texture is replaced

V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float len(V3 a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

} // namespace

V3 rotate(const Placement& p, const V3& v) {
    const auto& r = p.rotation;
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
            r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

V3 transformPoint(const Placement& p, const V3& v) { return add(p.position, mul(rotate(p, v), p.scale)); }

Placement compose(const Placement& parent, const Placement& child) {
    Placement o;
    o.position = transformPoint(parent, child.position);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0;
            for (int k = 0; k < 3; ++k)
                s += parent.rotation[size_t(r * 3 + k)] * child.rotation[size_t(k * 3 + c)];
            o.rotation[size_t(r * 3 + c)] = s;
        }
    o.scale = parent.scale * child.scale;
    return o;
}

EffectSystem::EffectSystem(const EffectsLibrary& library, uint32_t seed) : lib_(library), rng_(seed) {}

float EffectSystem::random(float a, float b) {
    if (!(b > a))
        return a;
    return std::uniform_real_distribution<float>(a, b)(rng_);
}

float EffectSystem::metaAge(const Emitter& e) {
    if (!e.meta || e.metaLife <= 0)
        return -1;
    float age = (e.time - e.startDelay) / e.metaLife;
    if (e.meta->rateCurveCycles == 0 && age > 1)
        age = std::fmod(age, 1.0f); // a steady meta emitter: a new one each life (simplified)
    return age;
}

void EffectSystem::addEffect(Instance& inst, uint32_t visual, const Placement& local, float emitScale,
                             float sizeScale, float alphaScale, float timeScale, float delay, float stopAt,
                             int depth, const MetaParticles* meta, float metaLife) {
    if (visual >= lib_.visuals.size() || depth > 8)
        return;
    const auto& comps = lib_.visuals[visual].components;
    // Selection groups: one component of each group, by its chance (u16
    // weights; the remainder picks none).
    std::unordered_map<uint16_t, int> chosen;
    for (size_t i = 0; i < comps.size(); ++i) {
        const uint16_t g = comps[i].selectionGroup;
        if (!g || chosen.count(g))
            continue;
        float roll = random(0, 65535), acc = 0;
        int pick = -1;
        for (size_t k = i; k < comps.size(); ++k)
            if (comps[k].selectionGroup == g) {
                acc += comps[k].selectionChance;
                if (pick < 0 && roll < acc)
                    pick = int(k);
            }
        chosen[g] = pick;
    }
    for (size_t i = 0; i < comps.size(); ++i) {
        const Component& c = comps[i];
        if (c.selectionGroup && chosen[c.selectionGroup] != int(i))
            continue;
        Placement p;
        p.position = c.offset;
        p.rotation = c.rotation;
        p.scale = c.scale > 0 ? c.scale : 1;
        const Placement here = compose(local, p);
        // The scales at full detail (the component's LOD end).
        const float es = emitScale * c.emitScale[1], ss = sizeScale * c.sizeScale[1], as = alphaScale * c.alphaScale[1];
        const float ts = timeScale * (c.timeScale > 0 ? c.timeScale : 1);
        switch (c.type) {
        case Block::Visual:
            addEffect(inst, uint32_t(c.index), here, es, ss, as, ts, delay, stopAt, depth + 1, meta, metaLife);
            break;
        case Block::Particles:
            if (c.index >= 0 && size_t(c.index) < lib_.particles.size()) {
                Emitter e;
                e.desc = &lib_.particles[size_t(c.index)];
                e.local = here;
                e.emitScale = es, e.sizeScale = ss, e.alphaScale = as, e.timeScale = ts;
                e.icon = (e.desc->flags & kIcon) != 0;
                e.localSpace = (e.desc->flags & kLocalSpace) != 0;
                e.startDelay = delay;
                e.stopAt = stopAt;
                e.meta = meta;
                e.metaLife = metaLife;
                inst.emitters.push_back(std::move(e));
            }
            break;
        case Block::MetaParticles:
            // Meta particles carry whole effects. Drawn as one meta particle
            // at the component (the balloons' case: rate 1, one cycle) whose
            // size and alpha curves scale and fade the carried effect over
            // the meta particle's life.
            if (c.index >= 0 && size_t(c.index) < lib_.metaParticles.size()) {
                const auto& m = lib_.metaParticles[size_t(c.index)];
                if (auto base = lib_.indexOf(m.baseEffect))
                    addEffect(inst, *base, here, es, ss, as, ts, delay, stopAt, depth + 1, &m,
                              std::max(0.01f, random(m.life[0], m.life[1])));
            }
            break;
        case Block::Sequence:
            // Items play one after another, each for a time in its range
            // (-1: until the effect stops).
            if (c.index >= 0 && size_t(c.index) < lib_.sequences.size()) {
                float t = delay;
                for (const auto& item : lib_.sequences[size_t(c.index)].items) {
                    auto child = lib_.indexOf(item.effect);
                    const float d = item.activateTime[0] < 0 ? -1 : random(item.activateTime[0], item.activateTime[1]);
                    if (child)
                        addEffect(inst, *child, here, es, ss, as, ts, t, d < 0 ? stopAt : t + d, depth + 1);
                    if (d < 0)
                        break;
                    t += d;
                }
            }
            break;
        case Block::Decal:
            if (c.index >= 0 && size_t(c.index) < lib_.decals.size())
                inst.decals.push_back({&lib_.decals[size_t(c.index)], here, ss, as, delay, 0});
            break;
        case Block::Model:
            if (c.index >= 0 && size_t(c.index) < lib_.models.size())
                inst.models.push_back({&lib_.models[size_t(c.index)], here, ss, as, delay, stopAt, 0});
            break;
        default: break; // sounds, cameras, screens, water: not here (screens: the host's)
        }
    }
}

int EffectSystem::start(std::string_view name, const Placement& where) {
    auto visual = lib_.indexOf(name);
    if (!visual)
        return -1;
    Instance inst;
    inst.where = where;
    addEffect(inst, *visual, Placement{}, 1, 1, 1, 1, 0, -1, 0);
    // Pre-roll: run each emitter for its preRoll so steady effects (smoke,
    // glows) start in their steady state.
    for (auto& e : inst.emitters) {
        // Only steady emitters (rate cycles 0); a burst's pre-roll would
        // outlive its particles (balloons: life 4, pre-roll 4). Inferred.
        if (e.startDelay > 0 || e.desc->rateCurveCycles != 0)
            continue;
        float left = std::min(e.desc->preRoll, kMaxPreRoll);
        while (left > 0) {
            const float dt = std::min(left, kMaxStep);
            stepEmitter(inst, e, dt, true);
            left -= dt;
        }
    }
    const int h = next_++;
    instances_.emplace(h, std::move(inst));
    return h;
}

void EffectSystem::stop(int handle, bool hard) {
    auto it = instances_.find(handle);
    if (it == instances_.end())
        return;
    if (hard)
        instances_.erase(it);
    else
        it->second.stopped = true;
}

void EffectSystem::move(int handle, const Placement& where) {
    if (auto it = instances_.find(handle); it != instances_.end())
        it->second.where = where;
}

void EffectSystem::setIcon(int handle, std::string texture, int frame) {
    if (auto it = instances_.find(handle); it != instances_.end()) {
        it->second.icon = std::move(texture);
        it->second.iconFrame = frame;
    }
}

bool EffectSystem::alive(int handle) const { return instances_.count(handle) != 0; }

V3 EffectSystem::toWorld(const Instance& inst, const Emitter& e, const V3& p) const {
    return e.localSpace ? transformPoint(inst.where, p) : p;
}

void EffectSystem::spawn(const Instance& inst, Emitter& e) {
    const Particles& d = *e.desc;
    if (e.particles.size() >= kMaxParticles)
        return;
    Particle p;
    // Birth point inside the emit volume (box, or the ellipsoid inside it).
    V3 u;
    if (d.flags & kEllipsoid) {
        do
            u = {random(-1, 1), random(-1, 1), random(-1, 1)};
        while (u[0] * u[0] + u[1] * u[1] + u[2] * u[2] > 1);
        for (int k = 0; k < 3; ++k)
            u[k] = (d.emitVolume.lo[k] + d.emitVolume.hi[k]) * 0.5f + u[k] * (d.emitVolume.hi[k] - d.emitVolume.lo[k]) * 0.5f;
    } else {
        for (int k = 0; k < 3; ++k)
            u[k] = random(d.emitVolume.lo[k], d.emitVolume.hi[k]);
    }
    // Direction: a random vector in the direction box, scaled to a speed
    // in the speed range (InitParticle's RandomVector3 + ScaleToMagnitude).
    V3 dir;
    for (int k = 0; k < 3; ++k)
        dir[k] = random(d.emitDirection.lo[k], d.emitDirection.hi[k]);
    const float l = len(dir);
    const float speed = random(d.emitSpeed[0], d.emitSpeed[1]);
    V3 v = l > 1e-6f ? mul(dir, speed / l) : V3{0, 0, 0};
    // Into the effect's frame; world-space particles take the instance's too.
    const Placement local = e.localSpace ? e.local : compose(inst.where, e.local);
    p.position = transformPoint(local, u);
    p.velocity = mul(rotate(local, v), local.scale);
    p.life = std::max(0.01f, random(d.life[0], d.life[1]));
    p.sizeScale = random(1 - d.sizeVary, 1 + d.sizeVary);
    p.aspectScale = random(1 - d.aspectVary, 1 + d.aspectVary);
    p.alphaScale = random(1 - d.alphaVary, 1 + d.alphaVary);
    for (int k = 0; k < 3; ++k)
        p.colorScale[size_t(k)] = random(1 - d.colorVary[size_t(k)], 1 + d.colorVary[size_t(k)]);
    p.walkTimer = random(d.randomWalkDelay[0], d.randomWalkDelay[1]);
    p.spin = random(d.rotateOffsetX, d.rotateOffsetY) * kTwoPi;
    e.particles.push_back(p);
}

void EffectSystem::stepEmitter(const Instance& inst, Emitter& e, float dt, bool emit) {
    const Particles& d = *e.desc;
    dt *= e.timeScale;
    e.time += dt;
    const float active = e.time - e.startDelay;
    const float ma = metaAge(e);
    if (ma > 1) { // the carrying meta particle died: so does its effect
        e.particles.clear();
        return;
    }
    if (emit && active >= 0 && (e.stopAt < 0 || e.time < e.stopAt)) {
        // Rate curve over rateCurveTime seconds, repeated rateCurveCycles
        // times (0: forever); particles per second.
        const float period = d.rateCurveTime > 0 ? d.rateCurveTime : 1;
        const bool done = d.rateCurveCycles > 0 && active >= period * float(d.rateCurveCycles);
        if (!done) {
            const float rate = sampleCurve(d.rateCurve, std::fmod(active, period) / period, 0) * e.emitScale;
            e.pending += rate * dt;
            // The first particle of a one-cycle burst comes out at once.
            if (d.rateCurveCycles > 0 && active - dt <= 0 && e.pending < 1 && rate > 0)
                e.pending = 1;
            while (e.pending >= 1) {
                spawn(inst, e);
                e.pending -= 1;
            }
        }
    }
    // Physics (UpdateParticlePhysicsTimeStep): position by velocity,
    // velocity by the constant force, then the drag factor 1 - drag x dt.
    // The force is in the effect's frame: a component's rotation turns its
    // emission (the stove's smoke billows out of the front) but not the
    // force (it still rises). Inferred.
    const V3 force = e.localSpace ? d.force : rotate(inst.where, d.force);
    const float drag = d.force2a;
    // (Not for particles that move with the effect: the balloons' leader
    // bubbles would wander off. Inferred.)
    const bool walk = (d.flags & kRandomWalk) != 0 && !e.localSpace;
    for (auto& p : e.particles) {
        p.age += dt;
        p.position = add(p.position, mul(p.velocity, dt));
        p.velocity = add(p.velocity, mul(force, dt));
        p.velocity = mul(p.velocity, std::max(0.0f, 1 - drag * dt));
        if (walk && (p.walkTimer -= dt) <= 0) {
            p.walkTimer = std::max(0.02f, random(d.randomWalkDelay[0], d.randomWalkDelay[1]));
            const float s = random(d.randomWalkStrength[0], d.randomWalkStrength[1]);
            const float a = random(0, kTwoPi);
            p.velocity = add(p.velocity, {std::cos(a) * s, std::sin(a) * s, 0});
        }
    }
    std::erase_if(e.particles, [](const Particle& p) { return p.age >= p.life; });
}

void EffectSystem::update(float seconds) {
    if (!(seconds > 0))
        return;
    seconds = std::min(seconds, 1.0f);
    for (auto it = instances_.begin(); it != instances_.end();) {
        Instance& inst = it->second;
        bool any = false;
        float left = seconds;
        while (left > 0) {
            const float dt = std::min(left, kMaxStep);
            for (auto& e : inst.emitters)
                stepEmitter(inst, e, dt, !inst.stopped);
            left -= dt;
        }
        for (auto& dp : inst.decals) {
            dp.age += seconds;
            const float life = dp.desc->life;
            if (dp.age < dp.delay || (life > 0 ? dp.age - dp.delay < life : !inst.stopped))
                any = true;
        }
        for (auto& mp : inst.models) {
            mp.age += seconds;
            if (!inst.stopped && (mp.stopAt < 0 || mp.age < mp.stopAt))
                any = true;
        }
        for (const auto& e : inst.emitters) {
            const auto& d = *e.desc;
            const float period = d.rateCurveTime > 0 ? d.rateCurveTime : 1;
            const bool emitting = !inst.stopped && metaAge(e) <= 1 && (e.stopAt < 0 || e.time < e.stopAt) &&
                                  (d.rateCurveCycles == 0 || e.time - e.startDelay < period * float(d.rateCurveCycles));
            if (!e.particles.empty() || emitting)
                any = true;
        }
        if (!any)
            it = instances_.erase(it);
        else
            ++it;
    }
}

void EffectSystem::collectModels(std::vector<EffectModel>& out) const {
    for (const auto& [h, inst] : instances_)
        for (size_t i = 0; i < inst.models.size(); ++i) {
            const ModelPart& m = inst.models[i];
            if (inst.stopped || m.age < m.delay || (m.stopAt >= 0 && m.age >= m.stopAt))
                continue;
            EffectModel em;
            em.handle = h;
            em.part = int(i);
            em.model = &m.desc->model;
            em.where = compose(inst.where, m.local);
            em.where.scale *= m.desc->size * m.sizeScale;
            em.color = {m.desc->color[0], m.desc->color[1], m.desc->color[2], m.desc->alpha * m.alphaScale};
            out.push_back(em);
        }
}

void EffectSystem::collect(std::vector<ParticleQuad>& out) const {
    for (const auto& [h, inst] : instances_)
        for (const auto& dp : inst.decals) {
            const Decal& d = *dp.desc;
            const float age = dp.age - dp.delay;
            if (age < 0 || (d.life > 0 && age >= d.life) || (d.life <= 0 && inst.stopped))
                continue;
            const float t = d.life > 0 ? age / d.life : 0;
            const Placement w = compose(inst.where, dp.local);
            ParticleQuad q;
            q.position = w.position;
            q.position[2] += 0.01f; // over the floor
            q.sortPosition = q.position;
            const float size = sampleCurve(d.sizeCurve, t, 1) * dp.sizeScale * w.scale;
            q.width = size;
            q.height = size * sampleCurve(d.aspectCurve, t, 1);
            q.rotation = sampleCurve(d.rotateCurve, t, 0) * kTwoPi;
            const V3 c = sampleColors(d.colors, t);
            for (int k = 0; k < 3; ++k)
                q.color[size_t(k)] = std::clamp(c[size_t(k)], 0.0f, 1.0f);
            q.color[3] = std::clamp(sampleCurve(d.alphaCurve, t, 1) * dp.alphaScale, 0.0f, 1.0f);
            q.texture = &d.texture;
            q.draw = d.drawType <= 7 ? DrawType(d.drawType) : DrawType::Decal;
            q.alignment = 1; // flat on the ground
            if (q.color[3] > 0.001f && size > 0)
                out.push_back(q);
        }
    for (const auto& [h, inst] : instances_)
        for (size_t ei = 0; ei < inst.emitters.size(); ++ei) {
            const Emitter& e = inst.emitters[ei];
            const Particles& d = *e.desc;
            const std::string* tex = e.icon && !inst.icon.empty() ? &inst.icon : &d.material;
            const float ma = metaAge(e);
            if (ma > 1 || (e.meta && e.time < e.startDelay))
                continue;
            const float metaSize = ma >= 0 ? sampleCurve(e.meta->sizeCurve, ma, 1) : 1;
            const float metaAlpha = ma >= 0 ? sampleCurve(e.meta->alphaCurve, ma, 1) : 1;
            const float scale = inst.where.scale * e.local.scale * metaSize;
            for (const auto& p : e.particles) {
                const float t = p.age / p.life;
                ParticleQuad q;
                q.position = toWorld(inst, e, p.position);
                q.sortPosition = e.localSpace ? inst.where.position : q.position;
                q.sortOrder = e.localSpace ? int(ei) : 0;
                q.velocity = e.localSpace ? rotate(inst.where, p.velocity) : p.velocity;
                const float size = sampleCurve(d.sizeCurve, t, 1) * p.sizeScale * e.sizeScale * scale;
                q.width = size;
                q.height = size * sampleCurve(d.aspectCurve, t, 1) * p.aspectScale;
                q.rotation = p.spin + sampleCurve(d.rotateCurveX, t, 0) * kTwoPi;
                const V3 c = sampleColors(d.colors, t);
                for (int k = 0; k < 3; ++k)
                    q.color[size_t(k)] = std::clamp(c[size_t(k)] * p.colorScale[size_t(k)], 0.0f, 1.0f);
                q.color[3] = std::clamp(sampleCurve(d.alphaCurve, t, 1) * p.alphaScale * e.alphaScale * metaAlpha, 0.0f, 1.0f);
                q.texture = tex;
                if (d.tileU > 1 || d.tileV > 1) {
                    const int cells = std::max(1, int(d.tileU) * int(d.tileV));
                    int frame = d.frameStart;
                    if (e.icon && inst.iconFrame >= 0)
                        frame = inst.iconFrame;
                    else if (d.frameCount > 0)
                        frame += int(p.age * d.frameSpeed) % d.frameCount;
                    frame %= cells;
                    const float du = 1.0f / std::max<uint8_t>(1, d.tileU), dv = 1.0f / std::max<uint8_t>(1, d.tileV);
                    const int cu = frame % std::max<uint8_t>(1, d.tileU), cv = frame / std::max<uint8_t>(1, d.tileU);
                    q.uv = {du * float(cu), dv * float(cv), du * float(cu + 1), dv * float(cv + 1)};
                }
                q.draw = d.drawType;
                q.alignment = d.alignment;
                q.layer = d.layer;
                if (q.color[3] > 0.001f && size > 0)
                    out.push_back(q);
            }
        }
}

} // namespace os2::fx
