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
constexpr uint64_t kLocalSpace = 0x3;     // bits 0/1: particles follow the effect (inferred)
constexpr uint64_t kMaintain = 0x2;       // bit 1: the rate is the count kept alive (GenerateNewParticles BV 0xeb8c90)
constexpr uint64_t kEllipsoid = 1u << 8;  // emit volume is an ellipsoid (OpenTS2)
constexpr uint64_t kRandomWalk = 1u << 20; // set on every effect with a real random walk
constexpr uint64_t kIcon = 1u << 24;      // balloon icon: the texture is replaced
// Meta particle flags (MetaParticles::flags, the binary's numbering; what
// they meant here was inferred from the data).
constexpr uint64_t kMetaKeepCount = mflag::RandomWalk;           // the rate is how many are kept alive (fish, cockroaches)
constexpr uint64_t kMetaContinues = mflag::Attractor | (1ull << 28); // continues the meta component before it
constexpr uint64_t kMetaBounded = mflag::Maintain;               // kept inside the first step's emit volume (the fish, not the cockroaches)
constexpr size_t kMaxMetaParticles = 64;          // per chain

V3 add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(V3 a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float len(V3 a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
V3 sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

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

void EffectSystem::addEffect(Instance& inst, uint32_t visual, const Placement& local, float emitScale,
                             float sizeScale, float alphaScale, float timeScale, float delay, float stopAt,
                             int depth) {
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
    MetaChain* chain = nullptr; // the meta component just added (a continuation joins it)
    for (size_t i = 0; i < comps.size(); ++i) {
        const Component& c = comps[i];
        if (c.selectionGroup && chosen[c.selectionGroup] != int(i))
            continue;
        if (c.type != Block::MetaParticles)
            chain = nullptr;
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
            addEffect(inst, uint32_t(c.index), here, es, ss, as, ts, delay, stopAt, depth + 1);
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
                e.order = inst.orderNext++;
                inst.emitters.push_back(std::move(e));
            }
            break;
        case Block::MetaParticles:
            // Meta particles carry whole effects (each its own instance),
            // moving as particles; see MetaChain.
            if (c.index >= 0 && size_t(c.index) < lib_.metaParticles.size()) {
                const MetaParticles& m = lib_.metaParticles[size_t(c.index)];
                const auto base = lib_.indexOf(m.baseEffect);
                if (chain && (m.flags & kMetaContinues)) {
                    chain->steps.push_back(&m);
                    chain->bases.push_back(base ? int(*base) : -1);
                    break;
                }
                MetaChain mc;
                mc.steps.push_back(&m);
                mc.bases.push_back(base ? int(*base) : -1);
                mc.local = here;
                mc.emitScale = es, mc.sizeScale = ss, mc.alphaScale = as, mc.timeScale = ts;
                mc.startDelay = delay;
                mc.stopAt = stopAt;
                mc.orderBase = inst.orderNext;
                inst.orderNext += 16;
                inst.metas.push_back(std::move(mc));
                chain = &inst.metas.back();
                (void)depth;
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
                    chain = nullptr; // (the vector may have moved)
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
        case Block::Sound:
            if (c.index >= 0 && size_t(c.index) < lib_.sounds.size())
                inst.sounds.push_back({&lib_.sounds[size_t(c.index)], here, delay, stopAt, 0});
            break;
        default: break; // cameras, screens, water: not here (screens: the host's)
        }
    }
}

int EffectSystem::start(std::string_view name, const Placement& where) {
    auto visual = lib_.indexOf(name);
    if (!visual)
        return -1;
    Instance inst;
    inst.where = where;
    inst.id = nextId_++;
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
    // Steady meta emitters likewise (the fish are swimming already); the
    // others get their first ones now.
    for (auto& m : inst.metas) {
        if (m.startDelay > 0 || m.steps[0]->rateCurveCycles != 0 || m.steps[0]->preRoll <= 0) {
            stepChain(inst, m, 0, true);
            continue;
        }
        float left = std::min(m.steps[0]->preRoll, kMaxPreRoll);
        while (left > 0) {
            const float dt = std::min(left, kMaxStep);
            stepChain(inst, m, dt, true);
            left -= dt;
        }
    }
    const int h = next_++;
    instances_.emplace(h, std::move(inst));
    return h;
}

void EffectSystem::setState(int handle, int state) {
    if (auto it = instances_.find(handle); it != instances_.end())
        it->second.state = std::clamp(state, 0, 3);
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
    p.spin = random(d.rotateOffset[0], d.rotateOffset[1]) * kTwoPi;
    e.particles.push_back(p);
}

void EffectSystem::stepEmitter(const Instance& inst, Emitter& e, float dt, bool emit) {
    const Particles& d = *e.desc;
    dt *= e.timeScale;
    e.time += dt;
    const float active = e.time - e.startDelay;
    if (emit && active >= 0 && (e.stopAt < 0 || e.time < e.stopAt)) {
        // Rate curve over rateCurveTime seconds, repeated rateCurveCycles
        // times (0: forever); particles per second.
        const float period = d.rateCurveTime > 0 ? d.rateCurveTime : 1;
        const bool done = d.rateCurveCycles > 0 && active >= period * float(d.rateCurveCycles);
        if (d.flags & kMaintain) {
            // Topped up after the physics below.
        } else if (!done) {
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
    const float drag = d.drag;
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
    // cTSParticlesEffect::MaintainParticles (BV 0xeb822c): flag bit 1 makes
    // the rate curve's first value the number of particles kept alive, the
    // dead replaced at once (one plumbob glow per component, one build-mode
    // cursor post, 100 abduction stars).
    if (emit && (d.flags & kMaintain) && active >= 0 && (e.stopAt < 0 || e.time < e.stopAt)) {
        const size_t want = size_t(std::lround(std::max(sampleCurve(d.rateCurve, 0, 0), 0.0f)));
        while (e.particles.size() < std::min(want, kMaxParticles))
            spawn(inst, e);
    }
}

void EffectSystem::enterStep(MetaChain& m, MetaParticle& p, size_t step, int depth) {
    const MetaParticles& d = *m.steps[step];
    p.step = step;
    p.age = 0;
    p.life = std::max(0.01f, random(d.life[0], d.life[1]));
    p.sizeScale = random(1 - d.sizeVary, 1 + d.sizeVary);
    p.walkTimer = 0;
    // Its carried effect (a new one when the step carries another, the
    // fish's dead model).
    const int base = m.bases[step];
    if (!p.child || step == 0 || base != m.bases[step - 1]) {
        p.child = std::make_unique<Instance>();
        p.child->id = nextId_++;
        if (base >= 0)
            addEffect(*p.child, uint32_t(base), Placement{}, m.emitScale, m.sizeScale, m.alphaScale, m.timeScale, 0, -1,
                      depth + 1);
    }
}

void EffectSystem::spawnMeta(Instance& inst, MetaChain& m) {
    if (m.particles.size() >= kMaxMetaParticles)
        return;
    const MetaParticles& d = *m.steps[0];
    MetaParticle p;
    V3 u;
    for (int k = 0; k < 3; ++k)
        u[size_t(k)] = random(d.emitVolume.lo[size_t(k)], d.emitVolume.hi[size_t(k)]);
    p.position = u;
    V3 dir;
    for (int k = 0; k < 3; ++k)
        dir[size_t(k)] = random(d.emitDirection.lo[size_t(k)], d.emitDirection.hi[size_t(k)]);
    const float l = len(dir), speed = random(d.emitSpeed[0], d.emitSpeed[1]);
    p.velocity = l > 1e-6f ? mul(dir, speed / l) : V3{0, 0, 0};
    p.yaw = random(0, kTwoPi);
    enterStep(m, p, std::min<size_t>(size_t(inst.state), m.steps.size() - 1), 0);
    m.particles.push_back(std::move(p));
}

// A meta particle's placement in the world, for its carried effect.
void EffectSystem::placeMeta(const Instance& inst, const MetaChain& m, MetaParticle& p) const {
    const MetaParticles& d = *m.steps[p.step];
    const float t = std::min(1.0f, p.age / p.life);
    Placement local;
    local.position = p.position;
    if (d.alignment == 2 || d.alignment == 3) {
        // Facing its motion: the model's +X along the velocity (alignment
        // 2 also pitches, 3 stays level; inferred from the fish, whose
        // models are long in X, and the cockroaches).
        const float cy = std::cos(p.yaw), sy = std::sin(p.yaw);
        const float pitch = d.alignment == 2 ? p.pitch : 0;
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        // Rz(yaw) * Ry(-pitch): columns are the local axes.
        local.rotation = {cy * cp, -sy, -cy * sp, sy * cp, cy, -sy * sp, sp, 0, cp};
    }
    local.scale = std::max(0.0f, sampleCurve(d.sizeCurve, t, 1) * p.sizeScale);
    if (p.child) {
        p.child->where = compose(inst.where, compose(m.local, local));
        p.child->alpha = inst.alpha * std::clamp(sampleCurve(d.alphaCurve, t, 1), 0.0f, 1.0f);
        p.child->icon = inst.icon;
        p.child->iconFrame = inst.iconFrame;
        p.child->stopped = inst.stopped;
    }
}

void EffectSystem::stepChain(Instance& inst, MetaChain& m, float dt, bool emit) {
    dt *= m.timeScale;
    m.time += dt;
    const MetaParticles& root = *m.steps[0];
    const float active = m.time - m.startDelay;
    // Births, in the first step and in state 0 only.
    if (emit && inst.state == 0 && active >= 0 && (m.stopAt < 0 || m.time < m.stopAt)) {
        const float period = root.rateCurveTime > 0 ? root.rateCurveTime : 1;
        const bool done = root.rateCurveCycles > 0 && active >= period * float(root.rateCurveCycles);
        const float rate = sampleCurve(root.rateCurve, std::fmod(std::max(active, 0.0f), period) / period, 0) * m.emitScale;
        if (root.flags & kMetaKeepCount) {
            // Kept alive: the rate is the count (one fish per fish chain).
            const size_t want = size_t(std::lround(std::max(rate, 0.0f)));
            while (!done && m.particles.size() < std::min(want, kMaxMetaParticles))
                spawnMeta(inst, m);
        } else if (!done) {
            m.pending += rate * dt;
            if (root.rateCurveCycles > 0 && active - dt <= 0 && m.pending < 1 && rate > 0)
                m.pending = 1; // a burst's first one at once (the balloons)
            while (m.pending >= 1) {
                spawnMeta(inst, m);
                m.pending -= 1;
            }
        }
    }
    const V3 lo = root.emitVolume.lo, hi = root.emitVolume.hi;
    for (auto& p : m.particles) {
        // The effect's state moves its particles to that step of the chain.
        const size_t want = std::min<size_t>(size_t(inst.state), m.steps.size() - 1);
        if (inst.state > 0 && p.step != want)
            enterStep(m, p, want, 0);
        else if (inst.state == 0 && p.step != 0 && m.steps[p.step]->life[1] > 1000)
            enterStep(m, p, 0, 0); // back to swimming after feeding (steps with an endless life)
        p.age += dt;
        if (p.age >= p.life) {
            // Its life in this step is over: the next step, if any.
            if (p.step + 1 < m.steps.size() && inst.state == 0)
                enterStep(m, p, p.step + 1, 0);
            else {
                p.child.reset();
                continue;
            }
        }
        const MetaParticles& d = *m.steps[p.step];
        const float t = std::min(1.0f, p.age / p.life);
        // The random walk: every delay a new heading, turned from the last by
        // up to turn[0] of a full turn, climbing by up to turn[2] (slope), at a
        // speed in the strength range (inferred: fish meander slowly, the
        // cockroaches dash; turn[1] is not used).
        const auto& w = d.walks[0];
        if (w.strength[1] > 0 && (p.walkTimer -= dt) <= 0) {
            p.walkTimer = std::max(0.05f, random(w.delay[0], w.delay[1]));
            const float speed = random(w.strength[0], w.strength[1]);
            float heading = std::atan2(p.velocity[1], p.velocity[0]);
            if (std::hypot(p.velocity[0], p.velocity[1]) < 1e-5f)
                heading = p.yaw;
            heading += random(-w.turn[0], w.turn[0]) * kTwoPi;
            const float slope = random(-w.turn[2], w.turn[2]);
            V3 v{std::cos(heading), std::sin(heading), slope};
            const float lv = len(v);
            p.velocity = mul(v, speed / lv);
        }
        // Attractor: pulled towards its point (in the chain's frame) by the
        // strength over the life, within its range.
        if (!d.attractorStrength.empty()) {
            const V3 to = sub(d.attractor, p.position);
            const float dist = len(to);
            const float range = d.attractorRange > 0 ? d.attractorRange : 1e9f;
            if (dist > 1e-4f && dist < range)
                p.velocity = add(p.velocity, mul(to, sampleCurve(d.attractorStrength, t, 0) * dt / dist));
        }
        p.velocity = add(p.velocity, mul(d.force, dt));
        p.velocity = mul(p.velocity, std::max(0.0f, 1 - d.drag * dt));
        p.position = add(p.position, mul(p.velocity, dt));
        // Kept inside the first step's volume: turned back at its sides.
        if (root.flags & kMetaBounded)
            for (size_t k = 0; k < 3; ++k) {
                if (p.position[k] < lo[k] && p.velocity[k] < 0)
                    p.velocity[k] = -p.velocity[k];
                if (p.position[k] > hi[k] && p.velocity[k] > 0)
                    p.velocity[k] = -p.velocity[k];
                p.position[k] = std::clamp(p.position[k], lo[k] - 0.02f, hi[k] + 0.02f);
            }
        // Facing, turned smoothly towards the motion (the damping: turns per
        // second, default 1; inferred).
        const float horiz = std::hypot(p.velocity[0], p.velocity[1]);
        if (horiz > 1e-4f || std::abs(p.velocity[2]) > 1e-4f) {
            const float yaw = std::atan2(p.velocity[1], p.velocity[0]);
            const float pitch = std::atan2(p.velocity[2], horiz);
            if (!p.faced) {
                p.yaw = yaw, p.pitch = pitch, p.faced = true;
            } else {
                const float rate = (d.alignDamp > 0 ? d.alignDamp : 1.0f) * kTwoPi * dt;
                const float dy = std::remainder(yaw - p.yaw, kTwoPi);
                p.yaw += std::clamp(dy, -rate, rate);
                p.pitch += std::clamp(pitch - p.pitch, -rate, rate);
            }
        }
        placeMeta(inst, m, p);
        if (p.child)
            stepInstance(*p.child, dt, emit);
    }
    std::erase_if(m.particles, [](const MetaParticle& p) { return !p.child; });
}

void EffectSystem::stepInstance(Instance& inst, float dt, bool emit) {
    for (auto& e : inst.emitters)
        stepEmitter(inst, e, dt, emit);
    for (auto& dp : inst.decals)
        dp.age += dt;
    for (auto& mp : inst.models)
        mp.age += dt;
    for (auto& sp : inst.sounds)
        sp.age += dt;
    for (auto& m : inst.metas)
        stepChain(inst, m, dt, emit);
}

bool EffectSystem::busy(const Instance& inst) {
    for (const auto& dp : inst.decals) {
        const float life = dp.desc->life;
        if (dp.age < dp.delay || (life > 0 ? dp.age - dp.delay < life : !inst.stopped))
            return true;
    }
    for (const auto& mp : inst.models)
        if (!inst.stopped && (mp.stopAt < 0 || mp.age < mp.stopAt))
            return true;
    // A sound keeps its effect while it is due (an effect of only a sound runs until stopped).
    for (const auto& sp : inst.sounds)
        if (!inst.stopped && (sp.stopAt < 0 || sp.age < sp.stopAt))
            return true;
    for (const auto& e : inst.emitters) {
        const auto& d = *e.desc;
        const float period = d.rateCurveTime > 0 ? d.rateCurveTime : 1;
        const bool emitting = !inst.stopped && (e.stopAt < 0 || e.time < e.stopAt) &&
                              (d.rateCurveCycles == 0 || e.time - e.startDelay < period * float(d.rateCurveCycles));
        if (!e.particles.empty() || emitting)
            return true;
    }
    for (const auto& m : inst.metas) {
        for (const auto& p : m.particles)
            if (p.child && busy(*p.child))
                return true;
        const auto& d = *m.steps[0];
        const float period = d.rateCurveTime > 0 ? d.rateCurveTime : 1;
        if (!inst.stopped && (m.stopAt < 0 || m.time < m.stopAt) &&
            (d.rateCurveCycles == 0 || m.time - m.startDelay < period * float(d.rateCurveCycles)))
            return true;
    }
    return false;
}

void EffectSystem::update(float seconds) {
    if (!(seconds > 0))
        return;
    seconds = std::min(seconds, 1.0f);
    for (auto it = instances_.begin(); it != instances_.end();) {
        Instance& inst = it->second;
        float left = seconds;
        while (left > 0) {
            const float dt = std::min(left, kMaxStep);
            stepInstance(inst, dt, !inst.stopped);
            left -= dt;
        }
        if (!busy(inst))
            it = instances_.erase(it);
        else
            ++it;
    }
}

void EffectSystem::collectModelsOf(int handle, const Instance& inst, std::vector<EffectModel>& out) const {
    for (size_t i = 0; i < inst.models.size(); ++i) {
        const ModelPart& m = inst.models[i];
        if (inst.stopped || m.age < m.delay || (m.stopAt >= 0 && m.age >= m.stopAt))
            continue;
        EffectModel em;
        em.handle = handle;
        em.part = inst.id * 64 + int(i % 64);
        em.model = &m.desc->model;
        em.where = compose(inst.where, m.local);
        em.where.scale *= m.desc->size * m.sizeScale;
        em.color = {m.desc->color[0], m.desc->color[1], m.desc->color[2], m.desc->alpha * m.alphaScale * inst.alpha};
        if (em.where.scale > 0)
            out.push_back(em);
    }
    for (const auto& mc : inst.metas)
        for (const auto& p : mc.particles)
            if (p.child)
                collectModelsOf(handle, *p.child, out);
}

void EffectSystem::collectSoundsOf(int handle, const Instance& inst, std::vector<EffectSound>& out) const {
    for (size_t i = 0; i < inst.sounds.size(); ++i) {
        const SoundPart& sp = inst.sounds[i];
        if (inst.stopped || sp.age < sp.delay || (sp.stopAt >= 0 && sp.age >= sp.stopAt))
            continue;
        // (part: a key unique among the instance's and its meta particles' sounds)
        out.push_back({handle, inst.id * 64 + int(i % 64), sp.desc->audioId,
                       compose(inst.where, sp.local).position});
    }
    for (const auto& mc : inst.metas)
        for (const auto& p : mc.particles)
            if (p.child)
                collectSoundsOf(handle, *p.child, out);
}

void EffectSystem::collectSounds(std::vector<EffectSound>& out) const {
    for (const auto& [h, inst] : instances_)
        collectSoundsOf(h, inst, out);
}

void EffectSystem::collectModels(std::vector<EffectModel>& out) const {
    for (const auto& [h, inst] : instances_)
        collectModelsOf(h, inst, out);
}

void EffectSystem::collect(std::vector<ParticleQuad>& out) const {
    for (const auto& [h, inst] : instances_)
        collectInstance(inst, inst.where.position, 0, out);
}

void EffectSystem::collectInstance(const Instance& inst, const V3& sortRoot, int orderBase,
                                   std::vector<ParticleQuad>& out) const {
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
            q.color[3] = std::clamp(sampleCurve(d.alphaCurve, t, 1) * dp.alphaScale * inst.alpha, 0.0f, 1.0f);
            q.texture = &d.texture;
            q.draw = d.drawType <= 7 ? DrawType(d.drawType) : DrawType::Decal;
            q.alignment = 1; // flat on the ground
            if (q.color[3] > 0.001f && size > 0)
                out.push_back(q);
        }
    for (size_t ei = 0; ei < inst.emitters.size(); ++ei) {
            const Emitter& e = inst.emitters[ei];
            const Particles& d = *e.desc;
            const std::string* tex = e.icon && !inst.icon.empty() ? &inst.icon : &d.material;
            const float scale = inst.where.scale * e.local.scale;
            for (const auto& p : e.particles) {
                const float t = p.age / p.life;
                ParticleQuad q;
                q.position = toWorld(inst, e, p.position);
                // Particles that move with the effect (balloons) sort by the
                // effect's root position, then in component order.
                q.sortPosition = e.localSpace ? sortRoot : q.position;
                q.sortOrder = e.localSpace ? orderBase + e.order : 0;
                q.velocity = e.localSpace ? rotate(inst.where, p.velocity) : p.velocity;
                const float size = sampleCurve(d.sizeCurve, t, 1) * p.sizeScale * e.sizeScale * scale;
                q.width = size;
                q.height = size * sampleCurve(d.aspectCurve, t, 1) * p.aspectScale;
                q.rotation = p.spin + sampleCurve(d.rotateCurveX, t, 0) * kTwoPi;
                const V3 c = sampleColors(d.colors, t);
                for (int k = 0; k < 3; ++k)
                    q.color[size_t(k)] = std::clamp(c[size_t(k)] * p.colorScale[size_t(k)], 0.0f, 1.0f);
                q.color[3] = std::clamp(sampleCurve(d.alphaCurve, t, 1) * p.alphaScale * e.alphaScale * inst.alpha, 0.0f, 1.0f);
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
    for (const auto& mc : inst.metas)
        for (const auto& p : mc.particles)
            if (p.child)
                collectInstance(*p.child, sortRoot, orderBase + mc.orderBase, out);
}

} // namespace os2::fx
