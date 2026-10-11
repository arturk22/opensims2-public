#pragma once

// Ties the live camera to the lot (docs/camera-ui-mac.md §1.3): the pivot is
// held to the lot's rectangle (shrunk by constraintwidth, in LotCamera: -10 lets it 10 tiles out) and
// sits on the floor of the level the camera shows, which follows the
// selected Sim's level as cCameraLive::Update does. Plain C++.

#include "formats/lot.hpp"
#include "render/camera.hpp"
#include "vm/vm.hpp"

#include <memory>

namespace os2 {

class LotCameraFollow {
public:
    LotCameraFollow(LotCamera& camera, const Lot& lot);

    // The selected Sim's level becomes the camera's (no Sim, or out of the
    // world: unchanged); a tracked Sim out of the dead zone moves the pivot.
    void follow(vm::VM& vm, vm::ObjectId sim);
    // Tracking (the live panel: right click on a family thumbnail): the
    // camera keeps a Sim in view as cTSCameraSmoothSims1Controller::Update
    // does, moving the pivot to it once its screen point is farther than
    // `subjecttrackingdeadzonemagnitude` (30) pixels from the pivot's. 0 stops.
    void setTracked(vm::ObjectId sim) { tracked_ = sim; }
    vm::ObjectId tracked() const { return tracked_; }
    // Centre on a Sim (smoothed); false if it is not in the world.
    bool centre(vm::VM& vm, vm::ObjectId sim);
    // Floor point of a Sim (for "Center on active Sim"), if it is in the world.
    std::optional<simd_float3> simPoint(vm::VM& vm, vm::ObjectId sim) const;
    int level() const { return *level_; } // the lot's numbering (0 = the ground)
    // Page Up / Page Down and the puck's floor buttons: the camera shows
    // another floor than the Sim's until the Sim changes floor itself
    // (follow() then takes his again). False if the lot has no such floor.
    bool setLevel(int level);
    // The lot's levels, in the lot's numbering.
    int lowestLevel() const;
    int highestLevel() const;

private:
    LotCamera* camera_;
    std::shared_ptr<const Lot> lot_;
    std::shared_ptr<int> level_;
    vm::ObjectId tracked_ = 0;
    vm::ObjectId followedSim_ = 0;
    int simLevel_ = -1000; // the followed Sim's level when last seen
};

} // namespace os2
