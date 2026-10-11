#pragma once

// Ties the live camera to the lot (docs/camera-ui-mac.md §1.3): the pivot is
// held to the lot's rectangle (grown by constraintwidth, in LotCamera) and
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
    // world: unchanged).
    void follow(vm::VM& vm, vm::ObjectId sim);
    // Floor point of a Sim (for "Center on active Sim"), if it is in the world.
    std::optional<simd_float3> simPoint(vm::VM& vm, vm::ObjectId sim) const;
    int level() const { return *level_; } // the lot's numbering (0 = the ground)

private:
    LotCamera* camera_;
    std::shared_ptr<const Lot> lot_;
    std::shared_ptr<int> level_;
};

} // namespace os2
