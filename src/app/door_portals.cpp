#include "door_portals.hpp"

#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {
constexpr float kOpenDistance = 0.9f, kCloseDistance = 1.3f;
}

// The leaf turns back the other way: "o-door-open-CCW" ends at -91 degrees,
// where "o-door-close-CW" starts (the doors' animation list, STR# 0x86 of
// semi-global 0x7FD0DEBA, pairs them so: 1 open-CCW, 2 close-CW, 3 open-CW,
// 4 close-CCW). The same suffix snapped the leaf to the other side first.
std::string DoorPortals::closeAnimation(const std::string& openSuffix) {
    return std::string("o-door-close-") + (openSuffix == "CW" ? "CCW" : "CW");
}

size_t DoorPortals::openCount() const {
    size_t n = 0;
    for (const auto& [id, d] : doors_)
        n += d.open;
    return n;
}

void DoorPortals::update(vm::VM& vm, const std::vector<Vec3>& sims, double seconds) {
    for (auto& [id, d] : doors_) {
        // A door the VM animates itself (Animate Object) is left to it.
        if (const vm::Object* o = vm.object(id)) {
            if (!d.seen)
                d.vmSerial = o->animation.overlaySerial, d.seen = true;
            if (o->animation.overlaySerial != d.vmSerial)
                continue;
        }
        const auto at = placement_ ? placement_(id) : std::nullopt;
        if (!at)
            continue;
        // The door's middle: the model's origin is on the tile's edge, half a tile back.
        const Vec3 front = at->rotate({0, 1, 0});
        const Vec3 c = at->apply({0, -0.5f, 0});
        float nearest = 1e9f, side = 0;
        for (const auto& p : sims) {
            const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < nearest)
                nearest = dist, side = dx * front[0] + dy * front[1];
        }
        // A Sim the VM holds at this door's portal (Route::Point::door,
        // vm/routing.cpp) opens it whatever the distance.
        bool atPortal = false;
        for (const auto& [sid, so] : vm.objects())
            if (so.person && so.route && !so.route->points.empty() && so.route->points.front().door == id)
                atPortal = true;
        if (!d.open && (nearest < kOpenDistance || atPortal)) {
            d.open = true;
            d.suffix = side >= 0 ? "CW" : "CCW";
            play_(id, "o-door-open-" + d.suffix, seconds);
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "opensims2: door %d opens (%s)\n", id, d.suffix.c_str());
        } else if (d.open && nearest > kCloseDistance) {
            d.open = false;
            play_(id, closeAnimation(d.suffix), seconds);
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "opensims2: door %d closes (%s)\n", id, d.suffix.c_str());
        }
    }
}

} // namespace os2
