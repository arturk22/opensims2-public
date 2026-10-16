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
    // 10 a second living 2 s: the pre-roll's 0.5 s (generation capped at
    // pre-roll - 0.01, GenerateNewParticles) and 1 s more: about 15 alive.
    if (q.size() < 13 || q.size() > 17)
        std::fprintf(stderr, "particles: %zu\n", q.size());
    check(q.size() >= 13 && q.size() <= 17, "steady particle count");
    bool rising = true, placed = true;
    for (const auto& p : q) {
        rising = rising && p.position[2] > 0;
        placed = placed && std::fabs(p.position[0] - 10.5f) < 2 && std::fabs(p.position[1] - 20) < 2;
    }
    check(rising, "particles rise");
    check(placed, "particles start at the component offset from the effect");
    // One emitter, one drawing group, sorted by the centre of its particles' box.
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    bool oneGroup = true;
    for (const auto& p : q) {
        oneGroup = oneGroup && p.group == q[0].group && p.layerKey == 7;
        for (int k = 0; k < 3; ++k)
            lo[k] = std::min(lo[k], p.position[k]), hi[k] = std::max(hi[k], p.position[k]);
    }
    check(oneGroup, "an emitter's particles are one group, key 7");
    check(!q.empty() && std::fabs(q[0].groupCenter[2] - (lo[2] + hi[2]) / 2) < 1e-4f, "the group sorts by its box's centre");
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

// Where particles live (cTSVisualEffect::SetTransforms): in the world, so
// they stay when the effect moves, unless the effect is rigid. A hard start
// leaves bursts out; a soft one plays them, after the trigger's wait.
// A balloon whose picture cannot be found shows no icon, not the icon
// particle's own texture (an atlas of 16 icons squeezed into one).
static void testHiddenIcon() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    lib.particles[0].flags |= pflag::AcceptOverride;
    EffectSystem sys(lib, 3);
    const int h = sys.start("Smoke", {});
    std::vector<ParticleQuad> q;
    sys.collect(q);
    check(!q.empty(), "the icon particle shows its own texture by default");
    sys.setIcon(h, "thumbnail:1");
    q.clear();
    sys.collect(q);
    check(!q.empty() && *q[0].texture == "thumbnail:1", "an icon replaces the texture");
    sys.hideIcon(h);
    q.clear();
    sys.collect(q);
    check(q.empty(), "a hidden icon draws nothing");
}

static void testFramesAndStarts() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    lib.particles[0].force = {0, 0, 0};
    lib.particles[0].emitSpeed = {0, 0};
    lib.particles[0].flags = 0; // no random walk
    auto spread = [](EffectSystem& sys, float x) {
        std::vector<ParticleQuad> q;
        sys.collect(q);
        float far = 0;
        for (const auto& p : q)
            far = std::max(far, std::fabs(p.position[0] - x));
        return std::make_pair(q.size(), far);
    };
    for (bool rigid : {false, true}) {
        lib.visuals[0].flags = rigid ? vflag::Rigid : 0;
        EffectSystem sys(lib, 3);
        Placement where;
        const int h = sys.start("Smoke", where);
        for (int i = 0; i < 30; ++i)
            sys.update(1.0f / 30);
        where.position = {5, 0, 0};
        sys.move(h, where);
        sys.update(1.0f / 30);
        const auto [count, far] = spread(sys, 5.5f);
        check(count > 5, "particles alive after the move");
        if (rigid)
            check(far < 0.01f, "a rigid effect's particles move with it");
        else
            check(far > 4.0f, "a world effect's particles stay where they were born");
    }
    lib.visuals[0].flags = 0;
    lib.particles[0].rateCurveCycles = 1; // a burst
    lib.particles[0].preRoll = 0;
    {
        EffectSystem sys(lib, 3);
        sys.start("Smoke", {}, true);
        sys.update(0.5f);
        check(spread(sys, 0.5f).first == 0, "a hard start leaves a burst out");
    }
    lib.particles[0].rateTrigger = {1, 1};
    {
        EffectSystem sys(lib, 3);
        sys.start("Smoke", {}, false);
        sys.update(0.5f);
        check(spread(sys, 0.5f).first == 0, "a soft start waits the trigger first");
        for (int i = 0; i < 30; ++i)
            sys.update(1.0f / 30);
        check(spread(sys, 0.5f).first > 0, "then the burst plays");
    }
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
    auto comp = [](Block type, int index, uint32_t flags = 0) {
        Component c;
        c.type = type;
        c.index = index;
        c.flags = flags;
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
    swim.walks[0].turn[0] = 0.02f, swim.walks[0].turn[1] = 0.2f, swim.walks[0].turn[2] = 0.1f;
    // The tank's six walls: plane colliders (corner, two edges, the normal
    // e1 x e2 inwards), bounce 0.7, radius 0.08.
    auto plane = [](V3 o, V3 e1, V3 e2) {
        ColliderInfo c;
        c.name = "plane";
        c.v = {o, e1, e2};
        c.f = {0.7f, 0, 0.08f, 0};
        return c;
    };
    swim.colliders = {plane({-0.4f, -0.2f, -0.15f}, {0.8f, 0, 0}, {0, 0.4f, 0}),
                      plane({-0.4f, -0.2f, 0.15f}, {0, 0.4f, 0}, {0.8f, 0, 0}),
                      plane({-0.4f, -0.2f, -0.15f}, {0, 0.4f, 0}, {0, 0, 0.3f}),
                      plane({0.4f, -0.2f, -0.15f}, {0, 0, 0.3f}, {0, 0.4f, 0}),
                      plane({-0.4f, -0.2f, -0.15f}, {0, 0, 0.3f}, {0.8f, 0, 0}),
                      plane({-0.4f, 0.2f, -0.15f}, {0.8f, 0, 0}, {0, 0, 0.3f})};
    MetaParticles feed = swim;
    feed.flags = 0x19000022ull;
    MetaParticles die = swim;
    die.flags = 0x19000021ull;
    die.life = {2, 2};
    die.rateCurveCycles = 1; // (the game's: one burst, of the moment)
    die.rateCurveTime = 0.001f;
    die.baseEffect = "deadmodel";
    lib.metaParticles = {swim, feed, die};
    // A state chain (component flag 0x8): swimming, feeding, dead.
    visual("tank", {comp(Block::MetaParticles, 0, 0x8), comp(Block::MetaParticles, 1, 0x8), comp(Block::MetaParticles, 2, 0x8)});
    MetaParticles stand = swim, run = swim;
    stand.flags = 0x01000021ull;
    stand.life = {0.5f, 0.5f};
    run.flags = 0x19000021ull;
    run.life = {0.5f, 0.5f};
    run.baseEffect = "deadmodel";
    lib.metaParticles.push_back(stand);
    lib.metaParticles.push_back(run);
    // A chain (component flag 0x2): each one's particles go to the next.
    visual("roach", {comp(Block::MetaParticles, 3, 0x2), comp(Block::MetaParticles, 4, 0x2)});
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
    check(m.size() == 1 && *m[0].model == "fishParticle1_cres", "one swimming fish (maintain: the rate is the count kept)");
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
    check(inside, "the fish stays in the tank (its plane colliders)");
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
    // State 2 switches the effect's description (SwitchDescription): the
    // swimming fish keeps its model and life under the dead one's physics,
    // and the dead description's one burst brings a dead fish.
    auto count = [&](const char* model) {
        size_t n = 0;
        for (const auto& e : m)
            n += *e.model == model;
        return n;
    };
    check(count("fishParticle1_cres") == 1 && count("fishParticle1Dead_cres") == 1,
          "state 2: the swimming fish stays, the burst brings a dead one");
    for (int i = 0; i < 30; ++i)
        sys.update(0.1f);
    m.clear();
    sys.collectModels(m);
    check(count("fishParticle1Dead_cres") == 0 && count("fishParticle1_cres") == 1,
          "the dead fish is gone when its life ends; the burst is not repeated");
    sys.setState(h, 0);
    for (int i = 0; i < 3; ++i)
        sys.update(0.1f);
    m.clear();
    sys.collectModels(m);
    check(m.size() == 1 && *m[0].model == "fishParticle1_cres", "back in state 0: still the one fish (maintain 1)");

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

// ApplyTractorForce (BV 0xf5fef0): a meta particle with the tractor and no
// path is steered to its next path point, there at the point's time.
static void testTractor() {
    EffectsLibrary lib = metaLibrary();
    MetaParticles& m = lib.metaParticles[3]; // the roach's first step
    m.flags = mflag::Tractor | mflag::Maintain;
    m.colliders.clear();
    m.life = {5, 5};
    m.emitVolume = {{0, 0, 0}, {0, 0, 0}};
    m.emitSpeed = {0, 0};
    m.path = {{{1, 0, 0}, {0, 0, 0}, 1}, {{1, 2, 0}, {0, 0, 0}, 3}};
    lib.visuals[lib.names["roach"]].components.resize(1);
    lib.visuals[lib.names["roach"]].components[0].flags = 0;
    EffectSystem sys(lib, 3);
    sys.start("roach", {});
    auto at = [&]() {
        std::vector<EffectModel> out;
        sys.collectModels(out);
        return out.empty() ? V3{-9, -9, -9} : out[0].where.position;
    };
    for (int i = 0; i < 30; ++i)
        sys.update(1.0f / 30);
    const V3 a = at();
    check(std::fabs(a[0] - 1) < 0.02f && std::fabs(a[1]) < 0.02f, "tractor: at its first point at its time");
    for (int i = 0; i < 60; ++i)
        sys.update(1.0f / 30);
    const V3 b = at();
    check(std::fabs(b[0] - 1) < 0.02f && std::fabs(b[1] - 2) < 0.02f, "tractor: then at the second");
}

static void testCurves() {
    check(sampleCurve({}, 0.5f, 3) == 3, "empty curve falls back");
    check(sampleCurve({1, 3}, 0.5f) == 2, "linear interpolation");
    check(sampleCurve({1, 3}, 2) == 3, "clamped");
    check(sampleColors({{0, 0, 0}, {1, 1, 1}}, 0.25f)[1] == 0.25f, "colour interpolation");
}

// The LOD (cTSEffectsManager::SetLOD(float) from the camera's zoom level + 1,
// UpdateVisualEffect, SetCurveCoefficients): a component shows only in its
// range and its scales go from its begin ones (far) to its end ones (near).
static void testLod() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    auto& c = lib.visuals[0].components[0];
    lib.particles[0].flags = 0;
    lib.particles[0].sizeVary = 0;
    lib.particles[0].sizeCurve = {1};
    lib.particles[0].alphaCurve = {0.25f};
    c.lodBegin = 1, c.lodEnd = 5;
    c.emitScale = {0.25f, 1}, c.sizeScale = {2, 1}, c.alphaScale = {2, 1};
    auto run = [&](float lod) {
        EffectSystem sys(lib, 5);
        sys.setLod(lod);
        sys.start("Smoke", {});
        for (int i = 0; i < 60; ++i)
            sys.update(1.0f / 30);
        std::vector<ParticleQuad> q;
        sys.collect(q);
        return q;
    };
    const auto near = run(EffectSystem::kMaxLod), far = run(1);
    check(!near.empty() && !far.empty(), "particles at both distances");
    check(far.size() * 3 < near.size(), "far: a quarter of the emission");
    check(!far.empty() && std::fabs(far[0].width - 2 * near[0].width) < 1e-3f, "far: twice the size");
    check(!far.empty() && std::fabs(far[0].color[3] - 0.5f) < 1e-3f && std::fabs(near[0].color[3] - 0.25f) < 2e-3f,
          "far: twice the alpha");
    // LOD 3 of a 1-5 range: halfway between the two.
    const auto mid = run(3);
    check(!mid.empty() && std::fabs(mid[0].width - 1.5f * near[0].width) < 1e-3f, "LOD 3: halfway");
    check(!near[0].lit, "no texture -light: unlit");
    lib.particles[0].flags = pflag::Light;
    const auto lit = run(EffectSystem::kMaxLod);
    check(!lit.empty() && lit[0].lit, "texture -light (bit 23): lit");
    lib.particles[0].flags = 0;
    // A component of the near levels only (2-5) is not there from the farthest zoom.
    c.lodBegin = 2;
    check(run(1).empty() && !run(2.5f).empty(), "a 2-5 component shows from LOD 2");
    // Zooming out stops it (its particles finish), zooming in starts it again.
    {
        EffectSystem sys(lib, 5);
        const int h = sys.start("Smoke", {});
        for (int i = 0; i < 30; ++i)
            sys.update(1.0f / 30);
        sys.setLod(1);
        for (int i = 0; i < 90; ++i)
            sys.update(1.0f / 30);
        std::vector<ParticleQuad> q;
        sys.collect(q);
        check(q.empty() && sys.alive(h), "out of its range: no particles, the effect still there");
        sys.setLod(4);
        for (int i = 0; i < 30; ++i)
            sys.update(1.0f / 30);
        q.clear();
        sys.collect(q);
        check(!q.empty(), "back in its range it emits again");
    }
}

// Colliders (ApplyColliderForces BV 0xeb3064, the response FUN_00ebdb5c):
// the floors one against the host's lot, a plane one, a death effect.
struct FlatFloor : ColliderWorld {
    int levelBelow(const V3&) const override { return 0; }
    bool floor(int level, float, float, float& z) const override {
        z = 0;
        return level == 0;
    }
};

static void testColliders() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    Particles& p = lib.particles[0];
    p.flags = pflag::Collide;
    p.force = {0, 0, -10};
    p.drag = 0;
    p.emitSpeed = {0, 0};
    p.emitVolume = {{0, 0, 1}, {0, 0, 1}};
    p.life = {3, 3};
    p.preRoll = 0;
    p.sizeCurve = {0.1f};
    p.sizeVary = 0;
    ColliderInfo floors;
    floors.name = "floors";
    floors.f = {0, 1, 1, 0}; // no bounce, keep sliding, radius x 1, no death
    p.colliders = {floors};
    auto lowest = [&](EffectSystem& sys) {
        std::vector<ParticleQuad> q;
        sys.collect(q);
        float z = 1e9f;
        for (const auto& x : q)
            z = std::min(z, x.position[2]);
        return std::make_pair(q.size(), z);
    };
    FlatFloor world;
    {
        EffectSystem sys(lib, 3);
        sys.start("Smoke", {});
        for (int i = 0; i < 60; ++i)
            sys.update(1.0f / 30);
        check(lowest(sys).second < -1, "no lot: the particles fall through");
    }
    {
        EffectSystem sys(lib, 3);
        sys.setWorld(&world);
        sys.start("Smoke", {});
        for (int i = 0; i < 60; ++i)
            sys.update(1.0f / 30);
        const auto [n, z] = lowest(sys);
        check(n > 5 && z > 0.04f && z < 0.06f, "floors: they lie half their size over the floor");
    }
    // A plane collider: a 2 x 2 rectangle at z 0.5, its normal up.
    ColliderInfo plane;
    plane.name = "plane";
    plane.v = {V3{-1, -1, 0.5f}, V3{2, 0, 0}, V3{0, 2, 0}};
    plane.f = {0, 1, 1, 0};
    p.colliders = {plane};
    {
        EffectSystem sys(lib, 3);
        sys.start("Smoke", {});
        for (int i = 0; i < 60; ++i)
            sys.update(1.0f / 30);
        const auto [n, z] = lowest(sys);
        check(n > 5 && z > 0.55f && z < 0.65f, "plane: they stop their size over it");
    }
    // A death chance of 1 with a death effect: the particle dies there and the effect starts.
    floors.f = {0, 1, 1, 1};
    floors.effect2 = "Smoke";
    p.colliders = {floors};
    {
        EffectSystem sys(lib, 3);
        sys.setWorld(&world);
        sys.start("Smoke", {});
        for (int i = 0; i < 20; ++i)
            sys.update(1.0f / 30);
        check(sys.instanceCount() > 1, "a death effect starts where the particle died");
    }
}

// cTSSequenceEffect (BV 0xf5ac84): items one after another, the one before
// stopped (here hard: -hardStop), looping with -loop; an item of time < 0
// waits for its effect to end by itself.
static void testSequence() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    lib.particles[0].preRoll = 0;
    Sequence seq;
    seq.items = {{{1, 1}, "Smoke"}, {{1, 1}, ""}};
    seq.flags = sflag::Loop | sflag::HardStop;
    lib.sequences = {seq};
    VisualEffect v;
    Component c;
    c.type = Block::Sequence;
    c.index = 0;
    v.components = {c};
    lib.names["seq"] = uint32_t(lib.visuals.size());
    lib.visuals.push_back(v);
    auto count = [](EffectSystem& sys) {
        std::vector<ParticleQuad> q;
        sys.collect(q);
        return q.size();
    };
    EffectSystem sys(lib, 3);
    const int h = sys.start("seq", {});
    auto run = [&](float s) {
        for (float t = 0; t < s; t += 1.0f / 30)
            sys.update(1.0f / 30);
    };
    run(0.5f);
    check(count(sys) > 0, "sequence: the first item plays");
    run(1.0f);
    check(count(sys) == 0 && sys.alive(h), "sequence: the pause (the item before stopped hard)");
    run(1.0f);
    check(count(sys) > 0, "sequence: -loop plays the first item again");
    // An item of time -1 waits until its effect ends: a burst of one cycle.
    lib.particles[0].rateCurveCycles = 1;
    lib.particles[0].life = {0.5f, 0.5f};
    lib.sequences[0].items = {{{-1, -1}, "Smoke"}, {{1, 1}, ""}};
    lib.sequences[0].flags = 0;
    EffectSystem once(lib, 3);
    const int h2 = once.start("seq", {});
    for (int i = 0; i < 30; ++i)
        once.update(1.0f / 30);
    check(once.alive(h2), "sequence: waiting for the burst to end");
    for (int i = 0; i < 90; ++i)
        once.update(1.0f / 30);
    check(!once.alive(h2), "sequence: no -loop, over after its last item");
}

// screw and wiggle move where a particle is drawn (the vertex fill, BV
// 0xeb8dbc), not where it is: about the emitter's vertical by its height x
// screw radians; stretch is handed to the layer.
static void testScrew() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    Particles& p = lib.particles[0];
    p.flags = 0;
    p.force = {0, 0, 0};
    p.drag = 0;
    p.emitSpeed = {1, 1};
    p.emitDirection = {{0, 0, 1}, {0, 0, 1}};
    p.emitVolume = {{0.5f, 0, 0}, {0.5f, 0, 0}};
    lib.visuals[0].components[0].offset = {0, 0, 0};
    auto first = [&]() {
        EffectSystem sys(lib, 3);
        sys.start("Smoke", {});
        sys.update(0.5f);
        std::vector<ParticleQuad> q;
        sys.collect(q);
        // The oldest particle (born first, highest).
        ParticleQuad top = q.empty() ? ParticleQuad{} : q[0];
        for (const auto& x : q)
            if (x.position[2] > top.position[2])
                top = x;
        return top;
    };
    const ParticleQuad straight = first();
    p.screw = 1;
    p.stretch = 2;
    const ParticleQuad turned = first();
    const float h = straight.position[2], a = std::atan2(turned.position[1], turned.position[0]);
    check(std::fabs(straight.position[1]) < 1e-4f, "no screw: straight up");
    check(std::fabs(a - h) < 0.02f && std::fabs(turned.position[2] - h) < 1e-4f, "screw 1: turned by its height in radians");
    check(turned.stretch == 2, "the stretch goes to the layer");
}

// cTSVisualEffect::SetTransforms (BV 0xeb1760): a view-relative effect turns
// about Z by the camera's view direction (its X away from the camera), a
// camera-facing one turns its X towards the camera.
static void testFacing() {
    const auto data = buildLibrary();
    EffectsLibrary lib = EffectsLibrary::parse(data);
    Particles& p = lib.particles[0];
    p.flags = 0;
    p.force = {0, 0, 0};
    p.emitSpeed = {0, 0};
    p.preRoll = 0;
    auto bornAt = [&](uint32_t flags) {
        lib.visuals[0].flags = flags;
        EffectSystem sys(lib, 3);
        // The camera at +X looking back along -X.
        sys.setView({10, 0, 0}, {0, 0, 1}, {-1, 0, 0});
        sys.start("Smoke", {});
        sys.update(0.2f);
        std::vector<ParticleQuad> q;
        sys.collect(q);
        return q.empty() ? V3{} : q[0].position;
    };
    // The component sits 0.5 along the effect's X.
    check(std::fabs(bornAt(vflag::ViewRelative)[0] + 0.5f) < 1e-3f, "view-relative: X along the view, away from the camera");
    check(std::fabs(bornAt(vflag::CameraFacing)[0] - 0.5f) < 1e-3f, "camera-facing: X towards the camera");
}

int main() {
    try {
        testTractor();
        testFacing();
        testScrew();
        testSequence();
        testParse();
        testLod();
        testColliders();
        testSimulation();
        testMaintainedCount();
        testFramesAndStarts();
        testHiddenIcon();
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
