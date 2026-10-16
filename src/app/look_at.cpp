#include "look_at.hpp"

#include "formats/anim.hpp"
#include "formats/sim_actor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr float kDeg = float(M_PI / 180.0);
// The look's limits, the angles of q (FUN_00c49a8c, the constants of the
// static initializer at BV 0x2447bb0): pitch (+ down) -51 to +36 degrees,
// yaw 70, roll 40.
constexpr float kPitchMin = -0.89011788f, kPitchMax = 0.62831855f, kYaw = 1.2217305f, kRoll = 0.69813168f;
constexpr float k10 = 0.17453292f;  // DAT_02d44e20: the twist's dead angle
constexpr float k30 = 0.52359879f;  // DAT_02d44e24: the angle of a full-speed turn
constexpr float k90 = 1.5707964f;   // DAT_02d44e1c: the twist's limit
constexpr float k200 = 3.4906585f;  // DAT_02d44e18: radians a second back from a target too near
constexpr int64_t kForever = 0x7FFFFF00; // DAT_02d45398: no end to the hold
// The spine's share of the twist (DAT_0273a700): spine0, spine1, spine2, neck.
constexpr float kSpineShare[4] = {0.2f, 0.08f, 0.05f, 0.1f};

float len(const Vec3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

// The angle between two looks' +Z (the binary's acos of the dot product of
// the rotated (0, 0, 1), 0 outside (-1, 1)).
float angleBetween(const Quat& a, const Quat& b) {
    const Vec3 za = Xform{a, {}}.rotate({0, 0, 1}), zb = Xform{b, {}}.rotate({0, 0, 1});
    const float d = za[0] * zb[0] + za[1] * zb[1] + za[2] * zb[2];
    return d <= -1 || d >= 1 ? 0.0f : std::acos(d);
}

// Euler angles x, y, z of q = qz * qy * qx (the inverse of quatFromEulerXYZ).
void eulerXYZ(const Quat& q, float e[3]) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float r20 = 2 * (x * z - w * y), r21 = 2 * (y * z + w * x), r22 = 1 - 2 * (x * x + y * y);
    const float r10 = 2 * (x * y + w * z), r00 = 1 - 2 * (y * y + z * z);
    e[0] = std::atan2(r21, r22);
    e[1] = std::asin(std::clamp(-r20, -1.0f, 1.0f));
    e[2] = std::atan2(r10, r00);
}

// FUN_00c49a8c: the angles held within the head's limits; true if any was.
bool clampLimits(const float in[3], float out[3]) {
    bool clamped = false;
    out[0] = in[0], out[1] = in[1], out[2] = in[2];
    if (in[0] < kPitchMin)
        out[0] = kPitchMin, clamped = true;
    else if (in[0] > kPitchMax)
        out[0] = kPitchMax, clamped = true;
    if (in[1] < -kYaw)
        out[1] = -kYaw, clamped = true;
    else if (in[1] > kYaw)
        out[1] = kYaw, clamped = true;
    if (in[2] < -kRoll)
        out[2] = -kRoll, clamped = true;
    else if (in[2] > kRoll)
        out[2] = kRoll, clamped = true;
    return clamped;
}

// The first frames of a turn (or a return): the share of full speed by the
// angle to turn (0.3-1 of a 30-degree turn's), the ramp's ticks (a quarter
// of the angle at full speed, 25-300) and its angle (the ramp at half speed).
bool debug() {
    static const bool on = std::getenv("OS2_DEBUG_LOOK") != nullptr;
    return on;
}

float speedShare(float angle) { return std::clamp(angle / k30, 0.3f, 1.0f); }
float rampTicks(float angle, float rate) { return std::clamp(angle * 0.25f / (rate * 0.001f), 25.0f, 300.0f); }

} // namespace

HeadLook::HeadLook(const SimActor& actor) : actor_(actor) {
    const Skeleton& sk = actor.skin.skeleton;
    head_ = sk.find("head");
    grip_ = sk.find("head_grip");
    neck_ = sk.find("neck");
    rootRot_ = sk.find("root_rot");
    for (const char* name : {"spine0", "spine1", "spine2", "neck"})
        spine_.push_back(sk.find(name));
    valid_ = head_ >= 0 && grip_ >= 0 && neck_ >= 0 && rootRot_ >= 0 && sk.bones[size_t(grip_)].parent == head_ &&
             sk.bones[size_t(head_)].parent == neck_;
    if (valid_)
        headRest_ = sk.bones[size_t(head_)].rest.q;
}

float HeadLook::weightAt(int64_t elapsed) const {
    if (!blend_)
        return weight_;
    if (wT1_ <= wT0_)
        return elapsed >= wT1_ ? wTo_ : wFrom_;
    const float t = std::clamp(float(elapsed - wT0_) / float(wT1_ - wT0_), 0.0f, 1.0f);
    return wFrom_ + (wTo_ - wFrom_) * t;
}

float HeadLook::weight() const { return weightAt(last_ == INT64_MIN ? 0 : last_ - start_); }

// cActiveAnimLookAt's state change (vtable 0xe8, BV 0xd3f46c): only onwards
// (or back to turning); into a return the request loses its blend and
// weighs 1; a turn starts at 0.3 of the rate towards, a return at 0.3 of the
// rate back, both from the look as it is.
void HeadLook::setState(State s) {
    if (s == state_ || (s > State::Turning && s < state_))
        return;
    if (state_ < State::HoldDone && s > State::Holding) {
        blend_ = false;
        weight_ = 1;
    }
    state_ = s;
    switch (s) {
    case State::Turning:
        towardsK_ = 1;
        speed_ = towards_ * 0.3f;
        qStart_ = q_;
        break;
    case State::HoldDone:
    case State::Returning:
        awayK_ = 1;
        speed_ = away_ * 0.3f;
        qStart_ = q_;
        break;
    default:
        break;
    }
    if (s > State::Holding)
        hold_ = 0;
    if (debug())
        std::fprintf(stderr, "look: state 0x%X\n", int(s));
}

void HeadLook::setRequest(const std::optional<Request>& request, const std::optional<Vec3>& target, float stopAwayRate) {
    // A Look At not seen before starts a look (the VM keeps the last one
    // after it has played out: it does not start again).
    if (request && (!seen_ || request->serial != *seen_)) {
        pending_ = request;
        seen_ = request->serial;
    }
    target_ = target;
    stop_ = !request;
    stopAway_ = stopAwayRate;
}

void HeadLook::apply(std::vector<Xform>& local, double now) {
    if (!valid_ || local.size() <= size_t(std::max({head_, grip_, neck_, rootRot_})))
        return;
    const int64_t nowMs = int64_t(std::llround(now * 1000.0));
    const int64_t lastPrev = last_;
    last_ = nowMs;
    out_ = false;
    // A request that ended last frame is gone (the queue drops it; with no
    // request left the look resets to inactive, BV 0xd3f124).
    if (state_ == State::Ended || state_ == State::Stopped) {
        state_ = State::Inactive;
        current_.reset();
        lastHeadValid_ = false;
    }
    // The request's weight before this frame (uVar43): 0.001 with no
    // request before (the first look, or after one ended).
    const float weightBefore = current_ ? weightAt(nowMs - start_) : 0.001f;
    bool first = false;
    if (pending_) {
        if (!current_) {
            // As the animation has the head, relative to its rest on the neck.
            q_ = normalized(conjugate(headRest_) * local[size_t(head_)].q);
            eulerXYZ(q_, euler_);
            twist_ = 0;
            inLimits_ = false;
        }
        current_ = pending_;
        pending_.reset();
        start_ = nowMs;
        towards_ = (current_->towards > 0 ? current_->towards : 90.0f) * kDeg;
        away_ = (current_->away > 0 ? current_->away : 90.0f) * kDeg;
        hold_ = current_->holdMs < 0 ? kForever : current_->holdMs;
        blend_ = false;
        weight_ = 1;
        state_ = State::Inactive;
        setState(State::Turning);
        first = true;
    }
    if (!current_)
        return;
    // A STOP (or a target gone): back from a turn or a hold.
    if ((stop_ || !target_) && state_ < State::HoldDone) {
        if (stop_ && stopAway_ > 0)
            away_ = stopAway_ * kDeg;
        setState(State::Returning);
    }
    const int64_t elapsed = nowMs - start_; // uVar4
    const int64_t since = lastPrev == INT64_MIN ? start_ : std::max(start_, lastPrev);
    const int64_t lVar25 = nowMs - since;
    float rateT = towards_ * towardsK_, rateA = away_ * awayK_; // dVar41, dVar42

    // The frames, in the model's space: the head as it would stand on the
    // neck at rest (Fref), and that frame at head_grip (Fh).
    const Skeleton& sk = actor_.skin.skeleton;
    std::vector<Xform> world = sk.world(local);
    const Xform& neckW = world[size_t(neck_)];
    const Quat refRot = normalized(neckW.q * headRest_);
    const Vec3 headPos = neckW.apply(local[size_t(head_)].t);
    const Vec3 gripOff = Xform{refRot, {}}.rotate(local[size_t(grip_)].t);
    const Xform Fh{refRot, {headPos[0] + gripOff[0], headPos[1] + gripOff[1], headPos[2] + gripOff[2]}};

    // The spine's twist: towards the look's heading from the root (less the
    // 10 degrees either way, within 90) with "including Spine", else 0; at
    // the current speed. Not on the request's first frame.
    const float twistBefore = twist_;
    if (!first) {
        float want = 0;
        if (current_->spine) {
            const Vec3 ahead = Fh.apply(Xform{q_, {}}.rotate({0, 0, 10}));
            const Vec3 p = world[size_t(rootRot_)].inverse().apply(ahead);
            const float a = std::atan2(p[0], p[2]);
            want = a >= 0 ? std::min(a - k10, k90) : std::max(a + k10, -k90);
        }
        const float step = speed_ * 0.001f * float(lVar25);
        const float d = want - twist_;
        twist_ = d < 0 ? (d < -step ? twist_ - step : want) : (step < d ? twist_ + step : want);
    }
    const float twistTurn = (twist_ - twistBefore) * 0.6f;

    // The target in Fh: its distance, and the look that would face it,
    // held within the head's limits.
    Vec3 G{0, 0, 1};
    if (target_)
        G = Fh.inverse().apply(actor_.placement().inverse().apply(*target_));
    const float dist = len(G);
    const float wanted[3] = {std::atan2(-G[1], std::hypot(G[0], G[2])), std::atan2(G[0], G[2]) - twistTurn, 0};
    float held[3];
    const bool clamped = clampLimits(wanted, held);
    const Quat qd = quatFromEulerXYZ(held[0], held[1], held[2]);
    {
        float tmp[3];
        if (!clampLimits(euler_, tmp))
            inLimits_ = true;
    }
    const bool noEarlyExit = current_->noEarlyExit;

    // The ticks to play: since the last frame (or the request's start); 1
    // on a request's first frame that has none.
    int64_t ticks = (!first || lVar25 > 0) ? lVar25 : 1;
    float thR = 0; // the angle left to turn (for the estimate below)
    while (ticks > 0 && state_ >= State::Turning && state_ <= State::Stopped) {
        if (state_ == State::Turning || state_ == State::Holding) {
            if (dist < 0.3f) {
                // A target that near: back at 200 degrees a second.
                away_ = k200;
                rateA = k200 * awayK_;
                setState(State::Returning);
                continue;
            }
            thR = angleBetween(q_, qd);
            const float thT = angleBetween(qStart_, qd);
            float v, ramp;
            if (!first) {
                v = speed_, ramp = rampTicks_;
            } else {
                const float f = speedShare(thT);
                rateT = rateT / towardsK_ * f;
                towardsK_ = f;
                const float oldAway = awayK_;
                awayK_ = f;
                v = f * speed_;
                speed_ = v;
                ramp = rampTicks(thT, rateT);
                rampTicks_ = ramp;
                rateA = rateA * (f / oldAway);
                rampAngle_ = 0.001f * rateT * 0.5f * ramp;
            }
            float vAvg = v;
            const float decel = rateT * 0.7f / ramp;
            const float T = float(ticks);
            if (state_ == State::Holding) {
                speed_ = rateT;
                vAvg = rateT;
            } else {
                float left = T;
                const float covered = std::fabs(thT - thR);
                if (covered < rampAngle_) {
                    // On the ramp: from 0.3 of full speed up.
                    const float frac = covered / rampAngle_;
                    const float use = std::min(T, ramp * (1 - frac));
                    left = T - use;
                    const float vNew = std::min(rateT * (0.3f + 0.7f * frac) + use * decel, rateT);
                    speed_ = vNew;
                    vAvg = (vAvg + vNew) * 0.5f;
                }
                if (left > 0) {
                    const float half = rampAngle_ * 0.5f;
                    bool done = false;
                    if (half < thR) {
                        // Full speed until half the ramp's angle is left.
                        speed_ = rateT;
                        const float full = (thR - half) / (rateT * 0.001f);
                        vAvg = (rateT + vAvg) * 0.5f;
                        left -= std::min(left, full);
                        done = left <= 0;
                    }
                    if (!done) {
                        // Slowing down near the end, never under 0.3.
                        const float g = thR / half;
                        const float share = g <= 1 ? 0.3f + 0.7f * g : 1.0f;
                        speed_ = std::max(rateT * share - decel * left, rateT * 0.3f);
                        vAvg = (vAvg + speed_) * 0.5f;
                    }
                }
            }
            const float step = 0.001f * vAvg * T;
            if (!blend_ && step <= thR) {
                // The weight comes in over half the time the turn takes.
                blend_ = true;
                wFrom_ = weightBefore, wTo_ = 1;
                wT0_ = elapsed, wT1_ = elapsed + int64_t(thR * 1000.0f / rateT * 0.5f);
            }
            if (step < thR) {
                q_ = slerp(q_, qd, step / thR);
                eulerXYZ(q_, euler_);
                if (state_ == State::Holding && hold_ != kForever) {
                    const int64_t left = hold_ - ticks;
                    if (left < 1) {
                        hold_ = 0;
                        setState(State::HoldDone);
                    } else {
                        hold_ = left;
                    }
                }
                ticks = 0;
            } else {
                q_ = qd;
                euler_[0] = held[0], euler_[1] = held[1], euler_[2] = held[2];
                inLimits_ = true;
                if (state_ != State::Turning) {
                    int64_t left = hold_;
                    if (hold_ != kForever) {
                        left = hold_ - ticks;
                        hold_ = std::max<int64_t>(left, 0);
                    }
                    if (!clamped || noEarlyExit) {
                        if (left < 1)
                            setState(State::HoldDone);
                    } else {
                        setState(State::Returning);
                    }
                    ticks = 0;
                } else {
                    // Arrived: hold on it, or (a target past the limits,
                    // without "no early exit") come back. The ticks left
                    // over go on as the binary counts them (it takes the
                    // leftover off what is left, BV 0xd43554).
                    const int64_t leftover = int64_t(T * (1 - thR / step));
                    setState(!clamped || noEarlyExit ? State::Holding : State::Returning);
                    ticks -= leftover;
                }
            }
            continue;
        }
        if (state_ == State::HoldDone || state_ == State::Returning) {
            // Back to the animation's head (q = identity), slowing down from
            // three ramps' angle off.
            const Quat neutral{};
            const float thBack = angleBetween(q_, neutral);
            const float thT = angleBetween(qStart_, neutral);
            float v, ramp;
            if (!blend_) {
                const float f = speedShare(thT);
                rateA = rateA / awayK_ * f;
                awayK_ = f;
                v = f * speed_;
                speed_ = v;
                ramp = rampTicks(thT, rateA);
                rampTicks_ = ramp;
                rampAngle_ = 0.001f * rateA * 0.5f * ramp;
            } else {
                v = speed_, ramp = rampTicks_;
            }
            float vAvg = v;
            const float decel = rateA * 0.7f / ramp;
            const float T = float(ticks);
            float left = T;
            const float covered = std::fabs(thT - thBack);
            if (covered < rampAngle_) {
                const float frac = covered / rampAngle_;
                const float use = std::min(T, ramp * (1 - frac));
                left = T - use;
                const float vNew = std::min(rateA * (0.3f + 0.7f * frac) + use * decel, rateA);
                speed_ = vNew;
                vAvg = (vAvg + vNew) * 0.5f;
            }
            if (left > 0) {
                const float span = rampAngle_ * 3.0f;
                bool done = false;
                if (span < thBack) {
                    speed_ = rateA;
                    const float full = (thBack - span) / (rateA * 0.001f);
                    vAvg = (rateA + vAvg) * 0.5f;
                    left -= std::min(left, full);
                    done = left <= 0;
                }
                if (!done) {
                    const float g = thBack / span;
                    const float share = g <= 1 ? 0.3f + 0.7f * g : 1.0f;
                    speed_ = std::max(rateA * share - decel * left, rateA * 0.3f);
                    vAvg = (vAvg + speed_) * 0.5f;
                }
            }
            const float step = vAvg * T * 0.001f;
            const State next = state_ == State::HoldDone ? State::Ended : State::Stopped;
            if (step < thBack) {
                q_ = slerp(q_, neutral, step / thBack);
                const Vec3 z = Xform{q_, {}}.rotate({0, 0, 1});
                euler_[0] = std::atan2(-z[1], std::hypot(z[0], z[2]));
                euler_[1] = std::atan2(z[0], z[2]);
                euler_[2] = 0;
                if (!blend_) {
                    // The weight goes out over the time the return takes.
                    blend_ = true;
                    wFrom_ = 1, wTo_ = 0.001f;
                    wT0_ = elapsed, wT1_ = elapsed + int64_t(thT * 1000.0f / rateA);
                    weight_ = 1;
                } else if (wT1_ < elapsed) {
                    setState(next);
                }
            } else {
                q_ = neutral;
                euler_[0] = euler_[1] = euler_[2] = 0;
                setState(next);
            }
            break;
        }
        // Ended or stopped: no blend, all but no weight.
        blend_ = false;
        weight_ = 0.001f;
        break;
    }
    if (inLimits_) {
        float tmp[3];
        if (clampLimits(euler_, tmp)) {
            euler_[0] = tmp[0], euler_[1] = tmp[1], euler_[2] = tmp[2];
            q_ = quatFromEulerXYZ(tmp[0], tmp[1], tmp[2]);
        }
    }
    // Done when nothing is left of the turn, the hold and the way back
    // (the request's portion reaching 1, BV 0xd44214).
    if (state_ < State::Ended && hold_ != kForever && state_ != State::Inactive) {
        float rest = thR * 1000.0f / rateT + float(hold_);
        if (!blend_)
            rest += angleBetween(q_, Quat{}) * 1000.0f / rateA;
        else if (elapsed < wT1_)
            rest += float(wT1_ - elapsed);
        if (int64_t(rest) <= 0 && elapsed > 0)
            setState(state_ == State::Returning ? State::Stopped : State::Ended);
    }

    // The IK's inputs: the point at the target's distance along the look,
    // the pole straight up Fh, the twist by the request's weight.
    const float w = weightAt(elapsed);
    const Vec3 ahead = Xform{q_, {}}.rotate({0, 0, dist});
    ikTarget_ = Fh.apply(ahead);
    ikPole_ = Fh.apply({0, 1, 0});
    ikTwist_ = w * twist_;
    out_ = true;
    if (debug())
        std::fprintf(stderr, "look: state 0x%X q (%.0f %.0f) twist %.0f weight %.2f dist %.2f%s\n", int(state_),
                     euler_[0] / kDeg, euler_[1] / kDeg, twist_ / kDeg, w, dist, clamped ? " (held at the limits)" : "");

    // The spine's twist chain: each bone turned about its Y by its share,
    // on top of the animation (delta channels of the request's weight).
    if (std::fabs(ikTwist_) > 1e-6f) {
        for (size_t i = 0; i < spine_.size() && i < 4; ++i)
            if (spine_[i] >= 0)
                local[size_t(spine_[i])].q =
                    normalized(local[size_t(spine_[i])].q * quatFromEulerXYZ(0, kSpineShare[i] * ikTwist_ * w, 0));
        world = sk.world(local);
    }
    // The head's chain: its IK channel (level 8, base) mixed with the
    // animation by the weight; with no solution it keeps the last one.
    const auto solution = lookAtIkRotation(world[size_t(head_)], world[size_t(neck_)].q,
                                           local[size_t(grip_)].t[1], ikTarget_, ikPole_);
    if (solution)
        lastHead_ = *solution, lastHeadValid_ = true;
    if (lastHeadValid_)
        local[size_t(head_)].q = nlerp(local[size_t(head_)].q, lastHead_, std::clamp(w, 0.0f, 1.0f));
}

} // namespace os2
