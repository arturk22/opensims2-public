#include "render/particle_system.hpp"

#include <algorithm>
#include <cmath>

namespace os2::fx {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kMaxStep = 0.1f;      // meta particles' physics sub-step (seconds; inferred)
constexpr float kTimeStep = 1.0f / 15; // the particles' longest physics step and emission slice (BV 0x3d888889)
constexpr float kFastDrag = 0.05f;    // dt x drag above this takes the time-stepped physics (BV 0x3d4ccccd)
constexpr size_t kMaxParticles = 512; // per emitter (a guard of ours)

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
float sqrlen(V3 a) { return a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; }

} // namespace

float SwarmRandom::range(float a, float b) {
    state_ *= 0x278dde6du;
    const float unit = float(int32_t(state_)) * 2.3283064e-10f;
    return float((double(unit) + 0.5) * double(b - a) + double(a));
}

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

Placement inverse(const Placement& p) {
    Placement o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o.rotation[size_t(r * 3 + c)] = p.rotation[size_t(c * 3 + r)];
    o.scale = p.scale != 0 ? 1 / p.scale : 1;
    o.position = mul(rotate(o, p.position), -o.scale);
    return o;
}

EffectSystem::EffectSystem(const EffectsLibrary& library, uint32_t seed) : lib_(library), rng_(seed) {}

float EffectSystem::random(float a, float b) { return rng_.range(a, b); }

void EffectSystem::addEffect(Instance& inst, uint32_t visual, const Placement& local, float emitScale,
                             float sizeScale, float alphaScale, float timeScale, float delay, float stopAt,
                             int depth, bool rigid) {
    if (visual >= lib_.visuals.size() || depth > 8)
        return;
    // A rigid effect (cTSVisualEffect::SetTransforms BV 0xeb1760 with bit 5)
    // hands its transform to its components' source: their particles live
    // in its frame, and so in the instance's, which moves with it.
    rigid = rigid || (lib_.visuals[visual].flags & vflag::Rigid);
    if (depth == 0)
        inst.hardStop = (lib_.visuals[visual].flags & vflag::HardStop) != 0;
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
            addEffect(inst, uint32_t(c.index), here, es, ss, as, ts, delay, stopAt, depth + 1, rigid);
            break;
        case Block::Particles:
            if (c.index >= 0 && size_t(c.index) < lib_.particles.size()) {
                Emitter e;
                e.desc = &lib_.particles[size_t(c.index)];
                e.local = here;
                e.emitScale = es, e.sizeScale = ss, e.alphaScale = as, e.timeScale = ts;
                e.icon = (e.desc->flags & pflag::AcceptOverride) != 0;
                e.rigid = rigid;
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
                        addEffect(inst, *child, here, es, ss, as, ts, t, d < 0 ? stopAt : t + d, depth + 1, rigid);
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

int EffectSystem::start(std::string_view name, const Placement& where, bool hard) {
    auto visual = lib_.indexOf(name);
    if (!visual)
        return -1;
    Instance inst;
    inst.where = where;
    inst.id = nextId_++;
    addEffect(inst, *visual, Placement{}, 1, 1, 1, 1, 0, -1, 0, false);
    // cTSVisualEffect::Start starts every component with the same
    // transition; a sequence's later items start soft when their time comes
    // (inferred: cTSSequenceEffect not read). The first ApplyEffect runs at
    // once (with the pre-roll of a hard start), so the effect shows from the start.
    for (auto& e : inst.emitters)
        if (e.startDelay <= 0) {
            startEmitter(inst, e, hard);
            applyEmitter(inst, e, 0);
        }
    // Steady meta emitters likewise (the fish are swimming already); the
    // others get their first ones now.
    for (auto& m : inst.metas) {
        if (m.startDelay > 0 || m.steps[0]->rateCurveCycles != 0 || m.steps[0]->preRoll <= 0) {
            stepChain(inst, m, 0, true);
            continue;
        }
        float left = std::min(m.steps[0]->preRoll, 12.0f);
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
    // cTSVisualEffect::Stop (BV 0xeaff64): an effect with hardStop
    // always stops hard (the particles go at once).
    if (hard || it->second.hardStop)
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

Placement EffectSystem::particleFrame(const Instance& inst, const Emitter& e) {
    return e.rigid ? inst.where : inst.source;
}

Placement EffectSystem::emitterFrame(const Instance& inst, const Emitter& e) const {
    if (e.rigid)
        return e.local;
    return compose(inverse(inst.source), compose(inst.where, e.local));
}

// cTSParticlesEffect::Start (BV 0xeb726c).
void EffectSystem::startEmitter(const Instance& inst, Emitter& e, bool hard) {
    const Particles& d = *e.desc;
    // A hard start leaves out the emitters with a finite number of cycles
    // (bursts), unless they sustain.
    if (hard && d.rateCurveCycles != 0 && !(d.flags & pflag::Sustain))
        return;
    e.started = true;
    e.stopping = false;
    e.cycleTime = 0;
    e.cycleIndex = 0;
    e.rateRemainder = 0;
    e.lastPosition = emitterFrame(inst, e).position;
    e.accel = d.force;
    e.walk = {0, 0, 0};
    if (d.flags & pflag::RandomWalk) {
        e.walkTimer = 0;
        e.walkAngle = {random(-0.5f, 0.5f), random(-0.5f, 0.5f)};
        updateRandomWalk(e, 0);
    }
    // The wait before the first particles: the delay, else the trigger.
    if (d.rateDelay[0] >= 0)
        e.delayLeft = rng_.range(d.rateDelay);
    else if (d.rateTrigger[0] >= 0)
        e.delayLeft = rng_.range(d.rateTrigger);
    else
        e.delayLeft = 0;
    e.preRollPending = hard;
}

// cTSParticlesEffect::UpdateRandomWalk (BV 0xeb70dc): one walking force for
// the whole effect, a new heading and elevation every delay.
void EffectSystem::updateRandomWalk(Emitter& e, float dt) {
    const Particles& d = *e.desc;
    e.walkTimer -= dt;
    for (int guard = 0; e.walkTimer <= 0 && guard < 64; ++guard) {
        e.walkTimer += rng_.range(d.randomWalkDelay);
        for (float& a : e.walkAngle) {
            const float turn = random(-d.randomWalkTurnX, d.randomWalkTurnX);
            a += turn;
            a += turn >= 0 ? d.randomWalkTurnY : -d.randomWalkTurnY;
        }
        const float strength = rng_.range(d.randomWalkStrength);
        const float h = e.walkAngle[0] * kTwoPi, el = e.walkAngle[1] * kTwoPi;
        e.walk = {strength * std::cos(h) * std::cos(el), strength * std::sin(h) * std::cos(el),
                  strength * std::sin(el)};
    }
}

// cTSParticlesEffect::InitParticle (BV 0xeb746c).
EffectSystem::Particle EffectSystem::initParticle(const Instance& inst, const Emitter& e) {
    const Particles& d = *e.desc;
    Particle p;
    p.life = std::max(1e-3f, rng_.range(d.life));
    // Birth point: in the emit box, or the ellipsoid inside it.
    V3 u;
    if (d.flags & pflag::Ellipsoid) {
        do
            u = {random(-1, 1), random(-1, 1), random(-1, 1)};
        while (sqrlen(u) > 1);
        for (int k = 0; k < 3; ++k)
            u[k] = (d.emitVolume.lo[k] + d.emitVolume.hi[k]) * 0.5f + u[k] * (d.emitVolume.hi[k] - d.emitVolume.lo[k]) * 0.5f;
    } else {
        for (int k = 0; k < 3; ++k)
            u[k] = random(d.emitVolume.lo[k], d.emitVolume.hi[k]);
    }
    // Direction: in the direction box (or the birth point, radial), scaled
    // to a speed in the speed range (ScaleToMagnitude).
    V3 dir;
    if (d.flags & pflag::Radial)
        dir = u;
    else
        for (int k = 0; k < 3; ++k)
            dir[k] = random(d.emitDirection.lo[k], d.emitDirection.hi[k]);
    const float speed = rng_.range(d.emitSpeed);
    const float l = len(dir);
    V3 v = l > 1e-6f ? mul(dir, speed / l) : V3{0, 0, 0};
    // The bomb: a speed away from its centre.
    if (d.bomb != 0) {
        const V3 away = sub(u, d.bombCenter);
        if (sqrlen(away) > 0)
            v = add(v, mul(away, d.bomb / len(away)));
    }
    // Into the particles' frame: the point by the emitter's transform, the
    // velocity by its rotation.
    const Placement t1 = emitterFrame(inst, e);
    p.position = transformPoint(t1, u);
    p.velocity = rotate(t1, v);
    p.sizeScale = random(1 - d.sizeVary, 1 + d.sizeVary) * e.sizeScale;
    p.aspectScale = random(1 - d.aspectVary, 1 + d.aspectVary);
    p.alphaScale = random(1 - d.alphaVary, 1 + d.alphaVary);
    for (int k = 0; k < 3; ++k)
        p.colorScale[size_t(k)] = random(1 - d.colorVary[size_t(k)], 1 + d.colorVary[size_t(k)]);
    for (int i = 0; i < std::max<int>(1, d.rotateCount) && i < 2; ++i) {
        const V2& r = d.rotateRange[size_t(i)];
        p.rotate[size_t(i)] = r[1] <= r[0] ? r[0] : rng_.range(r);
    }
    p.frameStart = d.frameStart;
    return p;
}

// cTSParticlesEffect::UpdateParticlePhysics (BV 0xeb6ecc): false once the
// particle's life is over. With none of the flags that need forces and a
// gentle drag, one step (BV 0x1c2aca0); otherwise steps of 1/15 s
// (UpdateParticlePhysicsTimeStep 0xeb6d50, ApplyForces 0xeb6604).
bool EffectSystem::updatePhysics(const Emitter& e, Particle& p, float dt) const {
    const Particles& d = *e.desc;
    p.age += dt;
    if (p.age >= p.life)
        return false;
    const V3& a = e.accel;
    if (!(d.flags & pflag::NeedsTimeStep) && dt * d.drag <= kFastDrag) {
        const V3 v0 = p.velocity;
        const float keep = 1 - dt * d.drag;
        p.velocity = mul(add(v0, mul(a, dt)), keep);
        p.position = add(p.position, mul(add(v0, p.velocity), dt * 0.5f));
        return true;
    }
    float left = dt;
    do {
        const float step = left > kTimeStep ? kTimeStep : left;
        p.position = add(p.position, mul(p.velocity, step));
        p.velocity = add(p.velocity, mul(a, step));
        // ApplyForces: the attractor's pull (BV 0x1c2c98c: towards its
        // point, by the strength curve over distance / range), then the drag.
        if (e.attracts) {
            const V3 to = sub(e.attractorPoint, p.position);
            const float dist = len(to);
            if (dist > 0) {
                const float pull = sampleCurve(d.attractorStrength, std::min(dist / std::max(d.attractorRange, 1e-6f), 1.0f), 0);
                p.velocity = add(p.velocity, mul(to, step * pull / dist));
            }
        }
        p.velocity = mul(p.velocity, 1 - step * d.drag);
        left -= step;
    } while (left > 0);
    return true;
}

// cTSParticlesEffect::MaintainParticles (BV 0xeb822c): ceil(rate[0]) alive,
// the missing ones made at once and moved this frame.
void EffectSystem::maintainParticles(const Instance& inst, Emitter& e, float dt) {
    const Particles& d = *e.desc;
    const float period = std::max(d.rateCurveTime, 1e-6f);
    e.cycleTime += dt / period;
    if (e.cycleTime < e.cycleIndex)
        return;
    const size_t want = size_t(std::max(0.0f, std::ceil(sampleCurve(d.rateCurve, 0, 0))));
    while (e.particles.size() < std::min(want, kMaxParticles)) {
        Particle p = initParticle(inst, e);
        if (!updatePhysics(e, p, dt))
            break; // (born dead: the binary drops it and goes on; avoid looping)
        e.particles.push_back(p);
    }
}

// cTSParticlesEffect::InjectParticles (BV 0xeb83f4): at each cycle's start
// ceil(rate[0]) particles at once, aged since that start.
void EffectSystem::injectParticles(const Instance& inst, Emitter& e, float dt) {
    const Particles& d = *e.desc;
    const float period = std::max(d.rateCurveTime, 1e-6f);
    e.cycleTime += dt / period;
    if (e.cycleTime < e.cycleIndex)
        return;
    // The emission scale counts only past 20 a burst (where the density
    // also scales the size; the density is 1 here).
    const float first = sampleCurve(d.rateCurve, 0, 0);
    const float scale = first > 20 ? e.emitScale : 1;
    const int count = int(std::ceil(first * scale));
    for (int guard = 0; guard < 1000; ++guard) {
        while (e.cycleTime >= e.cycleIndex) {
            if (d.flags & pflag::CycleKill)
                e.particles.clear();
            const float age = float((e.cycleTime - e.cycleIndex) * period);
            if (age < d.life[1])
                for (int i = 0; i < count && e.particles.size() < kMaxParticles; ++i) {
                    Particle p = initParticle(inst, e);
                    if (updatePhysics(e, p, age))
                        e.particles.push_back(p);
                }
            ++e.cycleIndex;
            if (d.rateCurveCycles != 0 && int(d.rateCurveCycles) <= e.cycleIndex) {
                e.stopping = true;
                return;
            }
            if (d.rateTrigger[0] >= 0)
                break;
        }
        if (e.cycleTime < e.cycleIndex && d.rateTrigger[0] < 0)
            return;
        // A trigger: the clock waits that long before the next burst.
        e.cycleTime -= rng_.range(d.rateTrigger) / period;
        if (e.cycleTime >= e.cycleIndex)
            continue;
        e.delayLeft = float((e.cycleIndex - e.cycleTime) * period);
        e.cycleTime = e.cycleIndex;
        return;
    }
}

// cTSParticlesEffect::AddRateCurveParticles (BV 0xeb8728): the rate curve
// over each cycle, in slices of at most 1/15 s; each new particle is born
// spread over its slice, set back along the emitter's way this frame and
// already moved for the time it has lived.
void EffectSystem::addRateCurveParticles(const Instance& inst, Emitter& e, float dt) {
    const Particles& d = *e.desc;
    if (dt <= 0)
        return;
    const float period = std::max(d.rateCurveTime, 1e-6f);
    const Placement t1 = emitterFrame(inst, e);
    float scale = e.emitScale;
    if (d.flags & pflag::SizeScale)
        scale *= t1.scale;
    else if (d.flags & pflag::AreaScale)
        scale *= t1.scale * t1.scale;
    else if (d.flags & pflag::VolumeScale)
        scale *= t1.scale * t1.scale * t1.scale;
    const V3 moved = sub(t1.position, e.lastPosition);
    if (d.rateSpeedScale > 0)
        scale += len(moved) / dt * d.rateSpeedScale * scale;
    const float total = dt;
    float left = dt;
    for (int guard = 0; guard < 10000; ++guard) {
        float slice = left > kTimeStep ? kTimeStep : left;
        const int next = e.cycleIndex + 1;
        e.cycleTime += slice / period;
        bool triggered = false;
        if (e.cycleTime > next) {
            e.cycleIndex = next;
            if (d.flags & pflag::CycleKill)
                e.particles.clear();
            if (d.rateCurveCycles != 0 && int(d.rateCurveCycles) == e.cycleIndex) {
                if (!(d.flags & pflag::Sustain)) {
                    e.stopping = true;
                    return;
                }
                // Sustain: the last cycle's end held.
                e.cycleIndex = int(d.rateCurveCycles) - 1;
                e.cycleTime = double(float(d.rateCurveCycles) - 1e-6f);
            } else if (d.rateTrigger[0] >= 0) {
                // A trigger: the slice ends with the cycle, the wait follows.
                triggered = true;
                const double over = e.cycleTime - e.cycleIndex;
                e.cycleTime = e.cycleIndex;
                slice = float(slice - over * period);
            }
        }
        const float value = sampleCurve(d.rateCurve, float(e.cycleTime - e.cycleIndex), 0);
        const float wanted = scale * slice * value + e.rateRemainder;
        const int n = std::max(0, int(std::floor(wanted)));
        e.rateRemainder = wanted - float(n);
        const float every = n > 0 ? slice / float(n) : 0;
        float age = left;
        for (int i = 0; i < n; ++i) {
            Particle p = initParticle(inst, e);
            p.position = sub(p.position, mul(moved, age / total));
            if (e.particles.size() < kMaxParticles && updatePhysics(e, p, age))
                e.particles.push_back(p);
            age -= every;
        }
        left -= slice;
        if (triggered) {
            left -= rng_.range(d.rateTrigger);
            if (left < 0) {
                e.delayLeft = -left;
                return;
            }
        }
        if (left <= 0)
            return;
    }
}

// cTSParticlesEffect::GenerateNewParticles (BV 0xeb8c90).
void EffectSystem::generate(const Instance& inst, Emitter& e, float dt) {
    const Particles& d = *e.desc;
    if (e.stopping)
        return;
    if (e.delayLeft > 0) {
        e.delayLeft -= dt;
        if (e.delayLeft > 0)
            return;
        dt = -e.delayLeft;
    }
    if (d.preRoll > 0)
        dt = std::min(dt, d.preRoll - 0.01f);
    if (d.flags & pflag::Maintain)
        maintainParticles(inst, e, dt);
    else if (d.flags & pflag::Inject)
        injectParticles(inst, e, dt);
    else
        addRateCurveParticles(inst, e, dt);
}

// cTSParticlesEffect::ApplyEffect (BV 0xebb7c8): the pre-roll on the first
// frame after a hard start, the particles alive moved first, then the new
// ones, then the random walk.
void EffectSystem::applyEmitter(const Instance& inst, Emitter& e, float dt) {
    const Particles& d = *e.desc;
    if (!e.started)
        return;
    if (e.preRollPending) {
        e.preRollPending = false;
        dt += d.preRoll;
    }
    const Placement t1 = emitterFrame(inst, e);
    const V3 at = t1.position;
    if (sqrlen(sub(at, e.lastPosition)) > 1) // a jump: no trail across it
        e.lastPosition = at;
    // The attractor's point follows the emitter (FUN_00f5d93c); one without
    // a point of its own pulls towards the effect's target, which nothing
    // sets here (inferred: no pull).
    e.attracts = (d.flags & pflag::Attractor) && (d.flags & pflag::AttractorPoint) && !d.attractorStrength.empty();
    if (e.attracts)
        e.attractorPoint = transformPoint(t1, d.attractor);
    // The force, the world's wind (none on a lot; inferred) and the walk,
    // in the particles' frame.
    e.accel = add(d.force, e.walk);
    std::erase_if(e.particles, [&](Particle& p) { return !updatePhysics(e, p, dt); });
    if (!inst.stopped && (e.stopAt < 0 || e.time < e.stopAt))
        generate(inst, e, dt);
    else
        e.stopping = true;
    if (d.flags & pflag::RandomWalk)
        updateRandomWalk(e, dt);
    e.lastPosition = at;
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
                      depth + 1, false);
        // InitParticle (meta, BV 0xf63310): the carried effect starts soft.
        for (auto& e : p.child->emitters)
            if (e.startDelay <= 0)
                startEmitter(*p.child, e, false);
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
        // The meta particles here move with their instance: their effects'
        // source is the instance's frame (UpdateActiveParticles BV 0xf5f5b0
        // hands the meta's source to them).
        p.child->source = inst.where;
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
    (void)emit; // (the emitters look at inst.stopped)
    for (auto& e : inst.emitters) {
        const float edt = dt * e.timeScale;
        e.time += edt;
        // A sequence's later item starts when its time comes.
        if (!e.started && e.startDelay > 0 && e.time >= e.startDelay && !inst.stopped &&
            (e.stopAt < 0 || e.time < e.stopAt))
            startEmitter(inst, e, false);
        applyEmitter(inst, e, edt);
    }
    for (auto& dp : inst.decals)
        dp.age += dt;
    for (auto& mp : inst.models)
        mp.age += dt;
    for (auto& sp : inst.sounds)
        sp.age += dt;
    // Meta particles in steps (theirs are not read yet).
    for (auto& m : inst.metas) {
        float left = dt;
        do {
            const float step = std::min(left, kMaxStep);
            stepChain(inst, m, step, !inst.stopped);
            left -= step;
        } while (left > 0);
    }
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
        if (!e.particles.empty())
            return true;
        if (inst.stopped)
            continue;
        if (e.started && !e.stopping)
            return true;
        if (!e.started && e.startDelay > 0 && (e.stopAt < 0 || e.time < e.stopAt)) // a sequence item to come
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
        stepInstance(inst, seconds, !inst.stopped);
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
    int group = 0;
    for (const auto& [h, inst] : instances_)
        collectInstance(inst, group, out);
}

void EffectSystem::collectInstance(const Instance& inst, int& group, std::vector<ParticleQuad>& out) const {
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
            q.group = group++;
            q.groupCenter = q.position; // (decals: cTSDecalEffect not read; its key taken as 7)
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
        // The particles' frame (t2) in the world: its scale scales them
        // (the vertex fill, BV 0xeb8dbc).
        const Placement frame = particleFrame(inst, e);
        const int cells = std::max(1, int(d.tileU) * int(d.tileV));
        // The box of the particles (TSTriggerCollisionEffect extends it by each
        // one; the stretch along the velocity is left out).
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& p : e.particles) {
            const V3 w = transformPoint(frame, p.position);
            for (size_t k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], w[k]), hi[k] = std::max(hi[k], w[k]);
        }
        const V3 center = mul(add(lo, hi), 0.5f);
        const int key = (d.flags & pflag::DirectLayer) ? int(d.layer) : int(d.layer) * 8 + 7;
        const int g = group++;
        for (const auto& p : e.particles) {
            const float t = p.age / p.life;
            ParticleQuad q;
            q.position = transformPoint(frame, p.position);
            q.group = g;
            q.groupCenter = center;
            q.layerKey = key;
            q.velocity = rotate(frame, p.velocity);
            const float size = p.sizeScale * sampleCurve(d.sizeCurve, t, 1) * frame.scale;
            q.width = size;
            q.height = size * sampleCurve(d.aspectCurve, t, 1) * p.aspectScale;
            q.rotation = (sampleCurve(d.rotateCurveX, t, 0) * p.rotate[0] + d.rotateOffset[0]) * kTwoPi;
            const V3 c = sampleColors(d.colors, t);
            for (int k = 0; k < 3; ++k)
                q.color[size_t(k)] = std::clamp(c[size_t(k)] * p.colorScale[size_t(k)], 0.0f, 1.0f);
            q.color[3] = std::clamp(sampleCurve(d.alphaCurve, t, 1) * p.alphaScale * e.alphaScale * inst.alpha, 0.0f, 1.0f);
            q.texture = tex;
            if (d.tileU > 1 || d.tileV > 1) {
                // The frame: int(speed x age + start), kept within the count
                // from the start; its cell by the bits (tile counts are powers of two).
                const int start = e.icon && inst.iconFrame >= 0 ? inst.iconFrame : p.frameStart;
                int frame = int(d.frameSpeed * p.age + float(start));
                if (d.frameCount > 0)
                    frame = start + (frame - start) % d.frameCount;
                frame = ((frame % cells) + cells) % cells;
                const int tu = std::max<int>(1, d.tileU), tv = std::max<int>(1, d.tileV);
                const float du = 1.0f / float(tu), dv = 1.0f / float(tv);
                const int cu = frame % tu, cv = (frame / tu) % tv;
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
                collectInstance(*p.child, group, out);
}

} // namespace os2::fx
