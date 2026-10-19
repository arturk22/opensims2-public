#include <cstdlib>
#include <cstdio>
// Layout after OpenTS2's AnimResourceConstBlock.cs (MPL-2.0).
#include "anim.hpp"

#include "binwriter.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <optional>
#include <cctype>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace os2 {

namespace {

// A string section is followed by (its length % 4) padding bytes, which
// ReadFrom skips without looking at them (Mac BV 0xd491a8: the next record
// starts at the end of the strings plus length & 3; it does not align to 4)
// and WriteTo writes as 0, 1, 2 (BV 0xd4a20c).
void readPadding(BinReader& r, size_t start) { r.bytes((r.pos() - start) % 4); }
void writePadding(BinWriter& w, size_t length) {
    for (size_t i = 0; i < length % 4; ++i)
        w.u8(uint8_t(i));
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

uint16_t tangent(BinReader& r) { return r.u16(); }

// The value's scale of a fixed-point type (the decoder's constants, BV
// 0x1c65e00: 1/128, 1/1024, 1/2048, 1/4096, 1/8192).
float fixedScale(Animation::Value type) {
    switch (type) {
    case Animation::Value::Fixed8_7:
    case Animation::Value::Fixed9_7: return 128.0f;
    case Animation::Value::Fixed5_10: return 1024.0f;
    case Animation::Value::Fixed5_11: return 2048.0f;
    case Animation::Value::Fixed3_12: return 4096.0f;
    case Animation::Value::Fixed3_13: return 8192.0f;
    case Animation::Value::Float: break;
    }
    return 1.0f;
}

// Writes a key's time (unless baked) and value, as readValue reads them.
void writeTimeAndValue(BinWriter& w, const Animation::Component& c, const Animation::Key& k) {
    const bool baked = c.curve == Animation::Curve::Baked;
    if (c.value == Animation::Value::Float) {
        if (!baked)
            w.u16(k.time);
        w.f32(k.value);
        return;
    }
    const int32_t raw = int32_t(std::lround(double(k.value) * fixedScale(c.value)));
    if (sixteenBits(c.value)) { // 17 bits: the lowest goes in the time's top bit
        w.u16(uint16_t(k.time | ((raw & 1) << 15)));
        w.i16(int16_t(raw >> 1));
    } else {
        if (!baked)
            w.u16(k.time);
        w.i16(int16_t(raw));
    }
}

void writeCrc(BinWriter& w, uint32_t v) {
    w.u32(v);
    w.u8(0);
}

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

float animTangentToFloat(uint16_t raw) {
    const float v = std::ldexp(float(raw & 0x3FF), int((raw >> 10) & 0x1F) - 31);
    return (raw & 0x8000) ? -v : v;
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
    if (b.time <= a.time)
        return a.value;
    const float dt = float(b.time - a.time);
    const float u = (ticks - a.time) / dt;
    if (curve == Curve::Baked)
        return a.value + (b.value - a.value) * u;
    // Cubic Hermite: the tangents are slopes per tick, times the span.
    const float m0 = a.tangentOut * dt, m1 = b.tangentIn * dt;
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * a.value + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b.value + (u3 - u2) * m1;
}

std::array<float, 3> Animation::Channel::euler(float ticks) const {
    std::array<float, 3> v{0, 0, 0};
    for (size_t k = 0; k < 3 && k < components.size(); ++k)
        v[k] = components[k].sample(ticks, duration());
    return v;
}

Quat Animation::Channel::rotation(float ticks) const {
    constexpr float kDegrees = 0.017453292f; // the double 0x3f91df46a0000000 the strategy multiplies by
    auto at = [&](float t) {
        const auto v = euler(t);
        return quatFromEulerXYZ(v[0] * kDegrees, v[1] * kDegrees, v[2] * kDegrees);
    };
    int frame = int(float(ticks * 30.0f) / 1000.0f);
    if (ticks < 0)
        --frame;
    const float t0 = float(int(float(frame) * kFrameTicks + 0.16666667f));
    const float t1 = float(int(float(frame + 1) * kFrameTicks + 0.16666667f));
    const float u = t1 > t0 ? std::clamp((float(int(ticks)) - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
    return slerp(at(t0), at(t1), u);
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
    readPadding(r, start);

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
    readPadding(r, start);

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
    readPadding(r, start);

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
                        key.time = uint16_t(int(float(k) * kFrameTicks + 0.16666667f));
                        break;
                    case Curve::Continuous:
                        key.time = r.u16();
                        key.value = readValue(r, comp.value, &key.time);
                        key.rawIn = key.rawOut = tangent(r);
                        break;
                    case Curve::Discontinuous:
                        key.time = r.u16();
                        key.value = readValue(r, comp.value, &key.time);
                        key.rawIn = tangent(r);
                        key.rawOut = tangent(r);
                        break;
                    }
                    key.tangentIn = animTangentToFloat(key.rawIn);
                    key.tangentOut = animTangentToFloat(key.rawOut);
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

std::vector<uint8_t> Animation::write() const {
    BinWriter w;
    constexpr uint32_t m = kRuntimeMarker;
    w.u32(version);
    w.str7("cSGResource");
    w.u32(0);
    w.u32(resource.version);
    w.str7(resource.fileName);
    w.u32(serializedSize);
    w.u16(duration);
    w.u16(uint16_t(targets.size()));
    w.u16(uint16_t(events.size()));
    w.u8(uint8_t(dataString.size()));
    w.u8(dataVersion);
    w.u8(flags);
    w.u8(priority);
    w.u8(locomotionType);
    w.u8(uint8_t(skeletonTag.size()));
    for (int i = 0; i < 4; ++i)
        w.u32(m);
    for (float f : {headingOffset, locomotionDistance, cyclePortionCompleted, strideLength, strideVelocity, velocity,
                    turnRotation, preStretchFactor, rootStretchDisplacement})
        w.f32(f);
    auto cstr = [&](const std::string& t) {
        w.raw({reinterpret_cast<const uint8_t*>(t.data()), t.size()});
        w.u8(0);
        return t.size() + 1;
    };
    // One statement each: the operands of + are unsequenced, and MSVC wrote
    // the data string first.
    const size_t tagLength = cstr(skeletonTag);
    writePadding(w, tagLength + cstr(dataString));
    for (const auto& t : targets) {
        w.u32(m), w.u32(m);
        w.u16(t.animType);
        w.u16(uint16_t(t.channels.size()));
        w.u8(uint8_t(t.ikChains.size()));
        w.u8(t.channelType);
        w.u16(t.tagLength);
        w.u32(m), w.u32(m), w.u32(m);
    }
    size_t len = 0;
    for (const auto& t : targets)
        len += cstr(t.tag);
    writePadding(w, len);
    for (const auto& t : targets)
        for (const auto& c : t.channels) {
            w.u32(m), w.u32(m);
            w.u32(c.nameHash);
            w.u32(m);
            w.u32(c.flags);
            w.u32(m);
        }
    len = 0;
    for (const auto& t : targets)
        for (const auto& c : t.channels)
            len += cstr(c.name);
    writePadding(w, len);
    for (const auto& t : targets)
        for (const auto& c : t.channels)
            for (const auto& comp : c.components) {
                w.u16(uint16_t(comp.keys.size()));
                w.u8(comp.format);
                w.u8(comp.unknown);
                w.u32(comp.unknown2);
            }
    for (const auto& t : targets)
        for (const auto& c : t.channels)
            for (const auto& comp : c.components)
                for (const auto& k : comp.keys) {
                    writeTimeAndValue(w, comp, k);
                    if (comp.curve == Curve::Continuous)
                        w.u16(k.rawIn);
                    else if (comp.curve == Curve::Discontinuous)
                        w.u16(k.rawIn), w.u16(k.rawOut);
                }
    for (const auto& t : targets)
        for (const auto& ch : t.ikChains) {
            w.u32(m), w.u32(m);
            w.u16(ch.targetIndex);
            w.u16(uint16_t(ch.targets.size()));
            writeCrc(w, ch.nameCrc);
            writeCrc(w, ch.mirrorCrc);
            w.u32(ch.strategy);
            for (uint32_t crc : ch.crcs)
                writeCrc(w, crc);
            w.u32(m);
        }
    for (const auto& t : targets)
        for (const auto& ch : t.ikChains)
            for (const auto& g : ch.targets) {
                w.u32(m), w.u32(m);
                w.u16(g.targetIndex);
                w.u16(g.chainIndex);
                w.u8(g.type);
                for (uint32_t crc : g.crcs)
                    writeCrc(w, crc);
            }
    for (const auto& e : events) {
        w.u32(m);
        w.u16(e.ticks);
        w.i16(e.type);
        w.u16(uint16_t(e.data.size()));
        w.u32(m);
    }
    for (const auto& e : events)
        cstr(e.data);
    return std::move(w.bytes());
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
    std::vector<Xform> pose = skeleton.restPose();
    for (const auto& c : target.channels) {
        const int bone = skeleton.find(c.name);
        if (bone < 0 || c.components.size() < 3 || c.isDelta())
            continue;
        Xform& x = pose[size_t(bone)];
        // The key turned by the joint's orientation (the clavicles' twist):
        // the rest pose is orient * the bind key (inferred from the data).
        if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation)
            x.q = skeleton.bones[size_t(bone)].orient * c.rotation(ticks);
        else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ) {
            const auto v = c.euler(ticks);
            x.t = {v[0], v[1], v[2]};
        }
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
    DeltaPose d;
    d.delta.assign(skeleton.bones.size(), Xform{});
    d.has.assign(skeleton.bones.size(), 0);
    for (const auto& c : target.channels) {
        if (!c.isDelta() || c.components.size() < 3)
            continue;
        const int bone = skeleton.find(c.name);
        if (bone < 0)
            continue;
        const auto v = c.euler(ticks);
        Xform& x = d.delta[size_t(bone)];
        if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation) {
            x.q = c.rotation(ticks);
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
// along +Y). Used for the wrist's share, inferred: the binary takes the
// middle angle of the order 0xff010200 there, which by the decomposition as
// read is the angle about Z (docs/anim-mac.md §14.1).
float twistY(const Quat& q) { return 2 * std::atan2(q.y, q.w); }
// The Y angle of q's Euler XYZ decomposition (q = qz * qy * qx, R = Rz Ry Rx:
// quatFromEulerXYZ's inverse), the middle angle of ConvertToEulerAngles with
// the order 0x01020100 (Mac BV 0x1003ee0 -> FUN_00edd840: atan2(-M[2][0],
// sqrt(M[0][0]^2 + M[1][0]^2)), M the rotation by rows).
float eulerY(const Quat& q) {
    const float r20 = 2 * (q.x * q.z - q.w * q.y);
    const float r00 = 1 - 2 * (q.y * q.y + q.z * q.z), r10 = 2 * (q.x * q.y + q.w * q.z);
    return std::atan2(-r20, std::sqrt(r00 * r00 + r10 * r10));
}

} // namespace

std::optional<Quat> lookAtIkRotation(const Xform& beginWorld, const Quat& parentWorld, float endOffset,
                                     const Vec3& target, const Vec3& pole) {
    // In the frame of the begin bone's place and its parent's rotation
    // (here in the given space): G the target, U the pole, from the bone.
    const Vec3 G = sub(target, beginWorld.t), U = sub(pole, beginWorld.t);
    const float d2 = dot(G, G), L = endOffset;
    if (!(d2 - L * L > 0))
        return std::nullopt;
    Vec3 c = cross(U, G);
    const float lc = length(c);
    if (!(lc > 0))
        return std::nullopt;
    c = scale(c, 1 / lc);
    Vec3 b = cross(G, c);
    b = scale(b, 1 / length(b)); // across G, on the pole's side
    const float d = std::sqrt(d2);
    // A, the angle at the bone between the target and the end bone, so
    // that the end bone sees the target straight along the bone's +Z: cos A
    // = ((d^2 + L^2) - (d^2 - L^2)) / (2 d L) = L / d (the game's sum);
    // pi / 2 when that is not above 0.
    const float cosA = L > 0 ? (d2 + L * L - (d2 - L * L)) / (d * 2 * L) : 0.0f;
    const float A = cosA > 0 ? std::acos(std::min(cosA, 1.0f)) : float(M_PI / 2);
    // The axes (c, b, G / d) turned by pi / 2 - A about the first (the
    // columns of that matrix times Rx(pi / 2 - A), FUN_00edd6e0).
    const float th = float(M_PI / 2) - A, ct = std::cos(th), st = std::sin(th);
    const Vec3 g = scale(G, 1 / d);
    const Vec3 Y = add(scale(b, ct), scale(g, st)), Z = sub(scale(g, ct), scale(b, st));
    return normalized(conjugate(parentWorld) * quatFromAxes(c, Y, Z));
}

void applyIk(const Animation::Target& target, const Skeleton& s, float ticks, std::vector<Xform>& local,
             const std::vector<Xform>* goals, const Xform& modelFrame, const IkObjectBone* ikObjectBone,
             bool mirrored, std::vector<IkReach>* reach, IkMemory* memory) {
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
        std::optional<Xform> x;
        for (const auto& c : target.channels) {
            if (c.nameHash != crc || c.components.size() < 3)
                continue;
            if (c.attribute() == Animation::Rotation && c.type() == Animation::EulerRotation) {
                if (!x)
                    x = Xform{};
                x->q = c.rotation(ticks);
            } else if (c.attribute() == Animation::Translation && c.type() == Animation::TransformXYZ) {
                if (!x)
                    x = Xform{};
                const auto v = c.euler(ticks);
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

    // The strategies: two joints (seven degrees of freedom) for arms and
    // legs, look at for the head (head -> head_grip, docs/anim-mac.md
    // §12.2; 338 animations of the base game, the sink's cleaning, the
    // chessboard's, the shelves'). The dog's look at is not in the base game.
    constexpr uint32_t k7Dof = 0x48EE7A41, kLookAt = 0x09F9E87E;
    for (size_t chainIndex = 0; chainIndex < target.ikChains.size(); ++chainIndex) {
        const auto& chain = target.ikChains[chainIndex];
        if ((chain.strategy != k7Dof && chain.strategy != kLookAt) || chain.targets.empty())
            continue;
        const int b0 = bone(chain.crcs[0]), b2 = bone(chain.crcs[2]);
        if (b0 < 0 || b2 < 0)
            continue;
        std::vector<int> bones; // the chain's bones, begin to end
        for (int b = b2; b >= 0; b = s.bones[size_t(b)].parent) {
            bones.push_back(b);
            if (b == b0)
                break;
        }
        if (bones.back() != b0)
            continue;
        std::reverse(bones.begin(), bones.end());
        // The look-at strategy solves two-bone chains only (its
        // ComputeIKTranslationSolution wants the chain's 0x4c == 2).
        if (chain.strategy == kLookAt && bones.size() != 2)
            continue;
        // Each bone's IK channel type (cIKStrategy7DOF::SetUberChannelIKTypes,
        // Mac BV 0xd36d38): three bones (thigh, calf, foot) are 1, 4, 6;
        // otherwise (upperarm, bicep, forearm, wrist, hand) 2, 3, 4, 7, 8
        // and 0 past the fifth.
        std::vector<int> types(bones.size(), 0);
        static constexpr int kThree[] = {1, 4, 6}, kFive[] = {2, 3, 4, 7, 8};
        for (size_t i = 0; i < bones.size(); ++i)
            types[i] = bones.size() == 3 ? kThree[i] : i < 5 ? kFive[i] : 0;
        int joint = -1; // the elbow or knee (type 4)
        for (size_t i = 0; i < bones.size(); ++i)
            if (types[i] == 4)
                joint = int(i);
        if (joint < 0 && chain.strategy == k7Dof)
            continue;
        const int b1 = joint >= 0 ? bones[size_t(joint)] : b2;
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
        if (total <= 0 && memory) {
            // No weight on the goal's outlet: cActiveAnimIK does not solve
            // (ComputeIKTranslationSolution, BV 0xd1e698) and the IK channels
            // keep their last solution, mixed with the animation by the
            // chain's IK weight (docs/anim-mac.md §14.2).
            if (const auto it = memory->find(chainIndex); it != memory->end() && it->second.size() == bones.size()) {
                for (size_t i = 0; i < bones.size(); ++i)
                    local[size_t(bones[i])].q = weight < 1 ? nlerp(local[size_t(bones[i])].q, it->second[i], weight) : it->second[i];
                continue;
            }
        }
        if (total <= 0) {
            // None solved yet (or no memory): the first target (inferred: the
            // IK channels' value before a first solution was not read).
            const auto g = targetGoal(chain.targets[0], true);
            if (!g)
                continue;
            T = g->t, endRotation = g->q;
            contact = chain.targets.size() > 1 ? 0.0f : 1.0f;
        }
        if (chain.strategy == kLookAt) {
            // The head turns to look at the goal (the head_ikctr), upright by
            // the pole (head_ikpole): its IK channel (type 1) takes the
            // solution, by the chain's weight.
            objectFrame.reset();
            if (!pole)
                continue; // no pole: no up for the head (the game needs the pole's outlet)
            const Xform poleWorld = inFrame(world, poleFrame, *pole, false);
            const Quat before = local[size_t(b0)].q;
            if (const auto q = lookAtIkRotation(world[size_t(b0)], parentWorld(s, world, b0).q,
                                                local[size_t(b2)].t[1], T, poleWorld.t))
                local[size_t(b0)].q = weight < 1 ? nlerp(before, *q, weight) : *q;
            continue;
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
        // The upper twist bone (type 3, the bicep) takes 0.65 (FLOAT_0274b6cc)
        // of the Y angle of the upper bone's Euler XYZ angles (order
        // 0x01020100), which keeps the rest (Ry(-0.65 y) taken off: Mac BV
        // 0xd3812c-0xd381c0; docs/anim-mac.md §14.1).
        constexpr float kUpperTwist = 0.65f, kLowerTwist = 0.75f;
        for (size_t i = 0; i < bones.size(); ++i)
            if (types[i] == 3) {
                const float t = eulerY(local[size_t(b0)].q);
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
        if (memory && total > 0) {
            std::vector<Quat>& kept = (*memory)[chainIndex];
            kept.resize(bones.size());
            for (size_t i = 0; i < bones.size(); ++i)
                kept[i] = local[size_t(bones[i])].q;
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
