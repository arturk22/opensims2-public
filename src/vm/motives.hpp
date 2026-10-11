#pragma once

// The motive engine: the Sims' motives over time, their mood, and the
// curves autonomy scores interactions with.
//
// What the game data and the Mac binary say (docs/primitives-mac.md §0x1D,
// docs/data-findings.md "Motores de motivos e autonomia"):
//
// - Motives are numbered as the editor's STR# 0x86: 3 Mood, 4 Mood+Mem,
//   5 Energy, 6 Comfort, 7 Hunger, 8 Hygiene, 9 Bladder, 13 Environment,
//   14 Social, 15 Fun (0-2, 10-12 unused in the base game), from -100 to 100.
// - Set Motive Change (0x1D) puts {motive, delta per hour / 60 / 30 per
//   tick, limit} on a list the engine applies every tick
//   (cTSPerson::OnSimulatorTick), unless person data 29 "Motives Static?"
//   is 2 or more. Interactions use it for what they give while they run.
// - The everyday decay is person data 93-101 "Decay <motive> per Day",
//   which the trees set ("Motives - Set Home Awake/Asleep" by age, from the
//   personality) and Set Motive Change can zero; no tree reads them, so the
//   engine applies them: per day = 24 game hours (inferred: the binary's
//   OnSimulatorTick does not; it may be in PersonalTimeMinutePassed).
// - Mood is the average of the motives weighted by the "Happy Curve Data"
//   (global STR# 0x226-0x22D, one per age: weight as a function of the
//   motive's value), leaving out the motives whose bit is set in person
//   data 118 "Motives Not Included in Mood". Checked against the lots' saved
//   moods (N001_Lot11: 56.3 from its motives; the PERS's second motive set
//   is the one the last mood was computed from).
// - Autonomy scores an interaction by how much its ads raise the "Interaction
//   Scores Curve" (global STR# 0x28A-0x291, per age) of each motive.

#include "vm.hpp"

#include <array>
#include <utility>
#include <vector>

namespace os2::vm {

namespace motive {
constexpr int16_t Mood = 3, MoodMem = 4, Energy = 5, Comfort = 6, Hunger = 7, Hygiene = 8, Bladder = 9,
                  Environment = 13, Social = 14, Fun = 15;
// The eight Find 5 Worst Motives sorts and autonomy considers ("random
// worst motive count" = 8 in the global FCNS 2).
constexpr std::array<int16_t, 8> kPhysicalAndMental = {Energy, Comfort, Hunger, Hygiene, Bladder, Environment, Social, Fun};
} // namespace motive

// Person data of the motive engine (STR# 0xC8).
namespace persondata {
constexpr int16_t MoneyOverHead = 1;
constexpr int16_t TickCounter = 27;
constexpr int16_t MotivesStatic = 29;
constexpr int16_t AutonomyLevel = 36;
constexpr int16_t FindBestActionObject = 53;
constexpr int16_t DecayHungerPerDay = 93; // 93 hunger, 94 comfort, 95 bladder, 96 energy, 97 hygiene, 99 social, 101 fun
constexpr int16_t MotivesNotInMood = 118;
} // namespace persondata

// A piecewise linear curve, as the game's curve STR#s write them:
// "(-100;0) (-90;6.5) ... (100;1)". Flat beyond its ends.
struct MotiveCurve {
    std::vector<std::pair<float, float>> points; // sorted by x
    float at(float x) const;
    static MotiveCurve parse(const std::string& text);
};
using MotiveCurveSet = std::array<MotiveCurve, kMotiveCount>;

// The age group of a Sim's curves: 0 elder, 1 adult, 2 teen, 3 child,
// 4 toddler, 5 baby (the order of the curve STR#s), by person data 58.
int curveAgeIndex(const Object& sim);
// The curves for a Sim's age (empty curves if the game data has none).
const MotiveCurveSet& happyCurves(VM& vm, const Object& sim);
const MotiveCurveSet& scoreCurves(VM& vm, const Object& sim);

// The mood the motives give (weighted average, see above), -100..100.
float computeMood(VM& vm, const Object& sim);

// Motives as the engine keeps them: with their fractions, picking up what
// the trees wrote since.
float exactMotive(Object& sim, int16_t m);
void setExactMotive(Object& sim, int16_t m, float value);

// The person data that holds the daily decay of a motive, or -1.
int16_t decayDataOf(int16_t motive);

// Switch for the "motiveDecay" cheat (on by default).
extern bool gMotiveDecay;

} // namespace os2::vm
