#pragma once

// The models an animation's snap events put in a Sim's hands (ANIM events
// type 1005, formats/anim.hpp AnimSnap): while a Sim plays an animation the
// VM's Animate Sim started, each object the snaps name shows its model
// ("newspaper:__newspaperRolled__") at the Sim's bone ("auSkel:__R_Hand_
// grip__") or nowhere ("Oblivion"), as the events up to that moment say;
// where they left an object stays through the next animations (the read
// loops only snap the open paper, the rolled one stays gone) until the Sim
// stops playing Animate Sim animations (inferred). The
// object the Sim holds whose model name starts with the snapped name (the
// "newspaper" object for "newspaper") is hidden meanwhile: the snapped
// models stand in for it (the rolled paper, then the open one). The objects'
// own animations the VM plays on that object (Animate Object:
// "o2a-newspaper-read-start-unfolded" animates newspaperReading's pages)
// play on the snapped model of the same skeleton. Plain C++.

#include "formats/anim.hpp"
#include "formats/sim_actor.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace os2 {

class Renderer;
class ResourceIndex;
class VmObjectView;
namespace vm {
class VM;
}

class AnimSnapView {
public:
    AnimSnapView(Renderer& renderer, ResourceIndex& index, VmObjectView& objects, vm::VM& vm);
    ~AnimSnapView();

    // A Sim starts an Animate Sim animation (null: something else) at `at` seconds.
    void simAnimation(int16_t sim, SimActor& actor, const Animation* animation, double at);
    // An object starts an animation the VM plays on it.
    void objectAnimation(int16_t object, const std::string& name, double at);
    // After the Sims are posed (their bones), before the objects are placed:
    // places, hides and poses the snapped models.
    void update(double now);

private:
    struct Model {
        std::unique_ptr<SimActor> actor; // null: failed to load
        std::string skeleton;            // root node name, lowercase ("newspaperrolled")
        std::string playing;             // object animation cued on it
        std::string pose;                // the snap's snapObjectPose cued on it
        double poseAt = -1;              // when (an older object animation does not replace it)
    };
    struct SimSnaps {
        SimActor* actor = nullptr;
        const Animation* animation = nullptr;
        double start = 0;
        std::vector<AnimSnap> snaps;
        size_t applied = 0;                  // snaps of this animation applied to `state`
        std::map<std::string, AnimSnap> state; // where each snapped object is now
        std::map<std::string, Model> models; // by object name and model
        std::set<int16_t> hidden;            // VM objects hidden for the snaps
    };
    Model& model(SimSnaps& s, const AnimSnap& snap);
    void release(SimSnaps& s);

    Renderer& renderer_;
    ResourceIndex& index_;
    VmObjectView& objects_;
    vm::VM& vm_;
    std::map<int16_t, SimSnaps> sims_;
    std::map<int16_t, std::pair<std::string, double>> objectAnims_; // last one per object
};

} // namespace os2
