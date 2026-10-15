#include <cstdlib>
#include <cstdio>
// Layout after OpenTS2's AnimResourceConstBlock.cs (MPL-2.0).
#include "anim.hpp"

#include "resindex.hpp"

#include <algorithm>
#include <optional>
#include <cctype>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace os2 {

namespace {

// String sections are followed by (length % 4) padding bytes 0, 1, 2 (OpenTS2's
// rule; it fits all 11,757 ANIM resources of the base game, though it does not
// align the section to 4 bytes).
void readPadding(BinReader& r, size_t start, const char* what) {
    const size_t len = r.pos() - start;
    const size_t pad = len % 4;
    const auto bytes = r.bytes(pad);
    for (size_t i = 0; i < pad; ++i)
        if (bytes[i] != i)
            throw FormatError(std::string("cAnimResourceConst: unexpected padding after ") + what);
}

bool sixteenBits(Animation::Value v) {
    return v == Animation::Value::Fixed9_7 || v == Animation::Value::Fixed5_11 || v == Animation::Value::Fixed3_13;
}

Animation::Value valueType(uint8_t format) {
    if (format & 0x10)
        return Animation::Value::Float;
    const bool baked = (format & 3) == 0;
    switch ((format >> 2) & 7) {
    case 0: return baked ? Animation::Value::Fixed8_7 : Animation::Value::Fixed9_7;
    case 1: return baked ? Animation::Value::Fixed5_10 : Animation::Value::Fixed5_11;
    case 3: return baked ? Animation::Value::Fixed3_12 : Animation::Value::Fixed3_13;
    default: throw FormatError("cAnimResourceConst: unknown value type " + hexString(format));
    }
}

// A key's value; 16-bit fixed types take their low bit from the top bit of
// `preceding` (the key's time), which is then cleared.
float readValue(BinReader& r, Animation::Value type, uint16_t* preceding) {
    if (type == Animation::Value::Float)
        return r.f32();
    if (sixteenBits(type)) {
        if (!preceding)
            throw FormatError("cAnimResourceConst: 16-bit fixed value in a baked curve");
        const int32_t raw = int32_t(r.i16()) * 2 | ((*preceding >> 15) & 1);
        *preceding &= 0x7FFF;
        return animFixedToFloat(type, raw);
    }
    return animFixedToFloat(type, r.i16());
}

float tangent(BinReader& r) { return halfToFloat(r.u16()); }

uint32_t readCrc(BinReader& r) {
    const uint32_t v = r.u32();
    r.u8(); // NUL after the CRC, so the game can treat it as a string
    return v;
}

} // namespace

float halfToFloat(uint16_t h) {
    const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float v;
    if (exponent == 0)
        v = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? NAN : INFINITY;
    else
        v = std::ldexp(float(1024 + mantissa), exponent - 25);
    return (h & 0x8000) ? -v : v;
}

float animFixedToFloat(Animation::Value type, int32_t raw) {
    switch (type) {
    case Animation::Value::Fixed8_7:
    case Animation::Value::Fixed9_7: return float(raw) / 128.0f;
    case Animation::Value::Fixed5_10: return float(raw) / 1024.0f;
    case Animation::Value::Fixed5_11: return float(raw) / 2048.0f;
    case Animation::Value::Fixed3_12: return float(raw) / 4096.0f;
    case Animation::Value::Fixed3_13: return float(raw) / 8192.0f;
    case Animation::Value::Float: break;
    }
    return float(raw);
}

float Animation::Component::sample(float ticks, uint32_t duration) const {
    if (keys.empty())
        return 0;
    if (keys.size() == 1 || ticks <= keys.front().time)
        return keys.front().value;
    if (ticks >= keys.back().time)
        return keys.back().value;
    // Keys are sorted by time (baked keys are spaced evenly at read time).
    auto it = std::upper_bound(keys.begin(), keys.end(), ticks, [](float t, const Key& k) { return t < k.time; });
    const Key& b = *it;
    const Key& a = *(it - 1);
    (void)duration;
    if (b.time == a.time)
        return b.value;
    const float dt = float(b.time - a.time);
    const float u = (ticks - a.time) / dt;
    if (curve == Curve::Baked)
        return a.value + (b.value - a.value) * u;
    // Cubic Hermite: tangents are slopes per kTangentTicks ticks.
    const float m0 = a.tangentOut * dt / kTangentTicks, m1 = b.tangentIn * dt / kTangentTicks;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a.value + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b.value + (u3 - u2) * m1;
}

Animation Animation::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    auto a = read(r);
    r.expectEnd("cAnimResourceConst");
    return a;
}

Animation Animation::read(BinReader& r) {
    Animation a;
    a.version = r.u32();
    a.resource = SgResource::read(r);
    a.serializedSize = r.u32();
    a.duration = r.u16();
    const uint16_t targetCount = r.u16();
    const uint16_t eventCount = r.u16();
    const uint8_t dataStringLength = r.u8();
    a.dataVersion = r.u8();
    a.flags = r.u8();
    a.priority = r.u8();
    a.locomotionType = r.u8();
    const uint8_t skeletonTagLength = r.u8();
    r.skip(16); // runtime pointers
    a.headingOffset = r.f32();
    a.locomotionDistance = r.f32();
    a.cyclePortionCompleted = r.f32();
    a.strideLength = r.f32();
    a.strideVelocity = r.f32();
    a.velocity = r.f32();
    a.turnRotation = r.f32();
    a.preStretchFactor = r.f32();
    a.rootStretchDisplacement = r.f32();

    size_t start = r.pos();
    a.skeletonTag = r.cstr();
    a.dataString = r.cstr();
    if (a.skeletonTag.size() != skeletonTagLength || a.dataString.size() != dataStringLength)
        throw FormatError("cAnimResourceConst: string lengths do not match the header");
    readPadding(r, start, "the header strings");

    a.targets.resize(targetCount);
    std::vector<uint16_t> channelCounts(targetCount);
    std::vector<uint8_t> chainCounts(targetCount);
    for (size_t i = 0; i < targetCount; ++i) {
        auto& t = a.targets[i];
        r.skip(8);
        t.animType = r.u16();
        channelCounts[i] = r.u16();
        chainCounts[i] = r.u8();
        t.channelType = r.u8();
        t.tagLength = r.u16();
        r.skip(12);
        t.channels.resize(channelCounts[i]);
    }
    start = r.pos();
    for (auto& t : a.targets)
        t.tag = r.cstr();
    readPadding(r, start, "the target tags");

    for (auto& t : a.targets)
        for (auto& c : t.channels) {
            r.skip(8);
            c.nameHash = r.u32();
            r.skip(4);
            c.flags = r.u32();
            r.skip(4);
        }
    start = r.pos();
    for (auto& t : a.targets)
        for (auto& c : t.channels)
            c.name = r.cstr();
    readPadding(r, start, "the channel names");

    for (auto& t : a.targets)
        for (auto& c : t.channels) {
            c.components.resize(c.componentCount());
            for (auto& comp : c.components) {
                comp.keys.resize(r.u16());
                comp.format = r.u8();
                comp.unknown = r.u8();
                comp.unknown2 = r.u32();
                if ((comp.format & 3) == 3)
                    throw FormatError("cAnimResourceConst: unknown curve type " + hexString(comp.format));
                comp.curve = Curve(comp.format & 3);
                comp.value = valueType(comp.format);
            }
        }

    for (auto& t : a.targets)
        for (auto& c : t.channels)
            for (auto& comp : c.components) {
                const size_t n = comp.keys.size();
                for (size_t k = 0; k < n; ++k) {
                    Key& key = comp.keys[k];
                    switch (comp.curve) {
                    case Curve::Baked:
                        key.value = readValue(r, comp.value, nullptr);
                        key.time = n > 1 ? uint16_t(uint32_t(c.duration()) * k / (n - 1)) : 0;
                        break;
                    case Curve::Continuous:
                        key.time = r.u16();
                        key.value = readValue(r, comp.value, &key.time);
                        key.tangentIn = key.tangentOut = tangent(r);
                        break;
                    case Curve::Discontinuous:
                        key.time = r.u16();
                        key.value = readValue(r, comp.value, &key.time);
                        key.tangentIn = tangent(r);
                        key.tangentOut = tangent(r);
                        break;
                    }
                }
            }

    for (size_t i = 0; i < targetCount; ++i) {
        auto& t = a.targets[i];
        t.ikChains.resize(chainCounts[i]);
        for (auto& ch : t.ikChains) {
            r.skip(8);
            ch.targetIndex = r.u16();
            ch.targets.resize(r.u16());
            ch.nameCrc = readCrc(r);
            ch.mirrorCrc = readCrc(r);
            ch.strategy = r.u32();
            for (auto& crc : ch.crcs)
                crc = readCrc(r);
            r.skip(4);
        }
    }
    for (auto& t : a.targets)
        for (auto& ch : t.ikChains)
            for (auto& it : ch.targets) {
                r.skip(8);
                it.targetIndex = r.u16();
                it.chainIndex = r.u16();
                it.type = r.u8();
                for (auto& crc : it.crcs)
                    crc = readCrc(r);
            }

    a.events.resize(eventCount);
    for (auto& e : a.events) {
        r.skip(4);
        e.ticks = r.u16();
        e.type = r.i16();
        r.u16(); // data length
        r.skip(4);
    }
    for (auto& e : a.events)
        e.data = r.cstr();
    return a;
}

float animationTime(const Animation& a, float seconds, bool loop) {
    float ticks = seconds * Animation::kTicksPerSecond;
    if (a.duration == 0)
        return 0;
    if (loop)
        ticks = std::fmod(ticks, float(a.duration));
    return std::clamp(ticks, 0.0f, float(a.duration));
}

std::vector<Xform> samplePose(const Animation::Target& target, const Skeleton& skeleton, float ticks) {
    constexpr float kDegrees = 3.14159265358979f / 180.0f;
    std::vector<Xform> pose = skeleton.restPose();
    for (const auto& c : target.channels) {
        const int bone = skeleton.find(c.name);
        if (bone < 0 || c.components.size() < 3)
            continue;
        float v[3];
        for (int k = 0; k < 3; ++k)
            v[k] = c.components[size_t(k)].sample(ticks, c.duration());
        if (c.isDelta())
            continue;
        Xform& x = pose[size_t(bone)];
        // The key turned by the joint's orientation (the clavicles' twist):
        // the rest pose is orient * the bind key (inferred from the data).
        if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation)
            x.q = skeleton.bones[size_t(bone)].orient * quatFromEulerXYZ(v[0] * kDegrees, v[1] * kDegrees, v[2] * kDegrees);
        else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ)
            x.t = {v[0], v[1], v[2]};
    }
    return pose;
}

bool DeltaPose::empty() const {
    for (uint8_t h : has)
        if (h)
            return false;
    return true;
}

DeltaPose sampleDeltas(const Animation::Target& target, const Skeleton& skeleton, float ticks) {
    constexpr float kDegrees = 3.14159265358979f / 180.0f;
    DeltaPose d;
    d.delta.assign(skeleton.bones.size(), Xform{});
    d.has.assign(skeleton.bones.size(), 0);
    for (const auto& c : target.channels) {
        if (!c.isDelta() || c.components.size() < 3)
            continue;
        const int bone = skeleton.find(c.name);
        if (bone < 0)
            continue;
        float v[3];
        for (int k = 0; k < 3; ++k)
            v[k] = c.components[size_t(k)].sample(ticks, c.duration());
        Xform& x = d.delta[size_t(bone)];
        if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation) {
            x.q = quatFromEulerXYZ(v[0] * kDegrees, v[1] * kDegrees, v[2] * kDegrees);
            d.has[size_t(bone)] |= 1;
        } else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ) {
            x.t = {v[0], v[1], v[2]};
            d.has[size_t(bone)] |= 2;
        }
    }
    return d;
}

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 scale(const Vec3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Xform parentWorld(const Skeleton& s, const std::vector<Xform>& world, int bone) {
    const int p = s.bones[size_t(bone)].parent;
    return p >= 0 ? world[size_t(p)] : Xform{};
}

// The rotation whose columns are the orthonormal axes x, y, z.
Quat quatFromAxes(const Vec3& x, const Vec3& y, const Vec3& z) {
    const float m00 = x[0], m10 = x[1], m20 = x[2], m01 = y[0], m11 = y[1], m21 = y[2], m02 = z[0], m12 = z[1], m22 = z[2];
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s};
    }
    return normalized(q);
}

Quat rotY(float a) { return {0, std::sin(a / 2), 0, std::cos(a / 2)}; }
Quat rotX(float a) { return {std::sin(a / 2), 0, 0, std::cos(a / 2)}; }
// The angle q turns about the bone's own Y axis (its twist; the bones lie
// along +Y): the Y component of the solution's Euler angles in the binary,
// whose exact Euler order (0x01020100) was not decoded (inferred).
float twistY(const Quat& q) { return 2 * std::atan2(q.y, q.w); }

} // namespace

void applyIk(const Animation::Target& target, const Skeleton& s, float ticks, std::vector<Xform>& local,
             const std::vector<Xform>* goals, const Xform& modelFrame, const IkObjectBone* ikObjectBone,
             bool mirrored, std::vector<IkReach>* reach) {
    if (target.ikChains.empty())
        return;
    std::unordered_map<uint32_t, int> byCrc;
    for (size_t i = 0; i < s.bones.size(); ++i)
        byCrc.emplace(nameInstanceHiHash(s.bones[i].name), int(i));
    auto bone = [&](uint32_t crc) {
        auto it = crc ? byCrc.find(crc) : byCrc.end();
        return it == byCrc.end() ? -1 : it->second;
    };
    const auto restWorld = s.world(s.restPose());
    // A goal node's transform in the space of `frame` (a bone, or the model
    // root when -1), from its local (animated) transform: the skeleton's own
    // parenting of the controls is not what the animations mean (the hand
    // controls hang under root_trans in auskel, yet a chain with no frame
    // bone keys them in model space, as OpenTS2 also found).
    const std::vector<Xform>& goalLocal = goals && goals->size() == local.size() ? *goals : local;
    // Goal and pole nodes the skeleton lacks: the child skeleton has no
    // *_handcontrol0 nodes and the toddler's none of the hand and foot IK
    // nodes, yet their animations key them (and move the arms and legs only
    // through them). Such a node is its channels alone, over an identity
    // rest transform (nullopt when the target has no channel for it).
    auto virtualNode = [&](uint32_t crc) -> std::optional<Xform> {
        if (!crc)
            return std::nullopt;
        constexpr float kDegrees = 3.14159265358979f / 180.0f;
        std::optional<Xform> x;
        for (const auto& c : target.channels) {
            if (c.nameHash != crc || c.components.size() < 3)
                continue;
            float v[3];
            for (int k = 0; k < 3; ++k)
                v[k] = c.components[size_t(k)].sample(ticks, c.duration());
            if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation) {
                if (!x)
                    x = Xform{};
                x->q = quatFromEulerXYZ(v[0] * kDegrees, v[1] * kDegrees, v[2] * kDegrees);
            } else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ) {
                if (!x)
                    x = Xform{};
                x->t = {v[0], v[1], v[2]};
            }
        }
        return x;
    };
    // A node's local transform: the skeleton's bone, else the channels.
    auto nodeLocal = [&](uint32_t crc) -> std::optional<Xform> {
        if (const int b = bone(crc); b >= 0)
            return goalLocal[size_t(b)];
        return virtualNode(crc);
    };
    // The frame of a type-2 target: the IK object's bone (its mirror twin,
    // reflected, for a layer played flipped: the whole pose is reflected
    // after the solve).
    std::optional<Xform> objectFrame;
    auto inFrame = [&](const std::vector<Xform>& world, int frame, const Xform& node, bool goalNode = true) {
        if (objectFrame && goalNode)
            return objectFrame->then(node);
        // The model root (or the skeleton's top node, auskel) is the
        // animation's frame: turned with it.
        if (frame < 0 || s.bones[size_t(frame)].parent < 0)
            return modelFrame.then(frame >= 0 ? world[size_t(frame)] : Xform{}).then(node);
        return world[size_t(frame)].then(node);
    };

    for (const auto& chain : target.ikChains) {
        if (chain.strategy != 0x48EE7A41 || chain.targets.empty())
            continue; // only the two-joint (seven degrees of freedom) chains
        const int b0 = bone(chain.crcs[0]), b2 = bone(chain.crcs[2]);
        if (b0 < 0 || b2 < 0)
            continue;
        // The chain's bones, begin to end, and each one's IK channel type
        // (cIKStrategy7DOF::SetUberChannelIKTypes, Mac BV 0xd36d38): three
        // bones (thigh, calf, foot) are 1, 4, 6; otherwise (upperarm, bicep,
        // forearm, wrist, hand) 2, 3, 4, 7, 8 and 0 past the fifth.
        std::vector<int> bones;
        for (int b = b2; b >= 0; b = s.bones[size_t(b)].parent) {
            bones.push_back(b);
            if (b == b0)
                break;
        }
        if (bones.back() != b0)
            continue;
        std::reverse(bones.begin(), bones.end());
        std::vector<int> types(bones.size(), 0);
        static constexpr int kThree[] = {1, 4, 6}, kFive[] = {2, 3, 4, 7, 8};
        for (size_t i = 0; i < bones.size(); ++i)
            types[i] = bones.size() == 3 ? kThree[i] : i < 5 ? kFive[i] : 0;
        int joint = -1; // the elbow or knee (type 4)
        for (size_t i = 0; i < bones.size(); ++i)
            if (types[i] == 4)
                joint = int(i);
        if (joint < 0)
            continue;
        const int b1 = bones[size_t(joint)];
        const std::optional<Xform> pole = nodeLocal(chain.crcs[4]);
        const int poleFrame = bone(chain.crcs[5]);
        float weight = 1;
        if (chain.crcs[7]) {
            weight = 0;
            for (const auto& c : target.channels)
                if (c.nameHash == chain.crcs[7] && c.attribute() == Animation::WeightIk && !c.components.empty())
                    weight = std::clamp(c.components[0].sample(ticks, c.duration()), 0.0f, 1.0f);
        }
        if (weight <= 0)
            continue;

        auto world = s.world(local);
        // A target's goal point and end rotation in model space.
        struct Goal {
            Vec3 t;
            std::optional<Quat> q;
        };
        auto targetGoal = [&](const Animation::IkTarget& goal, bool modelFallback) -> std::optional<Goal> {
            const std::optional<Xform> goalNode = nodeLocal(goal.crcs[4]);
            if (!goalNode)
                return std::nullopt;
            const std::optional<Xform> goalRotation = nodeLocal(goal.crcs[2]), goalRotation2 = nodeLocal(goal.crcs[3]);
            // Type 0: no frame bone; 1: the skeleton's own; 2: the IK object's.
            const int frame = goal.type == 1 ? bone(goal.crcs[0]) : -1;
            if (goal.type == 1 && frame < 0)
                return std::nullopt;
            objectFrame.reset();
            if (goal.type == 2) {
                if (ikObjectBone && *ikObjectBone)
                    objectFrame = (*ikObjectBone)(mirrored && goal.crcs[1] ? goal.crcs[1] : goal.crcs[0]);
                // No IK object (or not its bone): nothing to reach, or (the
                // only choice left) the goal in model space (inferred).
                if (!objectFrame && !modelFallback)
                    return std::nullopt;
                if (objectFrame && mirrored) {
                    Xform r;
                    r.q = {objectFrame->q.x, -objectFrame->q.y, -objectFrame->q.z, objectFrame->q.w};
                    r.t = {-objectFrame->t[0], objectFrame->t[1], objectFrame->t[2]};
                    objectFrame = r;
                }
            }
            Goal g{inFrame(world, frame, *goalNode).t, std::nullopt};
            // The goal's rotation for the end bone: through the second rotation
            // node when there is one (the hands' *_handcontrol0rot, which the
            // hand follows as it is), else the rotation node with the rest-pose
            // offset between it and the end bone (the feet).
            if (goalRotation && goalRotation2) {
                g.q = inFrame(world, frame, *goalRotation).then(*goalRotation2).q;
            } else if (goalRotation) {
                // The rest rotation of the node: the skeleton's, else the adult
                // skeleton's foot control (auskel *_foot_ikctr; the child and
                // toddler feet rest the same way).
                const int r = bone(goal.crcs[2]);
                const Quat rest = r >= 0 ? restWorld[size_t(r)].q : Quat{0, 0.9703f, -0.2419f, 0};
                g.q = inFrame(world, frame, *goalRotation).q * (conjugate(rest) * restWorld[size_t(b2)].q);
            }
            return g;
        };
        // The chain's targets, each weighed by its contact channel (the
        // target's last name: "r_handcontrol0_a" for the hand's own goal in
        // model space, "_b"/"_c" for the IK object's bones, a "_a" of a
        // *handcontrol1 for the thigh's): a hand goes from its goal to the
        // chair's seat grab and back as the get-out keys them. With only the
        // first target, the hand went to its own goal's channel while the
        // animation meant the chair (held at a stale key: the arm hung long
        // and straight, or stretched back to the table). A target without a
        // contact channel weighs 1; with none reachable or none in contact,
        // the first one.
        Vec3 T{0, 0, 0};
        std::optional<Quat> endRotation;
        float total = 0;
        for (const auto& goal : chain.targets) {
            float w = 1;
            if (goal.crcs[5] && chain.targets.size() > 1) {
                w = 0;
                for (const auto& c : target.channels)
                    if (c.nameHash == goal.crcs[5] && c.attribute() == Animation::ContactIk && !c.components.empty())
                        w = std::clamp(c.components[0].sample(ticks, c.duration()), 0.0f, 1.0f);
            }
            if (w <= 0)
                continue;
            const auto g = targetGoal(goal, false);
            if (!g)
                continue;
            total += w;
            const float k = w / total;
            for (int c = 0; c < 3; ++c)
                T[size_t(c)] += (g->t[size_t(c)] - T[size_t(c)]) * k;
            if (g->q)
                endRotation = endRotation ? nlerp(*endRotation, *g->q, k) : *g->q;
        }
        float contact = std::min(total, 1.0f);
        if (total <= 0) {
            const auto g = targetGoal(chain.targets[0], true);
            if (!g)
                continue;
            T = g->t, endRotation = g->q;
            contact = chain.targets.size() > 1 ? 0.0f : 1.0f;
        }
        if (reach)
            reach->push_back({b2, T, weight * contact, chain.crcs[0]});
        objectFrame.reset();
        // The two-joint solution of cIKStrategy7DOF (Mac BV 0xd37848 for the
        // translation, 0xd36ecc for the end's rotation; docs/anim-mac.md §4).
        // The bones lie along their +Y. a: the upper length (the Y of the
        // local translations of the chain's bones of type < 5 after the
        // first: bicep + forearm, the calf), b: the lower (types >= 5: wrist
        // + hand, the foot). G the goal and P the pole from the begin bone.
        float a = 0, b = 0;
        for (size_t i = 1; i < bones.size(); ++i)
            (types[i] < 5 ? a : b) += local[size_t(bones[i])].t[1];
        const Vec3 A = world[size_t(b0)].t;
        const Vec3 G = sub(T, A);
        // (No pole node: the joint as it is; inferred, the game needs the
        // pole's outlet.)
        const Vec3 P = sub(pole ? inFrame(world, poleFrame, *pole, false).t : world[size_t(b1)].t, A);
        const float d = length(G);
        Vec3 n = cross(G, P);
        if (a <= 0 || b <= 0 || d <= 0 || length(n) <= 0)
            continue; // no solution this frame
        n = scale(n, 1 / length(n));
        Vec3 m = cross(n, G);
        m = scale(m, 1 / length(m)); // in the goal-pole plane, towards the pole
        // The upper bone: at A from the goal towards the pole (cos A by the
        // law of cosines), straight across (90 degrees) when cos A <= 0.
        Vec3 v = m;
        const float cosA = (a * a + d * d - b * b) / (2 * a * d);
        if (cosA > 0) {
            v = add(G, scale(m, d * std::tan(std::acos(std::min(cosA, 1.0f)))));
            v = scale(v, 1 / length(v));
        }
        // Its +Y on v, rolled about it so that its X (the joint's hinge) is
        // across the goal-pole plane: the joint bends by +X towards the goal.
        const Vec3 X = scale(n, -1), Z = cross(X, v);
        std::vector<Quat> before(bones.size());
        for (size_t i = 0; i < bones.size(); ++i)
            before[i] = local[size_t(bones[i])].q;
        local[size_t(b0)].q = normalized(conjugate(parentWorld(s, world, b0).q) * quatFromAxes(X, v, Z));
        // The upper twist bone (type 3, the bicep) takes 0.65 of the upper
        // bone's twist (FLOAT_0274b6cc), which keeps the rest.
        constexpr float kUpperTwist = 0.65f, kLowerTwist = 0.75f;
        for (size_t i = 0; i < bones.size(); ++i)
            if (types[i] == 3) {
                const float t = twistY(local[size_t(b0)].q);
                local[size_t(b0)].q = normalized(local[size_t(b0)].q * rotY(-kUpperTwist * t));
                local[size_t(bones[i])].q = rotY(kUpperTwist * t);
            }
        // The joint: a hinge about X only, pi - the angle between the two
        // segments.
        local[size_t(b1)].q = rotX(float(M_PI) - std::acos(std::clamp((a * a + b * b - d * d) / (2 * a * b), -1.0f, 1.0f)));
        if (endRotation) {
            // The end's rotation in the frame of the joint (the parent of the
            // first lower bone): the foot takes it all (type 6); in the arm
            // the wrist (type 7) takes 0.75 of its twist and the hand (8) the
            // rest.
            world = s.world(local);
            const Quat E = normalized(conjugate(world[size_t(b1)].q) * *endRotation);
            int wrist = -1;
            for (size_t i = 0; i < bones.size(); ++i)
                if (types[i] == 7)
                    wrist = bones[i];
            if (wrist >= 0) {
                const float t = twistY(E);
                local[size_t(wrist)].q = rotY(kLowerTwist * t);
                local[size_t(b2)].q = normalized(rotY(-kLowerTwist * t) * E);
            } else {
                local[size_t(b2)].q = normalized(conjugate(parentWorld(s, world, b2).q) * *endRotation);
            }
        }
        if (weight < 1)
            for (size_t i = 0; i < bones.size(); ++i)
                local[size_t(bones[i])].q = nlerp(before[i], local[size_t(bones[i])].q, weight);
    }
}

namespace {
// The value of "key=value" on its own line of an event's text.
std::string eventField(const std::string& text, std::string_view key) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos)
            end = text.size();
        const std::string_view line(text.data() + at, end - at);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=')
            return std::string(line.substr(key.size() + 1));
        at = end + 1;
    }
    return {};
}
// "name:__inner__" -> {name, inner}.
std::pair<std::string, std::string> splitSnapName(const std::string& v) {
    const size_t colon = v.find(':');
    if (colon == std::string::npos)
        return {v, {}};
    std::string inner = v.substr(colon + 1);
    while (!inner.empty() && inner.front() == '_')
        inner.erase(inner.begin());
    while (!inner.empty() && inner.back() == '_')
        inner.pop_back();
    return {v.substr(0, colon), inner};
}
} // namespace

std::vector<AnimSnap> animSnaps(const Animation& a) {
    std::vector<AnimSnap> out;
    for (const auto& e : a.events) {
        if (e.type != 1005)
            continue;
        const std::string root = eventField(e.data, "snapObjectRoot"), slot = eventField(e.data, "snapTargetSlot");
        if (root.empty() || slot.empty())
            continue;
        AnimSnap s;
        s.ticks = e.ticks;
        std::tie(s.object, s.model) = splitSnapName(root);
        if (slot != "Oblivion")
            std::tie(s.skeleton, s.bone) = splitSnapName(slot);
        s.pose = eventField(e.data, "snapObjectPose");
        while (!s.pose.empty() && std::isspace(static_cast<unsigned char>(s.pose.back())))
            s.pose.pop_back();
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const AnimSnap& x, const AnimSnap& y) { return x.ticks < y.ticks; });
    return out;
}

std::map<std::string, AnimSnap> snapStateAt(const std::vector<AnimSnap>& snaps, float ticks) {
    std::map<std::string, AnimSnap> out;
    for (const auto& s : snaps)
        if (float(s.ticks) <= ticks)
            out[s.object] = s;
    return out;
}

bool Animation::Target::hasBaseChannels() const {
    for (const auto& c : channels)
        if (!c.isDelta() && (c.attribute() == Rotation || c.attribute() == Translation))
            return true;
    return false;
}

bool Animation::Target::hasDeltaChannels() const {
    for (const auto& c : channels)
        if (c.isDelta() && (c.attribute() == Rotation || c.attribute() == Translation))
            return true;
    return false;
}

bool Animation::additive() const {
    const Target* t = findTarget(skeletonTag);
    if (!t && !targets.empty())
        t = &targets[0];
    return t && t->hasDeltaChannels() && !t->hasBaseChannels();
}

const Animation::Target* Animation::findTarget(std::string_view tag) const {
    for (const auto& t : targets) {
        if (t.tag.size() != tag.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < tag.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(t.tag[i])) == std::tolower(static_cast<unsigned char>(tag[i]));
        if (same)
            return &t;
    }
    return nullptr;
}

} // namespace os2
