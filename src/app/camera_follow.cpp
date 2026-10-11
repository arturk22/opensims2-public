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
    if (lot_->hasLevel(level))
        *level_ = level;
}

std::optional<simd_float3> LotCameraFollow::simPoint(vm::VM& vm, vm::ObjectId sim) const {
    const vm::Object* o = sim ? vm.object(sim) : nullptr;
    if (!o || !o->position.inWorld())
        return std::nullopt;
    return simd_make_float3(o->position.x, o->position.y, 0);
}

} // namespace os2
