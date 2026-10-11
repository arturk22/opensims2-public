#pragma once

// Autonomy: Find Best Interaction (0x03) and Gosub Found Action (0x1E).
//
// The Sims' Main ("Function - Main - Loop", person semi-global 0x205B) calls
// "Function - Main - Process Autonomy" (0x2029) when the queue is empty:
// after a random wait of 1-20 ticks it runs Find Best Interaction with no
// motive mask; when that finds something, "Idle System - Stop", "Function -
// Main - Pre Interaction", Gosub Found Action (the interaction's action tree
// runs right there, as the Sim's current, autonomous interaction), "Post
// Interaction" and "Motives - Set Home Awake" (the everyday decay again).
// When it finds nothing, the Main does its idle actions.
//
// Find Best Interaction, as implemented here (docs/data-findings.md
// "Motores de motivos e autonomia"; what is inferred is marked there and in
// TODO.md):
// - operand: u16 at 0 = mask of motives (bit = motive number: "Fulfill
//   Hunger" passes 1 << 7, "Fulfill Bladder" 1 << 9, the deviance check
//   1 << 3 Mood), 0 = the Sim's worst motives ("random worst motive count"
//   in the global FCNS 2 is 8: all eight of Find 5 Worst Motives); flags u8
//   at 3: 0x02 only objects in the room in temp 0, 0x04 also objects out of
//   the world, 0x08 only nested interactions (not done: false), 0x10 only
//   the current interaction's object, 0x20 distances from the object in
//   temp 1 (docs/primitives-mac.md).
// - candidates: the TTAB entries of every object (interaction leaders; other
//   Sims too, not the Sim itself) that are not debug, whose "autonomy
//   threshold" is at most the Sim's person data 36 "autonomy level", and
//   that advertise one of the motives: ad = min + range x personality / 1000
//   (the entry's personality type from STR# 0xE8: nice/grouchy ... skills),
//   from the ad table of the Sim's age. Objects in use (object data 62 "use
//   count") are skipped.
// - score = sum over the advertised motives of InteractionScoreCurve(motive
//   + ad) - InteractionScoreCurve(motive), divided by (1 + attenuation x
//   distance): attenuation from the entry (custom value, none, low 0.1,
//   moderate 0.3, high 0.6; visitors 0.01/0.02/0.03, FCNS 2), distance in
//   tiles plus 10 per level ("autonomy floor to floor distance cost").
// - in the order of that score, the check trees run (autonomous, parameter
//   0 = 1, with the ads in data owners 0x0C/0x1C/0x1D so they can change
//   them), at most 200 ("max check tree in fba count"); the entries that
//   pass are scored again with the ads their check tree left, and the best
//   one with a score above 0 ("min autonomy score" 0) wins ("random
//   selection count" is 1: no random pick among the best).
// - the winner: stack object, person data 53, object data 20 "tree table
//   entry" = its TTA index, and Gosub Found Action runs it.

#include "vm.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace os2::vm {

// One scored entry of a Find Best Interaction.
struct ScoredInteraction {
    ObjectId object = 0;
    uint16_t interaction = 0; // TTA index
    std::string name;
    float score = 0;          // after the check tree and the attenuation
    float distance = 0;
};

// What a Find Best Interaction did, for hosts (s2pkg autonomy).
struct AutonomyDecision {
    ObjectId sim = 0;
    uint16_t motiveMask = 0;
    size_t candidates = 0;  // entries that advertise
    size_t checked = 0;     // check trees run
    std::vector<ScoredInteraction> best; // the passing entries, best first
    bool found = false;
};
// Called after every Find Best Interaction (empty by default).
std::function<void(const AutonomyDecision&)>& autonomyObserver();

// The value of a TTAB ad for a Sim: min + range x the personality or skill
// the type names (STR# 0xE8) / 1000.
float adValue(const Object& sim, int16_t min, int16_t range, uint16_t personality);

// The Generic Sims Calls of the idle system: 43 "Is It Ok to Idle Here"
// (true: nothing here tells a bad spot, inferred) and 52 "Wall in Front?"
// (direction in temp 0: true if a wall stands between the Sim's tile and
// the next one that way, or crosses that tile; the first object on it in
// temp 1). Nothing for the other calls.
std::optional<Exit> idleSimsCall(Context& c, uint8_t call);

} // namespace os2::vm
