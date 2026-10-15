// The motive engine (motives.hpp): the per-tick motive changes, the
// 60-tick update (decay, clamp, the old set, mood), Set Motive Change
// (0x1D) and Find 5 Worst Motives (0x21). docs/motives-mac.md.

#include "motives.hpp"

#include "animation.hpp"
#include "memories.hpp"
#include "formats/binreader.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// The game's float arithmetic is reproduced operation by operation (its
// fused multiply-adds are std::fmaf): no contraction here.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace os2::vm {

bool gMotiveDecay = true;

namespace {

// Global STR#s with the curves, one per age group in curveAgeIndex order
// (elder, adult, teen, child, toddler, baby; then dog and cat, Pets only).
constexpr uint16_t kHappyCurveStrings = 0x226;
constexpr uint16_t kScoreCurveStrings = 0x28A;
constexpr int kCurvePoints = 32; // the sets give each curve room for 32 points
constexpr uint64_t kUpdateTicks = 60; // OnSimulatorTick: the motives and the memories every 60 ticks

// cTSMotiveCurveSet::LoadFromFile (base 0xb910dc, BV 0x51cadc): text i of
// the STR# is curve i; the curves past the texts are constant 1.
size_t curveTexts(const StringTable* t) {
    if (!t)
        return 0;
    const size_t n = t->count(lang::EnglishUS);
    return n ? n : t->entries.size();
}

MotiveCurveSet loadCurves(const StringTable* t) {
    MotiveCurveSet set;
    const size_t n = curveTexts(t);
    for (size_t m = 0; m < kMotiveCount; ++m)
        set[m] = m < n ? MotiveCurve::parse(t->get(m, lang::EnglishUS), kCurvePoints) : MotiveCurve::constant(1.0f);
    return set;
}

const MotiveCurveSet& curvesFrom(VM& vm, uint32_t group, uint16_t instance) {
    // Parsed once per string table. The cache is keyed by the table's
    // address, which a later code library may reuse for another table (the
    // tests make one per fixture: that was the flaky autonomy test), so the
    // texts they came from are kept and compared.
    struct Entry {
        std::vector<std::string> texts;
        MotiveCurveSet curves;
    };
    static std::map<const StringTable*, Entry> cache;
    const StringTable* t = vm.code().strings(group, instance);
    auto [it, added] = cache.try_emplace(t);
    Entry& e = it->second;
    std::vector<std::string> texts;
    for (size_t m = 0; m < kMotiveCount && m < curveTexts(t); ++m)
        texts.push_back(t->get(m, lang::EnglishUS));
    if (added || e.texts != texts) {
        e.texts = std::move(texts);
        e.curves = loadCurves(t);
    }
    return e.curves;
}

} // namespace

// cTSPiecewiseFunction's value (base 0xc43110, BV 0xa910c4): from the last
// point back to the first one left of x; on a point the segment to its
// left. Single precision, in the game's order (no fused multiply-add).
float MotiveCurve::at(float x) const {
    if (points.empty())
        return 0;
    const int last = int(points.size()) - 1;
    int i = last;
    float d = 0;
    for (; i >= 0; --i) {
        d = x - points[size_t(i)].first;
        if (d > 0)
            break;
    }
    if (i == last)
        return points[size_t(last)].second;
    if (i < 0)
        return points[0].second;
    const float y0 = points[size_t(i)].second, y1 = points[size_t(i) + 1].second;
    const float t = d * inverse[size_t(i)];
    const float dy = y1 - y0;
    const float ty = t * dy;
    return ty + y0;
}

// AddPoint (base 0xc42b9c, BV 0xa919a4): nothing when full or when a point
// has that x already; else it goes in its place and the widths are redone.
void MotiveCurve::add(float x, float y) {
    if (maxPoints >= 0 && int(points.size()) == maxPoints)
        return;
    size_t at = 0;
    for (; at < points.size(); ++at) {
        if (points[at].first == x)
            return;
        if (x < points[at].first)
            break;
    }
    points.insert(points.begin() + std::ptrdiff_t(at), {x, y});
    inverse.assign(points.size() - 1, 0.0f);
    for (size_t k = 0; k + 1 < points.size(); ++k)
        inverse[k] = 1.0f / (points[k + 1].first - points[k].first);
}

// The text reader (base 0xc42f00, BV 0xa91d74): every "(" ... ")" is read
// with sscanf("(%f;%f)"); one that does not give two numbers is skipped.
MotiveCurve MotiveCurve::parse(const std::string& text, int maxPoints) {
    MotiveCurve c;
    c.maxPoints = maxPoints;
    size_t at = 0;
    for (;;) {
        at = text.find('(', at);
        if (at == std::string::npos)
            break;
        const size_t close = text.find(')', at);
        if (close == std::string::npos || close == at)
            break;
        float x = 0, y = 0;
        if (std::sscanf(text.substr(at, close - at + 1).c_str(), "(%f;%f)", &x, &y) == 2)
            c.add(x, y);
        at = close + 1;
    }
    return c;
}

MotiveCurve MotiveCurve::constant(float y) {
    MotiveCurve c;
    c.maxPoints = kCurvePoints;
    c.add(0.0f, y);
    return c;
}

// The person's age tests (base cTSPerson vtable 0x690-0x6a4 on person data
// 58), asked from the elder down; none true (a negative age) is the adult.
int curveAgeIndex(const Object& sim) {
    return curveAgeIndexOf(sim.person ? sim.person->data[persondata::PersonAge] : lifestage::Adult);
}

int curveAgeIndexOf(int16_t age) {
    if (age > 0x32)
        return 0;
    if (age >= lifestage::Adult)
        return 1;
    if (age >= lifestage::Teen)
        return 2;
    if (age >= lifestage::Child)
        return 3;
    if (age == lifestage::Toddler)
        return 4;
    if (age >= 0)
        return 5;
    return 1;
}

const MotiveCurveSet& happyCurves(VM& vm, const Object& sim) { return happyCurvesOfAge(vm, curveAgeIndex(sim)); }

const MotiveCurveSet& happyCurvesOfAge(VM& vm, int ageIndex) {
    return curvesFrom(vm, kGlobalGroup, uint16_t(kHappyCurveStrings + ageIndex));
}

const MotiveCurveSet& scoreCurves(VM& vm, const Object& sim) {
    // cTSPerson::LoadMotiveEffects (base 0xb773b0): the definition's field
    // 0x33 "motive effects id", a STR# of the object's own group (in the
    // base game only a window has one), else the age's.
    if (sim.definition)
        if (const uint16_t own = sim.definition->field(0x40 + 2 * 0x33))
            return curvesFrom(vm, sim.group, own);
    return curvesFrom(vm, kGlobalGroup, uint16_t(kScoreCurveStrings + curveAgeIndex(sim)));
}

// The base game's UpdateMood (cTSPerson, base 0xb76208; docs/motives-mac.md
// §4): the motives but 0-2 and 10-12 (and person data 118's), each
// weighted by its happy curve at its value (fmadds), plus person data 163
// "Mood Booster"; not clamped. (BV's cTSMotives::UpdateMood leaves out
// only 2 and 10-12 and clamps to +-100: base data gives 0-5 and 10-12 no
// weight, so only the clamp differs.)
float moodFrom(const MotiveCurveSet& curves, const std::array<float, kMotiveCount>& motives, int16_t notInMood,
               int16_t booster) {
    constexpr uint32_t kNotMood = 0x1C07; // 0, 1, 2, 10, 11, 12
    float sum = 0, weights = 0;
    for (int16_t m = 0; m < int16_t(kMotiveCount); ++m) {
        if (m <= 12 && (kNotMood & (1u << m)))
            continue;
        if (notInMood & (1 << m))
            continue;
        const float v = motives[size_t(m)];
        const float w = curves[size_t(m)].at(v);
        sum = std::fmaf(v, w, sum);
        weights = weights + w;
    }
    // (The game divides even by 0; here no weight at all is a mood of 0.)
    const float mean = weights != 0.0f ? sum / weights : 0.0f;
    return mean + float(booster);
}

float computeMood(VM& vm, Object& sim) {
    if (!sim.person)
        return 0;
    std::array<float, kMotiveCount> motives{};
    for (size_t k = 0; k < kMotiveCount; ++k)
        motives[k] = exactMotive(sim, int16_t(k));
    return moodFrom(happyCurves(vm, sim), motives, sim.person->data[persondata::MotivesNotInMood],
                    sim.person->data[persondata::MoodBooster]);
}

int16_t motiveView(float v) {
    // cTSObject::InterpValue (BV 0xb15f2c): (int)(v + 0.5), or - 0.5 below
    // 0, as a short; not clamped.
    const double r = v >= 0.0f ? double(v) + 0.5 : double(v) - 0.5;
    if (!(r > -2147483648.0 && r < 2147483648.0))
        return 0;
    return int16_t(int32_t(r));
}

float exactMotive(Object& sim, int16_t m) {
    Person& p = *sim.person;
    if (!p.exactMotivesValid) {
        for (size_t k = 0; k < kMotiveCount; ++k)
            p.exactMotives[k] = p.writtenMotives[k] = p.motives[k];
        p.exactMotivesValid = true;
    }
    if (!p.oldMotivesValid) {
        p.oldMotives = p.exactMotives;
        p.oldMotivesValid = true;
    }
    const auto k = size_t(m);
    if (p.motives[k] != p.writtenMotives[k]) // a tree wrote it: the game's float takes the value as it is
        p.exactMotives[k] = p.writtenMotives[k] = p.motives[k];
    return p.exactMotives[k];
}

float motiveNow(const Object& sim, int16_t m) {
    const Person& p = *sim.person;
    const auto k = size_t(m);
    if (!p.exactMotivesValid || p.motives[k] != p.writtenMotives[k])
        return p.motives[k];
    return p.exactMotives[k];
}

void setExactMotive(Object& sim, int16_t m, float value) {
    exactMotive(sim, m);
    Person& p = *sim.person;
    const auto k = size_t(m);
    p.exactMotives[k] = value;
    p.motives[k] = p.writtenMotives[k] = motiveView(value);
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

void updateMotives(Object& sim) {
    Person& p = *sim.person;
    // The daily decays, a 720th each (2 game minutes of 24 h), in the
    // base game's order and single precision: (float(data) / 24) / 30.
    static constexpr std::pair<int16_t, int16_t> kDecay[] = {
        {motive::Fun, 101},   {motive::Hygiene, 97}, {motive::Bladder, 95}, {motive::Social, 99},
        {motive::Hunger, 93}, {motive::Energy, 96},  {motive::Comfort, 94}};
    for (size_t k = 0; k < kMotiveCount; ++k)
        exactMotive(sim, int16_t(k));
    if (gMotiveDecay)
        for (const auto& [m, d] : kDecay) {
            const float perDay = float(p.data[size_t(d)]);
            const float perHour = perDay / 24.0f;
            const float step = perHour / 30.0f;
            p.exactMotives[size_t(m)] = p.exactMotives[size_t(m)] + step;
        }
    // The environment (13) would come from the environment score manager
    // (not modelled: it keeps its value). Every motive within -100..100,
    // and the old set takes them.
    for (size_t k = 0; k < kMotiveCount; ++k) {
        float& v = p.exactMotives[k];
        if (v > 100.0f)
            v = 100.0f;
        else if (v < -100.0f)
            v = -100.0f;
        p.oldMotives[k] = v;
        p.motives[k] = p.writtenMotives[k] = motiveView(v);
    }
}

void tickMotives(VM& vm) {
    for (Object* po : vm.objectTable()) {
        if (!po || !po->person)
            continue;
        Object& sim = *po;
        Person& p = *sim.person;
        // cTSPerson::OnSimulatorTick (base 0xb876bc, BV 0xad1040).
        if (p.memoriesTick + kUpdateTicks <= vm.ticks) {
            p.memoriesTick = vm.ticks;
            if (p.data[persondata::MotivesStatic] == 0) {
                updateMotives(sim);
                // UpdateMood; Mood+Mem is the mood (the memories do not
                // enter it; their score is person data 54, memories.cpp;
                // docs/relationships-mac.md §4).
                setExactMotive(sim, motive::Mood, computeMood(vm, sim));
                updateMemories(vm, sim, false);
                setExactMotive(sim, motive::MoodMem, exactMotive(sim, motive::Mood));
            }
        }
        // Person data 27 "tick counter" counts the ticks (the Main's
        // "Divide Tick Counts" reads and resets it).
        p.data[persondata::TickCounter] = int16_t(p.data[persondata::TickCounter] + 1);
        // Set Motive Change: up (or down) to the limit, without passing it.
        if (p.data[persondata::MotivesStatic] < 2)
            for (const MotiveChange& ch : p.motiveChanges) {
                const float v = exactMotive(sim, ch.motive);
                float next = v;
                if (ch.perTick >= 0) {
                    if (v < ch.limit) {
                        next = v + ch.perTick;
                        if (ch.limit < next)
                            next = ch.limit;
                    }
                } else if (ch.limit < v) {
                    next = v + ch.perTick;
                    if (next < ch.limit)
                        next = ch.limit;
                }
                if (next != v)
                    setExactMotive(sim, ch.motive, next);
            }
        // Person data 1 "Money Amount over Head" is cleared.
        if (p.data[persondata::MoneyOverHead] != 0)
            p.data[persondata::MoneyOverHead] = 0;
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
