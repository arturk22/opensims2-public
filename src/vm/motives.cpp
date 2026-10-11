// The motive engine (motives.hpp): the per-tick motive changes and decay,
// mood, Set Motive Change (0x1D) and Find 5 Worst Motives (0x21).

#include "motives.hpp"

#include "animation.hpp"
#include "formats/binreader.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace os2::vm {

bool gMotiveDecay = true;

namespace {

// Global STR#s with the curves, one per age group in curveAgeIndex order
// (elder, adult, teen, child, toddler, baby; then dog and cat, Pets only).
constexpr uint16_t kHappyCurveStrings = 0x226;
constexpr uint16_t kScoreCurveStrings = 0x28A;

const MotiveCurveSet& curvesFrom(VM& vm, uint16_t instance) {
    // Parsed once per string table (the code library keeps them alive).
    static std::map<const StringTable*, MotiveCurveSet> cache;
    static const MotiveCurveSet empty{};
    const StringTable* t = vm.code().strings(kGlobalGroup, instance);
    if (!t)
        return empty;
    auto [it, added] = cache.try_emplace(t);
    if (added)
        for (size_t m = 0; m < kMotiveCount; ++m)
            it->second[m] = MotiveCurve::parse(t->get(m, lang::EnglishUS));
    return it->second;
}

} // namespace

float MotiveCurve::at(float x) const {
    if (points.empty())
        return 0;
    if (x <= points.front().first)
        return points.front().second;
    if (x >= points.back().first)
        return points.back().second;
    for (size_t i = 1; i < points.size(); ++i) {
        const auto [x1, y1] = points[i];
        if (x <= x1) {
            const auto [x0, y0] = points[i - 1];
            return x1 == x0 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
    }
    return points.back().second;
}

MotiveCurve MotiveCurve::parse(const std::string& text) {
    MotiveCurve c;
    for (size_t at = 0; (at = text.find('(', at)) != std::string::npos;) {
        const size_t semi = text.find(';', at), close = text.find(')', at);
        if (semi == std::string::npos || close == std::string::npos || semi > close)
            break;
        const float x = std::strtof(text.c_str() + at + 1, nullptr);
        const float y = std::strtof(text.c_str() + semi + 1, nullptr);
        c.points.emplace_back(x, y);
        at = close + 1;
    }
    // A few unused motives list their points out of order.
    std::stable_sort(c.points.begin(), c.points.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return c;
}

int curveAgeIndex(const Object& sim) {
    const int16_t age = sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult;
    if (age <= lifestage::Baby)
        return 5;
    if (age == lifestage::Toddler)
        return 4;
    if (age < lifestage::Teen)
        return 3;
    if (age < lifestage::Adult)
        return 2;
    if (age < lifestage::Elder)
        return 1;
    return 0;
}

const MotiveCurveSet& happyCurves(VM& vm, const Object& sim) {
    return curvesFrom(vm, uint16_t(kHappyCurveStrings + curveAgeIndex(sim)));
}

const MotiveCurveSet& scoreCurves(VM& vm, const Object& sim) {
    return curvesFrom(vm, uint16_t(kScoreCurveStrings + curveAgeIndex(sim)));
}

float computeMood(VM& vm, const Object& sim) {
    if (!sim.person)
        return 0;
    const auto& curves = happyCurves(vm, sim);
    const int16_t excluded = sim.person->data[persondata::MotivesNotInMood];
    float sum = 0, weights = 0;
    for (int16_t m = motive::Energy; m < int16_t(kMotiveCount); ++m) {
        if (excluded & (1 << m))
            continue;
        const float v = sim.person->motives[size_t(m)];
        const float w = curves[size_t(m)].at(v);
        sum += w * v;
        weights += w;
    }
    return weights > 0 ? std::clamp(sum / weights, -100.0f, 100.0f) : 0.0f;
}

float exactMotive(Object& sim, int16_t m) {
    Person& p = *sim.person;
    if (!p.exactMotivesValid) {
        for (size_t k = 0; k < kMotiveCount; ++k)
            p.exactMotives[k] = p.writtenMotives[k] = p.motives[k];
        p.exactMotivesValid = true;
    }
    const auto k = size_t(m);
    if (p.motives[k] != p.writtenMotives[k]) // a tree wrote it
        p.exactMotives[k] = p.writtenMotives[k] = p.motives[k];
    return p.exactMotives[k];
}

void setExactMotive(Object& sim, int16_t m, float value) {
    exactMotive(sim, m);
    Person& p = *sim.person;
    const auto k = size_t(m);
    p.exactMotives[k] = std::clamp(value, -100.0f, 100.0f);
    p.motives[k] = p.writtenMotives[k] = int16_t(std::lround(p.exactMotives[k]));
}

int16_t decayDataOf(int16_t m) {
    switch (m) {
    case motive::Hunger: return 93;
    case motive::Comfort: return 94;
    case motive::Bladder: return 95;
    case motive::Energy: return 96;
    case motive::Hygiene: return 97;
    case motive::Social: return 99;
    case motive::Fun: return 101;
    case 0: return 100; // Bon Voyage's (docs/primitives-mac.md)
    default: return -1;
    }
}

void tickMotives(VM& vm) {
    const float ticksPerDay = 24.0f * 60.0f * float(vm.clock.ticksPerMinute);
    for (const auto& [id, cobj] : vm.objects()) {
        if (!cobj.person)
            continue;
        Object& sim = *vm.object(id);
        Person& p = *sim.person;
        // cTSPerson::OnSimulatorTick: person data 27 "tick counter" counts
        // the ticks (the Main's "Divide Tick Counts" reads and resets it) and
        // 1 "Money Amount over Head" is cleared.
        p.data[persondata::TickCounter] = int16_t(p.data[persondata::TickCounter] + 1);
        p.data[persondata::MoneyOverHead] = 0;
        const int16_t isStatic = p.data[persondata::MotivesStatic];
        if (isStatic >= 2)
            continue;
        // Set Motive Change: up (or down) to the limit, without passing it.
        for (const MotiveChange& ch : p.motiveChanges) {
            const float v = exactMotive(sim, ch.motive);
            float next = v + ch.perTick;
            if (ch.perTick >= 0)
                next = v >= ch.limit ? v : std::min(next, ch.limit);
            else
                next = v <= ch.limit ? v : std::max(next, ch.limit);
            if (next != v)
                setExactMotive(sim, ch.motive, next);
        }
        // Daily decay (inferred: engine side, see motives.hpp).
        if (gMotiveDecay && isStatic == 0)
            for (int16_t m : motive::kPhysicalAndMental) {
                const int16_t d = decayDataOf(m);
                if (d < 0 || p.data[size_t(d)] == 0)
                    continue;
                setExactMotive(sim, m, exactMotive(sim, m) + float(p.data[size_t(d)]) / ticksPerDay);
            }
        // Mood (cTSMotives::UpdateMood); Mood+Mem adds the memories, which
        // are not simulated, so it follows the mood (inferred).
        const float mood = computeMood(vm, sim);
        setExactMotive(sim, motive::Mood, mood);
        setExactMotive(sim, motive::MoodMem, mood);
    }
}

namespace {

// Set Motive Change (0x1D), docs/primitives-mac.md: delta per hour (owner u8
// at 0, data u16 at 4), limit (owner u8 at 1, data u16 at 6), motive u8 at 2,
// flags u8 at 3: 0x01 clears every change, 0x02 zeroes the motive's daily
// decay. A new change of a motive replaces the one running (inferred).
Exit setMotiveChange(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    if (!me.person)
        throw VMError("Set Motive Change: object " + std::to_string(me.id) + " is not a person");
    Person& p = *me.person;
    if (o[3] & 0x01) {
        p.motiveChanges.clear();
        return Exit::True;
    }
    const int16_t m = o[2];
    if (!(m == 0 || m == 1 || m == 5 || m == 6 || m == 7 || m == 8 || m == 9 || m == 14 || m == 15))
        throw VMError("Set Motive Change: Motive number out of range or not an acceptable value (" + std::to_string(m) + ")");
    const float perHour = c.get(o[0], int16_t(le16(&o[4])));
    const float limit = c.get(o[1], int16_t(le16(&o[6])));
    MotiveChange ch{m, perHour / 60.0f / float(c.vm.clock.ticksPerMinute), limit};
    auto it = std::find_if(p.motiveChanges.begin(), p.motiveChanges.end(),
                           [&](const MotiveChange& x) { return x.motive == m; });
    if (it != p.motiveChanges.end())
        *it = ch;
    else
        p.motiveChanges.push_back(ch);
    if (o[3] & 0x02)
        if (const int16_t d = decayDataOf(m); d >= 0)
            p.data[size_t(d)] = 0;
    return Exit::True;
}

// Find 5 Worst Motives (0x21): whose u16 at 4 (0 me, 1 the stack object),
// type u16 at 6 (all/mental/physical: validated, not used, as in the
// binary); the five lowest of the eight motives go to temps 0-4.
Exit find5WorstMotives(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t who = le16(&o[4]), type = le16(&o[6]);
    if (type > 2)
        throw VMError("Find 5 Worst Motives: Search type is invalid (" + std::to_string(type) + ")");
    Object& sim = who == 0 ? c.me() : c.stackObject();
    if (!sim.person)
        throw VMError("Find 5 Worst Motives: object " + std::to_string(sim.id) + " is not a person");
    std::array<int16_t, 8> ms = motive::kPhysicalAndMental;
    std::stable_sort(ms.begin(), ms.end(), [&](int16_t a, int16_t b) {
        return sim.person->motives[size_t(a)] < sim.person->motives[size_t(b)];
    });
    for (size_t i = 0; i < 5; ++i)
        c.thread.temps[i] = ms[i];
    return Exit::True;
}

} // namespace

void registerMotivePrimitives(VM& vm) {
    vm.setPrimitive(0x1D, setMotiveChange);
    vm.setPrimitive(0x21, find5WorstMotives);
}

} // namespace os2::vm
