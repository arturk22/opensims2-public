#pragma once

// Doors opening and closing as Sims walk through them (plain C++, driven
// from main.mm). The game makes doors routing portals (docs/routing-mac.md:
// AddDoorPortal, TriggerPortalTree) whose tree plays the door's object
// animation; the VM does not run portal trees yet, so the app plays them
// itself: when a Sim comes within reach of a door, or the VM holds a Sim at
// the door's portal (it waits there for the door to open), it plays
// "o-door-open-CW" or "o-door-open-CCW" (by the side the Sim comes from),
// and "o-door-close-..." once no Sim is near.
//
// Inferred: the distances (opens within 0.9 tile of the door's centre,
// closes past 1.3, measured in 3D from the bottom of the door's middle),
// the side rule (from the door's front, +Y of its model: CW; from behind:
// CCW). The close animation turns the other way (closeAnimation: from the
// game's data). Doors the VM animates itself
// (Animate Object) are left to it.

#include "formats/xform.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

namespace vm {
class VM;
}

class DoorPortals {
public:
    // `play(id, animation, at)` starts an object animation (the host's
    // onObjectAnimation).
    // `placement(id)` is where the door is drawn (the VM's positions of the
    // saved doors are not reliable: the lots keep stale object states).
    DoorPortals(std::function<void(int16_t id, const std::string& animation, double at)> play,
                std::function<std::optional<Xform>(int16_t id)> placement)
        : play_(std::move(play)), placement_(std::move(placement)) {}

    // A door to open (an object ID the host draws; `placement` gives where).
    void addDoor(int16_t id) { doors_[id]; }

    // Positions (world) of the Sims now; call each frame.
    void update(vm::VM& vm, const std::vector<Vec3>& sims, double seconds);

    // Doors open now (for tests and the log).
    size_t openCount() const;
    // The animation that closes a door opened by "o-door-open-<suffix>".
    static std::string closeAnimation(const std::string& openSuffix);

private:
    struct Door {
        bool open = false;
        std::string suffix; // "CW" / "CCW"
        uint32_t vmSerial = 0;
        bool seen = false;
    };
    std::function<void(int16_t, const std::string&, double)> play_;
    std::function<std::optional<Xform>(int16_t)> placement_;
    std::map<int16_t, Door> doors_;
};

} // namespace os2
