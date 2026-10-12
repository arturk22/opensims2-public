#pragma once

#include "anim.hpp"
#include "skeleton.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// Per-bone weights: 1 for `root` and every bone under it, 0 elsewhere (e.g.
// "spine0" for an upper-body overlay). Empty if the bone is missing.
std::vector<float> boneMask(const Skeleton& skeleton, std::string_view root);

// Bones an animation target drives: those with a rotation or translation
// channel and the bones of its IK chains.
std::vector<bool> keyedBones(const Animation::Target& target, const Skeleton& skeleton);

// Per bone: `from` moved towards `to` by weight * mask[bone] (mask empty = 1);
// rotations by nlerp, translations linearly.
void blendPose(std::vector<Xform>& from, const std::vector<Xform>& to, float weight,
               const std::vector<float>& mask = {});

// q raised to the power w (the rotation's angle times w about its axis),
// as the channel blender weighs a delta rotation.
Quat deltaPower(const Quat& q, float w);

// For each bone, its mirror twin: "l_..." <-> "r_..." (itself otherwise).
std::vector<int> mirrorBones(const Skeleton& skeleton);

// A local pose mirrored left to right across the skeleton's YZ plane (+Y is
// the Sim's front, X its side), as the game's flipped animations ("Flip
// Cinematic Anims Horizontally", Animate Sim's flip flag): each bone takes
// the reflection of its twin's motion relative to the rest pose
// (W'_b = S (W_t R_t^-1) S R_b in model space, S the reflection), so the
// skinned mesh comes out as the mirror image of the unflipped one whatever
// the joints' axis conventions are. `restWorld` = skeleton.world(restPose()).
std::vector<Xform> mirrorPose(const Skeleton& skeleton, const std::vector<int>& twins,
                              const std::vector<Xform>& restWorld, const std::vector<Xform>& local);

// Plays animations on one skeleton as layers. Each layer moves only the
// bones it keys (all of them for full-body animations, flag bit 0), with
// its fade weight and bone mask, and each bone mixes the layers that move
// it as the game's channel blender does (cChannelUberOutlet, Mac BV 0xee659c;
// docs/primitives-mac.md §4): by level, from the highest down, a level
// whose weights add up to 1 takes the bone whole and a lighter one takes
// its share of what is left; the layers of one level are averaged by
// weight (nlerp), and the weights are normalized, so a single layer fading
// in over nothing already has the whole bone, and a bone no layer moves
// keeps the base pose. play() replaces the layers of the same priority
// with a crossfade (the old one blends out as the new one blends in), so
// a base layer (walk, idle, Animate Sim) and the overlays mix by their
// levels (Options::level).
class AnimPlayer {
public:
    explicit AnimPlayer(const Skeleton* skeleton = nullptr) : skeleton_(skeleton) {}
    // The pose under every layer (the bones an animation does not key, and
    // the whole pose when nothing plays): an object's pose as the lot saved
    // it (a chair pushed in) instead of the skeleton's rest pose.
    void setBasePose(std::vector<Xform> local) { base_ = std::move(local); }
    // An object holds its pose when its animations are stopped (the game's
    // objects keep their bones where the last frame left them, and save
    // them so: a chair pushed in stays in): stop() then freezes the pose
    // of the moment as the base pose instead of fading back to it.
    void setHoldPose(bool hold) { hold_ = hold; }
    void setSkeleton(const Skeleton* skeleton) {
        skeleton_ = skeleton;
        twins_.clear();
        deltaToIk_.clear();
    }

    struct Options {
        float fade = 0.25f;      // seconds to blend in (and to blend the replaced layers out)
        bool loop = true;
        std::vector<float> mask; // per bone, empty = every bone it keys
        int priority = -1;       // -1: base layer 0 without a mask, else the animation's own
        bool mirror = false;     // flipped horizontally (left and right swapped)
        // Radians about +Z the animation's frame is turned from the model's:
        // an a2o animation is made facing its header heading (Animation::
        // headingOffset: 45 degrees for a chair's "getIn-SE", which starts
        // from the routing slot at the chair's front corner, facing that
        // way, and ends turned back by -45 degrees, seated facing the chair).
        float turn = 0;
        // Seconds into the animation it starts at (its frame 0 at now - phase).
        double phase = 0;
        // The channel level it mixes at (the game's request priority,
        // vm::animlevel); -1: its priority.
        int level = -1;
        // The request weight its fade is multiplied by (Animate Sim's byte 13 / 100).
        float weight = 1;
        // Playback speed (the locomotion plays blended clips at one length:
        // each runs at its own length over the blend's; docs/routing-mac.md §6.1).
        float rate = 1;
        // The request's IK object's bones in this skeleton's model space, for
        // the IK targets of type 2 (applyIk): the baby a2b-babybottle-feeder's
        // hands reach. Empty: none.
        IkObjectBone ikObjectBone;
    };
    // Starts `anim` (kept by pointer: it must outlive the player) at `now`
    // seconds; layers of the same priority fade out over the fade time.
    void play(const Animation& anim, double now, const Options& options);
    void play(const Animation& anim, double now) { play(anim, now, Options{}); }
    // Fades out the layers of `priority` (all if -1).
    void stop(double now, float fade = 0.25f, int priority = -1);
    // The model moved by `delta` (new model space <- old) while the layers
    // started so far go on where they were in the world: each is moved back
    // by it (the bones under the top node), and the layers started after
    // play in the new frame. A Sim placed in a chair's seat as it is
    // contained: the get-in that brought it there was played from where it
    // stood (a2o-chairDining-getIn-*-leaveOut-SE ends seated 0.67 from its
    // start, in the chair pulled out), and its last frame, held under the
    // chair's idles, went to sit that far beside the seat.
    // With `onlyOnce`, only the layers that play once (a held last frame).
    void reframe(const Xform& delta, bool onlyOnce = false);

    // Local bone transforms at `now` (drops the layers that have faded out).
    std::vector<Xform> pose(double now);

    // Morph weights at `now` (channels of attribute MorphWeight of every
    // target, e.g. "faceblends": l_brow, x_mouth, ...), blended like the
    // bones by layer order and fade. Call after pose() for the same time.
    std::map<std::string, float> morphs(double now) const;

    // Root motion: distance travelled along the Sim's forward axis (+Y of the
    // skeleton) since the last call (or time 0), from the layers' locomotion velocity
    // (Animation::velocity, per tick) weighted by their fades.
    float advance(double now);

    size_t layerCount() const { return layers_.size(); }

    // Events (ANIM "events": sounds, type 1014 "sound_name=...") of the
    // layers not fading out whose time falls in (from, to] seconds, looping
    // layers firing again each cycle; in time order.
    struct FiredEvent {
        double at = 0;
        const Animation* anim = nullptr;
        const Animation::Event* event = nullptr;
    };
    std::vector<FiredEvent> eventsBetween(double from, double to) const;

private:
    struct Layer {
        const Animation* anim = nullptr;
        const Animation::Target* target = nullptr;
        double start = 0;
        float fadeIn = 0;
        double fadeOutStart = -1; // < 0: not fading out
        float fadeOut = 0;
        bool replaced = false; // fading out under a layer of the same priority
        bool loop = true;
        int priority = 0;
        std::vector<float> mask;
        std::vector<bool> keyed;
        bool mirror = false;
        float turn = 0;
        Xform frame; // reframe()'s, after the turn (and the mirror)
        bool framed = false;
        int level = 0;
        float scale = 1; // Options::weight
        float rate = 1;  // Options::rate
        bool delta = false; // has delta (additive) channels
        IkObjectBone ikObjectBone; // Options::ikObjectBone
    };
    // The layer's pose at `ticks`, turned by its frame (Options::turn).
    std::vector<Xform> layerPose(const Layer& l, float ticks) const;
    // The bones under the skeleton's top node moved by the layer's frame (reframe()).
    void applyFrame(const Layer& l, std::vector<Xform>& pose) const;
    void prepareMirror();
    // A layer's deltas mirrored left to right (and their per-bone weights with them).
    DeltaPose mirrorDeltas(const DeltaPose& d, std::vector<float>& weights) const;
    std::vector<bool> deltaToIk_; // bones whose deltas apply with no base channel
    std::vector<int> twins_;          // mirrorBones of the skeleton (empty until a flipped layer plays)
    std::vector<Xform> restWorld_;
    float weight(const Layer& l, double now) const;
    float velocity(double now) const; // per second

    const Skeleton* skeleton_;
    std::vector<Layer> layers_;
    std::vector<Xform> base_; // setBasePose's, empty: the skeleton's rest pose
    bool hold_ = false;
    double lastAdvance_ = 0;
};

} // namespace os2
