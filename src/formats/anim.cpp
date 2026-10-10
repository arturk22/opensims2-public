// Layout after OpenTS2's AnimResourceConstBlock.cs (MPL-2.0).
#include "anim.hpp"

#include "resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

float tangent(BinReader& r) { return animFixedToFloat(Animation::Value::Fixed5_10, r.i16()); }

uint32_t readCrc(BinReader& r) {
    const uint32_t v = r.u32();
    r.u8(); // NUL after the CRC, so the game can treat it as a string
    return v;
}

} // namespace

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
    const float u = (ticks - a.time) / float(b.time - a.time);
    return a.value + (b.value - a.value) * u;
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
        Xform& x = pose[size_t(bone)];
        if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation)
            x.q = quatFromEulerXYZ(v[0] * kDegrees, v[1] * kDegrees, v[2] * kDegrees);
        else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ)
            x.t = {v[0], v[1], v[2]};
    }
    return pose;
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

// Shortest rotation taking direction u to direction v.
Quat rotationBetween(const Vec3& u, const Vec3& v) {
    const float lu = length(u), lv = length(v);
    if (lu == 0 || lv == 0)
        return {};
    const Vec3 c = cross(u, v);
    const float w = lu * lv + dot(u, v);
    if (w < 1e-6f * lu * lv) {
        // Opposite: half a turn about any axis perpendicular to u.
        Vec3 axis = cross(u, {1, 0, 0});
        if (length(axis) < 1e-6f)
            axis = cross(u, {0, 1, 0});
        axis = scale(axis, 1 / length(axis));
        return {axis[0], axis[1], axis[2], 0};
    }
    return normalized({c[0], c[1], c[2], w});
}

Xform parentWorld(const Skeleton& s, const std::vector<Xform>& world, int bone) {
    const int p = s.bones[size_t(bone)].parent;
    return p >= 0 ? world[size_t(p)] : Xform{};
}

} // namespace

void applyIk(const Animation::Target& target, const Skeleton& s, float ticks, std::vector<Xform>& local) {
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

    for (const auto& chain : target.ikChains) {
        if (chain.strategy != 0x48EE7A41 || chain.targets.empty())
            continue; // only the two-bone (seven degrees of freedom) chains
        const int b0 = bone(chain.crcs[0]), b2 = bone(chain.crcs[2]);
        if (b0 < 0 || b2 < 0)
            continue;
        const int b1 = s.bones[size_t(b2)].parent;
        if (b1 < 0 || s.bones[size_t(b1)].parent != b0)
            continue;
        const auto& goal = chain.targets[0];
        const int goalBone = bone(goal.crcs[4]), goalRotation = bone(goal.crcs[2]), pole = bone(chain.crcs[4]);
        if (goalBone < 0)
            continue;
        float weight = 1;
        if (chain.crcs[7]) {
            weight = 0;
            for (const auto& c : target.channels)
                if (c.nameHash == chain.crcs[7] && !c.components.empty())
                    weight = std::clamp(c.components[0].sample(ticks, c.duration()), 0.0f, 1.0f);
        }
        if (weight <= 0)
            continue;

        const auto world = s.world(local);
        const Vec3 A = world[size_t(b0)].t, B = world[size_t(b1)].t, C = world[size_t(b2)].t;
        const Vec3 T = world[size_t(goalBone)].t;
        const float a = length(sub(B, A)), b = length(sub(C, B));
        Vec3 toT = sub(T, A);
        float d = length(toT);
        if (a <= 0 || b <= 0 || d <= 1e-6f)
            continue;
        const Vec3 u = scale(toT, 1 / d);
        d = std::clamp(d, std::fabs(a - b) + 1e-4f, a + b - 1e-4f);
        // Knee in the plane of the hip, the goal and the pole (or the current knee).
        Vec3 toP = sub(pole >= 0 ? world[size_t(pole)].t : B, A);
        Vec3 v = sub(toP, scale(u, dot(toP, u)));
        if (length(v) < 1e-6f)
            continue;
        v = scale(v, 1 / length(v));
        const float cosA = std::clamp((a * a + d * d - b * b) / (2 * a * d), -1.0f, 1.0f);
        const Vec3 K = add(A, add(scale(u, a * cosA), scale(v, a * std::sqrt(1 - cosA * cosA))));

        const Xform p0 = parentWorld(s, world, b0);
        Xform w0 = world[size_t(b0)];
        w0.q = rotationBetween(sub(B, A), sub(K, A)) * w0.q;
        Xform l0 = local[size_t(b0)];
        l0.q = normalized(conjugate(p0.q) * w0.q);
        w0 = p0.then(l0);
        Xform w1 = w0.then(local[size_t(b1)]);
        const Vec3 C1 = w1.then(local[size_t(b2)]).t;
        w1.q = rotationBetween(sub(C1, w1.t), sub(T, w1.t)) * w1.q;
        Xform l1 = local[size_t(b1)];
        l1.q = normalized(conjugate(w0.q) * w1.q);
        w1 = w0.then(l1);
        Xform l2 = local[size_t(b2)];
        if (goalRotation >= 0) {
            const Quat offset = conjugate(restWorld[size_t(goalRotation)].q) * restWorld[size_t(b2)].q;
            l2.q = normalized(conjugate(w1.q) * (world[size_t(goalRotation)].q * offset));
        }

        local[size_t(b0)].q = nlerp(local[size_t(b0)].q, l0.q, weight);
        local[size_t(b1)].q = nlerp(local[size_t(b1)].q, l1.q, weight);
        local[size_t(b2)].q = nlerp(local[size_t(b2)].q, l2.q, weight);
    }
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
