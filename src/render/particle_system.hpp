#pragma once

// Runs the game's particle effects (formats/effects.hpp): instances of
// visual effects whose particle components emit, move and fade particles,
// collected as camera-facing quads for ParticleLayer. Plain C++ (no Metal),
// so tests can drive it. What follows the game's runtime
// (cTSParticlesEffect, Mac binary) and what is inferred is in
// docs/effects-mac.md.

#include "formats/effects.hpp"

#include <array>
#include <cstdint>
#include <random>
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
    // Sorting: particles that move with their effect (balloons) sort by the
    // effect's position, then in component order (later ones on top).
    V3 sortPosition{};
    int sortOrder = 0;
};

class EffectSystem {
public:
    explicit EffectSystem(const EffectsLibrary& library, uint32_t seed = 1);

    // Starts the named effect; -1 if the library has no such effect.
    int start(std::string_view name, const Placement& where);
    // Stops emitting; `hard` also removes the particles alive.
    void stop(int handle, bool hard = false);
    void move(int handle, const Placement& where);
    // Texture for the particles whose effect texture is an icon placeholder
    // (balloon icons: "...thumbnailprimitive" components).
    // `frame` picks the cell of an icon atlas (-1: the effect's own cell).
    void setIcon(int handle, std::string texture, int frame = -1);
    // True while the instance emits or has particles alive.
    bool alive(int handle) const;
    size_t instanceCount() const { return instances_.size(); }

    // Advances every instance; finished, stopped instances are dropped.
    void update(float seconds);
    void collect(std::vector<ParticleQuad>& out) const;

private:
    struct Particle {
        V3 position{}, velocity{};
        float age = 0, life = 1;
        float sizeScale = 1, aspectScale = 1, alphaScale = 1;
        V3 colorScale{1, 1, 1};
        float walkTimer = 0;
        float spin = 0; // rotate offset, radians
    };
    struct Emitter {
        const Particles* desc = nullptr;
        Placement local;  // relative to the instance
        float emitScale = 1, sizeScale = 1, alphaScale = 1, timeScale = 1;
        float time = 0, pending = 0;
        bool icon = false; // the texture is the instance's icon
        bool localSpace = false;
        float startDelay = 0, stopAt = -1; // sequences
        // The meta particle carrying this emitter's effect (balloons): it
        // scales and fades the effect over its life, which ends it.
        const MetaParticles* meta = nullptr;
        float metaLife = 0;
        std::vector<Particle> particles;
    };
    struct Instance {
        Placement where;
        std::vector<Emitter> emitters;
        bool stopped = false;
        std::string icon;
        int iconFrame = -1;
    };

    void addEffect(Instance& inst, uint32_t visual, const Placement& local, float emitScale, float sizeScale,
                   float alphaScale, float timeScale, float delay, float stopAt, int depth,
                   const MetaParticles* meta = nullptr, float metaLife = 0);
    // The meta particle's age fraction for an emitter (< 0: none; > 1 dead).
    static float metaAge(const Emitter& e);
    void stepEmitter(const Instance& inst, Emitter& e, float dt, bool emit);
    void spawn(const Instance& inst, Emitter& e);
    float random(float a, float b);
    V3 toWorld(const Instance& inst, const Emitter& e, const V3& p) const;

    const EffectsLibrary& lib_;
    std::mt19937 rng_;
    std::unordered_map<int, Instance> instances_;
    int next_ = 1;
};

// Composes `child` (relative to `parent`) into a world placement.
Placement compose(const Placement& parent, const Placement& child);
V3 transformPoint(const Placement& p, const V3& v);
V3 rotate(const Placement& p, const V3& v);

} // namespace os2::fx
