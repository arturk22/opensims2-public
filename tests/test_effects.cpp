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
    check(p.force[2] == 1 && p.force2a == 0.5f, "force and drag");
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
