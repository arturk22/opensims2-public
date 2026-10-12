#include "camera_follow.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

LotCameraFollow::LotCameraFollow(LotCamera& camera, const Lot& lot)
    : camera_(&camera), lot_(std::make_shared<Lot>(lot)), level_(std::make_shared<int>(0)) {
    camera.setBounds(simd_make_float2(0, 0), simd_make_float2(float(lot.width), float(lot.height)));
    *level_ = startLevel(lot);
    camera.setFloorSampler([lot = lot_, level = level_](float x, float y) {
        const int l = lot->hasLevel(*level) ? *level : 0;
        return lot->elevationAt(l, std::clamp(x, 0.0f, float(lot->width)), std::clamp(y, 0.0f, float(lot->height)));
    });
}

void LotCameraFollow::follow(vm::VM& vm, vm::ObjectId sim) {
    const vm::Object* o = sim ? vm.object(sim) : nullptr;
    if (!o || !o->position.inWorld())
        return;
    // The VM counts the ground as 1. The pool's level (-1, below the
    // ground) is not a floor the player can show (the floor buttons stop at
    // the ground): a swimming Sim keeps the view on the ground, with the
    // house and the water (inferred: the binary's level setter, a service's
    // vtable 0x68, was not read; live QA round 16 saw the house vanish).
    const int level = std::max(o->position.level - 1, lot_->hasLevel(0) ? 0 : lot_->minLevel);
    // A floor chosen by hand holds until the Sim moves to another one.
    // cCameraLive::Update (BV 0x6a5450): the view goes up to the Sim's
    // floor always, down to it only when the Sim's location is in a room
    // (cPartitionManager::RoomContaining(location, definition 0), vtable
    // 0x13c, non-zero): a Sim walking down outdoor stairs leaves the view
    // where it is until it steps into a room.
    const bool down = level < *level_;
    const bool inRoom = o->data[vm::objdata::Room] != 0;
    if (level != simLevel_ || sim != followedSim_) {
        if (!lot_->hasLevel(level) || !down || inRoom) {
            if (lot_->hasLevel(level))
                *level_ = level;
            simLevel_ = level; // else asked again next time (the Sim may walk into a room)
        }
        followedSim_ = sim;
    }
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

int LotCameraFollow::startLevel(const Lot& lot) {
    // Before the VM picks a Sim (headless shots without it), the floor of
    // the lot's first Sim, as the game's camera takes the selected Sim's
    // floor (cCameraLive::Update): the level whose floor under the Sim is
    // nearest its height (on a foundation the house floor, 0.75 up, not the
    // ground); the ground without Sims.
    for (const auto& o : lot.objects) {
        if (!o.person || (o.position[0] == 0 && o.position[1] == 0))
            continue;
        int best = 0;
        float bestD = 1e9f;
        for (int l = lot.minLevel; l < lot.minLevel + int(lot.levelCount); ++l) {
            const float d = std::fabs(lot.elevationAt(l, o.position[0], o.position[1]) - o.position[2]);
            if (d < bestD - 1e-3f)
                best = l, bestD = d;
        }
        return best;
    }
    return lot.hasLevel(0) ? 0 : lot.minLevel;
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
