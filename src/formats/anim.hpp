#pragma once

// cAnimResourceConst (ANIM, 0xFB00791E). Layout after OpenTS2's
// AnimResourceConstBlock.cs (MPL-2.0, https://github.com/LazyDuchess/OpenTS2).

#include "rcol.hpp"
#include "skeleton.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// One animation: targets (a skeleton such as "auskel", or another tag) with
// channels per bone, morph or material value; each channel has 1-4 components
// of keyframed curves, stored as fixed-point or float values.
//
// Times are in ticks. Baked curves hold 0.03 keys per tick (OpenTS2's
// FramesPerTick), and walk cycles move at velocity = distance / duration
// ≈ 0.00137 per tick; taking the keys as 30 frames per second makes a tick one
// millisecond and the walk 1.37 m/s (inferred; OpenTS2 assumes 24 fps).
struct Animation {
    static constexpr float kTicksPerSecond = 1000.0f;
    // Tangents are half floats in value per ~154 ticks: over the 150,325 keys
    // of smooth continuous curves of the base game, tangent / central slope
    // has a median of 154 ticks (128-189 for 90% of them) whatever the key
    // spacing or the channel length (inferred; the unit's origin is unknown).
    static constexpr float kTangentTicks = 154.0f;

    enum class Curve : uint8_t {
        Baked = 0,         // values only, evenly spaced over the channel's duration
        Continuous = 1,    // time, value, one tangent
        Discontinuous = 2, // time, value, tangent in and out
    };
    // Value encoding of a component. The 16-bit fixed types borrow the top
    // bit of the preceding u16 (the key's time) as their lowest bit.
    enum class Value : uint8_t { Fixed8_7, Fixed9_7, Fixed5_10, Fixed5_11, Fixed3_12, Fixed3_13, Float };

    // Channel attribute (bits 17-21 of the flags): what the channel drives.
    enum Attribute : uint8_t {
        Rotation = 0,    // cTransformNode rotation (Euler XYZ in degrees, replaces the rest rotation)
        Translation = 1, // cTransformNode translation (replaces the rest translation)
        MorphWeight = 2,
        ContactIk = 3,
        WeightIk = 4,
    };
    // Channel data type (bits 22-24).
    enum ChannelType : uint8_t { Float1 = 0, Float3 = 1, Float4 = 2, EulerRotation = 3, TransformXYZ = 4, Float2 = 5 };

    struct Key {
        uint16_t time = 0; // ticks; for baked curves the key's index scaled over the duration
        float value = 0;
        float tangentIn = 0, tangentOut = 0; // continuous keys: both the same
    };

    struct Component {
        uint8_t format = 0; // raw byte: curve type in bits 0-1, value type above
        Curve curve = Curve::Baked;
        Value value = Value::Float;
        uint8_t unknown = 0;
        uint32_t unknown2 = 0;
        std::vector<Key> keys;

        // Value at `ticks` (clamped to the keys; linear between baked keys,
        // cubic Hermite with the keys' tangents otherwise).
        float sample(float ticks, uint32_t duration) const;
    };

    struct Channel {
        std::string name; // bone, morph or material value name
        uint32_t nameHash = 0;
        uint32_t flags = 0;
        std::vector<Component> components;

        uint32_t duration() const { return flags & 0x7FFF; }
        uint8_t attribute() const { return uint8_t((flags >> 17) & 0x1F); }
        uint8_t type() const { return uint8_t((flags >> 22) & 0x7); }
        int ikChain() const { return ((flags >> 25) & 0xF) == 0xF ? -1 : int((flags >> 25) & 0xF); }
        uint32_t componentCount() const { return (flags >> 29) & 0x7; }
    };

    struct IkTarget {
        uint16_t targetIndex = 0, chainIndex = 0;
        uint8_t type = 0;
        // Name CRCs: bone, its mirror, rotation, rotation 2, translation, contact.
        std::array<uint32_t, 6> crcs{};
    };

    struct IkChain {
        uint16_t targetIndex = 0;
        uint32_t nameCrc = 0, mirrorCrc = 0;
        uint32_t strategy = 0; // 0x48EE7A41 seven degrees of freedom, 0x09F9E87E look at, 0x0FA99547 look at (dog)
        // Begin bone, its mirror, end bone, its mirror, twist vector, twist
        // vector bone, its mirror, IK weight channel.
        std::array<uint32_t, 8> crcs{};
        std::vector<IkTarget> targets;
    };

    struct Target {
        std::string tag; // e.g. "auskel"
        uint16_t animType = 0;
        uint8_t channelType = 0;
        uint16_t tagLength = 0;
        std::vector<Channel> channels;
        std::vector<IkChain> ikChains;
    };

    struct Event {
        uint16_t ticks = 0;
        int16_t type = 0;
        std::string data;
    };

    uint32_t version = 0;
    SgResource resource;
    uint32_t serializedSize = 0;
    uint16_t duration = 0; // ticks
    uint8_t dataVersion = 0;
    uint8_t flags = 0; // bit 0: full-body skeletal
    uint8_t priority = 0;
    uint8_t locomotionType = 0;
    float headingOffset = 0, locomotionDistance = 0, cyclePortionCompleted = 0, strideLength = 0,
          strideVelocity = 0, velocity = 0, turnRotation = 0, preStretchFactor = 0, rootStretchDisplacement = 0;
    std::string skeletonTag;
    std::string dataString;
    std::vector<Target> targets;
    std::vector<Event> events;

    static Animation read(BinReader& r);
    static Animation parse(std::span<const uint8_t> body);

    const Target* findTarget(std::string_view tag) const; // case-insensitive
};

// Local bone transforms of `skeleton` at `ticks` (wrapped to the animation's
// duration when `loop`): the rest pose with the rotation and translation
// channels of `target` applied to the bones of the same name. Bones the
// animation does not key keep their rest transform.
std::vector<Xform> samplePose(const Animation::Target& target, const Skeleton& skeleton, float ticks);
float animationTime(const Animation& a, float seconds, bool loop);

// Two-bone IK for the target's chains (e.g. legs: r_thigh -> r_calf -> r_foot
// reaching r_foot_ikctr, knee towards r_foot_ikpole): bones are named by the
// CRC32 of their lowercase names; the middle bone is the end bone's parent;
// the end bone takes the goal's rotation (keeping the rest-pose offset
// between them). Chains with an IK weight channel are blended by its value
// at `ticks`. Updates `local` in place.
void applyIk(const Animation::Target& target, const Skeleton& skeleton, float ticks, std::vector<Xform>& local);

// IEEE 754 half float (the curves' tangents).
float halfToFloat(uint16_t h);

// Decodes one fixed-point value (sign-extended `bits`-bit integer over 2^fraction).
float animFixedToFloat(Animation::Value type, int32_t raw);

} // namespace os2
