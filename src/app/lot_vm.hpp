#pragma once

// The app's side of the SimAntics VM: a lot's objects running in the VM while
// it is shown, with the Sims' actors following what the VM does to them.
// Plain C++ (no AppKit), driven from main.mm.

#include "formats/lot.hpp"
#include "formats/sim_actor.hpp"
#include "vm/sim_motion.hpp"
#include "vm/vm.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;
class ResourceIndex;
struct Character;

// The VM runs at 30 ticks a second (a game minute a second, normal speed).
// Walkers move as the VM moves them, from tile centre to tile centre at its
// walking speed, the speed of the Sim's walk animation (vm::walkTilesPerTick:
// 2 tiles/s for the normal adult walk), not by the walk animation's root motion: the VM's position is the truth (routing, slots,
// other trees read it), and the renderer only interpolates between the last
// two ticks and turns the Sim smoothly towards where it goes. The walk
// animation loops while the VM says the Sim walks (its locomotion set's
// "walk"), Animate Sim's animations play once when they start, and the
// Sim's idle plays otherwise.
class LotSimulation {
public:
    // The lot's objects resume their saved state (vm/lot_resume.hpp), or
    // start from their Inits with `fromInit`.
    LotSimulation(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                  const std::filesystem::path& lotFile, bool fromInit = false);
    LotSimulation(const LotSimulation&) = delete;
    LotSimulation& operator=(const LotSimulation&) = delete;

    vm::VM& machine() { return *vm_; }
    // What loading said (objects created, Inits that failed, ...).
    std::string summary() const { return summary_; }

    // Lets the VM drive `actor`, the Sim of object `id` (its character's
    // age and gender go into the person data the animation names need);
    // `idle` is what it plays standing.
    void attach(SimActor& actor, int16_t id, const Character& character, const std::string& idle);
    // Makes the Sim walk to an object (vm::startScriptedWalk), then play
    // entry `animation` of the global animation list if given.
    void walk(int16_t sim, int16_t target, std::optional<uint16_t> animation);

    // Runs the VM up to `seconds` (whole ticks) and places and animates the
    // attached actors for that moment (one tick behind the VM, so that
    // there is a tick to interpolate towards).
    void update(double seconds);
    uint64_t ticks() const { return vm_->ticks; }

private:
    struct Bound {
        SimActor* actor = nullptr;
        int16_t id = 0;
        std::string idle;
        vm::SimMotion motion;
        std::string playing; // what the actor was last cued to play
        uint32_t serial = 0;
        float zOffset = 0; // the cObject's height over the floor the VM's level gives
    };
    void sampleAll(double at);
    float floorHeight(int level, float x, float y) const;

    ResourceIndex& index_;
    Lot lot_;
    std::unique_ptr<vm::CodeLibrary> code_;
    std::unique_ptr<vm::VM> vm_;
    std::vector<Bound> bound_;
    std::string summary_;
    double last_ = 0;
    double slip_ = 0; // seconds the VM has fallen behind the caller's clock
};

} // namespace os2
