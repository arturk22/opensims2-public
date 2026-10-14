#pragma once

#include "anim_player.hpp"
#include "skinning.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

// ANIM resource by name ("a-idle-..." with or without "_anim"). Throws
// AnimationNotFound (a FormatError) when the game has none of that name.
struct AnimationNotFound : FormatError {
    using FormatError::FormatError;
};
Animation loadAnimation(ResourceIndex& index, std::string name);

// A skinned Sim (or any skinned model) placed in the lot, animated by an
// AnimPlayer. The placement (position in tiles, heading about Z) is applied to
// the posed vertices, so the renderer instance stays at the identity.
struct SimActor {
    SkinnedModel skin;
    AnimPlayer player;
    std::vector<std::unique_ptr<Animation>> animations; // owned, referenced by the player
    Vec3 position{0, 0, 0};
    float heading = 0; // radians about +Z; 0 = the skeleton's forward (+Y)
    bool rootMotion = false; // move along the heading by the locomotion velocity
    int model = -1;          // renderer model id
    ObjectModel posed;
    // When set, the placement instead of position and heading (an object
    // held in a Sim's hand or put in another object's slot).
    std::optional<Xform> transform;
    // Called on the animated local bone transforms before they are posed
    // (procedural changes such as turning the head to look at something).
    std::function<void(std::vector<Xform>& local, double now)> adjust;
    // World transforms of the skeleton's bones after the last update().
    std::vector<Xform> bones;
    // Morph weights that hold whatever the animation does: the body shape
    // ("fattop"/"fatbot", "pregtop"/"pregbot"; see bodyShapeMorphs).
    std::map<std::string, float> bodyMorphs;

    explicit SimActor(SkinnedModel s) : skin(std::move(s)) { player.setSkeleton(&skin.skeleton); }
    SimActor(const SimActor&) = delete;
    SimActor& operator=(const SimActor&) = delete;

    // Plays `anim` when update() reaches `at` seconds (as if started then).
    struct Cue {
        double at = 0;
        const Animation* anim = nullptr; // null: stop the layers of options.priority
        AnimPlayer::Options options;
    };
    std::vector<Cue> cues; // sorted by time, consumed by update()
    void cue(double at, const Animation& anim, AnimPlayer::Options options = {});
    // Stops the layers of `priority` when update() reaches `at` (AnimPlayer::stop).
    void cueStop(double at, int priority, float fade = 0.25f);
    // Starts (or stops) what the cues due by `now` say (update() does it first).
    void startCues(double now);

    // Loads (once) and returns an animation.
    const Animation& animation(ResourceIndex& index, const std::string& name);
    // Advances root motion to `now`, poses the skeleton and skins `posed`.
    void update(double now);
    // The placement as a transform (rotation about Z, then translation).
    Xform placement() const;
    // Where bone `name` is at `now` (world: the cues due started, posed at
    // `now`, at the placement), without update()'s skinning; nothing if the
    // skeleton lacks it. A chair's seat for the Sim in it, posed before the
    // objects' actors are (they are posed after the Sims).
    std::optional<Xform> bonePlacement(std::string_view name, double now);
};

// The body-shape morphs of a Sim, as the game sets them on its outfit
// (Mac BV 0x0094c318, the caller of the "fattop"/"fatbot" strings), from
// person data 81 "Body Flags" and 58 (age: 19 adults, 16 teens, ...): for
// ages over 2, "fattop"/"fatbot" = 1 with flag 0x1, else 0; for ages 19-50,
// "pregtop"/"pregbot" = 1 with flag 0x2, 0.5 with 0x4, else 0. Fitness is
// not a morph: fit and fat bodies are separate outfits (GZPS "fitness").
std::map<std::string, float> bodyShapeMorphs(int16_t bodyFlags, int16_t age);

} // namespace os2
