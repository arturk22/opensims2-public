// Skeletons, ANIM decoding and pose sampling on hand-built data.
#include "formats/anim.hpp"
#include "formats/skeleton.hpp"
#include "formats/xform.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
    void f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        u32(v);
    }
    void str7(const std::string& s) {
        u8(uint8_t(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
    void cstr(const std::string& s) {
        b.insert(b.end(), s.begin(), s.end());
        u8(0);
    }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    // OpenTS2's padding: (length % 4) bytes 0, 1, 2 after a string section.
    void pad(size_t start) {
        const size_t len = b.size() - start;
        for (size_t i = 0; i < len % 4; ++i)
            u8(uint8_t(i));
    }
};

// One target "auskel" with a rotation channel on "spine0" (continuous 9.7 X,
// baked 8.7 Y, baked float Z), a translation channel on "root_trans" and one event.
static std::vector<uint8_t> buildAnim() {
    Writer w;
    w.u32(6); // version
    w.str7("cSGResource");
    w.u32(0);
    w.u32(2);
    w.str7("test_anim");
    w.u32(0);      // serialized size
    w.u16(1000);   // duration
    w.u16(1);      // targets
    w.u16(1);      // events
    w.u8(0);       // data string length
    w.u8(1);       // data version
    w.u8(1);       // flags
    w.u8(2);       // priority
    w.u8(0);       // locomotion
    w.u8(6);       // skeleton tag length
    w.zeros(16);
    for (int i = 0; i < 9; ++i)
        w.f32(float(i));
    size_t start = w.b.size();
    w.cstr("auskel");
    w.cstr("");
    w.pad(start);
    // target
    w.zeros(8);
    w.u16(1); // anim type
    w.u16(2); // channels
    w.u8(0);  // IK chains
    w.u8(0);
    w.u16(6);
    w.zeros(12);
    start = w.b.size();
    w.cstr("auskel");
    w.pad(start);
    // channels: duration 1000, attribute, type, no IK chain (0xF), 3 components
    auto flags = [](uint32_t attr, uint32_t type) { return 1000u | attr << 17 | type << 22 | 0xFu << 25 | 3u << 29; };
    for (uint32_t f : {flags(0, 3), flags(1, 4)}) {
        w.zeros(8);
        w.u32(0x1234);
        w.zeros(4);
        w.u32(f);
        w.zeros(4);
    }
    start = w.b.size();
    w.cstr("spine0");
    w.cstr("root_trans");
    w.pad(start);
    // component headers: key count, format
    w.u16(2), w.u8(0x01), w.u8(0), w.u32(0); // continuous, 9.7
    w.u16(1), w.u8(0x00), w.u8(0), w.u32(0); // baked, 8.7
    w.u16(2), w.u8(0x10), w.u8(0), w.u32(0); // baked, float
    for (int k = 0; k < 3; ++k)
        w.u16(1), w.u8(0x10), w.u8(0), w.u32(0); // translation: baked floats
    // spine0 X: t=0 value 90 (raw 11520 = 5760*2), t=1000 value -45.0078125 (raw -5761, odd: low bit in time's top bit)
    w.u16(0), w.u16(5760), w.u16(1024);       // tangent 1.0
    w.u16(1000 | 0x8000), w.u16(uint16_t(int16_t(-2881))), w.u16(0);
    w.u16(uint16_t(int16_t(-128)));           // Y: -1.0
    w.f32(10), w.f32(30);                     // Z: 10 -> 30
    w.f32(1), w.f32(2), w.f32(3);             // root_trans
    // event
    w.zeros(4);
    w.u16(500);
    w.u16(3);
    w.u16(5);
    w.zeros(4);
    w.cstr("sound");
    return w.b;
}

static void testFixed() {
    check(near(animFixedToFloat(Animation::Value::Fixed8_7, 128), 1.0f), "8.7: 128 = 1");
    check(near(animFixedToFloat(Animation::Value::Fixed5_10, -512), -0.5f), "5.10: -512 = -0.5");
    check(near(animFixedToFloat(Animation::Value::Fixed3_13, 8192), 1.0f), "3.13: 8192 = 1");
}

static void testDecode() {
    const auto bytes = buildAnim();
    Animation a;
    try {
        a = Animation::parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: parse: %s\n", e.what());
        ++failures;
        return;
    }
    check(a.resource.fileName == "test_anim", "resource name");
    check(a.duration == 1000 && a.priority == 2 && a.skeletonTag == "auskel", "header");
    check(near(a.velocity, 5.0f), "header floats");
    check(a.targets.size() == 1 && a.findTarget("AUSKEL") == &a.targets[0], "target found case-insensitively");
    const auto& t = a.targets[0];
    check(t.channels.size() == 2 && t.channels[0].name == "spine0" && t.channels[1].name == "root_trans",
          "channel names");
    const auto& rot = t.channels[0];
    check(rot.attribute() == Animation::Rotation && rot.type() == Animation::EulerRotation && rot.ikChain() == -1 &&
              rot.duration() == 1000 && rot.components.size() == 3,
          "channel flags");
    const auto& x = rot.components[0];
    check(x.curve == Animation::Curve::Continuous && x.value == Animation::Value::Fixed9_7, "continuous 9.7");
    check(x.keys.size() == 2 && x.keys[0].time == 0 && near(x.keys[0].value, 90.0f) && near(x.keys[0].tangentIn, 1.0f),
          "first key");
    check(x.keys[1].time == 1000 && near(x.keys[1].value, -5761.0f / 128.0f), "16-bit value borrows the time's top bit");
    check(near(x.sample(500, 1000), (90.0f - 5761.0f / 128.0f) / 2), "linear sample");
    check(near(x.sample(-5, 1000), 90.0f) && near(x.sample(2000, 1000), -5761.0f / 128.0f), "clamped sample");
    check(rot.components[1].keys.size() == 1 && near(rot.components[1].keys[0].value, -1.0f), "baked 8.7");
    const auto& z = rot.components[2];
    check(z.keys.size() == 2 && z.keys[1].time == 1000 && near(z.sample(250, 1000), 15.0f), "baked keys span the duration");
    check(a.events.size() == 1 && a.events[0].ticks == 500 && a.events[0].data == "sound", "event");

    // Pose: two-bone skeleton root_trans -> spine0.
    Skeleton s;
    s.bones.push_back({"root_trans", -1, Xform{{}, {0, 0, 1}}, 1, 0});
    s.bones.push_back({"spine0", 0, Xform{{}, {0, 0, 0.5f}}, 2, 1});
    const auto pose = samplePose(t, s, 0);
    check(near(pose[0].t[0], 1) && near(pose[0].t[1], 2) && near(pose[0].t[2], 3), "translation channel");
    const Quat expect = quatFromEulerXYZ(90 * 3.14159265f / 180, -1 * 3.14159265f / 180, 10 * 3.14159265f / 180);
    check(near(pose[1].q.x, expect.x) && near(pose[1].q.w, expect.w), "rotation channel in degrees");
    check(near(pose[1].t[2], 0.5f), "untouched translation keeps the rest");
    check(near(animationTime(a, 1.25f, true), 250.0f) && near(animationTime(a, 1.25f, false), 1000.0f),
          "animation time");
}

static void testXform() {
    // 90 degrees about Z, then Y.
    const Quat qz = quatFromEulerXYZ(0, 0, 3.14159265f / 2);
    const Xform a{qz, {1, 2, 3}};
    const Vec3 p = a.apply({1, 0, 0});
    check(near(p[0], 1) && near(p[1], 3) && near(p[2], 3), "rotate then translate");
    const Xform id = a.then(a.inverse());
    const Vec3 q = id.apply({0.3f, -2, 5});
    check(near(q[0], 0.3f) && near(q[1], -2) && near(q[2], 5), "inverse");
    const Quat xyz = quatFromEulerXYZ(0.3f, 0.2f, 0.1f);
    const Quat composed = quatFromEulerXYZ(0, 0, 0.1f) * quatFromEulerXYZ(0, 0.2f, 0) * quatFromEulerXYZ(0.3f, 0, 0);
    check(near(xyz.x, composed.x) && near(xyz.y, composed.y) && near(xyz.z, composed.z) && near(xyz.w, composed.w),
          "euler order: x first, then y, then z");

    Skeleton s;
    s.bones.push_back({"a", -1, Xform{qz, {1, 0, 0}}, 0, 0});
    s.bones.push_back({"b", 0, Xform{{}, {1, 0, 0}}, 1, 1});
    const auto w = s.world(s.restPose());
    check(near(w[1].t[0], 1) && near(w[1].t[1], 1), "world composes parents");
    check(s.find("B") == 1 && s.findJoint(0) == 0 && s.find("c") < 0, "bone lookup");
}

int main() {
    testFixed();
    testDecode();
    testXform();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("anim tests passed\n");
    return failures ? 1 : 0;
}
