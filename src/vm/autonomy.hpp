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
// Find Best Interaction, as the Mac binary's cTSPerson::TryFindBestAction
// and FBA_* do it (docs/data-findings.md §16; what is inferred is marked
// there and in TODO.md):
// - operand: u16 at 0 = mask of motives that picks the candidates (bit =
//   motive number: "Fulfill Hunger" passes 1 << 7), 0 = the Sim's worst
//   motives ("random worst motive count" 8 in the global FCNS 2: all eight
//   of Find 5 Worst Motives); flags u8 at 3: 0x02 only objects in the room
//   in temp 0, 0x04 also objects out of the world, 0x08 only nested
//   interactions (not done: false), 0x10 only the current interaction's
//   object, 0x20 distances from the object in temp 1.
// - candidates: the TTAB entries of every object (interaction leaders;
//   other Sims too, not the Sim itself) that are not debug, are available
//   to the Sim's age (cTSPerson::GetAvailabilityBitset), whose "autonomy
//   threshold" is at most the Sim's person data 36 "autonomy level", and
//   that advertise a motive of the mask in the ad table of the Sim's age.
//   Objects in use (object data 62 "use count") are skipped.
// - score = sum over motives 0, 3, 5-9, 13-15 of InteractionScoreCurve(motive
//   + ad / 1000) - InteractionScoreCurve(motive), / 10, times 1 / (1 +
//   attenuation x distance): attenuation from the entry (custom value, none,
//   low 0.1, moderate 0.3, high 0.6, FCNS 2), distance in tiles plus 10 per
//   level ("autonomy floor to floor distance cost"). ad = min + range x
//   personality / 1000 (type 0: min + range). Times the biases of
//   FBA_ScoreInteraction: ownership (1 + 0.1 x the object's relationship
//   variable 1 towards the Sim); memory, relationship, weather and
//   temperature are 1 in the base game (autonomy.cpp, ownershipFactor).
// - in the order of that score, the check trees run (autonomous, parameter
//   0 = 1, with the ads in data owners 0x0C/0x1C/0x1D so they can change
//   them), at most 200 ("max check tree in fba count"); the entries that
//   pass are scored again with the ads their check tree left, and the best
//   one above 1e-5 wins ("random selection count" is 1). Interactions the
//   Sim ran lately that failed (action tree returned false) are left out
//   while it remembers them (the last 8 for 9999 ticks: "autonomy memory
//   count" and "max duration autonomy memory"; inferred use).
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
    std::vector<std::string> checkErrors; // check trees that failed (error, budget), why
    std::vector<ScoredInteraction> best; // the passing entries, best first
    bool found = false;
};
// Called after every Find Best Interaction (empty by default).
std::function<void(const AutonomyDecision&)>& autonomyObserver();

// Whether a TTAB entry serves the Sim's age (cTSPerson::GetAvailabilityBitset:
// adult 0x40, child 0x10, toddler 0x200, teen 0x800, elder 0x400, baby 0x1000
// in the entry's u32 of flags, 0x10/0x40 inverted before TTAB version 0x52).
// Autonomy and the pie menu leave out the others. True for non-Sims.
bool availableToAge(const Object& sim, const InteractionTable& table, const InteractionTable::Interaction& e);

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
