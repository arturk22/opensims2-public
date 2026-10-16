#pragma once

// Runs the game's particle effects (formats/effects.hpp): instances of
// visual effects whose particle components emit, move and fade particles,
// collected as camera-facing quads for ParticleLayer. Plain C++ (no Metal),
// so tests can drive it. What follows the game's runtime
// (cTSParticlesEffect, Mac binary) and what is inferred is in
// docs/effects-mac.md.

#include "formats/effects.hpp"

#include <array>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2::fx {

// Where an effect sits in the world (game space, Z up): position, rotation
// (row-major 3x3, columns are the local axes) and uniform scale.
struct Placement {
    V3 position{};
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float scale = 1;
};

// One particle, ready to draw: centre, size (width, height), rotation in
// radians about the view axis, colour with alpha, its texture (name as the
// effect gives it, "" for none, or the icon set on the instance), atlas
// cell, blending and alignment.
struct ParticleQuad {
    V3 position{};
    float width = 0, height = 0, rotation = 0;
    std::array<float, 4> color{1, 1, 1, 1};
    const std::string* texture = nullptr;
    std::array<float, 4> uv{0, 0, 1, 1}; // u0, v0, u1, v1
    DrawType draw = DrawType::Decal;
    uint8_t alignment = 0;
    float layer = 0;
    V3 velocity{}; // world units per second (velocity-aligned particles)
    // Drawing order (cTSParticlesRenderable::AddElementsToDisplayList): an
    // emitter's particles are one group, drawn together in their birth
    // order; the group sorts among the blended parts by its key (layer x 8
    // + 7, or the layer itself with -directLayer) and the depth of the
    // centre of its particles' box.
    int group = 0;
    V3 groupCenter{};
    int layerKey = 7;
};

// A model an effect shows (Block::Model: a CRES, e.g. the chessboard's FX
// pieces or the neighbourhood's cars), where and how big; the host draws it.
struct EffectModel {
    int handle = 0; // the instance's
    int part = 0;   // which of its models
    const std::string* model = nullptr; // CRES name ("..._cres" or not)
    Placement where;
    std::array<float, 4> color{1, 1, 1, 1};
};

// A sound an effect plays (Block::Sound: the track whose instance is
// audioId), while it is due: from its component's delay until the effect
// stops (or its sequence item ends). The host plays it at `position`.
struct EffectSound {
    int handle = 0; // the instance's
    int part = 0;   // which of its sounds
    uint32_t audioId = 0;
    V3 position{};
};

// The effects' random numbers: the game's generator (BV FUN_00171b0c,
// shared by RandomRange / RandomVector3): state *= 0x278dde6d, value =
// (int32 state x 2^-32 + 0.5) x (b - a) + a. The game seeds it from the
// clock; here the seed is given.
class SwarmRandom {
public:
    explicit SwarmRandom(uint32_t seed = 1) : state_(seed ? seed : 1) {}
    float range(float a, float b);
    float range(const V2& r) { return range(r[0], r[1]); }

private:
    uint32_t state_;
};

class EffectSystem {
public:
    explicit EffectSystem(const EffectsLibrary& library, uint32_t seed = 1);

    // The effects' level of detail (cTSEffectsManager::SetLOD(float), BV
    // 0xc061b0): the lot camera's zoom level + its fraction + 1
    // (cTSCameraSmoothSims1Controller::Update, BV 0x86ffb8), less the
    // particleLODOffset (0), held in [1, 4.9999]. A component shows only
    // with lodBegin <= int(LOD) < lodEnd and its scales go from its begin
    // ones to its end ones (SetCurveCoefficients BV 0xc021c8 and the
    // ApplyEffect's blend by the fraction). By default the closest.
    static constexpr float kMaxLod = 4.9999f;
    void setLod(float lod);
    float lod() const { return lod_; }

    // Starts the named effect; -1 if the library has no such effect. A hard
    // start (cTSParticlesEffect::Start with a transition) runs the pre-roll
    // and leaves out the emitters with a finite number of cycles; a soft
    // one does neither (the VM's Effect Stop/Start mode 1 / mode 0).
    int start(std::string_view name, const Placement& where, bool hard = true);
    // Stops emitting; `hard` also removes the particles alive.
    void stop(int handle, bool hard = false);
    void move(int handle, const Placement& where);
    // Texture for the particles whose effect texture is an icon placeholder
    // (balloon icons: "...thumbnailprimitive" components).
    // `frame` picks the cell of an icon atlas (-1: the effect's own cell).
    void setIcon(int handle, std::string texture, int frame = -1);
    // No icon: the particles whose texture is replaceable are not drawn
    // (a balloon whose thumbnail cannot be found).
    void hideIcon(int handle);
    // The effect's state (Effect Stop/Start modes 10-13: float parameter
    // 0xF = 0..3): the meta particles of a behaviour chain go to that step
    // (the aquarium's fish: 0 swimming, 1 feeding, 2 dead).
    void setState(int handle, int state);
    // The camera (its position and up axis): effects that face it
    // (vflag::ViewRelative / CameraFacing) turn towards it.
    void setView(const V3& eye, const V3& up) {
        eye_ = eye, up_ = up;
        hasView_ = true;
    }
    // True while the instance emits or has particles alive.
    bool alive(int handle) const;
    size_t instanceCount() const { return instances_.size(); }

    // Advances every instance; finished, stopped instances are dropped.
    void update(float seconds);
    void collect(std::vector<ParticleQuad>& out) const;
    // The models of the instances alive.
    void collectModels(std::vector<EffectModel>& out) const;
    // The sounds due now.
    void collectSounds(std::vector<EffectSound>& out) const;

private:
    // A part's components from the effect started down to it (each
    // nested effect's component, then its own): their LOD ranges and
    // scales multiply; `emit`/`size`/`alpha` are the scales handed in (a
    // meta particle's carried effect).
    struct Lod {
        std::vector<const Component*> path;
        float emit = 1, size = 1, alpha = 1;
    };
    // The scales at the current LOD; false when a component of the path is
    // out of its range (the part is not there at this distance).
    bool lodScales(const Lod& l, float& emit, float& size, float& alpha) const;

    // cTSParticle: position and velocity in its emitter's frame (the
    // world, or the effect's own with a rigid effect), and what
    // InitParticle draws for it.
    struct Particle {
        V3 position{}, velocity{};
        float age = 0, life = 1;
        float sizeScale = 1, aspectScale = 1, alphaScale = 1;
        V3 colorScale{1, 1, 1};
        std::array<float, 2> rotate{1, 1}; // the rotations' curve multipliers
        int frameStart = 0;
    };
    // cTSParticlesEffect: one particle component.
    struct Emitter {
        const Particles* desc = nullptr;
        Placement local;  // relative to the instance (the component's transform)
        // A rigid visual effect (vflag::Rigid) at any level keeps the
        // particles in the instance's frame; otherwise they live in the world.
        bool rigid = false;
        Lod lod;
        bool lodOn = true, lodOff = false; // in its LOD range; stopped by leaving it
        float emitScale = 1, sizeScale = 1, alphaScale = 1, timeScale = 1;
        bool icon = false; // the texture is the instance's icon
        float time = 0;    // since the instance started (scaled)
        float startDelay = 0, stopAt = -1; // sequences
        int order = 0; // draw order of the rigid effects' components (component order)
        // Start / Stop / ApplyEffect state.
        bool started = false, stopping = false, preRollPending = false;
        double cycleTime = 0;  // +0x40: the rate clock, in cycles
        int cycleIndex = 0;    // +0x48
        float rateRemainder = 0, delayLeft = 0; // +0x54, +0x88
        V3 lastPosition{};     // +0x128: the emitter last frame
        V3 accel{};            // +0x58: force + world wind + random walk
        float walkTimer = 0;   // +0x64
        std::array<float, 2> walkAngle{}; // heading, elevation (turns)
        V3 walk{};             // +0x70: the random walk's force
        V3 attractorPoint{};   // +0x7c: the attractor's point in the particles' frame
        bool attracts = false;
        std::vector<Particle> particles;
    };
    // A decal (Block::Decal): one quad flat on the ground under the
    // component, its size, alpha, rotation and colour over its life (life
    // <= 0: while the effect runs); a model (Block::Model) stays while the
    // effect runs.
    struct DecalPart {
        const Decal* desc = nullptr;
        Placement local;
        float sizeScale = 1, alphaScale = 1, delay = 0, age = 0;
        Lod lod;
        bool lodOn = true;
    };
    struct ModelPart {
        const Model* desc = nullptr;
        Placement local;
        float sizeScale = 1, alphaScale = 1, delay = 0, stopAt = -1, age = 0;
        Lod lod;
        bool lodOn = true;
    };
    struct SoundPart {
        const Sound* desc = nullptr;
        Placement local;
        float delay = 0, stopAt = -1, age = 0;
        Lod lod;
        bool lodOn = true;
    };
    struct Instance;
    // cTSMetaParticle: a whole effect (its own instance) moving as a
    // particle, in its chain's particles' frame (the world, or the
    // instance's with a rigid effect).
    struct MetaParticle {
        V3 position{}, velocity{};
        float age = 0, life = 1;
        float sizeScale = 1;
        float rotate = 1;          // the rotation curve's multiplier
        size_t step = 0;           // which of the chain's descriptions it is in
        // cTSRWState: timer, vertical and horizontal strength, heading and
        // elevation (radians), for each of the two random walks.
        struct Walk {
            float timer = 0, vertical = 0, horizontal = 0, heading = 0, pitch = 0;
        };
        std::array<Walk, 2> walks{};
        float heading = -999;      // the damped heading (turns), -999 before the first
        std::unique_ptr<Instance> child;
    };
    // A meta particle component (cTSMetaParticlesEffect) and the
    // descriptions it goes through: a chain (component flag cflag::Chain:
    // each one's particles go to the next when their life ends, and only the
    // first emits) or a state chain (cflag::StateChain: the effect's state
    // picks the description, the aquarium's swimming / feeding / dead fish).
    struct MetaChain {
        std::vector<const MetaParticles*> steps;
        std::vector<int> bases; // each step's carried effect (-1: none)
        bool byState = false;
        Placement local;        // relative to the instance
        bool rigid = false;
        Lod lod;
        bool lodOn = true, lodOff = false;
        float emitScale = 1, sizeScale = 1, alphaScale = 1, timeScale = 1;
        float time = 0, startDelay = 0, stopAt = -1;
        int orderBase = 0;
        // Start / GenerateNewParticles state, as the particles' (Emitter).
        bool started = false, stopping = false, preRollPending = false;
        double cycleTime = 0;
        int cycleIndex = 0;
        float rateRemainder = 0, delayLeft = 0;
        std::vector<MetaParticle> particles;
    };
    struct Instance {
        Placement where;  // the effect's transform (its node's), turned to the camera if it faces it
        Placement placed; // as it was placed (start / move)
        uint32_t flags = 0; // the effect's vflag bits
        // The source transform (t2) of the emitters that are not rigid: the
        // world for an effect started here, the frame of the meta particles
        // that carry it for a meta particle's effect.
        Placement source;
        bool hardStop = false; // vflag::HardStop: a soft stop also clears the particles
        std::vector<Emitter> emitters;
        std::vector<DecalPart> decals;
        std::vector<ModelPart> models;
        std::vector<SoundPart> sounds;
        std::vector<MetaChain> metas;
        bool stopped = false;
        std::string icon;
        int iconFrame = -1;
        bool iconHidden = false;
        int state = 0;
        float alpha = 1;  // a meta particle's alpha over its carried effect
        int orderNext = 0;
        int id = 0;       // unique among the instances and their meta particles' (model keys)
    };

    void addEffect(Instance& inst, uint32_t visual, const Placement& local, const Lod& lod, float timeScale,
                   float delay, float stopAt, int depth, bool rigid);
    // The parts' scales and presence at the current LOD; a part that leaves
    // its range stops emitting, one that comes into it starts (soft).
    void updateLod(Instance& inst);
    void stepInstance(Instance& inst, float dt, bool emit);
    void stepChain(Instance& inst, MetaChain& m, float dt, bool emit);
    void startChain(MetaChain& m, bool hard);
    void generateMeta(Instance& inst, MetaChain& m, float dt);
    void spawnMeta(Instance& inst, MetaChain& m, float age);
    void enterStep(MetaChain& m, MetaParticle& p, size_t step, int depth);
    void updateWalk(MetaParticle::Walk& w, const MetaParticles::Walk& d);
    bool stepMetaPhysics(const Instance& inst, const MetaChain& m, MetaParticle& p, float dt);
    void placeMeta(const Instance& inst, const MetaChain& m, MetaParticle& p) const;
    Placement chainFrame(const Instance& inst, const MetaChain& m) const;
    static bool busy(const Instance& inst);
    void collectInstance(const Instance& inst, int& group, std::vector<ParticleQuad>& out) const;
    void collectModelsOf(int handle, const Instance& inst, std::vector<EffectModel>& out) const;
    void collectSoundsOf(int handle, const Instance& inst, std::vector<EffectSound>& out) const;
    // cTSParticlesEffect, BV addresses in docs/effects-mac.md §4e.
    void startEmitter(const Instance& inst, Emitter& e, bool hard);
    void applyEmitter(const Instance& inst, Emitter& e, float dt);
    void generate(const Instance& inst, Emitter& e, float dt);
    void addRateCurveParticles(const Instance& inst, Emitter& e, float dt);
    void injectParticles(const Instance& inst, Emitter& e, float dt);
    void maintainParticles(const Instance& inst, Emitter& e, float dt);
    void updateRandomWalk(Emitter& e, float dt);
    Particle initParticle(const Instance& inst, const Emitter& e);
    bool updatePhysics(const Emitter& e, Particle& p, float dt) const;
    // The emitter's transform in its particles' frame (t1).
    Placement emitterFrame(const Instance& inst, const Emitter& e) const;
    // The particles' frame in the world (t2): the instance with a rigid
    // effect, the world otherwise.
    static Placement particleFrame(const Instance& inst, const Emitter& e);
    float random(float a, float b);

    // The instance's transform as it is used: turned to the camera for a
    // view-relative or camera-facing effect (cTSVisualEffect::SetTransforms
    // BV 0xeb1760, flags 0 and 1), without the node's turn with ignoreOrientation.
    Placement facing(const Instance& inst) const;

    const EffectsLibrary& lib_;
    SwarmRandom rng_;
    V3 eye_{}, up_{0, 0, 1};
    bool hasView_ = false;
    float lod_ = kMaxLod;
    std::unordered_map<int, Instance> instances_;
    int next_ = 1;
    int nextId_ = 1;
};

// Composes `child` (relative to `parent`) into a world placement.
Placement compose(const Placement& parent, const Placement& child);
Placement inverse(const Placement& p);
V3 transformPoint(const Placement& p, const V3& v);
V3 rotate(const Placement& p, const V3& v);

} // namespace os2::fx
