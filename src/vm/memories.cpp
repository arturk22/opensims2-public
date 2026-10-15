// A Sim's memories (memories.hpp).

#include "memories.hpp"

#include "motives.hpp"

#include <algorithm>
#include <cstdlib>

namespace os2::vm {

namespace {

constexpr size_t kMemoryProperties = 12; // more than 11 (TokenIsMemory)
constexpr size_t kPruneAbove = 1000;     // UpdateMemories: PruneMemories past this many tokens
constexpr size_t kPruneKeep = 100;       // PruneMemories looks at all but the last 100
constexpr uint64_t kRefreshTicks = 60;   // OnSimulatorTick

int32_t prop(const InventoryToken& t, size_t i) { return i < t.properties.size() ? t.properties[i] : 0; }

Inventory* inventoryOf(VM& vm, const Object& sim) {
    if (!sim.person || sim.person->data[persondata::NeighborId] == 0)
        return nullptr;
    auto it = vm.inventories.find({InventoryKind::Neighbor, sim.person->data[persondata::NeighborId]});
    return it == vm.inventories.end() ? nullptr : &it->second;
}

} // namespace

bool tokenIsMemory(const InventoryToken& t) {
    return (t.flags & tokenflag::Memory) && t.properties.size() >= kMemoryProperties;
}

std::vector<MemoryInfo> memoryInfo(const VM& vm, const Object& sim) {
    std::vector<MemoryInfo> out;
    if (!sim.person || sim.person->data[persondata::NeighborId] == 0)
        return out;
    auto it = vm.inventories.find({InventoryKind::Neighbor, sim.person->data[persondata::NeighborId]});
    if (it == vm.inventories.end())
        return out;
    for (const auto& t : it->second.singular)
        if (tokenIsMemory(t) && (t.flags & tokenflag::Visible))
            out.push_back({t.guid, uint32_t(uint16_t(prop(t, 6))) << 16 | uint16_t(prop(t, 5)), prop(t, 0),
                           prop(t, 10), prop(t, 7), prop(t, 8), prop(t, 11)});
    return out;
}

int16_t memoryScore(const std::vector<MemoryInfo>& memories, int16_t aspiration) {
    if (memories.empty())
        return 0;
    const float kBoost = 1.2f;
    auto weight = [&](const MemoryInfo& m) { return (m.aspiration & aspiration) ? kBoost : 1.0f; };
    // The three highest values now, then the three lowest (partial_sort_copy
    // with cMemInfoCompare(-1) and (1): stable for ties, inferred).
    std::vector<MemoryInfo> sorted = memories;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const MemoryInfo& a, const MemoryInfo& b) { return a.current > b.current; });
    float positive = 0, negative = 0, sum = 0;
    for (size_t i = 0; i < sorted.size() && i < 3; ++i)
        if (sorted[i].current > 0) {
            positive += weight(sorted[i]);
            sum += float(sorted[i].current) * weight(sorted[i]);
        }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const MemoryInfo& a, const MemoryInfo& b) { return a.current < b.current; });
    for (size_t i = 0; i < sorted.size() && i < 3; ++i)
        if (sorted[i].current < 0) {
            negative += weight(sorted[i]);
            sum += float(sorted[i].current) * weight(sorted[i]);
        }
    const float most = std::max(positive, negative);
    if (most == 0) // 0 / 0: the game's float to int of NaN keeps 0 in the low 16 bits
        return 0;
    const float average = sum / most;
    float total = 0;
    for (const auto& m : memories)
        total += (m.aspiration & aspiration) ? float(m.initial) * 2.0f : float(m.initial);
    float score;
    if (total <= 0)
        score = average - total * (average + 100.0f) / (total + 500.0f);
    else
        score = average + total * (100.0f - average) / (total + 500.0f);
    return int16_t(int32_t(score));
}

void updateMemories(VM& vm, Object& sim, bool decay) {
    if (!sim.person)
        return;
    Person& p = *sim.person;
    Inventory* inventory = inventoryOf(vm, sim);
    if (!inventory)
        return;
    if (decay) {
        auto& tokens = inventory->singular;
        if (tokens.size() > kPruneAbove) {
            // PruneMemories: invisible memories at their final value, of all
            // but the last 100 tokens, are marked and the inventory drops them.
            const size_t look = tokens.size() - kPruneKeep;
            std::vector<InventoryToken> kept;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const auto& t = tokens[i];
                const bool drop = i < look && tokenIsMemory(t) && !(t.flags & tokenflag::Visible) &&
                                  prop(t, 10) == prop(t, 8);
                if (!drop)
                    kept.push_back(t);
            }
            tokens = std::move(kept);
        }
        for (auto& t : tokens) {
            if (!tokenIsMemory(t))
                continue;
            int32_t step = std::abs(prop(t, 9));
            if (step == 0)
                continue;
            int32_t lo = prop(t, 7), hi = prop(t, 8);
            if (hi < lo) {
                step = -step;
                std::swap(lo, hi);
            }
            t.properties[10] = int16_t(std::clamp(prop(t, 10) + step, lo, hi));
        }
    }
    auto memories = memoryInfo(vm, sim);
    if (!decay && p.memoriesRead && memories == p.memories)
        return;
    p.memoriesRead = true;
    p.memories = std::move(memories);
    p.data[kPersonDataMemoryScore] = memoryScore(p.memories, p.data[kPersonDataAspiration]);
}

void tickMemories(VM& vm) {
    for (Object* o : vm.objectTable()) {
        if (!o || !o->person)
            continue;
        Person& p = *o->person;
        if (p.memoriesTick + kRefreshTicks > vm.ticks)
            continue;
        p.memoriesTick = vm.ticks;
        if (p.data[persondata::MotivesStatic] == 0)
            updateMemories(vm, *o, false);
    }
}

} // namespace os2::vm
