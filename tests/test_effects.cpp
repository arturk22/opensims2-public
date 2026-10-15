// The effects resource (formats/effects) and the particle simulation
// (render/particle_system) on hand-built data.
#include "formats/effects.hpp"
#include "render/particle_system.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace os2;
using namespace os2::fx;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

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
    void fs(std::initializer_list<float> v) {
        for (float f : v)
            f32(f);
    }
    void str(const std::string& s) {
        u32(uint32_t(s.size()));
        for (char c : s)
            u8(uint8_t(c));
    }
    void curve(std::initializer_list<float> v) {
        u32(uint32_t(v.size()));
        fs(v);
    }
};

// One version-3 particle block: rising smoke.
static void writeParticles(Writer& w) {
    w.u16(3);
    w.u32(0x00100000); // flags: random walk
    w.fs({2, 2, 0.5f}); // life, pre-roll
    w.fs({-1, -1, -1, -1}); // rate delay, trigger
    w.fs({0, 0, 1, 0, 0, 1}); // direction box
    w.fs({0.5f, 0.5f});     // speed
    w.fs({0, 0, 0, 0, 0, 0}); // volume
    w.f32(-1);              // torus (version > 2)
    w.curve({10});          // rate
    w.f32(1);               // rate curve time
    w.u16(0);               // cycles: forever
    w.f32(0);               // rate speed scale
    w.curve({0.2f, 0.4f});  // size
    w.f32(0);
    w.curve({1});           // aspect
    w.f32(0);
    w.fs({0, 0, 1}); w.fs({0, 0, 0}); w.u8(0); w.fs({0, 0}); // rotation unknowns
    w.fs({0, 0});           // rotate offsets
    w.curve({0}); w.curve({0});
    w.curve({1, 0});        // alpha
    w.f32(0);
    w.u32(1); w.fs({0.5f, 0.5f, 0.5f}); // colours
    w.fs({0, 0, 0});        // colour vary
    w.str("stinkSteam");
    w.u8(1); w.u8(1); w.u8(0); w.u8(0); // tiles, alignment, draw (decal)
    w.fs({0, 1}); w.u8(0); w.u8(0);    // layer, frame speed, start, count
    w.fs({0, 0, 1}); w.fs({0, 0});      // force, wind, ?
    w.fs({0, 0, 0}); w.fs({0.5f, 0});   // second group: drag 0.5
    w.f32(0);                           // screw
    w.u32(0);                           // wiggles
    w.u8(0); w.u8(0); w.u8(0); w.u8(0); // bloom
    w.u32(1);                           // one collider (base-game layout)
    w.u32(1); w.str("plane"); w.fs({0, 0, 0, 0, 0, 0, 0, 0, 0}); w.fs({0, 0, 0, 0}); w.str(""); w.str("");
    w.str("");
    w.fs({1, 0, 0, 0, 0, -1e9f, 0, 1}); // terrain
    w.fs({0, 0});
    w.fs({0.1f, 0.1f}); w.fs({0, 0}); w.fs({0, 0}); // random walk
    w.fs({0, 0, 0}); w.curve({}); w.f32(0);          // attractor (version < 8)
}

static std::vector<uint8_t> buildLibrary() {
    Writer w;
    w.u16(0);
    w.u32(1);
    writeParticles(w);
    for (int i = 0; i < 12; ++i) // meta, decals, brushes, scrubbers, sequences, sounds,
        w.u32(0);                // cameras, game, models, screens, waters, lights
    w.u16(3);                    // visual effects version
    w.u32(1);
    w.u32(0); w.u16(0); w.u32(0); w.u32(0); w.u32(0); w.fs({0, 0});
    w.u32(1); // one component
    w.str("psmoke");
    w.u8(1);  // particles
    w.u32(0);
    w.u8(4); w.f32(1); w.fs({1, 0, 0, 0, 1, 0, 0, 0, 1}); w.fs({0.5f, 0, 0});
    w.u8(1); w.u8(5);
    w.fs({1, 1, 1, 1, 1, 1});
    w.u16(0); w.u16(0); w.f32(1); w.u32(0);
    w.str("Smoke"); w.u32(0);
    w.str("end"); w.u32(0);
    return w.b;
}

static void testParse() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    check(lib.particles.size() == 1, "one particle block");
    check(lib.visuals.size() == 1 && lib.visuals[0].components.size() == 1, "one effect, one component");
    check(lib.find("smoke") != nullptr && lib.find("SMOKE") != nullptr, "names are case-insensitive");
    check(lib.find("end") == nullptr, "the end marker is not an effect");
    const Particles& p = lib.particles[0];
    check(p.material == "stinkSteam", "particle texture");
    check(p.rateCurve.size() == 1 && p.rateCurve[0] == 10, "rate curve");
    check(p.force[2] == 1 && p.drag == 0.5f, "force and drag");
    check(p.colliderName == "plane", "collider without the optional byte");
    check(lib.visuals[0].components[0].offset[0] == 0.5f, "component offset");
}

static void testSimulation() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    EffectSystem sys(lib, 7);
    check(sys.start("nothing", {}) == -1, "unknown effect");
    Placement where;
    where.position = {10, 20, 0};
    const int h = sys.start("Smoke", where);
    check(h >= 0, "effect starts");
    std::vector<ParticleQuad> q;
    sys.collect(q);
    check(!q.empty(), "pre-roll leaves particles");
    for (int i = 0; i < 30; ++i)
        sys.update(1.0f / 30);
    q.clear();
    sys.collect(q);
    // 10 a second living 2 s: about 20 alive.
    check(q.size() >= 15 && q.size() <= 25, "steady particle count");
    bool rising = true, placed = true;
    for (const auto& p : q) {
        rising = rising && p.position[2] > 0;
        placed = placed && std::fabs(p.position[0] - 10.5f) < 2 && std::fabs(p.position[1] - 20) < 2;
    }
    check(rising, "particles rise");
    check(placed, "particles start at the component offset from the effect");
    sys.stop(h);
    for (int i = 0; i < 90; ++i)
        sys.update(1.0f / 30);
    check(!sys.alive(h), "a stopped effect ends when its particles die");
}

// Flag bit 1 (MaintainParticles, BV 0xeb822c): the rate is the number kept
// alive, not particles per second (the plumbob's glow: one per component).
static void testMaintainedCount() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    lib.particles[0].flags |= 0x2;
    lib.particles[0].alphaCurve = {1}; // every one drawn, the newborn too
    lib.particles[0].sizeCurve = {1};
    EffectSystem sys(lib, 7);
    const int h = sys.start("Smoke", {});
    bool exact = true;
    for (int i = 0; i < 150; ++i) {
        sys.update(1.0f / 30);
        std::vector<ParticleQuad> q;
        sys.collect(q);
        exact = exact && q.size() == 10;
    }
    check(exact, "a maintained component keeps exactly its rate's count alive");
    sys.stop(h);
    for (int i = 0; i < 90; ++i)
        sys.update(1.0f / 30);
    check(!sys.alive(h), "a stopped maintained effect ends");
}

// A hand-built library: "tank" holds a fish chain (swimming / feeding /
// dead meta particles carrying "fishmodel" / "deadmodel"), "roach" a chain
// of two steps that follow one another as their lives end.
static EffectsLibrary metaLibrary() {
    EffectsLibrary lib;
    Model fish, dead;
    fish.model = "fishParticle1_cres";
    dead.model = "fishParticle1Dead_cres";
    lib.models = {fish, dead};
    auto visual = [&](const std::string& name, std::vector<Component> comps) {
        VisualEffect v;
        v.components = std::move(comps);
        lib.names[name] = uint32_t(lib.visuals.size());
        lib.visuals.push_back(std::move(v));
    };
    auto comp = [](Block type, int index) {
        Component c;
        c.type = type;
        c.index = index;
        return c;
    };
    visual("fishmodel", {comp(Block::Model, 0)});
    visual("deadmodel", {comp(Block::Model, 1)});
    MetaParticles swim;
    swim.flags = 0x01000022ull; // random walk, collision, maintain
    swim.life = {60000, 60000};
    swim.rateCurve = {1};
    swim.rateCurveTime = 1;
    swim.emitVolume = {{-0.4f, -0.2f, -0.15f}, {0.4f, 0.2f, 0.15f}};
    swim.baseEffect = "fishmodel";
    swim.alignment = 2;
    swim.walks[0].delay = {-0.25f, 0.75f};
    swim.walks[0].strength = {0.0625f, 0.1875f};
    swim.walks[0].turn[0] = 0.02f, swim.walks[0].turn[2] = 0.1f;
    MetaParticles feed = swim;
    feed.flags = 0x19000022ull;
    MetaParticles die = swim;
    die.flags = 0x19000021ull;
    die.life = {2, 2};
    die.baseEffect = "deadmodel";
    lib.metaParticles = {swim, feed, die};
    visual("tank", {comp(Block::MetaParticles, 0), comp(Block::MetaParticles, 1), comp(Block::MetaParticles, 2)});
    MetaParticles stand = swim, run = swim;
    stand.flags = 0x01000021ull;
    stand.life = {0.5f, 0.5f};
    run.flags = 0x19000021ull;
    run.life = {0.5f, 0.5f};
    run.baseEffect = "deadmodel";
    lib.metaParticles.push_back(stand);
    lib.metaParticles.push_back(run);
    visual("roach", {comp(Block::MetaParticles, 3), comp(Block::MetaParticles, 4)});
    return lib;
}

static void testMetaParticles() {
    const EffectsLibrary lib = metaLibrary();
    EffectSystem sys(lib, 3);
    Placement where;
    where.position = {5, 5, 1};
    const int h = sys.start("tank", where);
    std::vector<EffectModel> m;
    sys.collectModels(m);
    check(m.size() == 1 && *m[0].model == "fishParticle1_cres", "one swimming fish (the rate is the count kept)");
    const V3 first = m.empty() ? V3{} : m[0].where.position;
    bool inside = true;
    for (int i = 0; i < 300; ++i) {
        sys.update(0.1f);
        m.clear();
        sys.collectModels(m);
        for (const auto& e : m)
            inside = inside && std::fabs(e.where.position[0] - 5) < 0.45f && std::fabs(e.where.position[1] - 5) < 0.25f &&
                     std::fabs(e.where.position[2] - 1) < 0.2f;
    }
    check(m.size() == 1, "still one fish after 30 s");
    check(inside, "the fish stays in the tank's volume");
    const V3 later = m.empty() ? V3{} : m[0].where.position;
    check(std::fabs(later[0] - first[0]) + std::fabs(later[1] - first[1]) > 1e-3f, "the fish swims");
    if (!m.empty()) {
        // Facing its motion: the model's +X is horizontal-ish, not the identity forever.
        const auto& r = m[0].where.rotation;
        check(std::fabs(r[0] * r[0] + r[3] * r[3] + r[6] * r[6] - 1) < 1e-3f, "the facing is a rotation");
    }
    sys.setState(h, 2);
    sys.update(0.1f);
    m.clear();
    sys.collectModels(m);
    check(m.size() == 1 && *m[0].model == "fishParticle1Dead_cres", "state 2: the dead fish");
    for (int i = 0; i < 30; ++i)
        sys.update(0.1f);
    m.clear();
    sys.collectModels(m);
    check(m.empty(), "the dead fish is gone when its life ends, and none is born out of state 0");
    sys.setState(h, 0);
    sys.update(0.1f);
    m.clear();
    sys.collectModels(m);
    check(m.size() == 1 && *m[0].model == "fishParticle1_cres", "back in state 0 a fish is born again");

    const int r = sys.start("roach", where);
    m.clear();
    sys.collectModels(m);
    size_t standing = 0;
    for (const auto& e : m)
        standing += *e.model == "fishParticle1_cres";
    sys.update(0.3f);
    sys.update(0.3f);
    m.clear();
    sys.collectModels(m);
    size_t next = 0;
    for (const auto& e : m)
        next += *e.model == "fishParticle1Dead_cres";
    check(standing == 2 && next == 1, "a chain's particle goes to the next step when its life there ends");
    (void)r;
}

static void testCurves() {
    check(sampleCurve({}, 0.5f, 3) == 3, "empty curve falls back");
    check(sampleCurve({1, 3}, 0.5f) == 2, "linear interpolation");
    check(sampleCurve({1, 3}, 2) == 3, "clamped");
    check(sampleColors({{0, 0, 0}, {1, 1, 1}}, 0.25f)[1] == 0.25f, "colour interpolation");
}

int main() {
    try {
        testParse();
        testSimulation();
        testMaintainedCount();
        testMetaParticles();
        testCurves();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception %s\n", e.what());
        ++failures;
    }
    if (failures)
        return 1;
    std::printf("effects: ok\n");
    return 0;
}
