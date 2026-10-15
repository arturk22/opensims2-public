// A Sim's memories, as cTSPerson keeps them (docs/relationships-mac.md §4).
//
// A memory is a singular token of the neighbor's inventory with the memory
// flag (2) and more than 11 properties (TokenIsMemory, Mac BV 0x537c7c); the
// token's GUID is the memory object (an OBJD of type 15). The properties the
// engine reads: 0 kind, 5/6 the subject's GUID (low/high word), 7 the value
// it starts at, 8 the value it ends at, 9 the daily step, 10 the value now,
// 11 an aspiration mask.

#pragma once

#include "vm.hpp"

#include <vector>

namespace os2::vm {

constexpr int16_t kPersonDataMemoryScore = 54; // "Memory Score" (STR# 0xC8)
constexpr int16_t kPersonDataAspiration = 46;

bool tokenIsMemory(const InventoryToken& t);

// The Sim's visible memories in inventory order (the panel shows them newest
// first: the list backwards, cTSMemorySimulator::GetMemories BV 0x5372b0).
std::vector<MemoryInfo> memoryInfo(const VM& vm, const Object& sim);

// The Memory Score of these memories for a Sim of aspiration `aspiration`
// (BV 0xad05c8): the three highest and the three lowest values now (the
// positive and the negative ones of each, weight 1.2 when the memory's
// aspiration mask has the Sim's, else 1) averaged over the larger weight
// sum; then pulled toward ±100 by the total of the starting values (×2 for
// the aspiration's): avg + T(100 - avg)/(T + 500), or for T <= 0
// avg - T(avg + 100)/(T + 500); truncated. 0 without memories.
int16_t memoryScore(const std::vector<MemoryInfo>& memories, int16_t aspiration);

// cTSPerson::UpdateMemories (BV 0xad0a34). With `decay` (DayPassed): an
// inventory of more than 1000 tokens is pruned (PruneMemories BV 0xad0380:
// of the first count - 100 tokens, the invisible memories already at their
// final value go), and each memory's value now moves its daily step from
// the start toward the end value, within them. Then (also without decay,
// but only when the memories changed or were never read) the Sim's list is
// read again and its Memory Score (person data 54) written.
void updateMemories(VM& vm, Object& sim, bool decay);

// Every tick for each Sim (cTSPerson::OnSimulatorTick): every 60 ticks, if
// its motives are not static (person data 29), updateMemories(no decay).
void tickMemories(VM& vm);

} // namespace os2::vm
