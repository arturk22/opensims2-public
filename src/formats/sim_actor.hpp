#pragma once

#include "anim_player.hpp"
#include "skinning.hpp"

#include <memory>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

// ANIM resource by name ("a-idle-..." with or without "_anim").
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

    explicit SimActor(SkinnedModel s) : skin(std::move(s)) { player.setSkeleton(&skin.skeleton); }
    SimActor(const SimActor&) = delete;
    SimActor& operator=(const SimActor&) = delete;

    // Plays `anim` when update() reaches `at` seconds (as if started then).
    struct Cue {
        double at = 0;
        const Animation* anim = nullptr;
        AnimPlayer::Options options;
    };
    std::vector<Cue> cues; // sorted by time, consumed by update()
    void cue(double at, const Animation& anim, AnimPlayer::Options options = {});

    // Loads (once) and returns an animation.
    const Animation& animation(ResourceIndex& index, const std::string& name);
    // Advances root motion to `now`, poses the skeleton and skins `posed`.
    void update(double now);
    // The placement as a transform (rotation about Z, then translation).
    Xform placement() const;
};

} // namespace os2
