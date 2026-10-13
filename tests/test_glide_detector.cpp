// GlideDetector (app/glide_detector.hpp): Sims whose drawn pelvis moves on
// while their legs hold still.
#include "app/glide_detector.hpp"

#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using os2::GlideDetector;

// Frame i at 30 a second, the pelvis at x, the feet swinging by `swing`.
static GlideDetector::Sample at(int i, float x, float swing, bool inWorld = true) {
    GlideDetector::Sample s;
    s.id = 3, s.time = i / 30.0, s.tick = uint64_t(i);
    s.pelvis[0] = x, s.pelvis[1] = 10;
    const float ph = std::sin(float(i) * 0.4f) * swing;
    s.lfoot[0] = -0.1f, s.lfoot[1] = ph, s.lfoot[2] = -0.9f;
    s.rfoot[0] = 0.1f, s.rfoot[1] = -ph, s.rfoot[2] = -0.9f;
    s.inWorld = inWorld;
    return s;
}

int main() {
    {
        GlideDetector d; // a walk: the feet swing
        for (int i = 0; i < 120; ++i)
            d.add(at(i, 10 + i * 0.04f, 0.3f));
        check(d.episodes() == 0, "a walk is no glide");
    }
    {
        GlideDetector d; // standing still, the feet still
        for (int i = 0; i < 120; ++i)
            d.add(at(i, 10, 0));
        check(d.episodes() == 0, "standing is no glide");
    }
    {
        GlideDetector d; // moving with the feet still: one episode
        for (int i = 0; i < 120; ++i)
            d.add(at(i, 10 + i * 0.04f, 0.005f));
        check(d.episodes() == 1, "a glide is one episode");
        check(d.seconds() > 2.5 && d.seconds() < 3.5, "its seconds");
    }
    {
        GlideDetector d; // a Snap far away with still feet: no glide
        for (int i = 0; i < 60; ++i)
            d.add(at(i, i < 30 ? 10.0f : 14.0f, 0));
        check(d.episodes() == 0, "a jump is no glide");
    }
    {
        GlideDetector d; // carried (out of the world): nothing
        for (int i = 0; i < 120; ++i)
            d.add(at(i, 10 + i * 0.04f, 0, false));
        check(d.episodes() == 0, "out of the world is not checked");
    }
    if (failures)
        return 1;
    std::printf("glide_detector: ok\n");
    return 0;
}
