#pragma once

// The seat module (round 21): where a Sim the VM drives is drawn, as one
// rule for standing, walking, sitting down, staying seated, changing seats
// and getting up (and lying in a bed or a tub, sitting on a toilet,
// climbing stairs, swimming). It follows the Mac binary's semantics
// (docs/primitives-mac.md §4-§5: contain/SetContainer, slots, Snap,
// Animate Sim/Object/Overlay by levels):
//
//   1. The root. The Sim is drawn where the VM has it: its position and
//      direction (vm::SimMotion: interpolated between the last two ticks
//      while it walks, at once after a Snap or any move farther than a step,
//      the direction taken at once while Animate Sim plays). A Sim contained
//      in an object's slot (object data 2/3, set by Snap mode 1 and the
//      trees' "contain") is attached to that slot instead: the container's
//      slot bone, in the container's model frame, as the container is posed
//      at that very moment (VmObjectView::slotPlacement: a dining chair
//      pulled out or turned by its leaveOut get-in carries its seat along).
//   2. The body. The animations play from that root with their own root
//      motion, header heading and request heading (AnimPlayer::Options::
//      turn), mixed bone by bone by levels as the channel blender does
//      (AnimPlayer); there is no other adjustment of the Sim's place.
//   3. A jump of the root (contained or released, a Snap, a stair's end
//      putting the climber on top): what the body shows does not move with
//      it. The layers that play once (a held last frame, a get-in under
//      way) and those fading out stay where they were in the world
//      (AnimPlayer::stayInWorld), as the game's PostSnap puts the object
//      where the animation left it; looping layers go on from the new root,
//      and what starts afterwards plays in it (the leaveOut get-in, made
//      from the corner the Sim stood at, ends seated 0.67 from it in the
//      chair pulled out: placed at the seat after, its held last frame
//      stays in the seat).
//   4. Seated, the body holds its pose when nothing plays on it: the game
//      writes no bone that no request plays, so the Sim does not stand up in
//      the standing idle (the app's stand-in for the idle system) between
//      the chair's idles.
//   5. IK in the same frame: the objects the VM animates are posed and
//      placed before the Sims (app/lot_actors.cpp poseActors), so a contained Sim's
//      slot and its hands' goals on the object's bones are of the same
//      moment, and an IK chain weighs by its layer's share of the bones
//      (AnimPlayer::pose).
// Plain C++, driven by LotSimulation once per frame for each attached Sim.

#include "formats/xform.hpp"
#include "vm/sim_motion.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>

namespace os2 {

struct SimActor;

class SeatModule {
public:
    // Where the slot holding a Sim is drawn at `now` (world), nothing when
    // the Sim is in no object's slot (VmObjectView::slotPlacement).
    std::function<std::optional<Xform>(int16_t sim, double now)> slot;

    // Places `actor` (the Sim of object `id`) for the moment `now` of the
    // scene clock: `t` is the fraction of the tick `motion` interpolates at,
    // `dt` the frame's seconds (for turning), `z` the floor height there and
    // `zOffset` the Sim's height over it. Applies rules 1 and 3.
    void place(int16_t id, SimActor& actor, vm::SimMotion& motion, double now, float t, float dt, float z, float zOffset);
    // Rule 4: whether the Sim is in a seat (its body holds its pose there).
    bool seated(int16_t id) const;
    void forget(int16_t id) { tracks_.erase(id); }

    // A jump of the root: moved farther than any walk does in a frame
    // (vm::SimMotion::kMaxStep) or turned more than a turn does.
    static bool isJump(const Xform& from, const Xform& to);

private:
    struct Track {
        bool seated = false;
        std::optional<Xform> last; // the root drawn last frame
    };
    std::map<int16_t, Track> tracks_;
};

} // namespace os2
