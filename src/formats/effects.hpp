#pragma once

// The game's particle effects (Andrew Willmott's "Swarm" system): the one
// resource of type 0xEA5118B0 in TSData/Res/Effects/effects.package, a
// compiled form of Maxis' effect scripts. Layout from OpenTS2's EffectsCodec
// (refs/OpenTS2 Files/Formats/DBPF/EffectsCodec.cs), checked against the
// base-game file (every block parses to the "end" name). See
// docs/effects-mac.md.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace os2::fx {

constexpr uint32_t kTypeEffects = 0xEA5118B0;

using V2 = std::array<float, 2>;
using V3 = std::array<float, 3>;
// A curve sampled evenly over a particle's (or emitter's) life.
using Curve = std::vector<float>;

// Linear interpolation of `c` at t in [0, 1]; `fallback` when empty.
float sampleCurve(const Curve& c, float t, float fallback = 1.0f);
V3 sampleColors(const std::vector<V3>& c, float t);

struct Box {
    V3 lo{}, hi{};
};

// How a particle is blended (ParticleDrawing.DrawType).
enum class DrawType : uint8_t {
    Decal = 0,              // alpha blend
    DecalInvertDepth = 1,
    DecalIgnoreDepth = 2,
    DepthDecal = 3,
    Additive = 4,
    AdditiveInvertDepth = 5,
    AdditiveIgnoreDepth = 6,
    Modulate = 7,
};

struct Wiggle {
    float timeRate = 0;
    V3 rateDirection{}, wiggleDirection{};
};

struct Particles {
    uint16_t version = 0;
    uint64_t flags = 0;          // bit 8 ellipsoid emitter, 21 material, 22 shape, 23 material is light
    V2 life{};                   // seconds, random in [x, y]
    float preRoll = 0;           // seconds simulated before the first frame
    V2 rateDelay{}, rateTrigger{};
    Box emitDirection;           // velocity direction, random inside the box
    V2 emitSpeed{};
    Box emitVolume;              // where particles are born
    float torusWidth = -1;       // > 0: torus emitter
    Curve rateCurve;             // particles per second over rateCurveTime
    float rateCurveTime = 0;
    uint32_t rateCurveCycles = 0; // 0 = loop forever
    float rateSpeedScale = 0;
    Curve sizeCurve;
    float sizeVary = 0;
    Curve aspectCurve;
    float aspectVary = 0;
    V3 rotateAxis{};
    float rotateOffsetX = 0, rotateOffsetY = 0;
    Curve rotateCurveX, rotateCurveY; // turns over life
    Curve alphaCurve;
    float alphaVary = 0;
    std::vector<V3> colors;
    V3 colorVary{};
    std::string material;        // texture name (TXTR "<name>_txtr")
    uint8_t tileU = 1, tileV = 1, alignment = 0;
    DrawType drawType = DrawType::Decal;
    float layer = 0, frameSpeed = 0;
    uint8_t frameStart = 0, frameCount = 0;
    // Physics: the two (vector, float, float) groups after the drawing block.
    // The first is the constant force (gravity: z < 0 falls) with wind
    // strength and drag; the second one OpenTS2 leaves unnamed.
    V3 force{};
    float windStrength = 0, drag = 0;
    V3 force2{};
    float force2a = 0, force2b = 0;
    float screw = 0;
    std::vector<Wiggle> wiggles;
    std::array<uint8_t, 4> bloom{};
    std::string colliderName;
    float terrainBounce = 0, killHeight = 0;
    V2 randomWalkDelay{}, randomWalkStrength{};
    float randomWalkTurnX = 0, randomWalkTurnY = 0;
};

// A particle system whose particles are whole effects (meta particles).
struct MetaParticles {
    uint16_t version = 0;
    uint64_t flags = 0;
    V2 life{};
    float preRoll = 0;
    V2 rateDelay{}, rateTrigger{};
    Box emitDirection;
    V2 emitSpeed{};
    Box emitVolume;
    float torusWidth = -1;
    Curve rateCurve;
    float rateCurveTime = 0;
    uint32_t rateCurveCycles = 0;
    Curve sizeCurve;
    float sizeVary = 0;
    Curve aspectCurve;
    float aspectVary = 0;
    std::vector<V3> colors;
    Curve alphaCurve;
    std::string baseEffect;      // the visual effect each meta particle carries
    uint8_t alignment = 0;
    V3 force{};
    float windStrength = 0, drag = 0;
    // The second (vector, float) group: the float taken as the drag factor,
    // as the particles' (inferred).
    V3 force2{};
    float force2a = 0;
    float screw = 0;
    std::vector<Wiggle> wiggles;
    // Two random walks (OpenTS2's RandomWalk: delay range, strength range,
    // three turn floats) and the direction they prefer.
    struct Walk {
        V2 delay{}, strength{};
        float turn[3]{};
    };
    std::array<Walk, 2> walks{};
    V3 walkPreferDirection{};
    float alignDamp = 0, alignBankWind = 0, alignBank = 0;
    // Attractor (version < 4): point, strength over life, range.
    V3 attractor{};
    Curve attractorStrength;
    float attractorRange = 0;
    // Tractor points (OpenTS2: two vectors and a float each).
    struct Tractor {
        V3 a{}, b{};
        float f = 0;
    };
    std::vector<Tractor> tractors;
};

struct Decal {
    std::string texture;
    uint8_t drawType = 0, lifeType = 0;
    float life = 0;
    Curve rotateCurve, sizeCurve, alphaCurve;
    std::vector<V3> colors;
    Curve aspectCurve;
    float textureRepeat = 1;
    V2 textureOffset{};
};

struct Sequence {
    struct Item {
        V2 activateTime{};
        std::string effect;
    };
    std::vector<Item> items;
    uint32_t flags = 0;          // bit 0 loop
};

struct Sound {
    uint32_t flags = 0, audioId = 0;
    float locationUpdateDelta = 0, playTime = 0, volume = 0;
};

struct Model {
    std::string model;           // CRES name
    float size = 1;
    V3 color{1, 1, 1};
    float alpha = 1;
};

struct Screen {
    uint8_t mode = 0;
    uint32_t flags = 0;
    std::vector<V3> colors;
    Curve strength;
    float length = 0, delay = 0;
    std::string texture;
};

// Block types a visual effect's component refers to.
enum class Block : uint8_t {
    Visual = 0, Particles = 1, MetaParticles = 2, Decal = 3, Sequence = 6, Sound = 7,
    Camera = 8, Model = 10, Screen = 11, Water = 12,
};

struct Component {
    std::string name;            // the referenced block's own name
    Block type = Block::Visual;
    uint32_t flags = 0;
    // Placement relative to the effect: flags byte, uniform scale, 3x3
    // rotation (row-major, as stored) and offset.
    uint8_t transformFlags = 0;
    float scale = 1;
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 offset{};
    uint8_t lodBegin = 0, lodEnd = 0;
    V2 emitScale{1, 1}, sizeScale{1, 1}, alphaScale{1, 1};
    uint16_t selectionGroup = 0, selectionChance = 0;
    float timeScale = 1;
    int32_t index = -1;          // into the block type's array
};

struct VisualEffect {
    uint32_t flags = 0;
    std::vector<Component> components;
};

struct EffectsLibrary {
    std::vector<Particles> particles;
    std::vector<MetaParticles> metaParticles;
    std::vector<Decal> decals;
    std::vector<Sequence> sequences;
    std::vector<Sound> sounds;
    size_t cameras = 0, waters = 0;
    std::vector<Model> models;
    std::vector<Screen> screens;
    std::vector<VisualEffect> visuals;
    // Effect name (lowercase as stored) -> index in `visuals`.
    std::unordered_map<std::string, uint32_t> names;

    static EffectsLibrary parse(std::span<const uint8_t> data);

    // Visual effect by name, case-insensitive.
    const VisualEffect* find(std::string_view name) const;
    std::optional<uint32_t> indexOf(std::string_view name) const;
};

} // namespace os2::fx
