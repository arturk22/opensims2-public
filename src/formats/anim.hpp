#pragma once

// cAnimResourceConst (ANIM, 0xFB00791E). Layout after OpenTS2's
// AnimResourceConstBlock.cs (MPL-2.0, https://github.com/LazyDuchess/OpenTS2).

#include "rcol.hpp"
#include "skeleton.hpp"

#include <array>
#include <map>
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
// ≈ 0.002 per tick. A tick is one millisecond: in 92 of the 144 animations
// with a single sound the duration in ticks is the sound's length in ms. The
// normal adult walk then moves 2.0 m/s (1.45 cycles a second, two steps of
// 0.70 m); 1.37 m/s is the elders' and the Grim Reaper's walk.
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
        // Bit 15 (cSharedChannelCubicKeysConstProxy4::IsBaseDataReturned,
        // Mac BV 0xee4680): set on a base channel, clear on a delta
        // (additive) one, which cActiveAnimSkeletal::SetupChannel (BV
        // 0xd5b8d4) adds to the bone's outlet with AddDeltaChannel instead
        // of AddBaseChannel: docs/primitives-mac.md §4.
        bool isDelta() const { return (flags & 0x8000) == 0; }
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

        // Bone channels (rotation or translation) that replace (base) or add
        // to (delta) what the other animations put on the bone. In the base
        // game the delta channels are all in targets of anim type 6, and
        // those have no base channels (the "*Blend" walk postures, the
        // talking "SWAT" shifts, the sofa bounces, the haunted chair, the
        // elders' bent spine: s2pkg anim-sweep).
        bool hasBaseChannels() const;
        bool hasDeltaChannels() const;
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
    // The skeleton's target only adds to the bones (delta channels and no
    // base ones): it plays over the body, never instead of it.
    bool additive() const;
};

// Local bone transforms of `skeleton` at `ticks` (wrapped to the animation's
// duration when `loop`): the rest pose with the rotation and translation
// channels of `target` applied to the bones of the same name. Bones the
// animation does not key keep their rest transform.
// Delta channels are left out (sampleDeltas).
std::vector<Xform> samplePose(const Animation::Target& target, const Skeleton& skeleton, float ticks);

// The target's delta (additive) channels at `ticks`: per bone a rotation
// (identity where none) and a translation (zero where none), and which
// bones have one (bit 0 rotation, bit 1 translation). The channel blender
// multiplies the rotations onto the bone's base mix and adds the
// translations (cChannelUberOutletQ/XYZ::ChannelData, Mac BV 0xd5e1c4 and
// 0xd6b0fc; docs/primitives-mac.md §4).
struct DeltaPose {
    std::vector<Xform> delta;
    std::vector<uint8_t> has;
    bool empty() const;
};
DeltaPose sampleDeltas(const Animation::Target& target, const Skeleton& skeleton, float ticks);
float animationTime(const Animation& a, float seconds, bool loop);

// Two-joint IK for the target's chains (legs: r_thigh -> r_calf -> r_foot
// reaching r_foot_ikctr, knee towards r_foot_ikpole; arms: r_upperarm ->
// r_forearm -> r_hand reaching r_handcontrol0, elbow towards r_hand_ikpole,
// the bicep and wrist twist bones carried along): bones are named by the
// CRC32 of their lowercase names; the joint is the chain's bone farthest
// from both ends. The goal (and the pole) are their nodes' animated local
// transforms in the space of the target's (the chain's twist) bone, or of the
// model root when it has none (spine0 for a2o-newspaper-read-start's hands,
// none for the read loops), not where the skeleton hangs them. The end bone
// takes the goal's rotation: the second rotation node's (the hands'
// *_handcontrol0rot) as it is, else the rotation node's with the rest-pose
// offset between them (the feet). Chains with an IK weight channel are
// blended by its value at `ticks`. Updates `local` in place. `goals`: the
// local transforms the goal and pole nodes are read from (the animation's
// own pose when `local` is a blend of several layers); default `local`.
// `modelFrame`: the frame of the goals keyed in model space (an animation
// played turned by its header heading: AnimPlayer::Options::turn).
void applyIk(const Animation::Target& target, const Skeleton& skeleton, float ticks, std::vector<Xform>& local,
             const std::vector<Xform>* goals = nullptr, const Xform& modelFrame = Xform{});

// Snap events (ANIM event type 1005): "snapObjectRoot=<object>:__<model>__"
// and "snapTargetSlot=<skeleton>:__<bone>__" or "Oblivion". They move a
// model an object shows into a slot of the animated skeleton or out of
// sight while the animation plays: a2o-newspaper-read-start holds the
// rolled paper ("newspaper:__newspaperRolled__") in the right hand grip,
// passes a second copy ("newspaperRoll_02") to the left hand, swaps it for
// the open paper ("newspaperRead_02:__newspaperReading__") and gives that
// to the right hand. The object names are the object's own (its 3D object
// name, "newspaper") or extra copies the animation names.
struct AnimSnap {
    uint16_t ticks = 0;
    std::string object, model;  // "newspaper", "newspaperRolled"
    std::string skeleton, bone; // "auSkel", "R_Hand_grip"; both empty: Oblivion
};
std::vector<AnimSnap> animSnaps(const Animation& a);
// Where every object the snaps name is at `ticks` (events at or before it,
// in order): its model and slot (empty bone: out of sight).
std::map<std::string, AnimSnap> snapStateAt(const std::vector<AnimSnap>& snaps, float ticks);

// IEEE 754 half float (the curves' tangents).
float halfToFloat(uint16_t h);

// Decodes one fixed-point value (sign-extended `bits`-bit integer over 2^fraction).
float animFixedToFloat(Animation::Value type, int32_t raw);

} // namespace os2
