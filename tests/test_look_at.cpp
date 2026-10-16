// The head's look at (app/look_at.cpp) as cActiveAnimLookAt::AnimUpdate does
// it (docs/anim-mac.md §12.3), on a hand-built skeleton: the turn, the
// hold, the limits, the return and the spine's twist.

#include "app/look_at.hpp"
#include "formats/sim_actor.hpp"

#include <cmath>
#include <cstdio>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

namespace {

constexpr float kDeg = float(M_PI / 180);

// auskel's spine to head_grip, upright at the origin facing +Y: the bones
// along their +Y, root_trans turned so that +Y is up and +Z the front.
SkinnedModel skeleton() {
    SkinnedModel m;
    Skeleton& s = m.skeleton;
    s.bones.push_back({"root_trans", -1, Xform{Quat{0, 0.70710678f, 0.70710678f, 0}, {0, 0, 1.0f}}, 0, 0});
    s.bones.push_back({"root_rot", 0, Xform{}, 1, 1});
    s.bones.push_back({"spine0", 1, Xform{{}, {0, 0.12f, 0}}, 2, 2});
    s.bones.push_back({"spine1", 2, Xform{{}, {0, 0.16f, 0}}, 3, 3});
    s.bones.push_back({"spine2", 3, Xform{{}, {0, 0.16f, 0}}, 4, 4});
    s.bones.push_back({"neck", 4, Xform{{}, {0, 0.16f, 0}}, 5, 5});
    s.bones.push_back({"head", 5, Xform{{}, {0, 0.075f, 0}}, 6, 6});
    s.bones.push_back({"head_grip", 6, Xform{{}, {0, 0.08f, 0}}, -1, 7});
    return m;
}

struct Rig {
    SimActor actor{skeleton()};
    HeadLook look{actor};
    double now = 0;
    std::vector<Xform> local;

    // Plays `seconds` at 30 frames a second; the animation holds the rest pose.
    void run(double seconds, const std::optional<HeadLook::Request>& r, const std::optional<Vec3>& target,
             float stopAway = 90) {
        for (int i = 0; i < int(seconds * 30 + 0.5); ++i) {
            now += 1.0 / 30;
            look.setRequest(r, target, stopAway);
            local = actor.skin.skeleton.restPose();
            look.apply(local, now);
        }
    }
    // The head's face (+Z) from head_grip against the way to `p` (cosine).
    float facing(const Vec3& p) const {
        const auto w = actor.skin.skeleton.world(local);
        const Vec3 f = w[6].rotate({0, 0, 1}), g = w[7].t;
        const Vec3 d{p[0] - g[0], p[1] - g[1], p[2] - g[2]};
        return (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    float headYaw() const { // the face's heading from +Y, + to the left
        const auto w = actor.skin.skeleton.world(local);
        const Vec3 f = w[6].rotate({0, 0, 1});
        return std::atan2(-f[0], f[1]);
    }
};

HeadLook::Request request(uint32_t serial, int holdMs = -1, bool spine = false, bool noEarlyExit = false) {
    return HeadLook::Request{serial, 90, 90, holdMs, spine, noEarlyExit};
}

} // namespace

int main() {
    const Vec3 left{-1.2f, 1.5f, 1.5f}; // ahead and to the left, a little lower
    {
        Rig r;
        r.run(0.1, request(1), left);
        check(r.look.state() == HeadLook::State::Turning, "a new Look At turns");
        const float early = std::fabs(r.headYaw());
        r.run(2, request(1), left);
        check(r.look.state() == HeadLook::State::Holding, "on the target it holds (no end)");
        check(r.facing(left) > 0.999f, "the head's +Z from head_grip meets the target");
        check(early < std::fabs(r.headYaw()) * 0.5f, "the turn starts slowly (0.3 of the rate)");
        const auto w = r.actor.skin.skeleton.world(r.local);
        check(std::fabs(w[5].rotate({0, 0, 1})[0]) < 1e-5f, "the neck does not turn (only the head)");
        check(std::fabs(r.look.twist()) < 1e-6f, "no twist without \"including Spine\"");
    }
    {
        // A target behind: the head stops at 70 degrees and, without "no
        // early exit", comes back and the look ends.
        Rig r;
        const Vec3 behind{-1.0f, -1.0f, 1.7f};
        r.run(1.5, request(1), behind);
        check(r.look.state() >= HeadLook::State::Returning || r.look.state() == HeadLook::State::Inactive,
              "a target past the limits is given up");
        r.run(3, request(1), behind);
        check(std::fabs(r.headYaw()) < 2 * kDeg, "back on the animation's head");
        Rig n;
        n.run(3, request(1, -1, false, true), behind);
        check(n.look.state() == HeadLook::State::Holding, "with \"no early exit\" it holds at the limit");
        check(std::fabs(std::fabs(n.headYaw()) - 70 * kDeg) < 3 * kDeg, "held at 70 degrees of yaw");
    }
    {
        // A duration: held that long from when the head gets there, then
        // back, then done; the weight fades out on the way back.
        Rig r;
        r.run(2, request(1, 500), left);
        check(r.look.state() >= HeadLook::State::HoldDone || r.look.state() == HeadLook::State::Inactive,
              "a 500 ms hold is over 2 s after the request");
        r.run(3, request(1, 500), left);
        check(r.look.state() == HeadLook::State::Inactive, "the look ends and the request goes");
        check(std::fabs(r.headYaw()) < 1e-3f, "the head is the animation's again");
        // The same Look At kept by the VM does not start again.
        r.run(1, request(1, 500), left);
        check(r.look.state() == HeadLook::State::Inactive, "a Look At plays once");
        r.run(0.5, request(2, 500), left);
        check(r.look.state() != HeadLook::State::Inactive, "a new Look At looks again");
    }
    {
        // "Including Spine": the spine twists towards the look's heading
        // less 10 degrees, its bones by 0.2, 0.08, 0.05 and 0.1 of it.
        Rig r;
        r.run(3, request(1, -1, true), left);
        check(r.look.twist() > 15 * kDeg, "the spine twists towards a target to the left");
        const auto w = r.actor.skin.skeleton.world(r.local);
        check(w[2].rotate({0, 0, 1})[0] < -1e-3f, "spine0 turns to the left");
        check(r.facing(left) > 0.999f, "the head still meets the target");
    }
    {
        // STOP: back at its rate; a target that near: back at once.
        Rig r;
        r.run(2, request(1), left);
        r.run(0.1, std::nullopt, left, 180);
        check(r.look.state() == HeadLook::State::Returning, "a STOP returns");
        r.run(2, std::nullopt, left, 180);
        check(r.look.state() == HeadLook::State::Inactive, "and ends");
        Rig n;
        n.run(0.2, request(1), Vec3{0, 0.25f, 1.75f});
        check(n.look.state() >= HeadLook::State::Returning || n.look.state() == HeadLook::State::Inactive,
              "a target within 0.3 is given up");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("look at tests passed\n");
    return failures ? 1 : 0;
}
