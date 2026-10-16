#include "render/particle_system.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace os2::fx {

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kTimeStep = 1.0f / 15; // the particles' longest physics step and emission slice (BV 0x3d888889)
constexpr float kFastDrag = 0.05f;    // dt x drag above this takes the time-stepped physics (BV 0x3d4ccccd)
constexpr size_t kMaxParticles = 512; // per emitter (a guard of ours)

constexpr size_t kMaxMetaParticles = 64;          // per chain (a guard of ours)
constexpr size_t kMaxSpawned = 256;               // instances, past which collide/death effects are dropped (ours)
constexpr float kCollideAway = -1e-5f;            // the plane collider's "moving towards it" (BV 0x274bc04)

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

void EffectSystem::setLod(float lod) { lod_ = std::clamp(lod, 1.0f, kMaxLod); }

// SetCurveCoefficients (BV 0xc021c8) and ApplyEffect's blend by the LOD's
// fraction (0xebb7c8): over a range of more than one level the scale is the
// begin one to the end one by (LOD - begin) / (end - begin); over one level,
// by the fraction. Nested effects' components multiply (inferred: a nested
// cTSVisualEffect's scales handed down).
bool EffectSystem::lodScales(const Lod& l, float& emit, float& size, float& alpha) const {
    emit = l.emit, size = l.size, alpha = l.alpha;
    const int level = int(lod_);
    const float fraction = lod_ - float(level);
    for (const Component* c : l.path) {
        if (!(int(c->lodBegin) <= level && level < int(c->lodEnd)))
            return false;
        const float t = int(c->lodEnd) - int(c->lodBegin) > 1
                            ? (lod_ - float(c->lodBegin)) / float(int(c->lodEnd) - int(c->lodBegin))
                            : fraction;
        emit *= c->emitScale[0] + (c->emitScale[1] - c->emitScale[0]) * t;
        size *= c->sizeScale[0] + (c->sizeScale[1] - c->sizeScale[0]) * t;
        alpha *= c->alphaScale[0] + (c->alphaScale[1] - c->alphaScale[0]) * t;
    }
    return true;
}

void EffectSystem::addEffect(Instance& inst, uint32_t visual, const Placement& local, const Lod& lod,
                             float timeScale, float delay, float stopAt, int depth, bool rigid) {
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
    // The open chain (cTSEffectsManager::CreateComponentFromDescription BV
    // 0xc042e4: a cflag::Chain component is the chain target of the one
    // before; UpdateVisualEffect 0xc04544: after a cflag::StateChain one the
    // following state-chain components are its descriptions).
    MetaChain* chain = nullptr;
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
        // Its LOD range and scales, under the ones of the effects that hold it.
        Lod lc = lod;
        lc.path.push_back(&c);
        float es = 1, ss = 1, as = 1;
        const bool on = lodScales(lc, es, ss, as);
        const float ts = timeScale * (c.timeScale > 0 ? c.timeScale : 1);
        switch (c.type) {
        case Block::Visual:
            addEffect(inst, uint32_t(c.index), here, lc, ts, delay, stopAt, depth + 1, rigid);
            break;
        case Block::Particles:
            if (c.index >= 0 && size_t(c.index) < lib_.particles.size()) {
                Emitter e;
                e.desc = &lib_.particles[size_t(c.index)];
                e.local = here;
                e.lod = lc;
                e.lodOn = on;
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
                const bool joins = chain && (chain->byState ? (c.flags & cflag::StateChain) != 0 : (c.flags & cflag::Chain) != 0);
                if (joins) {
                    chain->steps.push_back(&m);
                    chain->bases.push_back(base ? int(*base) : -1);
                    if (c.flags & (chain->byState ? cflag::StateChainEnd : cflag::ChainEnd))
                        chain = nullptr;
                    break;
                }
                MetaChain mc;
                mc.steps.push_back(&m);
                mc.bases.push_back(base ? int(*base) : -1);
                mc.byState = (c.flags & cflag::StateChain) != 0;
                mc.rigid = rigid;
                mc.local = here;
                mc.lod = lc;
                mc.lodOn = on;
                mc.emitScale = es, mc.sizeScale = ss, mc.alphaScale = as, mc.timeScale = ts;
                mc.startDelay = delay;
                mc.stopAt = stopAt;
                mc.orderBase = inst.orderNext;
                inst.orderNext += 16;
                inst.metas.push_back(std::move(mc));
                chain = (c.flags & (cflag::StateChain | cflag::Chain)) && !(c.flags & cflag::ChainEnd)
                            ? &inst.metas.back() : nullptr;
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
                        addEffect(inst, *child, here, lc, ts, t, d < 0 ? stopAt : t + d, depth + 1, rigid);
                    chain = nullptr; // (the vector may have moved)
                    if (d < 0)
                        break;
                    t += d;
                }
            }
            break;
        case Block::Decal:
            if (c.index >= 0 && size_t(c.index) < lib_.decals.size())
                inst.decals.push_back({&lib_.decals[size_t(c.index)], here, ss, as, delay, 0, lc, on});
            break;
        case Block::Model:
            if (c.index >= 0 && size_t(c.index) < lib_.models.size())
                inst.models.push_back({&lib_.models[size_t(c.index)], here, ss, as, delay, stopAt, 0, lc, on});
            break;
        case Block::Sound:
            if (c.index >= 0 && size_t(c.index) < lib_.sounds.size())
                inst.sounds.push_back({&lib_.sounds[size_t(c.index)], here, delay, stopAt, 0, lc, on});
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
    inst.placed = where;
    inst.flags = lib_.visuals[*visual].flags;
    inst.where = facing(inst);
    inst.id = nextId_++;
    addEffect(inst, *visual, Placement{}, Lod{}, 1, 0, -1, 0, false);
    // cTSVisualEffect::Start starts every component with the same
    // transition; a sequence's later items start soft when their time comes
    // (inferred: cTSSequenceEffect not read). The first ApplyEffect runs at
    // once (with the pre-roll of a hard start), so the effect shows from the start.
    // (Only the parts in the LOD's range: UpdateVisualEffect creates the others when it comes to them.)
    for (auto& e : inst.emitters)
        if (e.startDelay <= 0 && e.lodOn) {
            startEmitter(inst, e, hard);
            applyEmitter(inst, e, 0);
        }
    for (auto& m : inst.metas)
        if (m.startDelay <= 0 && m.lodOn) {
            startChain(m, hard);
            stepChain(inst, m, 0, true);
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
    if (auto it = instances_.find(handle); it != instances_.end()) {
        it->second.placed = where;
        it->second.where = facing(it->second);
    }
}

Placement EffectSystem::facing(const Instance& inst) const {
    Placement p = inst.placed;
    if (!(inst.flags & (vflag::ViewRelative | vflag::CameraFacing | vflag::IgnoreOrientation)))
        return p;
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    p.rotation = identity;
    if (!hasView_ || !(inst.flags & (vflag::ViewRelative | vflag::CameraFacing)))
        return p;
    V3 x = sub(eye_, p.position);
    if (!(inst.flags & vflag::CameraFacing))
        x[2] = 0; // view-relative: only the heading
    const float l = len(x);
    if (l < 1e-6f)
        return p;
    x = mul(x, 1 / l);
    // +X towards the camera, Z up as the camera's up (inferred: the axes
    // after X are read only roughly).
    const V3 up = (inst.flags & vflag::CameraFacing) ? up_ : V3{0, 0, 1};
    V3 y{up[1] * x[2] - up[2] * x[1], up[2] * x[0] - up[0] * x[2], up[0] * x[1] - up[1] * x[0]};
    const float ly = len(y);
    if (ly < 1e-6f)
        return p;
    y = mul(y, 1 / ly);
    const V3 z{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    p.rotation = {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
    return p;
}

void EffectSystem::setIcon(int handle, std::string texture, int frame) {
    if (auto it = instances_.find(handle); it != instances_.end()) {
        it->second.icon = std::move(texture);
        it->second.iconFrame = frame;
    }
}

void EffectSystem::hideIcon(int handle) {
    if (auto it = instances_.find(handle); it != instances_.end())
        it->second.iconHidden = true;
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
    // The size scale (the LOD's): at birth (ApplyEffect's +0x174), or with
    // -scaleExisting on every particle as it is drawn (+0x178).
    p.sizeScale = random(1 - d.sizeVary, 1 + d.sizeVary) * ((d.flags & pflag::ScaleExisting) ? 1 : e.sizeScale);
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

// cTSPlaneCollider's set-up: the rectangle (a corner and two edges) of each
// "plane" collider record in the emitter's frame, taken into the particles'.
std::vector<EffectSystem::Plane> EffectSystem::setUpPlanes(const std::vector<ColliderInfo>& infos,
                                                           const Placement& t1) const {
    std::vector<Plane> out(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].name != "plane")
            continue;
        Plane& pl = out[i];
        pl.o = transformPoint(t1, infos[i].v[0]);
        pl.e1 = mul(rotate(t1, infos[i].v[1]), t1.scale);
        pl.e2 = mul(rotate(t1, infos[i].v[2]), t1.scale);
        V3 n{pl.e1[1] * pl.e2[2] - pl.e1[2] * pl.e2[1], pl.e1[2] * pl.e2[0] - pl.e1[0] * pl.e2[2],
             pl.e1[0] * pl.e2[1] - pl.e1[1] * pl.e2[0]};
        const float nl = len(n);
        if (nl < 1e-9f)
            continue;
        pl.n = mul(n, 1 / nl);
        pl.ok = true;
    }
    return out;
}

namespace {

float dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// cTSPlaneCollider's test (BV 0xebce60): moving against its normal, the
// moment the particle comes within `r` of the plane (or it starts in front
// of it within that), at a point inside the rectangle: the segment's end
// moved there.
} // namespace

bool EffectSystem::hitPlane(const Plane& pl, const V3& p0, V3& p1, float r) {
    const V3 seg = sub(p1, p0);
    const float vn = dot(seg, pl.n);
    if (!(vn < kCollideAway))
        return false;
    const float dist = -dot(sub(p0, pl.o), pl.n);
    const float t = (r + dist) / vn;
    if (!(t <= 1 && (t >= 0 || dist / vn >= 0)))
        return false;
    const V3 q = add(p0, mul(seg, t));
    const V3 rel = sub(q, pl.o);
    const float a11 = dot(pl.e1, pl.e1), a12 = dot(pl.e1, pl.e2), a22 = dot(pl.e2, pl.e2);
    const float b1 = dot(rel, pl.e1), b2 = dot(rel, pl.e2);
    const float det = a11 * a22 - a12 * a12;
    if (std::fabs(det) < 1e-12f)
        return false;
    const float ua = (b1 * a22 - b2 * a12) / det, ub = (b2 * a11 - b1 * a12) / det;
    if (ua < 0 || ua > 1 || ub < 0 || ub > 1)
        return false;
    p1 = q;
    return true;
}

// cFloorsCollider's test (TSSGEffectInteraction.cpp, BV 0x1bc2a58), in the
// world: the floor of the level below the new point (nTSWorld::LevelBelow)
// and the one of the level above it; crossing the lower one going down (or
// starting within `r` over it) the particle stops `r` over it, normal up;
// crossing the upper one going up, `r` under it, normal down; a floor counts
// only where the lot has one.
bool EffectSystem::hitFloors(const V3& p0, V3& p1, float r, V3& n) const {
    if (!world_)
        return false;
    const int below = world_->levelBelow(p1);
    float zb = 0, za = 0;
    const bool hasBelow = world_->floor(below, p1[0], p1[1], zb);
    const bool hasAbove = world_->floor(below + 1, p1[0], p1[1], za);
    const V3 up{0, 0, 1}, down{0, 0, -1};
    auto lowerOnly = [&]() {
        if (!hasBelow || zb < p1[2] - r)
            return false;
        p1[2] = zb + r;
        n = up;
        return true;
    };
    auto upperOnly = [&]() {
        if (!hasAbove || p1[2] + r < za)
            return false;
        p1[2] = za - r;
        n = down;
        return true;
    };
    const float z0 = p0[2], z1 = p1[2];
    if (z0 <= z1) {
        if (z1 <= z0) { // level
            if (hasBelow && z1 <= zb + r) {
                p1[2] = zb + r;
                n = up;
                return true;
            }
            return upperOnly();
        }
        // Rising: the ceiling.
        const float top = za - r, from = std::min(z0, za);
        if (top <= from) {
            if (!hasAbove)
                return lowerOnly();
            p1 = {p0[0], p0[1], top};
            n = down;
            return true;
        }
        if (z1 < top || !hasAbove)
            return lowerOnly();
        if (top < z1) {
            const float f = (top - from) / (z1 - from);
            p1 = {p0[0] + (p1[0] - p0[0]) * f, p0[1] + (p1[1] - p0[1]) * f, top};
        }
        n = down;
        return true;
    }
    // Falling: the floor.
    const float bottom = zb + r, from = std::max(z0, zb);
    if (from <= bottom) {
        if (!hasBelow)
            return upperOnly();
        p1 = {p0[0], p0[1], bottom};
        n = up;
        return true;
    }
    if (bottom < z1 || !hasBelow)
        return upperOnly();
    if (z1 < bottom) {
        const float f = (bottom - from) / (z1 - from);
        p1 = {p0[0] + (p1[0] - p0[0]) * f, p0[1] + (p1[1] - p0[1]) * f, bottom};
    }
    n = up;
    return true;
}

// The collider response (BV FUN_00ebdb5c), collider by collider: a hit kills
// the particle with its death chance (1, or a draw under it), starting the
// death effect there; otherwise the collide effect starts there and,
// moving against the normal, the normal speed bounces back x bounce and the
// rest keeps x slide (the "stick" float, +0x34, is 0 in the base game). The
// effects start soft at the point (FUN_00ebd9ac). The world colliders
// (floors, walls) take the points into the world and half the radius
// (BV 0x1bc3248); "wallsAndFloors" hits only when both do (0x1bc2e8c).
bool EffectSystem::collide(const std::vector<ColliderInfo>& infos, const std::vector<Plane>& planes,
                           const Placement& toWorld, const V3& prev, V3& pos, V3& vel, float radius) {
    for (size_t i = 0; i < infos.size(); ++i) {
        const ColliderInfo& c = infos[i];
        V3 n{};
        bool hit = false;
        if (c.name == "plane") {
            if (i < planes.size() && planes[i].ok && hitPlane(planes[i], prev, pos, radius * c.f[2])) {
                hit = true;
                n = planes[i].n;
            }
        } else if (world_) {
            const std::string kind = [&] {
                std::string k = c.name;
                for (char& ch : k)
                    ch = char(std::tolower(static_cast<unsigned char>(ch)));
                return k;
            }();
            const float r = radius * c.f[2] * 0.5f;
            const V3 w0 = transformPoint(toWorld, prev);
            V3 w1 = transformPoint(toWorld, pos);
            if (kind == "floors")
                hit = hitFloors(w0, w1, r, n);
            else if (kind == "walls") {
                V3 at{};
                hit = world_->wall(w0, w1, r, at, n);
                if (hit)
                    w1 = at;
            } else if (kind == "wallsandfloors") {
                V3 at{}, nw{};
                hit = hitFloors(w0, w1, r, n) && world_->wall(w0, w1, r, at, nw);
                if (hit)
                    w1 = at, n = nw;
            }
            if (hit) {
                const Placement back = inverse(toWorld);
                pos = transformPoint(back, w1);
                n = rotate(back, n);
            }
        }
        if (!hit)
            continue;
        const V3 at = transformPoint(toWorld, pos);
        if (c.f[3] != 0 && (c.f[3] == 1 || rng_.range(0, 1) <= c.f[3])) {
            if (!c.effect2.empty())
                spawns_.push_back({c.effect2, at});
            return false;
        }
        if (!c.effect1.empty())
            spawns_.push_back({c.effect1, at});
        const float vn = dot(vel, n);
        if (vn < 0) {
            const V3 normal = mul(n, vn), tangent = sub(vel, normal);
            vel = sub(vel, add(mul(normal, 1 + c.f[0]), mul(tangent, 1 - c.f[1])));
        }
    }
    return true;
}

// cTSParticlesEffect::UpdateParticlePhysics (BV 0xeb6ecc): false once the
// particle's life is over. With none of the flags that need forces and a
// gentle drag, one step (BV 0x1c2aca0); otherwise steps of 1/15 s
// (UpdateParticlePhysicsTimeStep 0xeb6d50, ApplyForces 0xeb6604).
bool EffectSystem::updatePhysics(const Instance& inst, const Emitter& e, Particle& p, float dt) {
    const Particles& d = *e.desc;
    p.age += dt;
    if (p.age >= p.life)
        return false;
    const bool colliders = (d.flags & (pflag::Collide | pflag::CollideKill)) && !d.colliders.empty();
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
        const V3 before = p.position;
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
        // ApplyColliderForces (BV 0xeb3064): the particle's size (x its aspect
        // when over 1, x 1 + its aspect's variation) as the radius.
        if (colliders) {
            const float t = std::min(1.0f, p.age / p.life);
            const Placement frame = particleFrame(inst, e);
            const float radius = p.sizeScale * sampleCurve(d.sizeCurve, t, 1) *
                                 std::max(1.0f, sampleCurve(d.aspectCurve, t, 1) * p.aspectScale) *
                                 (1 + std::fabs(d.aspectVary)) * frame.scale *
                                 ((d.flags & pflag::ScaleExisting) ? e.sizeScale : 1);
            if (!collide(d.colliders, e.planes, frame, before, p.position, p.velocity, radius))
                return false;
        }
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
        if (!updatePhysics(inst, e, p, dt))
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
                    if (updatePhysics(inst, e, p, age))
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
            if (e.particles.size() < kMaxParticles && updatePhysics(inst, e, p, age))
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
    // The colliders are set up once, on the first ApplyEffect (+0x1d0).
    if (!e.planesSet) {
        e.planes = setUpPlanes(d.colliders, t1);
        e.planesSet = true;
    }
    // The attractor's point follows the emitter (FUN_00f5d93c); one without
    // a point of its own pulls towards the effect's target, which nothing
    // sets here (inferred: no pull).
    e.attracts = (d.flags & pflag::Attractor) && (d.flags & pflag::AttractorPoint) && !d.attractorStrength.empty();
    if (e.attracts)
        e.attractorPoint = transformPoint(t1, d.attractor);
    // The force, the world's wind (none on a lot; inferred) and the walk,
    // in the particles' frame.
    e.accel = add(d.force, e.walk);
    std::erase_if(e.particles, [&](Particle& p) { return !updatePhysics(inst, e, p, dt); });
    if (!inst.stopped && (e.stopAt < 0 || e.time < e.stopAt))
        generate(inst, e, dt);
    else
        e.stopping = true;
    if (d.flags & pflag::RandomWalk)
        updateRandomWalk(e, dt);
    e.lastPosition = at;
}

// The chain's emitter transform in its particles' frame (t1).
Placement EffectSystem::chainFrame(const Instance& inst, const MetaChain& m) const {
    if (m.rigid)
        return m.local;
    return compose(inverse(inst.source), compose(inst.where, m.local));
}

// cTSMetaParticlesEffect::Start (BV 0xf61028; read as the particles' Start,
// which it mirrors: the hard start's pre-roll and finite bursts, the waits).
void EffectSystem::startChain(MetaChain& m, bool hard) {
    const MetaParticles& d = *m.steps[0];
    if (hard && d.rateCurveCycles != 0 && !(d.flags & mflag::Sustain))
        return;
    m.started = true;
    m.stopping = false;
    m.cycleTime = 0;
    m.cycleIndex = 0;
    m.rateRemainder = 0;
    if (d.rateDelay[0] >= 0)
        m.delayLeft = rng_.range(d.rateDelay);
    else if (d.rateTrigger[0] >= 0)
        m.delayLeft = rng_.range(d.rateTrigger);
    else
        m.delayLeft = 0;
    m.preRollPending = hard;
}

// cTSRWState::Update (BV 0xf60d9c): a new wait, strength, and heading and
// elevation turned by up to turn[0] turns plus turn[1] in the same sense;
// turn[2] mixes the vertical in.
void EffectSystem::updateWalk(MetaParticle::Walk& w, const MetaParticles::Walk& d) {
    w.timer += rng_.range(d.delay);
    const float strength = rng_.range(d.strength);
    const float mix = d.turn[2];
    if (mix >= 0.5f) {
        w.vertical = strength;
        w.horizontal = 2 * strength * (1 - mix);
    } else {
        w.horizontal = strength;
        w.vertical = 2 * strength * mix;
    }
    auto turn = [&](float& angle) {
        const float a = random(-(d.turn[0] + 1e-5f), d.turn[0] + 1e-5f);
        angle += a * kTwoPi;
        angle += (a >= 0 ? d.turn[1] : -d.turn[1]) * kTwoPi;
    };
    if (mix < 1)
        turn(w.heading);
    if (mix > 0)
        turn(w.pitch);
}

void EffectSystem::enterStep(MetaChain& m, MetaParticle& p, size_t step, int depth) {
    const MetaParticles& d = *m.steps[step];
    p.step = step;
    p.age = 0;
    p.life = std::max(0.01f, rng_.range(d.life));
    // Its carried effect: a new one when the step carries another (the
    // fish's dead model; HandOffParticleToChainSystem BV 0xf63d30).
    const int base = m.bases[step];
    if (!p.child || step == 0 || base != m.bases[step - 1]) {
        p.child = std::make_unique<Instance>();
        p.child->id = nextId_++;
        if (base >= 0)
        {
            Lod carried;
            carried.emit = m.emitScale, carried.size = m.sizeScale, carried.alpha = m.alphaScale;
            addEffect(*p.child, uint32_t(base), Placement{}, carried, m.timeScale, 0, -1, depth + 1, false);
        }
        // InitParticle (meta, BV 0xf63310): the carried effect starts soft.
        for (auto& e : p.child->emitters)
            if (e.startDelay <= 0)
                startEmitter(*p.child, e, false);
        for (auto& c : p.child->metas)
            if (c.startDelay <= 0)
                startChain(c, false);
    }
}

// cTSMetaParticlesEffect::InitParticle (BV 0xf63310).
void EffectSystem::spawnMeta(Instance& inst, MetaChain& m, float age) {
    if (m.particles.size() >= kMaxMetaParticles)
        return;
    const MetaParticles& d = *m.steps[0];
    MetaParticle p;
    const Placement t1 = chainFrame(inst, m);
    V3 dir{0, 0, 1};
    if ((d.flags & mflag::Path) && !d.path.empty() && d.path[0].time == 0) {
        // On a path: its first point.
        p.position = d.path[0].position;
        p.velocity = d.path[0].velocity;
    } else {
        V3 u;
        if (d.flags & mflag::Ellipsoid) {
            do
                u = {random(-1, 1), random(-1, 1), random(-1, 1)};
            while (sqrlen(u) > 1);
            for (int k = 0; k < 3; ++k)
                u[k] = (d.emitVolume.lo[k] + d.emitVolume.hi[k]) * 0.5f + u[k] * (d.emitVolume.hi[k] - d.emitVolume.lo[k]) * 0.5f;
        } else {
            for (int k = 0; k < 3; ++k)
                u[k] = random(d.emitVolume.lo[k], d.emitVolume.hi[k]);
        }
        for (int k = 0; k < 3; ++k)
            dir[k] = random(d.emitDirection.lo[k], d.emitDirection.hi[k]);
        const float speed = rng_.range(d.emitSpeed);
        const float l = len(dir);
        p.position = u;
        p.velocity = l > 1e-6f ? mul(dir, speed / l) : V3{0, 0, 0};
        if (d.bomb > 0) {
            const V3 away = sub(u, d.bombCenter);
            if (sqrlen(away) > 1e-12f)
                p.velocity = add(p.velocity, mul(away, d.bomb / len(away)));
        }
    }
    p.position = transformPoint(t1, p.position);
    p.velocity = rotate(t1, p.velocity);
    // (Its first step: the state's for a state chain.)
    const size_t step = m.byState ? std::min<size_t>(size_t(inst.state), m.steps.size() - 1) : 0;
    enterStep(m, p, step, 0);
    if ((d.flags & mflag::Path) && !d.path.empty() && d.path.back().time > 0)
        p.life = d.path.back().time;
    p.sizeScale = random(1 - d.sizeVary, 1 + d.sizeVary) * t1.scale;
    p.rotate = d.rotateVary > 0 ? random(-d.rotateVary, d.rotateVary) : 1;
    if (d.flags & mflag::RandomWalk) {
        // cTSRWState::Init: the heading and elevation of the birth direction;
        // with -wait the first walk only after its delay.
        const float horizontal = std::hypot(dir[0], dir[1]);
        for (size_t k = 0; k < 2; ++k) {
            auto& w = p.walks[k];
            w.heading = std::atan2(dir[1], dir[0]);
            w.pitch = std::atan2(dir[2], horizontal);
            if (d.flags & mflag::RandomWalkWait) {
                w.timer = rng_.range(d.walks[k].delay);
                const float strength = rng_.range(d.walks[k].strength), mix = d.walks[k].turn[2];
                w.vertical = mix >= 0.5f ? strength : 2 * strength * mix;
                w.horizontal = mix >= 0.5f ? 2 * strength * (1 - mix) : strength;
            } else {
                w.timer = 0;
                updateWalk(w, d.walks[k]);
            }
        }
    }
    p.age = age;
    m.particles.push_back(std::move(p));
}

// GenerateNewParticles (meta, BV 0xf64f1c): the waits, then maintain
// (0xf64694: the missing ones made, not moved), inject or the rate curve
// (read as the particles', which they mirror; the rate's new ones are not
// set back along the emitter's way here).
void EffectSystem::generateMeta(Instance& inst, MetaChain& m, float dt) {
    const MetaParticles& d = *m.steps[0];
    if (m.stopping)
        return;
    if (m.delayLeft > 0) {
        m.delayLeft -= dt;
        if (m.delayLeft > 0)
            return;
        dt = -m.delayLeft;
    }
    if (d.preRoll > 0)
        dt = std::min(dt, d.preRoll - 0.01f);
    const float period = std::max(d.rateCurveTime, 1e-6f);
    if (d.flags & mflag::Maintain) {
        m.cycleTime += dt / period;
        const size_t want = size_t(std::max(0.0f, std::ceil(sampleCurve(d.rateCurve, 0, 0))));
        while (m.particles.size() < std::min(want, kMaxMetaParticles))
            spawnMeta(inst, m, 0);
        return;
    }
    if (d.flags & mflag::Inject) {
        m.cycleTime += dt / period;
        const float first = sampleCurve(d.rateCurve, 0, 0);
        const int count = int(std::ceil(first * (first > 20 ? m.emitScale : 1)));
        for (int guard = 0; guard < 1000 && m.cycleTime >= m.cycleIndex; ++guard) {
            const float age = float((m.cycleTime - m.cycleIndex) * period);
            if (age < d.life[1])
                for (int i = 0; i < count; ++i)
                    spawnMeta(inst, m, age);
            ++m.cycleIndex;
            if (d.rateCurveCycles != 0 && int(d.rateCurveCycles) <= m.cycleIndex) {
                m.stopping = true;
                return;
            }
            if (d.rateTrigger[0] >= 0) {
                m.cycleTime -= rng_.range(d.rateTrigger) / period;
                if (m.cycleTime < m.cycleIndex) {
                    m.delayLeft = float((m.cycleIndex - m.cycleTime) * period);
                    m.cycleTime = m.cycleIndex;
                    return;
                }
            }
        }
        return;
    }
    float left = dt;
    for (int guard = 0; guard < 10000 && left > 0; ++guard) {
        float slice = left > kTimeStep ? kTimeStep : left;
        const int next = m.cycleIndex + 1;
        m.cycleTime += slice / period;
        if (m.cycleTime > next) {
            m.cycleIndex = next;
            if (d.rateCurveCycles != 0 && int(d.rateCurveCycles) == m.cycleIndex) {
                if (!(d.flags & mflag::Sustain)) {
                    m.stopping = true;
                    return;
                }
                m.cycleIndex = int(d.rateCurveCycles) - 1;
                m.cycleTime = double(float(d.rateCurveCycles) - 1e-6f);
            }
        }
        const float wanted = m.emitScale * slice * sampleCurve(d.rateCurve, float(m.cycleTime - m.cycleIndex), 0) + m.rateRemainder;
        const int n = std::max(0, int(std::floor(wanted)));
        m.rateRemainder = wanted - float(n);
        for (int i = 0; i < n; ++i)
            spawnMeta(inst, m, 0);
        left -= slice;
    }
}

// cTSMetaParticlesEffect::UpdateParticlePhysicsTimeStep (BV 0xf62d10) in
// steps of 1/15 s, or the path (SnapParticleToPath 0xf5fb18): false when
// the particle is gone (past its path's end, or killed by a collider).
bool EffectSystem::stepMetaPhysics(const Instance& inst, MetaChain& m, MetaParticle& p, float dt) {
    const MetaParticles& d = *m.steps[p.step];
    const Placement t1 = chainFrame(inst, m);
    if ((d.flags & mflag::Path) && !d.path.empty()) {
        // Hermite between the path's points (position, velocity, time) in
        // the emitter's frame; the start is the emit box's centre with the
        // direction box's centre at the first speed.
        size_t k = 0;
        while (k < d.path.size() && !(p.age < d.path[k].time))
            ++k;
        if (k == d.path.size())
            k = d.path.size() - 1;
        float t1end = d.path[k].time;
        if (t1end == 0)
            t1end = p.life;
        else if (t1end < p.age)
            return false;
        V3 p0, v0;
        float t0 = 0;
        if (k > 0) {
            p0 = d.path[k - 1].position, v0 = d.path[k - 1].velocity, t0 = d.path[k - 1].time;
        } else {
            for (int a = 0; a < 3; ++a) {
                p0[a] = (d.emitVolume.lo[a] + d.emitVolume.hi[a]) * 0.5f;
                v0[a] = (d.emitDirection.lo[a] + d.emitDirection.hi[a]) * 0.5f * d.emitSpeed[0];
            }
        }
        const float span = t1end - t0;
        if (span > 1e-6f) {
            const float u = (p.age - t0) / span, u2 = u * u, u3 = u2 * u;
            const V3& p1 = d.path[k].position;
            const V3& v1 = d.path[k].velocity;
            const float h00 = 2 * u3 - 3 * u2 + 1, h01 = -2 * u3 + 3 * u2, h10 = (u3 - 2 * u2 + u) * span,
                        h11 = (u3 - u2) * span;
            const float d00 = (6 * u2 - 6 * u) / span, d01 = (6 * u - 6 * u2) / span, d10 = 3 * u2 - 4 * u + 1,
                        d11 = 3 * u2 - 2 * u;
            V3 pos, vel;
            for (int a = 0; a < 3; ++a) {
                pos[a] = h00 * p0[a] + h01 * p1[a] + h10 * v0[a] + h11 * v1[a];
                vel[a] = d00 * p0[a] + d01 * p1[a] + d10 * v0[a] + d11 * v1[a];
            }
            p.position = transformPoint(t1, pos);
            p.velocity = rotate(t1, vel);
        }
        return true;
    }
    const float radius = p.sizeScale * sampleCurve(d.sizeCurve, std::min(1.0f, p.age / p.life), 1);
    float left = dt;
    do {
        const float step = left > kTimeStep ? kTimeStep : left;
        const V3 before = p.position;
        p.position = add(p.position, mul(p.velocity, step));
        p.velocity = add(p.velocity, mul(d.force, step));
        if (d.flags & mflag::RandomWalk) {
            V3 pull{0, 0, 0};
            for (size_t k = 0; k < 2; ++k) {
                auto& w = p.walks[k];
                w.timer -= step;
                if (w.timer < 0)
                    updateWalk(w, d.walks[k]);
                // Its force (BV 0xf5e5bc).
                if (w.vertical == 0) {
                    if (w.horizontal != 0)
                        pull = add(pull, {w.horizontal * std::cos(w.heading), w.horizontal * std::sin(w.heading), 0});
                } else if (w.horizontal == 0) {
                    pull[2] += w.vertical * std::sin(w.pitch);
                } else {
                    pull = add(pull, {w.horizontal * std::cos(w.heading) * std::cos(w.pitch),
                                      w.horizontal * std::sin(w.heading) * std::cos(w.pitch), w.vertical * std::sin(w.pitch)});
                }
            }
            p.velocity = add(p.velocity, mul(pull, step));
        }
        // The attractor: towards its point (with bit 28 its own, in the
        // emitter's frame), the strength over distance / range.
        if ((d.flags & mflag::Attractor) && (d.flags & (1ull << 28)) && !d.attractorStrength.empty()) {
            const V3 to = sub(transformPoint(t1, d.attractor), p.position);
            const float dist = len(to);
            if (dist > 0)
                p.velocity = add(p.velocity, mul(to, step * sampleCurve(d.attractorStrength, std::min(dist / std::max(d.attractorRange, 1e-6f), 1.0f), 0) / dist));
        }
        p.velocity = mul(p.velocity, 1 - step * d.drag);
        // The colliders (the common response, BV FUN_00ebdb5c; the plane
        // ones set up once per step's description, in the emitter's frame).
        if (!d.colliders.empty()) {
            if (m.planes.size() < m.steps.size())
                m.planes.resize(m.steps.size());
            auto& planes = m.planes[p.step];
            if (planes.size() != d.colliders.size())
                planes = setUpPlanes(d.colliders, t1);
            if (!collide(d.colliders, planes, m.rigid ? inst.where : inst.source, before, p.position, p.velocity, radius))
                return false;
        }
        left -= step;
    } while (left > 0);
    return true;
}

// A meta particle in the world for its carried effect (ProcessMetaParticle
// BV 0xf5f1a4): its position, the facing of its alignment, its size.
void EffectSystem::placeMeta(const Instance& inst, const MetaChain& m, MetaParticle& p) const {
    const MetaParticles& d = *m.steps[p.step];
    const float t = std::min(1.0f, p.age / p.life);
    Placement local;
    local.position = p.position;
    auto rz = [](float turns) {
        const float c = std::cos(turns * kTwoPi), s = std::sin(turns * kTwoPi);
        return std::array<float, 9>{c, -s, 0, s, c, 0, 0, 0, 1};
    };
    if (d.alignment >= 2 && d.alignment <= 4 && len(p.velocity) > 1e-6f) {
        // The axis along the velocity: X (2), Y (3) or Z (4); the others
        // from the vertical.
        const V3 f = mul(p.velocity, 1 / len(p.velocity));
        V3 x, y, z;
        auto norm = [](V3 v) { const float l = len(v); return l > 1e-6f ? mul(v, 1 / l) : V3{0, 0, 0}; };
        auto cross = [](V3 a, V3 b) { return V3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; };
        if (d.alignment == 2) {
            x = f, y = norm(cross({0, 0, 1}, f));
            if (len(y) == 0)
                y = {0, 1, 0};
            z = cross(x, y);
        } else if (d.alignment == 3) {
            y = f, x = norm(cross(f, {0, 0, 1}));
            if (len(x) == 0)
                x = {1, 0, 0};
            z = cross(x, y);
        } else {
            z = f;
            x = std::fabs(f[0]) <= std::fabs(f[1]) ? norm(cross({0, 1, 0}, z)) : norm(cross({1, 0, 0}, z));
            if (std::fabs(f[0]) > std::fabs(f[1]))
                x = mul(x, -1);
            y = cross(z, x);
        }
        if (d.alignDamp <= 0) {
            local.rotation = {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
        } else {
            // Damped: only the heading, turned towards the motion by (1/15) /
            // damp of the way each frame (inferred: each step here).
            float want = std::atan2(x[1], x[0]) / kTwoPi;
            float h = p.heading;
            if (h != -999) {
                if (want > h + 0.5f)
                    h += 1;
                else if (want < h - 0.5f)
                    h -= 1;
                h = (kTimeStep / d.alignDamp) * (want - h) + h;
                if (h < -0.5f)
                    h += 1;
                else if (h > 0.5f)
                    h -= 1;
            } else {
                h = want;
            }
            p.heading = h;
            local.rotation = rz(h);
        }
    } else if (d.alignment >= 2 && d.alignment <= 4 && p.heading != -999) {
        local.rotation = rz(p.heading);
    }
    // Its own rotation about Z over its life.
    if (!d.rotateCurve.empty()) {
        const auto r = rz(p.rotate * sampleCurve(d.rotateCurve, t, 0));
        Placement spin;
        spin.rotation = r;
        local.rotation = compose(local, spin).rotation;
    }
    local.scale = std::max(0.0f, sampleCurve(d.sizeCurve, t, 1) * p.sizeScale);
    if (p.child) {
        const Placement frame = m.rigid ? inst.where : inst.source;
        p.child->where = compose(frame, local);
        // The carried effect's source is the meta's (UpdateActiveParticles BV 0xf5f5b0).
        p.child->source = frame;
        p.child->alpha = inst.alpha * std::clamp(sampleCurve(d.alphaCurve, t, 1), 0.0f, 1.0f) * m.alphaScale;
        p.child->icon = inst.icon;
        p.child->iconFrame = inst.iconFrame;
        p.child->iconHidden = inst.iconHidden;
        p.child->stopped = inst.stopped;
    }
}

// A meta particle that is gone (FUN_00f64084, BV): its carried effect stops
// soft and runs on until its particles die (+0x1c0); the "<base>_death"
// effect, when the library has one (InitFromDescription BV 0xf60654), starts
// soft where it was.
void EffectSystem::retireMeta(const Instance& inst, MetaChain& m, MetaParticle& p) {
    if (p.child) {
        p.child->stopped = true;
        if (busy(*p.child) && m.dying.size() < kMaxMetaParticles)
            m.dying.push_back(std::move(p.child));
        p.child.reset();
    }
    const std::string& base = m.steps[p.step]->baseEffect;
    if (!base.empty() && !inst.stopped && lib_.indexOf(base + "_death"))
        spawns_.push_back({base + "_death", transformPoint(m.rigid ? inst.where : inst.source, p.position)});
}

void EffectSystem::stepChain(Instance& inst, MetaChain& m, float dt, bool emit) {
    dt *= m.timeScale;
    m.time += dt;
    if (!m.started && m.startDelay > 0 && m.time >= m.startDelay && !inst.stopped && m.lodOn &&
        (m.stopAt < 0 || m.time < m.stopAt))
        startChain(m, false);
    if (m.preRollPending) {
        m.preRollPending = false;
        dt += m.steps[0]->preRoll;
    }
    for (auto& p : m.particles) {
        // A state chain: the effect's state picks the description
        // (SelectDescriptionState BV 0xeb04f4 -> SwitchDescription).
        if (m.byState) {
            const size_t want = std::min<size_t>(size_t(inst.state), m.steps.size() - 1);
            if (p.step != want)
                enterStep(m, p, want, 0);
        }
        p.age += dt;
        if (p.age >= p.life) {
            // Its life over: a chain hands it to the next description
            // (HandOffParticleToChainSystem BV 0xf63d30), else it is gone.
            if (!m.byState && p.step + 1 < m.steps.size()) {
                enterStep(m, p, p.step + 1, 0);
            } else {
                retireMeta(inst, m, p);
                continue;
            }
        }
        if (!stepMetaPhysics(inst, m, p, dt)) {
            retireMeta(inst, m, p);
            continue;
        }
        placeMeta(inst, m, p);
        if (p.child)
            stepInstance(*p.child, dt, emit);
    }
    std::erase_if(m.particles, [](const MetaParticle& p) { return !p.child; });
    // The carried effects of the dead ones run until their particles are gone.
    for (auto& c : m.dying)
        stepInstance(*c, dt, false);
    std::erase_if(m.dying, [](const std::unique_ptr<Instance>& c) { return !busy(*c); });
    // New ones from the first description (state chains only in state 0,
    // inferred: SwitchDescription not followed).
    const bool window = m.stopAt < 0 || m.time < m.stopAt;
    if (m.started && emit && !inst.stopped && window && (!m.byState || inst.state == 0))
        generateMeta(inst, m, dt);
    else if (!window || inst.stopped)
        m.stopping = true;
}

// UpdateVisualEffect (BV 0xc04544) when the LOD changes: the components
// out of the new range go (stopped soft: their particles finish; the game
// stops them hard unless effectLODSoftChange, whose default was not found:
// inferred), the ones in it come (started soft, as a running effect's new
// components), and every part takes the scales of the new LOD.
void EffectSystem::updateLod(Instance& inst) {
    for (auto& e : inst.emitters) {
        const bool on = lodScales(e.lod, e.emitScale, e.sizeScale, e.alphaScale);
        if (on == e.lodOn)
            continue;
        e.lodOn = on;
        if (!on && e.started && !e.stopping) {
            e.stopping = true;
            e.lodOff = true;
        } else if (on && (!e.started || e.lodOff) && !inst.stopped && e.startDelay <= 0 &&
                   (e.stopAt < 0 || e.time < e.stopAt)) {
            startEmitter(inst, e, false);
            e.lodOff = false;
        }
    }
    for (auto& m : inst.metas) {
        const bool on = lodScales(m.lod, m.emitScale, m.sizeScale, m.alphaScale);
        if (on == m.lodOn)
            continue;
        m.lodOn = on;
        if (!on && m.started && !m.stopping) {
            m.stopping = true;
            m.lodOff = true;
        } else if (on && (!m.started || m.lodOff) && !inst.stopped && m.startDelay <= 0 &&
                   (m.stopAt < 0 || m.time < m.stopAt)) {
            startChain(m, false);
            m.lodOff = false;
        }
    }
    float e, s, a;
    for (auto& d : inst.decals)
        d.lodOn = lodScales(d.lod, e, d.sizeScale, d.alphaScale);
    for (auto& m : inst.models)
        m.lodOn = lodScales(m.lod, e, m.sizeScale, m.alphaScale);
    for (auto& snd : inst.sounds)
        snd.lodOn = lodScales(snd.lod, e, s, a);
}

void EffectSystem::stepInstance(Instance& inst, float dt, bool emit) {
    (void)emit; // (the emitters look at inst.stopped)
    updateLod(inst);
    for (auto& e : inst.emitters) {
        const float edt = dt * e.timeScale;
        e.time += edt;
        // A sequence's later item starts when its time comes.
        if (!e.started && e.startDelay > 0 && e.time >= e.startDelay && !inst.stopped && e.lodOn &&
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
    for (auto& m : inst.metas)
        stepChain(inst, m, dt, !inst.stopped);
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
        if (!e.lodOn || e.lodOff) // out of the LOD's range: back when the camera comes to it
            return true;
    }
    for (const auto& m : inst.metas) {
        for (const auto& p : m.particles)
            if (p.child && busy(*p.child))
                return true;
        if (!m.dying.empty())
            return true;
        if (inst.stopped)
            continue;
        if (m.started && !m.stopping)
            return true;
        if (!m.lodOn || m.lodOff)
            return true;
        if (!m.started && m.startDelay > 0 && (m.stopAt < 0 || m.time < m.stopAt))
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
        inst.where = facing(inst);
        stepInstance(inst, seconds, !inst.stopped);
        if (!busy(inst))
            it = instances_.erase(it);
        else
            ++it;
    }
    // The collide and death effects of this step: at their point (only the
    // translation), started soft, left to end by themselves (FUN_00ebd9ac).
    std::vector<Spawn> spawns;
    spawns.swap(spawns_);
    for (const auto& sp : spawns) {
        if (instances_.size() >= kMaxSpawned)
            break;
        Placement at;
        at.position = sp.at;
        start(sp.name, at, false);
    }
}

void EffectSystem::collectModelsOf(int handle, const Instance& inst, std::vector<EffectModel>& out) const {
    for (size_t i = 0; i < inst.models.size(); ++i) {
        const ModelPart& m = inst.models[i];
        if (!m.lodOn || inst.stopped || m.age < m.delay || (m.stopAt >= 0 && m.age >= m.stopAt))
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
        if (!sp.lodOn || inst.stopped || sp.age < sp.delay || (sp.stopAt >= 0 && sp.age >= sp.stopAt))
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
            if (!dp.lodOn || age < 0 || (d.life > 0 && age >= d.life) || (d.life <= 0 && inst.stopped))
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
        if (e.icon && inst.iconHidden)
            continue;
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
            const float size = p.sizeScale * sampleCurve(d.sizeCurve, t, 1) * frame.scale *
                               ((d.flags & pflag::ScaleExisting) ? e.sizeScale : 1);
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
            q.lit = (d.flags & pflag::Light) != 0;
            if (q.color[3] > 0.001f && size > 0)
                out.push_back(q);
        }
    }
    for (const auto& mc : inst.metas) {
        for (const auto& p : mc.particles)
            if (p.child)
                collectInstance(*p.child, group, out);
        for (const auto& c : mc.dying)
            collectInstance(*c, group, out);
    }
}

} // namespace os2::fx
