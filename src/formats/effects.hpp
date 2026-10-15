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

// Flag bits of a particle description (the binary's std::bitset<38> at
// cTSParticlesDescription +0x158; a version <= 4 block stores bits 0-31 as
// one u32), named after the effects script's options (docs/effects-mac.md
// §4e.2).
namespace pflag {
constexpr uint64_t Inject = 1ull << 0;        // rate -> bursts at each cycle's start (InjectParticles)
constexpr uint64_t Maintain = 1ull << 1;      // rate -> the count kept alive (MaintainParticles)
constexpr uint64_t Sustain = 1ull << 2;       // rate -sustain: the last cycle is held, not ended
constexpr uint64_t Radial = 1ull << 6;        // emit -radial: the direction is the birth point
constexpr uint64_t Ellipsoid = 1ull << 8;     // source ellipse / ellipsoid
constexpr uint64_t SizeScale = 1ull << 11;    // rate -sizeScale: emission x the effect's scale
constexpr uint64_t AreaScale = 1ull << 12;    // ... x scale^2
constexpr uint64_t VolumeScale = 1ull << 13;  // ... x scale^3
constexpr uint64_t RandomWalk = 1ull << 20;   // randomWalk: one walking force for the whole effect
constexpr uint64_t Shape = 1ull << 22;        // the "texture" is a shape (mesh)
constexpr uint64_t Light = 1ull << 23;        // texture -light: lit by the scene
constexpr uint64_t AcceptOverride = 1ull << 24; // the texture can be replaced (balloon icons)
constexpr uint64_t Attractor = 1ull << 26;    // force -attractor
constexpr uint64_t AttractorPoint = 1ull << 27; // the attractor has its own point (else the effect's target)
constexpr uint64_t CycleKill = 1ull << 30;    // rate -cycleKill: each new cycle clears the old ones
constexpr uint64_t DirectLayer = 1ull << 31;  // texture -directLayer: the layer as it is, not x 8 + 7
// Flags that need the full time-stepped physics (the binary's mask, built
// in a __StaticInit at BV 0x26447f4): colliders 3/4, terrain 15/17, attractor 26.
constexpr uint64_t NeedsTimeStep = (1ull << 3) | (1ull << 4) | (1ull << 15) | (1ull << 17) | (1ull << 26);
} // namespace pflag

// Flag bits of a meta particle description (std::bitset<34>; the file has
// bits 32-33 in its first u32 and bits 0-31 in the second, as the binary's
// operator>> reads them, BV 0x1c229d0).
namespace mflag {
constexpr uint64_t Inject = 1ull << 0;
constexpr uint64_t Maintain = 1ull << 1;
constexpr uint64_t Sustain = 1ull << 2;
constexpr uint64_t Ellipsoid = 1ull << 11;
constexpr uint64_t RandomWalkSync = 1ull << 23;
constexpr uint64_t RandomWalk = 1ull << 24;   // two random walks per meta particle (cTSRWState)
constexpr uint64_t RandomWalkWait = 1ull << 25;
constexpr uint64_t Attractor = 1ull << 27;
constexpr uint64_t Tractor = 1ull << 29;
constexpr uint64_t Path = 1ull << 30;         // the particle follows the path points (SnapParticleToPath)
constexpr uint64_t ResetIncoming = 1ull << 32;
} // namespace mflag

// Flag bits of a visual effect (the "effect" block's options, parser BV
// 0x1a7e510) and of its components (CreateComponentFromDescription 0xc042e4).
namespace vflag {
constexpr uint32_t CameraFacing = 1u << 1;
constexpr uint32_t HardStop = 1u << 4;
constexpr uint32_t Rigid = 1u << 5;           // the particles live in the effect's frame and move with it
constexpr uint32_t RealTime = 1u << 12;
} // namespace vflag
namespace cflag {
constexpr uint32_t Chain = 1u << 1;           // the chain target of the chain component before it
constexpr uint32_t ChainEnd = 1u << 2;        // ... and the last of that chain
constexpr uint32_t StateChain = 1u << 3;      // the next ones (up to one with bit 4) are its states
constexpr uint32_t StateChainEnd = 1u << 4;
} // namespace cflag

struct Particles {
    uint16_t version = 0;
    uint64_t flags = 0;          // pflag::
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
    uint8_t rotateCount = 1;     // rotate commands (1 or 2)
    // Per rotation: each particle's multiplier of its curve, random in
    // this range (a stored x > 0 gives [-x, x], else [1, 1]), and a fixed
    // offset in turns.
    std::array<V2, 2> rotateRange{V2{1, 1}, V2{1, 1}};
    std::array<float, 2> rotateOffset{};
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
    // Physics (force command): the constant force (gravity and wind, one
    // vector), the world wind's multiplier, the bomb (a speed away from its
    // centre at birth) and the drag (v *= 1 - drag x dt); stretch along the
    // velocity.
    V3 force{};
    float globalWind = 0, bomb = 0;
    V3 bombCenter{};
    float drag = 0, stretch = 0;
    float screw = 0;
    std::vector<Wiggle> wiggles;
    std::array<uint8_t, 4> bloom{};
    std::string colliderName;
    float terrainBounce = 0, killHeight = 0;
    V2 randomWalkDelay{}, randomWalkStrength{};
    float randomWalkTurnX = 0, randomWalkTurnY = 0;
    // The attractor (version < 8; cAttractorDescription): its point (in the
    // emitter's frame, with pflag::AttractorPoint), the pull over the
    // distance / range (clamped to 1) and that range.
    V3 attractor{};
    Curve attractorStrength;
    float attractorRange = 1;
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
    float globalWind = 0, bomb = 0;
    V3 bombCenter{};
    float drag = 0;
    float screw = 0;
    std::vector<Wiggle> wiggles;
    // Two random walks (cRandomWalk: delay range, strength range, turn,
    // minimum turn, vertical mix) and the direction they prefer.
    struct Walk {
        V2 delay{}, strength{};
        float turn[3]{};
    };
    std::string colliderName;
    std::array<Walk, 2> walks{};
    V3 walkPreferDirection{};
    float alignDamp = 0, alignBankWind = 0, alignBank = 0;
    // Attractor (version < 4): point, strength over life, range.
    V3 attractor{};
    Curve attractorStrength;
    float attractorRange = 0;
    // The path (version < 3; OpenTS2 calls them tractor points): position,
    // velocity and time of each point, in the emitter's frame.
    struct PathPoint {
        V3 position{}, velocity{};
        float time = 0;
    };
    std::vector<PathPoint> path;
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
