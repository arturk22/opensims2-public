#pragma once

// The want simulator (cTSWantSimulator, docs/wants-mac.md): each Sim's 4
// wants and 3 fears, chosen from the want trees by priority and check
// tree, its achievement history and personal want clock, Want Satisfy
// (0x7C) marking wants as achieved and the aspiration score they change,
// and Generic Sims Call 39 ("Force Recalc of Wants"). The state comes
// from and goes to the SWAF resources of the neighborhood package.
//
// It follows the BV 1.10 binary (Ghidra headless; the base game's
// simulator is the same without wants locks, extra slots, lifetime wants
// and influence, which stay off):
// - OnTick: the want clock += 2 a tick (3600 a game hour: tree lifetimes
//   are hours × 0xE10), then DoWork, one Sim at a time (the selected Sim
//   first): find the candidates, prioritize them, pick the slots, report.
// - MarkWantAsAchieved: an achievement in the history; the slots it fulfils
//   are emptied and their points go to the aspiration score; the
//   definition's feedback tree runs on the want controller object.

#include "vm.hpp"

#include "formats/wants.hpp"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2::hood {
struct Neighborhood;
}

namespace os2::vm {

// cTSWantInstance at run time.
struct WantInstance {
    wants::Instance data;            // owner, want, subject, time achieved, score, flags
    int node = -1;                   // its tree node (wants::Library::nodes), -1 if none
    std::shared_ptr<WantInstance> source; // the achievement it follows in its tree
    float priority = 0;
};
using WantPtr = std::shared_ptr<WantInstance>;

struct SimWants {
    std::vector<WantPtr> wants = std::vector<WantPtr>(4), fears = std::vector<WantPtr>(3); // slots
    std::vector<WantPtr> oldWants, oldFears; // as they were before an update of type < 2
    // The personal want clock (cTSWantAchievementHistory +0/+4): it moves
    // with the simulator's clock whenever it is read.
    uint32_t lastClock = 0;
    int32_t personal = 0;
    uint32_t historyVersion = 2;
    std::map<uint32_t, std::vector<WantPtr>> history; // by want GUID
    std::optional<wants::Record> record; // the SWAF as read
    bool read = false; // ReadPerson done (on the lot)
};

// What happened, for the hosts (s2pkg autonomy/wants, the live panel).
struct WantEvent {
    enum class Kind : uint8_t { Rolled, Dropped, Achieved, FearRealized, Satisfied, Score } kind = Kind::Rolled;
    int16_t sim = 0;  // neighbor ID
    uint32_t guid = 0;
    wants::SubjectType subjectType = wants::SubjectType::None;
    uint32_t subjectValue = 0;
    uint16_t subjectData = 0;
    int slot = -1;     // Rolled/Dropped/Achieved: the slot
    bool fear = false;
    int32_t points = 0; // Achieved/FearRealized/Score: points; Score: the new raw score
    int16_t before = 0, after = 0; // Score: aspiration score (person data 160) before and after
    uint64_t tick = 0;
};

struct WantSystem {
    std::shared_ptr<const wants::Library> library;
    uint32_t clock = 0; // +0x14C
    std::map<int16_t, SimWants> sims;
    std::map<int16_t, int> updates; // pending update type by neighbor ID (0/1 reroll all, 2 fears, 3 fill)
    // DoWork
    int state = 0;
    int16_t current = 0;
    int currentType = 3;
    std::vector<WantPtr> candidates;
    std::vector<WantPtr> beforeWants, beforeFears; // the Sim's slots when its update started
    std::map<const WantInstance*, bool> checked;   // check tree results during a selection (+0x188)
    bool skipCheckTrees = false;                   // the ToggleSkipCheckTrees cheat; GSC 39 op 8
    std::deque<WantEvent> events;                  // the latest 4096
    std::vector<std::string> warnings;             // check/feedback trees that failed (the latest 64)
};

// The want system of a VM, or null (no game data).
WantSystem* wantSystem(VM& vm);
const SimWants* simWants(VM& vm, int16_t neighborId);

// Called from loadNeighborhood: the want library (once per resource index)
// and every Sim's SWAF from the neighborhood package. A VM without an index
// or a package without want data gets no want system.
void loadWants(VM& vm, const hood::Neighborhood& h);
// A want system with this library and no saved state (tests).
WantSystem& installWants(VM& vm, std::shared_ptr<const wants::Library> library);
// Called from VM::tick.
void tickWants(VM& vm);
// Runs every pending update to its end (cTSWantSimulator::WritePerson does
// it for a Sim before saving it).
void flushWants(VM& vm);

// cTSWantSimulator::RequestWantsUpdate.
void requestWantsUpdate(VM& vm, int16_t neighborId, int type);

struct WantSubject {
    wants::SubjectType type = wants::SubjectType::None;
    uint32_t value = 0;
    uint16_t data = 0;
};
// cTSWantSimulator::MarkWantAsAchieved: true if a slot was fulfilled.
bool markWantAchieved(VM& vm, int16_t neighborId, uint32_t guid, const WantSubject& subject, int16_t objectParam = 0);

// Aspiration score (cTSWantSimulator::NormalizedAspirationScore and
// GetAspirationLevel): the raw score (person data 162 × 10) on the meter
// (-100..100) and the level (0..5: below severe failure, severe failure,
// neutral, bronze, silver, gold) for an age (wants::ageEnum).
int16_t normalizedAspirationScore(const wants::Library& lib, int age, int32_t raw);
int aspirationLevel(const wants::Library& lib, int age, int32_t raw);
// cTSWantSimulator::ChangeScore for a neighbor (its Sim on the lot if any).
void changeAspirationScore(VM& vm, int16_t neighborId, int32_t points);

// Generic Sims Call 39 "Force Recalc of Wants" (cTSObject::TryGenericSimCall
// case 0x27): temp 0 = operation (+10: the Sim is the neighbor in temp 1 and
// the argument temp 2; else the Sim is me and the argument temp 1).
Exit wantsGenericCall(Context& c);
// Want Satisfy (0x7C).
Exit wantSatisfyPrimitive(Context& c);

// The neighbor ID of a Sim object (person data 31), 0 if none.
int16_t neighborIdOf(const Object& sim);

// The SWAF record of a Sim from the VM's state; and whether it differs from
// the one read (the tail of stale bytes apart).
wants::Record wantRecord(VM& vm, int16_t neighborId);
bool wantRecordChanged(VM& vm, int16_t neighborId);
// Differences in the want state between two VMs (slots, history, clocks).
std::vector<std::string> compareWantState(VM& a, VM& b, size_t limit = 50);

// Text of a want for people: its name with the subject put in ("$Sim"...).
std::string wantText(VM& vm, const wants::Instance& w);
std::string subjectText(VM& vm, const wants::Instance& w);

} // namespace os2::vm
