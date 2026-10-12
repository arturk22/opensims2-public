#pragma once

// How a Sim's walk looks and how fast it goes: the locomotion set's clips
// (STR# 0x84 of the top of the locomotion stack, docs/routing-mac.md §6.1)
// as nTSSG::cLocomotable and the scene graph's cLocoSet pick them.
//
// Each set is six strings: walk turns, walks, walk blends, run turns, runs,
// run blends. The turn string names the start, stop, step and turn clips by
// role ("turnAndGo-alt(turnAndGo)", "turnInPlace-alt@1.5(turnInPlace@1.5)":
// the run set plays them 1.5 times as fast), each made at 0, 45, 90, 135 and
// 180 degrees ("a-turnAndGo-alt-90", "a-TurnInPlace-90", "a-step-45").

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace os2::vm {

class VM;
struct Object;

// The walk cycle and its speed (BeginFollow, BV 0x6b5770; the set's speed
// FUN_00d0c444): person data 112 and 113 ("LocoWeight 0/1") weigh the walk
// cycles alt0 and alt1, 114 ("LocoPersonalityIndex", 2-19) names another
// with the weight 115/100 ("LocoPersonalityWeight"), all of them times
// 1 - 117/100, and 116 ("LocoMoodIndex", 2-19) adds 117/100
// ("LocoMoodWeight") to another ("Sneak Here": index 8 weight 80, the
// alt8 sneak; "Skip Here": 7 and 75, the alt7 skip). The speed is the
// cycles' speeds (ANIM velocity) weighed by those weights over the cycles
// that weigh more than 0.01 (alt0 always counts), or alt0's when none does.
// Object data 17 ("walk style", 1 = run: "Go Here Frantically" and the
// trees that hurry) takes the run string instead of the walk one.
// The game blends the weighed cycles (cLocoCycle, one cycle of them at a
// time): `cycles` are those with their weights (adding up to 1), each played
// at its length over the blend's (`seconds`, the cycles' lengths weighed),
// so that their feet stay in step; `animation` is the one that weighs the
// most. With `shortWay` the "walkShort"/"runShort" cycles (half the stride)
// are taken instead.
// A clip of the locomotion with its blend weight, mirrored or not, and its
// playback rate (its length over the blend's).
struct LocoLayer {
    std::string animation;
    float weight = 1;
    bool mirror = false;
    float rate = 1;
};
struct Gait {
    std::string animation; // resolved resource name, empty if none
    float speed = 0;       // metres per second
    bool run = false;
    std::vector<LocoLayer> cycles;
    float seconds = 0;     // the blended cycle's length
    // The set's "*Blend" poses over the cycles (loco type 200 + k, header
    // 30 + k: "walk-alt0-normalBlend", the elders' and the pregnant
    // posture; cActiveAnimLoco::PushTurnBlendCycles, BV 0xd3107c): for each
    // weighed cycle altK that has one, the pose at the cycle's weight.
    std::vector<LocoLayer> poses;
    // What plays: the cycles, then the poses.
    std::vector<LocoLayer> layers() const {
        std::vector<LocoLayer> all = cycles;
        all.insert(all.end(), poses.begin(), poses.end());
        return all;
    }
};
Gait gaitOf(VM& vm, const Object& sim, bool shortWay = false);

// The walk and walkShort cycles blended by stride (cAnimResourceConst::ReadFrom
// gives both the same loco type 100 + k; InitChannelNetwork, BV 0xd2cd68,
// blends the pair LocoResourcesWithClosestVelocity finds): `shortWeight` is
// walkShort's weight (ComputeTurnWeight: (s_long - s) / (s_long - s_short)).
// Each gait's cycles keep their weights within it, times 1 - w or w; the
// blend lasts the two lengths weighed, covers the two strides weighed, and
// each cycle plays at its length over the blend's.
Gait blendGaits(const Gait& full, const Gait& shortGait, float shortWeight);

// The walk's walkShort weight for a way: the forward walk covers `stretch`
// less the start's and the stop's distances (`clipsLong` with the long
// clips, `clipsShort` with the short ones, which go with the same weight),
// in a whole number of half cycles: the fewest that the long stride
// reaches (rounded up, as InitChannelNetwork rounds the cycles up in half
// cycles), the strides blended down to fit (inferred: the binary's
// fitting loop, ten tries in steps of 0.1 of the weight, was not read in
// full; this solves the same fit exactly). 0 without a short gait.
float strideWeight(const Gait& full, const Gait& shortGait, float stretch, float clipsLong, float clipsShort);

// The frame's way: a uniform cubic B-spline (cPathStrategyCubic::UpdatePathData,
// BV 0xd56704) whose control points are the first point three times, the
// route's points and the last one three times; each window of four is a
// segment with the matrix 1/6 [[1,4,1,0],[-3,0,3,0],[3,-6,3,0],[-1,3,-3,1]].
// The curve starts and ends at the ends and cuts the corners in between.
// Arc lengths are measured per segment from kSamples chords (the binary
// halves its step while a chord is over 0.005; with the route cut every 0.2
// the chords here are at most 0.0125, inferred close enough).
struct LocoCurve {
    static constexpr int kSamples = 16;
    std::vector<std::array<float, 2>> control;
    size_t points = 0;         // the route points it goes through (without the start)
    std::vector<float> length; // per segment
    std::vector<float> after;  // the length of the segments after each
    std::vector<float> table;  // per segment, kSamples + 1 arc lengths
    LocoCurve(float x, float y, const std::vector<std::array<float, 2>>& through);
    size_t segments() const { return length.size(); }
    std::array<float, 2> at(size_t segment, float u) const;
    std::array<float, 2> slope(size_t segment, float u) const;
    // The parameter `arc` along a segment.
    float parameterAt(size_t segment, float arc) const;
    // The route points passed once `segment` is done (a middle point when
    // the segment that ends nearest it is done, the last at the very end).
    size_t passedAfter(size_t segment) const;
    float remaining(size_t segment, float arc) const;
    float total() const { return length.empty() ? 0 : length.front() + after.front(); }
    // The first route point not yet passed, given `taken` passed.
    const std::array<float, 2>& point(size_t index) const { return control[3 + index]; }
};

// A start, stop, step or turn clip of the walker's set for `role`
// ("turnAndGo", "turnAndGoR", "turnAndGoShort", "turnAndStop",
// "turnAndStopShortR", "step", "stepShort", "turnInPlace") turning or
// stepping `steps` 45-degree steps (counter-clockwise positive). The clips
// are made clockwise (header turn -45 x steps); the set finds the one whose
// turn is the wanted one, else the one whose turn is its opposite, played
// mirrored (cLocoSet vtable 0x30, BV 0xd0c7ec). An "R" role (the other
// foot) falls back to the plain one, as the set takes a start or stop of
// either foot when there is no other (cycle portion 0 or 0.5).
struct LocoPick {
    std::string animation;
    bool mirror = false;
    float speed = 1; // the set's playback multiplier ("@1.5")
};
std::optional<LocoPick> locoClip(VM& vm, const Object& sim, const std::string& role, int steps);

} // namespace os2::vm
