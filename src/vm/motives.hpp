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
// - The game keeps the motives as floats (cTSMotives, docs/motives-mac.md
//   §3), a current set and an old one; the trees see the current ones
//   rounded half away from zero, not clamped, and write them as they are.
// - Set Motive Change (0x1D) puts {motive, delta per hour / 60 / 30 per
//   tick, limit} on a list the engine applies every tick
//   (cTSPerson::OnSimulatorTick), unless person data 29 "Motives Static?"
//   is 2 or more. Interactions use it for what they give while they run.
// - Every 60 ticks, when person data 29 is 0, the motives are updated
//   (cTSMotives vtable 0x10): the everyday decay, person data 93-101 "Decay
//   <motive> per Day" (the trees set them: "Motives - Set Home
//   Awake/Asleep" by age, from the personality), a 720th of it each time
//   (per 24 game hours), the environment from the environment score
//   manager (vm/environment.hpp), every motive clamped to -100..100 and
//   copied to the old set; then the mood, and the memories (memories.hpp).
//   The step comes when the simulator's tick (VM::simulatorTicks, from the
//   lot's SIMI) is 60 past the person's last one (0 at load: with a saved
//   counter of 60 or more the first step is the first tick).
// - Mood is the average of the motives weighted by the "Happy Curve Data"
//   (global STR# 0x226-0x22D, one per age: weight as a function of the
//   motive's value), leaving out 0-2, 10-12 and the motives whose bit is
//   set in person data 118 "Motives Not Included in Mood", plus person data
//   163 "Mood Booster", not clamped (the base game's UpdateMood). Checked
//   against the lots' saved moods (N001_Lot11: 56.3 from its motives;
//   Mortimer's 232.1 with a booster of 200; the PERS's second motive set
//   is the one the last mood was computed from).
// - Autonomy scores an interaction by how much its ads raise the "Interaction
//   Scores Curve" (global STR# 0x28A-0x291, per age) of each motive.

#include "vm.hpp"

#include <array>
#include <optional>
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
constexpr int16_t MoodBooster = 163;
} // namespace persondata

// A piecewise linear curve, as the game's curve STR#s write them:
// "(-100;0) (-90;6.5) ... (100;1)" (cTSPiecewiseFunction, docs/motives-mac.md
// §1): points kept in increasing x, a point whose x is already there is
// left out (the first one stays), at most `maxPoints` (the curve sets use
// 32), with the inverse widths 1 / (x[i+1] - x[i]) beside them. Flat beyond
// its ends; 0 without points.
struct MotiveCurve {
    std::vector<std::pair<float, float>> points; // increasing x
    std::vector<float> inverse;                  // 1 / (x[i+1] - x[i]), as floats
    int maxPoints = -1;                          // -1: no limit
    float at(float x) const;
    void add(float x, float y); // AddPoint (vtable 0x14)
    static MotiveCurve parse(const std::string& text, int maxPoints = 32);
    static MotiveCurve constant(float y); // the one point (0; y)
};
using MotiveCurveSet = std::array<MotiveCurve, kMotiveCount>;

// The age group of a Sim's curves: 0 elder, 1 adult, 2 teen, 3 child,
// 4 toddler, 5 baby (the order of the curve STR#s), by person data 58 as
// the person's age tests have it (baby 0-1, toddler 2, child 3-15, teen
// 16-18, adult 19-50, elder above; none, a negative age: adult).
int curveAgeIndex(const Object& sim);
int curveAgeIndexOf(int16_t personAge);
// The curves for a Sim's age (cTSMotiveCurveSet::LoadFromFile: a STR# with
// fewer than 16 texts, or none, leaves the rest constant 1; an empty text
// is a curve without points, 0). The interaction score curves are the
// Sim's own when its definition's field 0x33 "motive effects id" names a
// STR# of its group (cTSPerson::LoadMotiveEffects).
const MotiveCurveSet& happyCurves(VM& vm, const Object& sim);
const MotiveCurveSet& happyCurvesOfAge(VM& vm, int ageIndex);
const MotiveCurveSet& scoreCurves(VM& vm, const Object& sim);

// The mood the motives give (weighted average plus the booster, see above):
// of a Sim's motives now, or of a set of motives.
float computeMood(VM& vm, Object& sim);
float moodFrom(const MotiveCurveSet& happy, const std::array<float, kMotiveCount>& motives, int16_t notInMood,
               int16_t booster);

// Motives as the engine keeps them (floats), picking up what the trees
// wrote since; what the trees see of a float.
float exactMotive(Object& sim, int16_t m);
float motiveNow(const Object& sim, int16_t m); // the same without picking up
void setExactMotive(Object& sim, int16_t m, float value);
int16_t motiveView(float value);

// The 60-tick update of a Sim's motives (cTSMotives vtable 0x10, base
// 0xb945f0): decay, the environment (13) the environment score manager
// gives (vm/environment.hpp; none: it keeps its value), clamp, the old set.
void updateMotives(Object& sim, std::optional<float> environment = std::nullopt);

// The person data that holds the daily decay of a motive, or -1.
int16_t decayDataOf(int16_t motive);

// Switch for the "motiveDecay" cheat (on by default).
extern bool gMotiveDecay;

} // namespace os2::vm
