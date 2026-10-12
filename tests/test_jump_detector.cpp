// JumpDetector (app/jump_detector.hpp): drawn jumps the VM gave no reason
// for, back-and-forth jumps, repeated VM snaps.
#include "app/jump_detector.hpp"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using os2::JumpDetector;

static JumpDetector::Sample at(double t, float dx, float dy, float vx, float vy, bool walking = false, int container = 0) {
    JumpDetector::Sample s;
    s.id = 7, s.time = t, s.tick = uint64_t(t * 30);
    s.drawn[0] = dx, s.drawn[1] = dy, s.drawn[2] = 0.75f;
    s.vm[0] = vx, s.vm[1] = vy, s.level = 2;
    s.walking = walking, s.container = container;
    return s;
}

int main() {
    {
        // A walk: small steps, nothing flagged.
        JumpDetector d;
        for (int i = 0; i < 60; ++i)
            d.add(at(i / 30.0, 10 + i * 0.07f, 10, 10 + i * 0.07f, 10, true));
        check(d.events().empty(), "a walk raises nothing");
    }
    {
        // A VM Snap moves the VM position too: not a drawn bug.
        JumpDetector d;
        d.add(at(0, 10, 10, 10, 10));
        d.add(at(1 / 30.0, 13, 10, 13, 10));
        check(d.drawnJumps() == 0, "a jump the VM made (Snap) is explained");
    }
    {
        // Into a seat: the drawn place moves to the slot bone with the container.
        JumpDetector d;
        d.add(at(0, 10, 10, 10, 10));
        d.add(at(1 / 30.0, 11.2f, 10, 10, 10, false, 643));
        check(d.drawnJumps() == 0, "a seat in or out is explained");
    }
    {
        // Drawn jumps with the VM standing still, there and back: flagged.
        JumpDetector d;
        d.add(at(0, 10, 10, 10, 10));
        d.add(at(1 / 30.0, 12, 10, 10, 10));
        d.add(at(2 / 30.0, 10, 10, 10, 10));
        check(d.drawnJumps() == 2, "two unexplained drawn jumps");
        check(d.backAndForth() == 1, "the second goes back");
    }
    {
        // The VM snapping every few frames (a route failing and snapping).
        JumpDetector d;
        for (int i = 0; i < 10; ++i) {
            const float x = i % 2 ? 12.f : 10.f;
            d.add(at(i * 0.5, x, 10, x, 10));
        }
        check(d.repeatedSnaps() == 1, "repeated VM snaps flagged once");
        check(d.drawnJumps() == 0, "but no drawn jump");
    }
    {
        // Out of the world (at work) and back: no jump.
        JumpDetector d;
        d.add(at(0, 10, 10, 10, 10));
        auto away = at(1, 0, 0, -1, -1);
        away.inWorld = false;
        d.add(away);
        d.add(at(2, 30, 30, 30, 30));
        check(d.events().empty(), "back in the world is not a jump");
    }
    {
        // A passenger moves with the car the VM drives.
        JumpDetector d;
        for (int i = 0; i < 5; ++i) {
            auto x = at(i / 30.0, 36, 7 + 2.1f * i, 10, 10, false, 153);
            x.holder[0] = 36, x.holder[1] = 7 + 2.1f * i;
            x.seat[0] = 36, x.seat[1] = 7 + 2.1f * i;
            d.add(x);
        }
        check(d.drawnJumps() == 0, "a passenger goes with the car");
    }
    if (failures)
        return EXIT_FAILURE;
    std::printf("jump detector: ok\n");
    return 0;
}
