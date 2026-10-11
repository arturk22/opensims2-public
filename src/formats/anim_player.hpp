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

// Plays animations on one skeleton as layers. Layers are applied over the rest
// pose in order of the animation's priority, then of start time; each one
// moves only the bones it keys (all of them for full-body animations, flag
// bit 0), scaled by its fade weight and bone mask. play() replaces the
// layers of the same priority with a crossfade, so a base layer (walk, idle)
// and an overlay of higher priority (an upper-body gesture) mix as in the game
// (inferred: the game's blending is not decoded; priorities from the ANIM
// header).
class AnimPlayer {
public:
    explicit AnimPlayer(const Skeleton* skeleton = nullptr) : skeleton_(skeleton) {}
    void setSkeleton(const Skeleton* skeleton) { skeleton_ = skeleton; }

    struct Options {
        float fade = 0.25f;      // seconds to blend in (and to blend the replaced layers out)
        bool loop = true;
        std::vector<float> mask; // per bone, empty = every bone it keys
        int priority = -1;       // -1: base layer 0 without a mask, else the animation's own
    };
    // Starts `anim` (kept by pointer: it must outlive the player) at `now`
    // seconds; layers of the same priority fade out over the fade time.
    void play(const Animation& anim, double now, const Options& options);
    void play(const Animation& anim, double now) { play(anim, now, Options{}); }
    // Fades out the layers of `priority` (all if -1).
    void stop(double now, float fade = 0.25f, int priority = -1);

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
    };
    float weight(const Layer& l, double now) const;
    float velocity(double now) const; // per second

    const Skeleton* skeleton_;
    std::vector<Layer> layers_;
    double lastAdvance_ = 0;
};

} // namespace os2
