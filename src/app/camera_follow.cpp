#include "camera_follow.hpp"

#include <algorithm>

namespace os2 {

LotCameraFollow::LotCameraFollow(LotCamera& camera, const Lot& lot)
    : camera_(&camera), lot_(std::make_shared<Lot>(lot)), level_(std::make_shared<int>(0)) {
    camera.setBounds(simd_make_float2(0, 0), simd_make_float2(float(lot.width), float(lot.height)));
    camera.setFloorSampler([lot = lot_, level = level_](float x, float y) {
        const int l = lot->hasLevel(*level) ? *level : 0;
        return lot->elevationAt(l, std::clamp(x, 0.0f, float(lot->width)), std::clamp(y, 0.0f, float(lot->height)));
    });
}

void LotCameraFollow::follow(vm::VM& vm, vm::ObjectId sim) {
    const vm::Object* o = sim ? vm.object(sim) : nullptr;
    if (!o || !o->position.inWorld())
        return;
    const int level = o->position.level - 1; // the VM counts the ground as 1
    // A floor chosen by hand holds until the Sim moves to another one.
    if (lot_->hasLevel(level) && (level != simLevel_ || sim != followedSim_))
        *level_ = level;
    simLevel_ = level, followedSim_ = sim;
    // Subject tracking (BV 0x0086ffb8): the pivot's and the subject's screen
    // points; past the dead zone the subject becomes the pivot's target.
    if (const vm::Object* t = tracked_ ? vm.object(tracked_) : nullptr; t && t->position.inWorld()) {
        const float dead = camera_->definition().param("subjecttrackingdeadzonemagnitude", 30);
        const simd_float3 pivot = camera_->pivot();
        const simd_float3 at = simd_make_float3(t->position.x, t->position.y, pivot.z);
        const auto a = camera_->project(pivot), b = camera_->project(at);
        if (a && b && simd_distance(*a, *b) > dead)
            camera_->moveTo(at);
    }
}

bool LotCameraFollow::setLevel(int level) {
    if (!lot_->hasLevel(level))
        return false;
    *level_ = level;
    return true;
}

int LotCameraFollow::lowestLevel() const { return lot_->minLevel; }
int LotCameraFollow::highestLevel() const { return lot_->minLevel + lot_->levelCount - 1; }

bool LotCameraFollow::centre(vm::VM& vm, vm::ObjectId sim) {
    const auto p = simPoint(vm, sim);
    if (p)
        camera_->moveTo(*p);
    return p.has_value();
}

std::optional<simd_float3> LotCameraFollow::simPoint(vm::VM& vm, vm::ObjectId sim) const {
    const vm::Object* o = sim ? vm.object(sim) : nullptr;
    if (!o || !o->position.inWorld())
        return std::nullopt;
    return simd_make_float3(o->position.x, o->position.y, 0);
}

} // namespace os2
