#pragma once

// A Sim's head turning towards what the VM's Look At (0x6E) points it at
// (vm::Object::lookAt), on top of whatever animation plays, as the game's
// nGZSceneDBAnim::cActiveAnimLookAt does it (Mac BV; docs/anim-mac.md §12):
//
// - AnimUpdate (BV 0xd41e04) keeps a look rotation q in the frame of the
//   head as it would stand on the neck at rest, and turns it towards the
//   target's direction from head_grip, within the head's limits (pitch
//   -51 to +36 degrees, + down; yaw 70; roll 40: FUN_00c49a8c), with a
//   speed profile (a ramp from 0.3 of the rate, 25-300 ticks long, and a
//   slow down near the end), through the states 0x26AD turning, 0x26AE
//   holding (the duration counts down there), 0x26AF/0x26B0 returning to
//   the animation's head, 0x26B1/0x26B2 done; and the spine's twist
//   (with Look At's "including Spine"): the look's heading from the root
//   less 10 degrees, within 90.
// - Its IK (two chains of cIKStrategyLookAt): the head (head -> head_grip)
//   turned so that its +Z from head_grip meets the point at the target's
//   distance along q, upright by a pole straight up the reference frame
//   (lookAtIkRotation), mixed with the animation's head by the request's
//   weight, which fades in over half the turn and out over the return;
//   spine0, spine1, spine2 and neck turned about their Y by 0.2, 0.08, 0.05
//   and 0.1 of the weighed twist, on top of the animation (delta channels).
//
// Inferred: the root the twist is measured from (root_rot), that the rest
// head rotation is the skeleton's, the blends' linear shape, the order of
// the spine's shares, and that the look runs on the frame's animated pose.

#include "formats/xform.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace os2 {

struct SimActor;

class HeadLook {
public:
    // A Look At as the VM asked for it (vm/view.cpp).
    struct Request {
        uint32_t serial = 0;         // which Look At: a new one starts a new turn
        float towards = 0, away = 0; // degrees a second (0: 90, a test aid's)
        int32_t holdMs = -1;         // how long to hold on the target: -1 until the next Look At
        bool spine = false;          // byte 0 0x04 "including Spine"
        bool noEarlyExit = false;    // byte 0 0x02: hold at the limits a target past them
    };
    // cIChoreoRequestLookAt::tLookState.
    enum class State : int {
        Inactive = 0x26AC,
        Turning = 0x26AD,
        Holding = 0x26AE,
        HoldDone = 0x26AF,
        Returning = 0x26B0,
        Ended = 0x26B1,
        Stopped = 0x26B2,
    };

    explicit HeadLook(const SimActor& actor);

    // This frame's Look At and where its target is (world space). No
    // request (a STOP, or none yet): a look under way returns at
    // `stopAwayRate` degrees a second (the STOP's own, try_look_at); no
    // target (gone, out of the world): it returns at its own rate (ours).
    void setRequest(const std::optional<Request>& request, const std::optional<Vec3>& target, float stopAwayRate);
    // SimActor::adjust: the look's IK over the animated pose.
    void apply(std::vector<Xform>& local, double now);

    // For tests and debugging.
    State state() const { return state_; }
    const Quat& look() const { return q_; }
    float twist() const { return twist_; }
    float weight() const;
    bool hasTarget() const { return target_.has_value(); }

private:
    void setState(State s);
    float weightAt(int64_t elapsed) const;

    const SimActor& actor_;
    bool valid_ = false; // the skeleton has the bones the look needs
    int head_ = -1, grip_ = -1, neck_ = -1, rootRot_ = -1;
    std::vector<int> spine_; // spine0, spine1, spine2, neck
    Quat headRest_;          // the head's rest rotation on the neck (+0xec)

    std::optional<Request> pending_, current_;
    std::optional<uint32_t> seen_; // the last Look At started
    std::optional<Vec3> target_;
    float stopAway_ = 0;
    bool stop_ = false;

    State state_ = State::Inactive;
    Quat q_, qStart_;           // the look (+0xa0) and where the phase began (+0xd4)
    float euler_[3] = {0, 0, 0};// q's angles (+0xb0)
    float twist_ = 0;           // +0xbc
    bool inLimits_ = false;     // +0xc4
    float rampAngle_ = 0;       // +0xc8
    float speed_ = 0;           // +0xcc, radians a second
    float rampTicks_ = 25;      // +0xd0
    float towardsK_ = 1, awayK_ = 1; // +0xe4, +0xe8
    float towards_ = 0, away_ = 0;   // the request's rates, radians a second
    int64_t hold_ = 0;          // the request's HoldTicks
    int64_t last_ = INT64_MIN, start_ = 0; // +0x98, the request's start (ms)
    // The request's weight (its cBlendFunction2): from `from` to `to` over
    // [t0, t1] of the request's time, `blend` while one is set.
    bool blend_ = false;
    float wFrom_ = 1, wTo_ = 1, weight_ = 1;
    int64_t wT0_ = 0, wT1_ = 0;
    // The IK's inputs, this frame (model space).
    bool out_ = false;
    Vec3 ikTarget_{0, 0, 0}, ikPole_{0, 0, 0};
    float ikTwist_ = 0;
    Quat lastHead_;
    bool lastHeadValid_ = false;
};

} // namespace os2
