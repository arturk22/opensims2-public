#pragma once

// A cinematic scene playing in the lot (plain C++, driven from main.mm):
// the camera (app/cinematic_camera.hpp) and what the Mac binary's
// cTSSGSystem does when the scene starts (FUN_007c3898, the TSSG system's
// vtable +0x70; docs/cinematics-mac.md §6):
//
// - Actors with an auto model ("cineLightRig-template", "vehicleTaxi",
//   "plumbobCinematic", "UFO3", ...) are created for the scene; actors with
//   auto-origin are put at the origin actor's placement; each actor's auto
//   animation list plays one animation after the other from the start
//   (mirrored like the camera, the actors' bones too: AnimPlayer::Options::mirror).
// - The light rig ("tsCineLightRig.i", skeleton "lightrig") is not drawn:
//   its animation places and colours its lights (point1a/b, spot1a/b of rig
//   1, 2 and 3: "diffuse" x "diffuseScale"), and the actors whose
//   cinematic render groups have bit 0x1/0x2/0x4 ("Cinematic1/2/3", the
//   binary's FUN_0097cbe4 render-group switch) are lit by rig 1/2/3.
//   Spots are lit as points (inferred).
// - The scene is over when the camera's animation is (IsCinematicSceneActive:
//   the camera's animation request still running); the created actors go
//   away and the moved ones go back.
//
// Without "Start Animations Now" (the primitive's flag 0x04) the binary
// prepares the scene and waits (IsCinematicSceneReadyToPlay); here the
// scene starts when one of its actors next starts an animation (inferred:
// the trees play the actors' cinematic animations right after the
// primitive), or after a second.

#include "app/cinematic_camera.hpp"
#include "formats/sim_actor.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class Renderer;
class ResourceIndex;

class CinematicPlayer {
public:
    struct Host {
        // A Sim's actor, or null.
        std::function<SimActor*(int16_t id)> sim;
        // Plays an animation on an object (Animate Object's path: the host
        // swaps in a skinned model) at `at`; returns its actor, or null.
        std::function<SimActor*(int16_t id, const std::string& anim, double at)> objectAnimation;
        // Where an object or Sim stands.
        std::function<std::optional<Xform>(int16_t id)> placement;
    };
    // `actors`: the object IDs the tree passed, in the CINE's actor order (0
    // where none); `origin` the fallback origin actor.
    CinematicPlayer(Renderer& renderer, ResourceIndex& index, Host host, const std::string& scene,
                    std::vector<int16_t> actors, int16_t origin, bool mirror);
    ~CinematicPlayer();

    // Starts the scene (camera, actors, rig) at `now` (the scene clock).
    // False if the origin actor is not in the world.
    bool start(double now);
    // Test aid (--cinematic): the Sim actors also play their own part,
    // which the trees would play ("<ages>-cinematic-<actor>-<scene>_anim",
    // found by name: a2a, t2t, a2o, a, t, c, e, p, b2a ...).
    void setGuessSimAnimations(bool on) { guessSims_ = on; }
    bool started() const { return started_; }
    bool active(double now) const { return started_ && camera_->active(now); }
    // Poses the created actors, samples the rig.
    void update(double now);
    // Removes the created actors and puts back the moved ones.
    void finish();

    const CinematicCamera& camera() const { return *camera_; }
    const std::vector<int16_t>& actors() const { return actorIds_; }
    // The created actors (their animations' effect and sound events).
    std::vector<const SimActor*> createdActors() const;

    struct Light {
        Vec3 position{}, color{};
        float falloff = 1, range = 0;
    };
    // The rig lights an object or Sim gets (its render groups), if it is one of the actors.
    std::vector<Light> lightsFor(int16_t id) const;

private:
    struct Created {
        std::unique_ptr<SimActor> actor;
        int instance = -1;
    };
    struct RigLight {
        std::string name; // point1a, spot3b, ...
        int group = 0;    // 1..3
        int bone = -1;    // in rigSkeleton_
        Vec3 color{1, 1, 1};
        float falloff = 0; // FalloffRate (0: no falloff)
    };
    void loadRig(const std::string& cres);

    Renderer& renderer_;
    ResourceIndex& index_;
    Host host_;
    std::unique_ptr<CinematicCamera> camera_;
    std::vector<int16_t> actorIds_;
    int16_t originId_ = 0;
    bool mirror_ = false, started_ = false, guessSims_ = false;
    double start_ = 0;
    std::vector<Created> created_;
    std::map<SimActor*, std::optional<Xform>> moved_; // existing actors put at the origin: their transform before
    // The light rig.
    int rigActor_ = -1;
    Skeleton rigSkeleton_;
    std::vector<std::unique_ptr<Animation>> rigAnims_;
    std::vector<RigLight> rigLights_;
    std::vector<std::vector<Light>> rigOut_{4}; // per group, this frame
};

} // namespace os2
