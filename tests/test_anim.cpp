// Skeletons, ANIM decoding and pose sampling on hand-built data.
#include "formats/anim.hpp"
#include "formats/anim_player.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/property_set.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/skeleton.hpp"
#include "formats/skinning.hpp"
#include "formats/xform.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
    // channels: duration 1000, base (bit 15), attribute, type, no IK chain (0xF), 3 components
    auto flags = [](uint32_t attr, uint32_t type) {
        return 1000u | 0x8000u | attr << 17 | type << 22 | 0xFu << 25 | 3u << 29;
    };
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
    w.u16(0), w.u16(5760), w.u16(0x3C00);     // tangent 1.0 (half float)
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
    // Hermite at u = 0.5: the mean plus (m0 - m1) / 8, m0 = 1 * 1000 / kTangentTicks.
    check(near(x.sample(500, 1000), (90.0f - 5761.0f / 128.0f) / 2 + 1000.0f / Animation::kTangentTicks / 8),
          "Hermite sample");
    check(near(halfToFloat(0x3C00), 1) && near(halfToFloat(0xC000), -2) && halfToFloat(0x8000) == 0 &&
              near(halfToFloat(0x0001), 5.96e-8f, 1e-9f),
          "half floats");
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

static void testSimCatalog() {
    Writer w;
    w.u32(0xCBE750E0);
    w.u16(2);
    w.u32(3);
    w.u32(0xEB61E4F7), w.u32(3), w.b.insert(w.b.end(), {'a', 'g', 'e'}), w.u32(8);
    w.u32(0x0B8BEA18), w.u32(4), w.b.insert(w.b.end(), {'n', 'a', 'm', 'e'}), w.u32(3),
        w.b.insert(w.b.end(), {'a', 'm', 'x'});
    w.u32(0xCBA908E1), w.u32(1), w.u8('b'), w.u8(1);
    const auto props = readPropertySet(w.b);
    check(props.size() == 3 && props.at("age") == "8" && props.at("name") == "amx" && props.at("b") == "true",
          "binary property set");
    const std::string xml = "<cGZPropertySetString><AnyUint32 key=\"age\" type=\"0xeb61e4f7\">8</AnyUint32>"
                            "</cGZPropertySetString>";
    check(readPropertySet({reinterpret_cast<const uint8_t*>(xml.data()), xml.size()}).at("age") == "8",
          "XML property set through readPropertySet");

    Writer r;
    r.u32(0xDEADBEEF), r.u32(2), r.u32(2);
    r.u32(0xE519C933), r.u32(0x1C0532FA), r.u32(0xFF597350), r.u32(0x66002389);
    r.u32(0x49596978), r.u32(0x1C0532FA), r.u32(0xFF000001), r.u32(0x00000002);
    const auto refs = parseReferenceList(r.b);
    check(refs.size() == 2 && refs[0].type == 0xE519C933 && refs[0].instance == 0xFF597350 &&
              refs[0].instanceHi == 0x66002389 && refs[1].type == 0x49596978,
          "3IDR: type, group, instance, instanceHi");
    SimPart part;
    part.properties = {{"resourcekeyidx", "1"}, {"shapekeyidx", "5"}};
    part.references = refs;
    check(part.reference("resourcekeyidx") && part.reference("resourcekeyidx")->type == 0x49596978 &&
              !part.reference("shapekeyidx") && !part.reference("missing"),
          "part references by index");
}

static void testSkinning() {
    // One bone at the origin, rotated 90 degrees about Z in the pose; a vertex
    // half bound to it and half to a bone that stays put.
    Skeleton s;
    s.bones.push_back({"a", -1, Xform{}, 0, 0});
    s.bones.push_back({"b", -1, Xform{}, 1, 1});
    ObjectModel m;
    m.skeleton = s;
    ObjectModel::Part p;
    p.mesh.positions = {{1, 0, 0}};
    p.mesh.normals = {{1, 0, 0}};
    p.mesh.bones = {{0, 1, 0xFF, 0xFF}};
    p.mesh.weights = {{0.5f, 0.5f, 0, 0}};
    p.bindPoses = {Xform{}, Xform{}};
    m.parts.push_back(p);
    SkinnedModel skin(s);
    skin.add(m);
    auto pose = s.restPose();
    pose[0].q = quatFromEulerXYZ(0, 0, 3.14159265f / 2);
    ObjectModel out;
    skin.pose(s.world(pose), out);
    const auto& v = out.parts[0].mesh.positions[0];
    check(near(v[0], 0.5f) && near(v[1], 0.5f) && near(v[2], 0), "linear blend of two bones");
    const auto& n = out.parts[0].mesh.normals[0];
    check(near(n[0], std::sqrt(0.5f)) && near(n[1], std::sqrt(0.5f)), "normals renormalized");

    // Morphs: stream 1 (byte 1 of the map) moves the vertex by morph 2.
    skin.model.parts[0].mesh.morphNames = {"a", "b", "smile"};
    skin.model.parts[0].mesh.morphMap = {0x00000200};
    skin.model.parts[0].mesh.morphDeltas[0] = {{5, 5, 5}};
    skin.model.parts[0].mesh.morphDeltas[1] = {{0, 0, 2}};
    skin.pose(s.world(s.restPose()), out, {{"smile", 0.5f}});
    const auto& mv = out.parts[0].mesh.positions[0];
    check(near(mv[0], 1) && near(mv[1], 0) && near(mv[2], 1), "morph delta by weight, stream k = byte k");

    // A new model with as many parts but other vertices (a Sim changing into
    // a swimsuit keeps its posed output): the output is rebuilt, not the old
    // body's vertices left over (live QA round 16: a T-posed shell at the
    // diving board).
    SkinnedModel swim(s);
    ObjectModel m2 = m;
    m2.parts[0].mesh.positions = {{1, 0, 0}, {0, 1, 0}};
    m2.parts[0].mesh.normals = {{1, 0, 0}, {0, 1, 0}};
    m2.parts[0].mesh.bones = {{0, 0xFF, 0xFF, 0xFF}, {0, 0xFF, 0xFF, 0xFF}};
    m2.parts[0].mesh.weights = {{1, 0, 0, 0}, {1, 0, 0, 0}};
    m2.parts[0].material = "swimsuit";
    swim.add(m2);
    swim.pose(s.world(pose), out);
    check(out.parts.size() == 1 && out.parts[0].mesh.positions.size() == 2 && out.parts[0].material == "swimsuit",
          "posed output rebuilt for a new model of as many parts");
    if (out.parts.size() == 1 && out.parts[0].mesh.positions.size() == 2) {
        const auto& w = out.parts[0].mesh.positions[1];
        check(near(w[0], -1) && near(w[1], 0), "the new model's vertices posed");
    }
}

static void testIk() {
    // A straight leg along -Z (thigh at z=1, knee at 0.5, foot at 0), the goal
    // raised to z=0.3 and the pole in front (+Y): the knee must bend forward
    // and the foot reach the goal.
    Skeleton s;
    s.bones.push_back({"root", -1, Xform{}, -1, 0});
    // The bones lie along their +Y, as the game's skeletons (the solver
    // relies on it): the thigh turned to point down.
    s.bones.push_back({"thigh", 0, Xform{quatFromEulerXYZ(-float(M_PI) / 2, 0, 0), {0, 0, 1}}, 0, 1});
    s.bones.push_back({"calf", 1, Xform{{}, {0, 0.5f, 0}}, 1, 2});
    s.bones.push_back({"foot", 2, Xform{{}, {0, 0.5f, 0}}, 2, 3});
    s.bones.push_back({"goal", 0, Xform{{}, {0, 0, 0}}, -1, 4});
    s.bones.push_back({"pole", 0, Xform{{}, {0, 1, 0.5f}}, -1, 5});
    Animation::Target t;
    Animation::IkChain chain;
    chain.strategy = 0x48EE7A41;
    chain.crcs[0] = nameInstanceHiHash("thigh");
    chain.crcs[2] = nameInstanceHiHash("foot");
    chain.crcs[4] = nameInstanceHiHash("pole");
    Animation::IkTarget goal;
    goal.crcs[2] = goal.crcs[4] = nameInstanceHiHash("goal");
    chain.targets.push_back(goal);
    t.ikChains.push_back(chain);
    auto local = s.restPose();
    local[4].t = {0, 0, 0.3f};
    applyIk(t, s, 0, local);
    const auto w = s.world(local);
    check(near(w[3].t[0], 0, 1e-3f) && near(w[3].t[1], 0, 1e-3f) && near(w[3].t[2], 0.3f, 1e-3f), "IK: foot on the goal");
    check(w[2].t[1] > 0.1f, "IK: knee bends towards the pole");
    const float thigh = std::sqrt(w[2].t[1] * w[2].t[1] + (w[2].t[2] - 1) * (w[2].t[2] - 1));
    check(near(thigh, 0.5f, 1e-3f), "IK: bone lengths kept");
}

static void testArmIk() {
    // An arm along +X from the shoulder at (0, 0, 1.5): upperarm 0.3, a
    // twist bone halfway, forearm 0.3, wrist twist bone, hand. The goal is
    // keyed in the frame of "spine" (at z = 1), as a2o-newspaper-read-start
    // keys the hand controls; the skeleton hangs the control elsewhere.
    Skeleton s;
    s.bones.push_back({"root", -1, Xform{}, -1, 0});
    s.bones.push_back({"spine", 0, Xform{{}, {0, 0, 1}}, 0, 1});
    // (Along their +Y, the upperarm turned to point along +X.)
    s.bones.push_back({"upperarm", 1, Xform{quatFromEulerXYZ(0, 0, -float(M_PI) / 2), {0, 0, 0.5f}}, 1, 2});
    s.bones.push_back({"bicep", 2, Xform{{}, {0, 0.15f, 0}}, 2, 3});
    s.bones.push_back({"forearm", 3, Xform{{}, {0, 0.15f, 0}}, 3, 4});
    s.bones.push_back({"wrist", 4, Xform{{}, {0, 0.15f, 0}}, 4, 5});
    s.bones.push_back({"hand", 5, Xform{{}, {0, 0.15f, 0}}, 5, 6});
    s.bones.push_back({"handcontrol", 0, Xform{{}, {5, 5, 5}}, -1, 7}); // misplaced on purpose
    s.bones.push_back({"handcontrolrot", 7, Xform{}, -1, 8});
    s.bones.push_back({"pole", 0, Xform{{}, {0.3f, -1, 1.5f}}, -1, 9});
    Animation::Target t;
    Animation::IkChain chain;
    chain.strategy = 0x48EE7A41;
    chain.crcs[0] = nameInstanceHiHash("upperarm");
    chain.crcs[2] = nameInstanceHiHash("hand");
    chain.crcs[4] = nameInstanceHiHash("pole");
    Animation::IkTarget goal;
    goal.type = 1; // a bone of the skeleton's own
    goal.crcs[0] = nameInstanceHiHash("spine");
    goal.crcs[2] = goal.crcs[4] = nameInstanceHiHash("handcontrol");
    goal.crcs[3] = nameInstanceHiHash("handcontrolrot");
    chain.targets.push_back(goal);
    t.ikChains.push_back(chain);
    auto local = s.restPose();
    local[7].t = {0.3f, 0, 0.6f}; // world (0.3, 0, 1.6): 0.32 from the shoulder
    applyIk(t, s, 0, local);
    const auto w = s.world(local);
    check(near(w[6].t[0], 0.3f, 1e-3f) && near(w[6].t[1], 0, 1e-3f) && near(w[6].t[2], 1.6f, 1e-3f),
          "arm IK: hand on the goal in the frame bone's space");
    check(w[4].t[1] < -0.1f, "arm IK: elbow (forearm, past the twist bone) towards the pole");
    check(near(w[6].q.w, 1, 1e-3f), "arm IK: hand takes the control rotation node's");
    // cIKStrategy7DOF: the elbow is a hinge about the forearm's X only.
    check(near(local[4].q.y, 0, 1e-4f) && near(local[4].q.z, 0, 1e-4f) && local[4].q.x > 0.1f,
          "arm IK: the joint turns about its X only (a hinge)");
    {
        // The twist split: the bicep takes 0.65 of the upperarm's twist and
        // the wrist 0.75 of the hand's; the hand still ends on the control's
        // rotation.
        auto l = s.restPose();
        l[7].t = {0.3f, 0, 0.6f};
        l[8].q = quatFromEulerXYZ(0, 0, 0.6f); // the hand control turned
        applyIk(t, s, 0, l);
        const auto wl = s.world(l);
        const Quat goalQ = wl[1].q * l[7].q * l[8].q; // spine * control * rotation node
        const Quat diff = conjugate(goalQ) * wl[6].q;
        check(std::fabs(diff.w) > 0.9999f, "arm IK, twist split: the hand on the control's rotation");
        auto twist = [](const Quat& q) { return 2 * std::atan2(q.y, q.w); };
        const Quat upper = l[2].q * l[3].q, end = l[5].q * l[6].q;
        check(near(twist(l[3].q), 0.65f * twist(upper), 1e-3f) && near(l[3].q.x, 0, 1e-4f) && near(l[3].q.z, 0, 1e-4f) && near(l[5].q.x, 0, 1e-4f) && near(l[5].q.z, 0, 1e-4f),
              "arm IK, twist split: the bicep takes 0.65 of the upperarm's twist, it and the wrist about their Y only");
        check(std::fabs(twist(end)) > 0.1f && near(twist(l[5].q), 0.75f * twist(end), 1e-3f),
              "arm IK, twist split: the wrist takes 0.75 of the hand's twist");
    }

    // Type 2: the frame bone is the IK object's (a2b-babybottle-feeder's
    // right hand at the baby's head_grip), given in this skeleton's model
    // space; the skeleton's own bone of that name does not count.
    t.ikChains[0].targets[0].type = 2;
    t.ikChains[0].targets[0].crcs[0] = nameInstanceHiHash("spine");
    const IkObjectBone other = [](uint32_t crc) -> std::optional<Xform> {
        if (crc != nameInstanceHiHash("spine"))
            return std::nullopt;
        return Xform{{}, {0, 0.1f, 1.0f}}; // the other's "spine", 0.1 in front of ours
    };
    local = s.restPose();
    local[7].t = {0.3f, 0, 0.6f};
    applyIk(t, s, 0, local, nullptr, Xform{}, &other);
    const auto w2 = s.world(local);
    check(near(w2[6].t[0], 0.3f, 1e-3f) && near(w2[6].t[1], 0.1f, 1e-3f) && near(w2[6].t[2], 1.6f, 1e-3f),
          "arm IK: a type-2 goal in the IK object's bone");
    // Flipped: the twin bone's frame reflected (the pose is reflected after).
    const IkObjectBone twin = [](uint32_t crc) -> std::optional<Xform> {
        return crc == nameInstanceHiHash("spinetwin") ? std::optional<Xform>(Xform{{}, {-0.05f, 0, 1.0f}}) : std::nullopt;
    };
    t.ikChains[0].targets[0].crcs[1] = nameInstanceHiHash("spinetwin");
    local = s.restPose();
    local[7].t = {0.3f, 0, 0.6f};
    applyIk(t, s, 0, local, nullptr, Xform{}, &twin, true);
    const auto w3 = s.world(local);
    check(near(w3[6].t[0], 0.35f, 1e-3f) && near(w3[6].t[2], 1.6f, 1e-3f), "arm IK: flipped, the twin's frame reflected");

    // Several targets, each weighed by its contact channel (attribute 3, the
    // target's last name), as a2o-chairDining-getOut-SE's hands go from
    // their own goal ("r_handcontrol0_a") to the chair's seat grab ("_b",
    // the IK object's bone) and back. Only the first target was used: the
    // hand reached its own goal's stale key while the animation meant the
    // chair (an arm hanging long and straight, or stretched back).
    t.ikChains[0].targets[0].crcs[1] = 0;
    Animation::IkTarget own = t.ikChains[0].targets[0];
    own.type = 1;
    own.crcs[5] = nameInstanceHiHash("handcontrol_a");
    Animation::IkTarget chair = t.ikChains[0].targets[0];
    chair.type = 2;
    chair.crcs[5] = nameInstanceHiHash("handcontrol_b");
    t.ikChains[0].targets = {own, chair};
    auto contact = [](const char* name, float a, float b) {
        Animation::Channel c;
        c.name = name;
        c.nameHash = nameInstanceHiHash(name);
        c.flags = 1000u | 0x8000u | uint32_t(Animation::ContactIk) << 17 | 0u << 22 | 0xFu << 25 | 1u << 29;
        c.components.resize(1);
        c.components[0].keys.push_back({0, a, 0, 0});
        c.components[0].keys.push_back({1000, b, 0, 0});
        return c;
    };
    t.channels = {contact("handcontrol_a", 1, 0), contact("handcontrol_b", 0, 1)};
    auto handAt = [&](float ticks) {
        auto l = s.restPose();
        l[7].t = {0.3f, 0, 0.6f};
        applyIk(t, s, ticks, l, nullptr, Xform{}, &other);
        return s.world(l)[6].t;
    };
    const Vec3 h0 = handAt(0), h1 = handAt(1000), hHalf = handAt(500);
    check(near(h0[1], 0, 1e-3f) && near(h1[1], 0.1f, 1e-3f), "arm IK: the target in contact (own goal, then the IK object's)");
    check(near(hHalf[1], 0.05f, 2e-3f), "arm IK: halfway, between the two targets");
}

static void testSnaps() {
    Animation a;
    a.events.push_back({0, 1005, "event=101\ncomment=x\nsnapObjectRoot=newspaper:__newspaperRolled__\nsnapTargetSlot=auSkel:__R_Hand_grip__\nsnapObjectPose=\n"});
    a.events.push_back({0, 1014, "sound_name=newsp_start\n"});
    a.events.push_back({50, 1005, "event=101\nsnapObjectRoot=babyBottle:__babybottle__\nsnapTargetSlot=auSkel:__R_Hand_grip__\nsnapObjectPose=o2a-fridge-babyBottle-get-babyBottlePose\n"});
    a.events.push_back({2100, 1005, "event=102\nsnapObjectRoot=newspaper:__newspaperRolled__\nsnapTargetSlot=Oblivion\n"});
    a.events.push_back({2466, 1005, "event=301\nsnapObjectRoot=newspaperRead_02:__newspaperReading__\nsnapTargetSlot=auSkel:__L_Hand_grip__\n"});
    auto snaps = animSnaps(a);
    check(snaps.size() == 4 && snaps[1].pose == "o2a-fridge-babyBottle-get-babyBottlePose" && snaps[0].pose.empty(),
          "snap pose parsed");
    std::erase_if(snaps, [](const AnimSnap& x) { return x.object == "babyBottle"; });
    check(snaps.size() == 3 && snaps[0].object == "newspaper" && snaps[0].model == "newspaperRolled" &&
              snaps[0].skeleton == "auSkel" && snaps[0].bone == "R_Hand_grip" && snaps[1].bone.empty(),
          "snap events parsed");
    auto at = snapStateAt(snaps, 1000);
    check(at.size() == 1 && at["newspaper"].bone == "R_Hand_grip", "snap state: rolled paper in the right hand");
    at = snapStateAt(snaps, 3000);
    check(at.size() == 2 && at["newspaper"].bone.empty() && at["newspaperRead_02"].bone == "L_Hand_grip",
          "snap state: rolled paper gone, open paper in the left hand");
}

// An animation on one bone: rotation about Z by `degrees`, constant.
// One bone held at `degrees` about Z (X with `axis` 0); a delta channel
// (bit 15 clear) when `delta`.
static Animation constantAnim(const char* bone, float degrees, uint8_t priority, float velocity, bool delta = false,
                              int axis = 2) {
    Animation a;
    a.duration = 1000;
    a.priority = priority;
    a.velocity = velocity;
    a.skeletonTag = "auskel";
    Animation::Target t;
    t.tag = "auskel";
    Animation::Channel c;
    c.name = bone;
    c.flags = 1000u | (delta ? 0u : 0x8000u) | 0u << 17 | 3u << 22 | 0xFu << 25 | 3u << 29;
    c.components.resize(3);
    for (auto& comp : c.components)
        comp.keys.push_back({0, 0, 0, 0});
    c.components[size_t(axis)].keys[0].value = degrees;
    t.channels.push_back(c);
    a.targets.push_back(t);
    return a;
}

// The game's outlets write no bone that no channel feeds, and the scene
// graph bone keeps its last value (docs/anim-mac.md §2): a bone only a
// stopped overlay wrote holds where it was left, not the rest pose, and a
// delta over no base channel (the look-at's head) does not pile up.
static void testHeldBones() {
    Skeleton s;
    s.bones.push_back({"root_trans", -1, Xform{}, 0, 0});
    s.bones.push_back({"pelvis", 0, Xform{}, 1, 1});
    s.bones.push_back({"spine0", 1, Xform{}, 2, 2});
    s.bones.push_back({"head", 2, Xform{}, 3, 3});
    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    const Animation idle = constantAnim("pelvis", 0, 0, 0), wave = constantAnim("spine0", 30, 5, 0);
    AnimPlayer p(&s);
    p.play(idle, 0, {.fade = 0});
    p.play(wave, 0, {.fade = 0, .mask = boneMask(s, "spine0"), .priority = 101, .level = 1});
    check(near(angle(p.pose(0.1)[2].q), 30, 1e-2f), "held bones: the overlay plays");
    p.stop(0.2, 0.1f, 101);
    p.pose(0.25);
    p.pose(0.4); // faded out and dropped
    check(p.layerCount() == 1 && near(angle(p.pose(0.5)[2].q), 30, 0.5f),
          "held bones: a bone no layer writes keeps its last value");
    const Animation nod = constantAnim("head", 10, 0, 0, true);
    AnimPlayer d(&s);
    d.play(idle, 0, {.fade = 0});
    d.play(nod, 0, {.fade = 0, .priority = 3});
    float last = 0;
    for (int i = 1; i <= 5; ++i)
        last = angle(d.pose(0.1 * i)[3].q);
    check(near(last, 10, 0.1f), "held bones: a delta over no base channel does not pile up");
}

static void testPlayer() {
    Skeleton s;
    s.bones.push_back({"root_trans", -1, Xform{}, 0, 0});
    s.bones.push_back({"pelvis", 0, Xform{}, 1, 1});
    s.bones.push_back({"spine0", 1, Xform{}, 2, 2});
    s.bones.push_back({"spine1", 2, Xform{}, 3, 3});
    const auto mask = boneMask(s, "spine0");
    check(mask.size() == 4 && mask[0] == 0 && mask[1] == 0 && mask[2] == 1 && mask[3] == 1, "bone mask: subtree");

    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    const Animation walk = constantAnim("pelvis", 40, 0, 0.002f), idle = constantAnim("pelvis", 0, 0, 0);
    AnimPlayer p(&s);
    p.play(walk, 0);
    check(near(angle(p.pose(0.1)[1].q), 40, 1e-2f), "first base layer plays at once");
    check(near(p.pose(0.1)[2].q.w, 1), "bones the layer does not key keep the rest pose");
    p.play(idle, 1.0, {.fade = 0.5f});
    check(near(angle(p.pose(1.25)[1].q), 20, 0.5f), "crossfade halfway");
    check(near(angle(p.pose(1.6)[1].q), 0, 1e-2f) && p.layerCount() == 1, "crossfade done, old layer dropped");

    const Animation wave = constantAnim("spine1", 30, 5, 0);
    p.play(wave, 2.0, {.fade = 0, .mask = boneMask(s, "spine0")});
    const auto both = p.pose(2.1);
    check(near(angle(both[3].q), 30, 1e-2f) && near(angle(both[1].q), 0, 1e-2f) && p.layerCount() == 2,
          "overlay of a higher priority on its own bones");

    // stayInWorld(): a layer that plays once is moved back by the model's
    // move (the bones under the top node), the ones started after are not.
    AnimPlayer r(&s);
    r.play(walk, 0, {.fade = 0, .loop = false});
    r.stayInWorld(Xform{quatFromEulerXYZ(0, 0, float(M_PI / 2)), {1, 0, 0}}, 0.05);
    const auto moved = r.pose(0.1);
    check(near(angle(moved[1].q), 130, 1e-2f) && near(moved[1].t[0], 1, 1e-4f), "stayInWorld moves a layer that plays once");
    r.play(idle, 0.2, {.fade = 0, .loop = false});
    check(near(angle(r.pose(0.3)[1].q), 0, 1e-2f), "a layer started after the jump plays in the new frame");

    // A looping layer still playing goes on in the new frame; one fading out stays.
    AnimPlayer once(&s);
    once.play(walk, 0, {.fade = 0, .loop = true});
    once.stayInWorld(Xform{quatFromEulerXYZ(0, 0, float(M_PI / 2)), {1, 0, 0}}, 0.05);
    check(near(angle(once.pose(0.1)[1].q), 40, 1e-2f), "stayInWorld leaves a looping layer");
    AnimPlayer fading(&s);
    fading.play(walk, 0, {.fade = 0, .loop = true});
    fading.play(idle, 0.1, {.fade = 0.5f, .loop = true}); // replaces it: the walk fades out
    fading.stayInWorld(Xform{quatFromEulerXYZ(0, 0, float(M_PI / 2)), {1, 0, 0}}, 0.1);
    check(near(fading.pose(0.1)[1].t[0], 1, 1e-3f), "stayInWorld moves a looping layer fading out");

    // An object's animation (target "book") binds nothing on a Sim's skeleton.
    {
        Skeleton sim = s;
        sim.name = "auSkel_cres";
        Animation book = constantAnim("pelvis", 40, 2, 0);
        book.skeletonTag = "book";
        book.targets[0].tag = "book";
        AnimPlayer b(&sim);
        b.play(idle, 0, {.fade = 0});
        b.play(book, 0, {.fade = 0, .mask = boneMask(sim, "root_trans"), .level = 2});
        check(near(angle(b.pose(0.1)[1].q), 0, 1e-2f), "an object's animation leaves a Sim's bones alone");
        AnimPlayer onBook(&s); // (an object's own skeleton: plays)
        onBook.play(book, 0, {.fade = 0});
        check(near(angle(onBook.pose(0.1)[1].q), 40, 1e-2f), "an object's animation plays on the object");
    }

    AnimPlayer m(&s);
    m.play(walk, 0);
    m.advance(0);
    check(near(m.advance(2.0), 4.0f, 1e-3f), "root motion: velocity per tick over two seconds");
}

// The game's channel blender (cChannelUberOutlet): per bone, the highest
// level whose weights reach 1 takes it, lighter levels share, one level's
// layers are averaged, and the weights are normalized.
static void testChannelLevels() {
    Skeleton s;
    s.bones.push_back({"root_trans", -1, Xform{}, 0, 0});
    s.bones.push_back({"pelvis", 0, Xform{}, 1, 1});
    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    const Animation sit = constantAnim("pelvis", 40, 0, 0), idle = constantAnim("pelvis", 0, 0, 0),
                    carry = constantAnim("pelvis", 80, 0, 0);
    {
        // An idle overlay (level 1) under an Animate Sim (level 2): the Sim's wins.
        AnimPlayer p(&s);
        p.play(sit, 0, {.fade = 0, .level = 2});
        p.play(idle, 0, {.fade = 0, .priority = 101, .level = 1});
        check(near(angle(p.pose(0.1)[1].q), 40, 1e-2f), "a lower level gives way on shared bones");
        // A carry pose (level 5) over it wins.
        p.play(carry, 0.2, {.fade = 0, .priority = 102, .level = 5});
        check(near(angle(p.pose(0.3)[1].q), 80, 1e-2f), "a higher level takes the bone");
    }
    {
        // Two layers of one level at full weight: averaged.
        AnimPlayer p(&s);
        p.play(sit, 0, {.fade = 0, .priority = 101, .level = 3});
        p.play(idle, 0, {.fade = 0, .priority = 102, .level = 3});
        check(near(angle(p.pose(0.1)[1].q), 20, 0.1f), "one level's layers are averaged");
    }
    {
        // A lone layer half faded in already has the whole bone (normalized).
        AnimPlayer p(&s);
        p.play(sit, 0, {.fade = 1.0f, .priority = 101, .level = 4});
        check(near(angle(p.pose(0.5)[1].q), 40, 1e-2f), "a lone layer fading in is normalized");
        // Under a level-6 layer at half weight: half each (0.5 / (0.5 + 0.5)).
        p.play(carry, 1.0, {.fade = 1.0f, .priority = 102, .level = 6});
        check(near(angle(p.pose(1.5)[1].q), 60, 0.5f), "a lighter level shares with the ones under it");
        // The request weight scales the fade.
        AnimPlayer w(&s);
        w.play(sit, 0, {.fade = 0, .priority = 101, .level = 2});
        w.play(carry, 0, {.fade = 0, .priority = 102, .level = 3, .weight = 0.25f});
        check(near(angle(w.pose(0.1)[1].q), 50, 0.5f), "request weight: 0.25 of the bone over the rest");
    }
}

// Delta (additive) channels: bit 15 of the channel flags clear
// (IsBaseDataReturned); multiplied onto the base mix by their weight
// (cChannelUberOutletQ::ChannelData), translations added.
static void testDeltaChannels() {
    Skeleton s;
    s.bones.push_back({"root_trans", -1, Xform{}, 0, 0});
    s.bones.push_back({"pelvis", 0, Xform{}, 1, 1});
    s.bones.push_back({"spine0", 1, Xform{}, 2, 2});
    s.bones.push_back({"l_clavicle", 2, Xform{}, 3, 3});
    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    const Animation walk = constantAnim("pelvis", 40, 0, 0);
    Animation hunch = constantAnim("pelvis", 10, 2, 0, true);
    hunch.flags = 1; // full-body by its flag, as the talking "SWAT" shifts
    check(hunch.targets[0].channels[0].isDelta() && !walk.targets[0].channels[0].isDelta(), "delta: bit 15 clear");
    check(hunch.additive() && !walk.additive(), "delta-only target: additive");
    const auto keyed = keyedBones(hunch.targets[0], s);
    check(!keyed[1], "delta channels key no bone");
    {
        AnimPlayer p(&s);
        p.play(walk, 0);
        p.play(hunch, 0, {.fade = 0, .priority = 61, .level = 0});
        const auto pose = p.pose(0.1);
        check(near(angle(pose[1].q), 50, 1e-2f), "delta added to the base rotation");
        check(near(pose[2].q.w, 1) && near(pose[3].q.w, 1), "a full-body delta replaces no other bone");
        // At half weight: half the angle (q^0.5).
        AnimPlayer h(&s);
        h.play(walk, 0);
        h.play(hunch, 0, {.fade = 0, .priority = 61, .level = 0, .weight = 0.5f});
        check(near(angle(h.pose(0.1)[1].q), 45, 1e-2f), "delta weighed: q^w");
        // A lower level does not matter: deltas are not shared out by level.
        AnimPlayer l(&s);
        l.play(walk, 0, {.fade = 0, .level = 6});
        l.play(hunch, 0, {.fade = 0, .priority = 61, .level = 0});
        check(near(angle(l.pose(0.1)[1].q), 50, 1e-2f), "deltas apply at any level");
    }
    {
        // Base * delta: the delta turns about the bone's own axes. Base 90
        // degrees about Z, delta 30 about X: base * delta, not delta * base.
        const Animation base = constantAnim("pelvis", 90, 0, 0);
        const Animation tilt = constantAnim("pelvis", 30, 2, 0, true, 0);
        AnimPlayer p(&s);
        p.play(base, 0);
        p.play(tilt, 0, {.fade = 0, .priority = 61});
        const Quat q = p.pose(0.1)[1].q;
        const float d = 3.14159265f / 180;
        const Quat want = quatFromEulerXYZ(0, 0, 90 * d) * quatFromEulerXYZ(30 * d, 0, 0);
        check(near(std::fabs(q.x * want.x + q.y * want.y + q.z * want.z + q.w * want.w), 1, 1e-4f),
              "delta multiplied after the base (base * delta)");
    }
    {
        // A bone no base layer moves keeps its pose, deltas or not, except
        // the ones the look-at drives (spine0: SetApplyDeltaDataToIK).
        Animation clav = constantAnim("l_clavicle", 20, 2, 0, true);
        Animation spine = constantAnim("spine0", 20, 2, 0, true);
        AnimPlayer p(&s);
        p.play(walk, 0);
        p.play(clav, 0, {.fade = 0, .priority = 61});
        p.play(spine, 0, {.fade = 0, .priority = 62});
        const auto pose = p.pose(0.1);
        check(near(pose[3].q.w, 1), "no delta on a bone without a base channel");
        check(near(angle(pose[2].q), 20, 1e-2f), "spine0 takes its delta over the rest pose");
    }
    check(near(deltaPower(quatFromEulerXYZ(0, 0, 1.0f), 0.25f).z, std::sin(0.125f)), "q^w scales the angle");
}

// A delta on an IK goal node moves the chain that reaches it (the sofa
// bounces' *_handcontrol1 deltas over the sitting animation's arm chains).
static void testDeltaIk() {
    Skeleton s;
    s.bones.push_back({"root", -1, Xform{}, -1, 0});
    // The bones lie along their +Y, as the game's skeletons (the solver
    // relies on it): the thigh turned to point down.
    s.bones.push_back({"thigh", 0, Xform{quatFromEulerXYZ(-float(M_PI) / 2, 0, 0), {0, 0, 1}}, 0, 1});
    s.bones.push_back({"calf", 1, Xform{{}, {0, 0.5f, 0}}, 1, 2});
    s.bones.push_back({"foot", 2, Xform{{}, {0, 0.5f, 0}}, 2, 3});
    s.bones.push_back({"goal", 0, Xform{{}, {0, 0, 0}}, -1, 4});
    s.bones.push_back({"pole", 0, Xform{{}, {0, 1, 0.5f}}, -1, 5});
    auto translation = [](const char* bone, float x, float y, float z, bool delta) {
        Animation::Channel c;
        c.name = bone;
        c.nameHash = nameInstanceHiHash(bone);
        c.flags = 1000u | (delta ? 0u : 0x8000u) | (uint32_t(Animation::Translation) << 17) |
                  (uint32_t(Animation::TransformXYZ) << 22) | 0xFu << 25 | 3u << 29;
        for (float v : {x, y, z}) {
            Animation::Component k;
            k.keys.push_back({0, v, 0, 0});
            c.components.push_back(k);
        }
        return c;
    };
    Animation base;
    base.duration = 1000;
    base.skeletonTag = "skel";
    base.targets.resize(1);
    base.targets[0].tag = "skel";
    base.targets[0].channels.push_back(translation("goal", 0, 0, 0.3f, false));
    Animation::IkChain chain;
    chain.strategy = 0x48EE7A41;
    chain.crcs[0] = nameInstanceHiHash("thigh");
    chain.crcs[2] = nameInstanceHiHash("foot");
    chain.crcs[4] = nameInstanceHiHash("pole");
    Animation::IkTarget goal;
    goal.crcs[2] = goal.crcs[4] = nameInstanceHiHash("goal");
    chain.targets.push_back(goal);
    base.targets[0].ikChains.push_back(chain);
    Animation bounce;
    bounce.duration = 1000;
    bounce.skeletonTag = "skel";
    bounce.targets.resize(1);
    bounce.targets[0].tag = "skel";
    bounce.targets[0].channels.push_back(translation("goal", 0, 0, 0.1f, true));
    AnimPlayer p(&s);
    p.play(base, 0, {.fade = 0});
    auto foot = s.world(p.pose(0.1))[3].t;
    check(near(foot[2], 0.3f, 1e-3f), "delta IK: the base goal");
    AnimPlayer q(&s);
    q.play(base, 0, {.fade = 0});
    q.play(bounce, 0, {.fade = 0, .priority = 61});
    foot = s.world(q.pose(0.1))[3].t;
    check(near(foot[2], 0.4f, 1e-3f), "delta IK: the goal node's delta moves the foot");
}

// Body shape morphs from person data 81 (body flags) and 58 (age).
static void testBodyShape() {
    auto m = bodyShapeMorphs(0x10, 19);
    check(m["fatbot"] == 0 && m["fattop"] == 0 && m["pregbot"] == 0, "no body flags: thin, not pregnant");
    m = bodyShapeMorphs(0x1, 19);
    check(m["fatbot"] == 1 && m["fattop"] == 1, "flag 0x1: fat");
    m = bodyShapeMorphs(0x4, 19);
    check(m["pregtop"] == 0.5f, "flag 0x4: half pregnant");
    m = bodyShapeMorphs(0x3, 16);
    check(m["fatbot"] == 1 && !m.count("pregbot"), "teens: no pregnancy morph");
    check(bodyShapeMorphs(0x1, 2).empty(), "toddlers: no body morphs");
    // The CAS's outfit fitness (BV 0x8141c4): only fitness 2 is fat.
    m = outfitFitnessMorphs(2);
    check(m["fattop"] == 1 && m["fatbot"] == 1, "outfit fitness 2: fat morphs at 1");
    m = outfitFitnessMorphs(1);
    check(m.count("fattop") && m["fattop"] == 0 && m["fatbot"] == 0, "outfit fitness 1 (fit): morphs at 0");
    check(outfitFitnessMorphs(0)["fatbot"] == 0, "outfit fitness 0: morphs at 0");
}

// Mirroring: a left arm's motion lands on the right arm, reflected across
// the YZ plane, whatever the joints' own axes (the right one here is turned
// 180 degrees about Y in its rest pose).
static void testMirror() {
    Skeleton s;
    s.bones.push_back({"root", -1, Xform{}, 0, 0});
    s.bones.push_back({"l_arm", 0, Xform{Quat{}, {0.2f, 0, 1}}, 1, 1});
    s.bones.push_back({"r_arm", 0, Xform{Quat{0, 1, 0, 0}, {-0.2f, 0, 1}}, 2, 2});
    s.bones.push_back({"l_hand", 1, Xform{Quat{}, {0.3f, 0, 0}}, 3, 3});
    s.bones.push_back({"r_hand", 2, Xform{Quat{}, {0.3f, 0, 0}}, 4, 4});
    const auto twins = mirrorBones(s);
    check(twins == std::vector<int>({0, 2, 1, 4, 3}), "mirror twins by l_/r_");
    std::vector<Xform> local = s.restPose();
    local[1].q = quatFromEulerXYZ(0, 0, 0.5f); // the left arm swings forward (+Y)
    const auto restWorld = s.world(s.restPose());
    const auto mirrored = mirrorPose(s, twins, restWorld, local);
    const auto w = s.world(local), mw = s.world(mirrored);
    // The right hand ends where the left one was, reflected (x -> -x).
    check(near(mw[4].t[0], -w[3].t[0], 1e-4f) && near(mw[4].t[1], w[3].t[1], 1e-4f) && near(mw[4].t[2], w[3].t[2], 1e-4f),
          "mirrored hand position");
    check(near(mw[3].t[0], 0.5f, 1e-4f) && near(mw[3].t[1], 0, 1e-4f), "the other arm stays at rest");
    const auto twice = mirrorPose(s, twins, restWorld, mirrored);
    check(near(s.world(twice)[3].t[1], w[3].t[1], 1e-4f), "mirroring twice gives the pose back");
}

// A lot's saved transform nodes pose the object's skeleton (a chair pushed
// in); the rest pose saved gives nothing; a held player keeps its pose.
static void testSavedPose() {
    Skeleton s;
    s.bones.push_back({"chairdining", -1, Xform{}, 0, 0});
    s.bones.push_back({"slot_chairdining_trans", 0, Xform{}, 1, 1});
    check(!savedPose(s, {{"chairdining", Xform{}}, {"slot_chairdining_trans", Xform{}}}), "rest pose saved: no pose");
    const auto p = savedPose(s, {{"slot_chairdining_trans", Xform{{}, {0, 0.373f, 0}}}, {"unknown_bone", Xform{}}});
    check(p && p->size() == 2 && near((*p)[1].t[1], 0.373f) && near((*p)[0].t[1], 0), "saved node replaces the rest");
    check(poseKey(*p) != poseKey(s.restPose()) && poseKey(*p) == poseKey(*p), "pose key");
    AnimPlayer player(&s);
    player.setBasePose(*p);
    check(near(player.pose(0)[1].t[1], 0.373f), "base pose under no animation");
}

// A two-tile door's leaves, as the host plays what the VM plays on an object
// (app/lot_vm_setup.mm onObjectOverlay): the open animation on the right leaf and
// the same one flipped on the left, each its own layer (priority and level
// 1 + the overlay's id), both open at once; taken off when the VM takes
// them off, the leaves stay open (held); the close of one leaf closes it
// and leaves the other open. As one layer replacing the other, the first
// leaf fell back closed as the second opened.
static void testDoorLeaves() {
    Skeleton s;
    s.bones.push_back({"door", -1, Xform{}, 0, 0});
    s.bones.push_back({"r_door_rot", 0, Xform{{}, {-0.5f, 0, 0}}, 1, 1});
    s.bones.push_back({"l_door_rot", 0, Xform{{}, {0.5f, 0, 0}}, 2, 2});
    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    Animation open = constantAnim("r_door_rot", 90, 2, 0), close = constantAnim("r_door_rot", 0, 2, 0);
    open.skeletonTag = close.skeletonTag = "door";
    open.targets[0].tag = close.targets[0].tag = "door";
    SimActor actor{SkinnedModel(s)};
    actor.player.setHoldPose(true);
    actor.cue(0, open, {.fade = 0, .loop = false, .priority = 2, .mirror = false, .level = 2});
    actor.cue(0, open, {.fade = 0, .loop = false, .priority = 3, .mirror = true, .level = 3});
    actor.startCues(0.5);
    auto p = actor.player.pose(0.5);
    check(near(angle(p[1].q), 90, 0.5f) && near(angle(p[2].q), -90, 0.5f), "door: both leaves open at once");
    actor.cueStop(0.6, 2, 0);
    actor.cueStop(0.6, 3, 0);
    actor.startCues(0.7);
    p = actor.player.pose(0.7);
    check(actor.player.layerCount() == 0 && near(angle(p[1].q), 90, 0.5f) && near(angle(p[2].q), -90, 0.5f),
          "door: the leaves stay open when their overlays end");
    actor.cue(1.0, close, {.fade = 0, .loop = false, .priority = 4, .mirror = false, .level = 4});
    actor.startCues(1.0);
    p = actor.player.pose(1.2);
    check(near(angle(p[1].q), 0, 0.5f) && near(angle(p[2].q), -90, 0.5f), "door: closing one leaf leaves the other");
}

// With the game's data (OS2_DATA): a Sim in a dining chair sits where the
// chair's seat is, and its get-out starts there. N001_Lot68's chair 643
// ("Chair - Dining - Value1" at (17.5, 25.5), direction 0: its +y toward the
// table at -x) is saved pushed in: "slot_chairdining_trans" 0.373 forward,
// the seat "slot_chairDining_0" under it (the getIn's baked 0.375 too). The
// host drew the seated Sim at the chair's origin (the chair actor's bones
// were never used), 0.37 behind the seat; the get-out, which the trees play
// from the corner bone "route_chairdining_2_se" (Snap, then "Snap - Away"
// turns the Sim around) with its header heading of -180 degrees, starts
// seated at the true seat: the Sim jumped to sit in the air at the table's
// corner beside the chair, the chair pulling out behind it (user playtest).
static void testDiningChairSeat() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    SimActor anims{SkinnedModel(Skeleton{})}; // owns the loaded animations
    auto load = [&](const std::string& name) -> const Animation* {
        try {
            return &anims.animation(index, name);
        } catch (const std::exception&) {
            return nullptr;
        }
    };
    const ObjectModel chairModel = loadObjectModel(index, "chairDiningValue1_cres");
    SimActor chair{SkinnedModel(chairModel.skeleton)};
    std::vector<Xform> saved = chairModel.skeleton.restPose();
    const int trans = chairModel.skeleton.find("slot_chairdining_trans");
    check(trans >= 0, "dining chair: slot_chairdining_trans");
    if (trans < 0)
        return;
    saved[size_t(trans)].t[1] = 0.373f;
    chair.player.setBasePose(saved);
    chair.player.setHoldPose(true);
    chair.position = {17.5f, 25.5f, 0.75f};
    chair.heading = float(M_PI / 2); // direction 0 (vm::headingOfDirection)
    const Animation* getIn = load("o2a-chairDining-getIn-SE");
    const Animation* getOut = load("a2o-chairDining-getOut-SE");
    check(getIn && getOut, "dining chair: its animations");
    if (!getIn || !getOut)
        return;
    // As the host cues the VM's overlay on the chair (lot_vm_setup.mm onObjectOverlay).
    chair.cue(0, *getIn, {.fade = 0.1f, .loop = false, .priority = 2, .mirror = true, .level = 2});
    const auto seat = chair.bonePlacement("slot_chairDining_0", 5.0);
    check(seat.has_value(), "dining chair: seat bone");
    if (!seat)
        return;
    check(near(seat->t[0], 17.5f - 0.375f, 0.02f) && near(seat->t[1], 25.5f, 0.02f),
          "dining chair: the seat is the pushed-in chair's, 0.375 toward the table");
    // Mid pull-out, the seat stays (only chairdining_trans moves).
    const auto pulled = chair.bonePlacement("slot_chairDining_0", 1.5);
    check(pulled && near(pulled->t[0], seat->t[0], 0.02f), "dining chair: the seat does not move with the pull");
    // The get-out from the corner bone, turned around, with its header heading.
    const auto corner = chair.bonePlacement("route_chairdining_2_se", 5.0);
    const ObjectModel adult = loadObjectModel(index, "auskel_cres");
    check(corner && adult.skeleton.find("root_rot") >= 0, "dining chair: corner bone and adult skeleton");
    if (!corner || adult.skeleton.find("root_rot") < 0)
        return;
    const Vec3 f = corner->rotate({0, 1, 0});
    SimActor sim{SkinnedModel(adult.skeleton)};
    sim.position = corner->t;
    sim.heading = std::atan2(-f[0], f[1]) + float(M_PI);
    sim.player.play(*getOut, 0, {.fade = 0, .loop = false, .turn = getOut->headingOffset});
    const auto root = sim.bonePlacement("root_rot", 0.0);
    check(root && std::hypot(root->t[0] - seat->t[0], root->t[1] - seat->t[1]) < 0.1f,
          "dining chair: the get-out starts seated in the seat");
}

// With the game's data (OS2_DATA): a dining chair's whole sit and get-up,
// as the host plays them (N001_Lot68's chair 643, saved pushed in). The
// plain get-in/get-out pull the chair out and push it back; the "leaveOut"
// pair (a Sim with a mug sits in the chair pulled out, 0.33 back and turned
// 15.6 degrees, and the get-out pushes it in again) ends pushed in too, and
// left out only while the Sim sits. The Sim's get-in that leaves the chair
// out is played from where it stood (the corner bone "route_chairdining_2_sw",
// heading 0 in its header) and ends seated 0.67 from there, in the chair as
// it is pulled out; once in the seat the Sim is placed at the seat bone, and
// the held last frame must stay where it was (AnimPlayer::stayInWorld): placed
// again at the seat, it sat 0.67 beside the chair, between two (user QA).
static void testDiningChairCycle() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    SimActor anims{SkinnedModel(Skeleton{})};
    auto load = [&](const std::string& name) -> const Animation* {
        try {
            return &anims.animation(index, name);
        } catch (const std::exception&) {
            return nullptr;
        }
    };
    const ObjectModel chairModel = loadObjectModel(index, "chairDiningValue1_cres");
    const int trans = chairModel.skeleton.find("slot_chairdining_trans");
    check(trans >= 0, "chair cycle: slot_chairdining_trans");
    if (trans < 0)
        return;
    std::vector<Xform> saved = chairModel.skeleton.restPose();
    saved[size_t(trans)].t[1] = 0.373f; // as N001_Lot68 saved it
    auto makeChair = [&]() {
        auto c = std::make_unique<SimActor>(SkinnedModel(chairModel.skeleton));
        c->player.setBasePose(saved);
        c->player.setHoldPose(true);
        c->position = {17.5f, 25.5f, 0.75f};
        c->heading = float(M_PI / 2);
        return c;
    };
    auto yaw = [](const Xform& x) {
        const Vec3 f = x.rotate({0, 1, 0});
        return std::atan2(-f[0], f[1]) * 180 / float(M_PI);
    };
    const Animation* getIn = load("o2a-chairDining-getIn-SE");
    const Animation* getOut = load("o2a-chairDining-getOut-SE");
    const Animation* inOut = load("o2a-chairDining-getIn-leaveOut-SE");
    const Animation* outIn = load("o2a-chairDining-getOut-fromOut-SE");
    const Animation* simIn = load("a2o-chairDining-getIn-rightHand-leaveOut-SE");
    check(getIn && getOut && inOut && outIn && simIn, "chair cycle: its animations");
    if (!getIn || !getOut || !inOut || !outIn || !simIn)
        return;
    const Vec3 pushedIn{17.5f - 0.373f, 25.5f, 0.75f};
    // At load: the lot's four Value1 chairs (643, 645, 648, 651) are saved
    // pushed in and square to the table, and drawn so (savedPose).
    {
        DbpfPackage pkg(std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/N001/Lots/N001_Lot68.package");
        const Lot lot = Lot::load(pkg);
        int square = 0;
        for (const auto& o : lot.objects) {
            const uint16_t id = uint16_t(o.instance & 0xFFFF);
            if (o.definition.find("Value1") == std::string::npos || (id != 643 && id != 645 && id != 648 && id != 651))
                continue;
            const auto p = savedPose(chairModel.skeleton, o.nodes);
            if (!p)
                continue;
            const auto w = chairModel.skeleton.world(*p);
            const int seatBone = chairModel.skeleton.find("slot_chairDining_0");
            if (seatBone >= 0 && near(w[size_t(seatBone)].t[1], 0.373f, 0.01f) && near(w[size_t(seatBone)].t[0], 0, 0.01f) &&
                near(std::abs(w[size_t(seatBone)].q.w), 1, 1e-3f))
                ++square;
        }
        check(square == 4, "chair cycle: N001_Lot68's dining chairs load pushed in, square");
    }
    // Overlays as lot_vm_setup.mm's onObjectOverlay cues them: each its own layer
    // (level 1 + the overlay's id), flipped, never taken off.
    {
        auto chair = makeChair();
        chair->cue(0, *getIn, {.fade = 0.1f, .loop = false, .priority = 2, .mirror = true, .level = 2});
        chair->cue(10, *getOut, {.fade = 0.1f, .loop = false, .priority = 3, .mirror = true, .level = 3});
        const auto seat = chair->bonePlacement("slot_chairDining_0", 20);
        check(seat && near(seat->t[0], pushedIn[0], 0.01f) && near(seat->t[1], pushedIn[1], 0.01f) && near(yaw(*seat), 90, 0.5f),
              "chair cycle: get-in, get-out: pushed in again");
    }
    auto chair = makeChair();
    const auto corner = chair->bonePlacement("route_chairdining_2_sw", 0);
    chair->cue(0, *inOut, {.fade = 0.1f, .loop = false, .priority = 2, .mirror = true, .level = 2});
    const auto out = chair->bonePlacement("slot_chairDining_0", 5);
    check(out && std::hypot(out->t[0] - pushedIn[0], out->t[1] - pushedIn[1]) > 0.3f && near(std::abs(yaw(*out) - 90), 15.6f, 0.5f),
          "chair cycle: leaveOut: the chair stays out, turned, while the Sim sits");
    chair->cue(10, *outIn, {.fade = 0.1f, .loop = false, .priority = 3, .mirror = true, .level = 3});
    const auto back = chair->bonePlacement("slot_chairDining_0", 20);
    check(back && near(back->t[0], pushedIn[0], 0.01f) && near(back->t[1], pushedIn[1], 0.01f) && near(yaw(*back), 90, 0.5f),
          "chair cycle: get-out from out: pushed in again");
    // The Sim: from the corner, its get-in ends in the chair pulled out.
    const ObjectModel adult = loadObjectModel(index, "auskel_cres");
    check(corner && out && adult.skeleton.find("root_trans") >= 0, "chair cycle: corner bone and adult skeleton");
    if (!corner || !out || adult.skeleton.find("root_trans") < 0)
        return;
    SimActor sim{SkinnedModel(adult.skeleton)};
    sim.position = corner->t;
    sim.heading = yaw(*corner) * float(M_PI) / 180;
    sim.player.play(*simIn, 0, {.fade = 0, .loop = false, .mirror = true, .turn = simIn->headingOffset});
    const auto seated = sim.bonePlacement("root_trans", 5);
    check(seated && std::hypot(seated->t[0] - out->t[0], seated->t[1] - out->t[1]) < 0.06f,
          "chair cycle: the leaveOut get-in ends seated in the pulled-out chair");
    // Contained: placed at the seat, the held frame stays in the world (the
    // seat module's rule 3), seated in the chair.
    const Xform before = sim.placement();
    sim.position = out->t;
    sim.heading = yaw(*out) * float(M_PI) / 180;
    sim.player.stayInWorld(sim.placement().inverse().then(before), 5.5);
    const auto still = sim.bonePlacement("root_trans", 6);
    check(seated && still && std::hypot(still->t[0] - seated->t[0], still->t[1] - seated->t[1]) < 0.01f,
          "chair cycle: placed at the seat, the held get-in stays seated in the chair");
}

// An object's overlays the VM never takes off (a dining chair: each sit and
// get-up adds a play-once layer at level 1 + its id, held on its last
// frame): with the hold pose, once every layer has finished, the pose of
// that moment becomes the base and the layers go (the game's requests
// expire, the bones keep their values), so the list stays bounded and the
// object stays as it was left, a bone only one old layer keyed included.
static void testHeldLayersSettle() {
    Skeleton s;
    s.name = "chairdining";
    s.bones.push_back({"root_trans", -1, Xform{}, 0, 0});
    s.bones.push_back({"pelvis", 0, Xform{}, 1, 1});
    s.bones.push_back({"spine0", 1, Xform{}, 2, 2});
    const Animation pull = constantAnim("pelvis", 30, 0, 0), push = constantAnim("pelvis", 0, 0, 0),
                    tilt = constantAnim("spine0", 15, 0, 0);
    auto angle = [](const Quat& q) { return 2 * std::atan2(q.z, q.w) * 180 / 3.14159265f; };
    AnimPlayer p(&s);
    p.setHoldPose(true);
    size_t most = 0;
    bool pulled = true, pushed = true, tilted = true;
    int id = 0;
    for (int cycle = 0; cycle < 200; ++cycle) {
        const double t = cycle * 10.0;
        const int a = 1 + id, b = 2 + id;
        id += 2;
        p.play(pull, t, {.fade = 0.1f, .loop = false, .priority = a, .level = a});
        if (cycle == 3) // a bone only one layer ever keys
            p.play(tilt, t, {.fade = 0.1f, .loop = false, .priority = 1000, .level = a});
        pulled = pulled && near(angle(p.pose(t + 2.0)[1].q), 30, 0.1f);
        most = std::max(most, p.layerCount());
        p.play(push, t + 5, {.fade = 0.1f, .loop = false, .priority = b, .level = b});
        const auto x = p.pose(t + 8.0);
        pushed = pushed && near(angle(x[1].q), 0, 0.1f);
        if (cycle >= 3)
            tilted = tilted && near(angle(x[2].q), 15, 0.1f);
        most = std::max(most, p.layerCount());
    }
    check(most <= 2, "held layers: the list stays bounded over 200 sit/stand cycles");
    check(pulled && pushed, "held layers: each settles where its last frame left the object");
    check(tilted, "held layers: a bone only an old layer keyed keeps its value");
}

// With the game's data (OS2_DATA): N001_Lot68's dining chair through 60
// sits and get-ups as lot_vm_setup.mm cues them (level and priority 1 + the overlay
// id, flipped, never taken off), plain and "leaveOut" in turn: the layers
// stay few, the chair is pulled out and turned while seated after a
// leaveOut get-in and pushed in square after every get-out.
static void testDiningChairLayersBounded() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    SimActor anims{SkinnedModel(Skeleton{})};
    auto load = [&](const std::string& name) -> const Animation* {
        try {
            return &anims.animation(index, name);
        } catch (const std::exception&) {
            return nullptr;
        }
    };
    const ObjectModel chairModel = loadObjectModel(index, "chairDiningValue1_cres");
    const int trans = chairModel.skeleton.find("slot_chairdining_trans");
    const Animation* getIn = load("o2a-chairDining-getIn-SE");
    const Animation* getOut = load("o2a-chairDining-getOut-SE");
    const Animation* inOut = load("o2a-chairDining-getIn-leaveOut-SE");
    const Animation* outIn = load("o2a-chairDining-getOut-fromOut-SE");
    check(trans >= 0 && getIn && getOut && inOut && outIn, "chair layers: the chair and its animations");
    if (trans < 0 || !getIn || !getOut || !inOut || !outIn)
        return;
    std::vector<Xform> saved = chairModel.skeleton.restPose();
    saved[size_t(trans)].t[1] = 0.373f;
    SimActor chair{SkinnedModel(chairModel.skeleton)};
    chair.player.setBasePose(saved);
    chair.player.setHoldPose(true);
    chair.position = {17.5f, 25.5f, 0.75f};
    chair.heading = float(M_PI / 2);
    const Vec3 pushedIn{17.5f - 0.373f, 25.5f, 0.75f};
    auto yaw = [](const Xform& x) {
        const Vec3 f = x.rotate({0, 1, 0});
        return std::atan2(-f[0], f[1]) * 180 / float(M_PI);
    };
    size_t most = 0;
    bool outSeated = true, backIn = true;
    uint32_t id = 40;
    for (int cycle = 0; cycle < 60; ++cycle) {
        const double t = cycle * 20.0;
        const bool out = cycle % 2 == 1;
        const int a = 1 + int(id), b = 2 + int(id);
        chair.cue(t, out ? *inOut : *getIn, {.fade = 0.1f, .loop = false, .priority = a, .mirror = true, .level = a});
        chair.cue(t + 10, out ? *outIn : *getOut, {.fade = 0.1f, .loop = false, .priority = b, .mirror = true, .level = b});
        id += 2;
        if (const auto seat = chair.bonePlacement("slot_chairDining_0", t + 9.0); seat && out)
            outSeated = outSeated && std::hypot(seat->t[0] - pushedIn[0], seat->t[1] - pushedIn[1]) > 0.3f &&
                        near(std::abs(yaw(*seat) - 90), 15.6f, 0.5f);
        most = std::max(most, chair.player.layerCount());
        if (const auto seat = chair.bonePlacement("slot_chairDining_0", t + 19.0))
            backIn = backIn && near(seat->t[0], pushedIn[0], 0.01f) && near(seat->t[1], pushedIn[1], 0.01f) &&
                     near(yaw(*seat), 90, 0.5f);
        most = std::max(most, chair.player.layerCount());
    }
    check(most <= 2, "chair layers: bounded over 60 sit/stand cycles");
    check(outSeated, "chair layers: pulled out and turned while seated after a leaveOut get-in");
    check(backIn, "chair layers: pushed in square after every get-out");
}

// With the game's data (OS2_DATA): the carry poses a Sim walks with (and
// those that stayed on after the get-out in live QA 15: two of the seated
// "a2o-coffee-DN-carry", the delta "a-coffeeshake-blend", the plate's grasp
// with its half-turn heading) move the arm only, as the game's channel
// blender mixes them by level: the walk's pelvis, head and feet stay where
// the walk alone puts them (the leaning walk the QA saw was not theirs).
static void testCarryOverlaysKeepWalkUpright() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const ObjectModel adult = loadObjectModel(index, "auskel_cres");
    if (adult.skeleton.find("pelvis") < 0)
        return;
    auto pose = [&](const std::vector<std::pair<std::string, int>>& overlays, double t) {
        SimActor sim{SkinnedModel(adult.skeleton)};
        const Animation& walk = sim.animation(index, "a-male-walk-alt0-normal");
        sim.player.play(walk, 0, {.fade = 0, .loop = true});
        int k = 0;
        for (const auto& [name, level] : overlays) {
            const Animation& a = sim.animation(index, name);
            sim.player.play(a, 0, {.fade = 0.233f, .loop = name.find("grasp") == std::string::npos, .priority = 100 + k++,
                                   .turn = a.headingOffset, .level = level});
        }
        sim.update(t);
        return sim.bones;
    };
    const std::vector<std::pair<std::string, int>> carry = {
        {"a2o-coffeeMug-carry", 5}, {"a2o-coffee-DN-carry", 5}, {"a2o-coffee-DN-carry", 5}, {"a-coffeeshake-blend", 3},
        {"a2o-ingredients-carry", 5}, {"a2o-plateDessert-grasp", 3}, {"a2o-plateDessert-carry", 5}, {"a2o-dishware-DN-carry", 5}};
    float worst = 0;
    for (double t : {0.6, 0.8, 1.0, 1.2}) {
        const auto alone = pose({}, t), carried = pose(carry, t);
        for (const char* b : {"pelvis", "spine2", "head", "l_foot", "r_foot"}) {
            const int i = adult.skeleton.find(b);
            if (i < 0)
                continue;
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(alone[size_t(i)].t[size_t(c)] - carried[size_t(i)].t[size_t(c)]));
        }
    }
    check(worst < 0.01f, "carry overlays leave the walking body upright (pelvis, spine, head and feet as the walk's)");
}

// With the game's data (OS2_DATA): the clavicles' joint orientation (their
// cBoneDataExtension, a 14 degree twist) turns the animations' keys: in the
// bare standing idle the arms hang straight with the hands beside the
// thighs, a little ahead of the shoulders (playtest 30.png), not 8 cm
// behind them as with the raw keys.
static void testIdleArmsHangForward() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(data);
    const ObjectModel adult = loadObjectModel(index, "auskel_cres");
    const int lc = adult.skeleton.find("l_clavicle"), lu = adult.skeleton.find("l_upperarm"),
              lh = adult.skeleton.find("l_hand"), ru = adult.skeleton.find("r_upperarm"), rh = adult.skeleton.find("r_hand"),
              pelvis = adult.skeleton.find("pelvis");
    if (lc < 0 || lu < 0 || lh < 0 || ru < 0 || rh < 0 || pelvis < 0)
        return;
    check(std::abs(adult.skeleton.bones[size_t(lc)].orient.x + 0.1219f) < 0.001f, "clavicle joint orientation read");
    SimActor sim{SkinnedModel(adult.skeleton)};
    sim.player.play(sim.animation(index, "a-idle-neutral-armsSide-wtC-breatheLoop"), 0, {.fade = 0, .loop = true});
    sim.update(0.1);
    for (auto [u, h] : {std::pair{lu, lh}, std::pair{ru, rh}}) {
        const float ahead = sim.bones[size_t(h)].t[1] - sim.bones[size_t(u)].t[1];
        const float down = sim.bones[size_t(u)].t[2] - sim.bones[size_t(h)].t[2];
        check(ahead > 0.02f && ahead < 0.12f && down > 0.5f, "idle hand hangs a little ahead of the shoulder");
        check(sim.bones[size_t(h)].t[1] > sim.bones[size_t(pelvis)].t[1] - 0.02f, "idle hand not behind the hips");
    }
}

int main() {
    testIdleArmsHangForward();
    testCarryOverlaysKeepWalkUpright();
    testDiningChairCycle();
    testHeldLayersSettle();
    testDiningChairLayersBounded();
    testDiningChairSeat();
    testDoorLeaves();
    testSavedPose();
    testMirror();
    testBodyShape();
    testPlayer();
    testHeldBones();
    testChannelLevels();
    testDeltaChannels();
    testDeltaIk();
    testIk();
    testArmIk();
    testSnaps();
    testSimCatalog();
    testSkinning();
    testFixed();
    testDecode();
    testXform();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("anim tests passed\n");
    return failures ? 1 : 0;
}
