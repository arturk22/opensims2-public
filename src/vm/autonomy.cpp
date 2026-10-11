// Find Best Interaction (0x03) and Gosub Found Action (0x1E): autonomy.hpp.

#include "errors.hpp"
#include "engine.hpp"
#include "autonomy.hpp"

#include "formats/binreader.hpp"
#include "interactions.hpp"
#include "motives.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace os2::vm {

namespace {

// The global FCNS 2 (objects.package, group 0x7FD46CD0) autonomy constants.
constexpr float kAttenuation[] = {0.0f, 0.0f, 0.1f, 0.3f, 0.6f}; // custom, none, low, moderate, high
constexpr float kFloorToFloorDistance = 10.0f;
constexpr size_t kMaxCheckTrees = 200;
constexpr float kMinScore = 1e-5f; // FCNS 2 "min autonomy score" is 0; the binary also needs 1e-5
constexpr size_t kMemoryCount = 8;        // "autonomy memory count"
constexpr uint64_t kMemoryDuration = 9999; // "max duration autonomy memory", taken as ticks (inferred)

// Whether the Sim lately ran this interaction autonomously and it failed
// (its action tree returned false, e.g. no route): it is not picked again
// while remembered (inferred use of the autonomy memory).
bool rememberedFailure(const VM& vm, const Object& sim, ObjectId object, uint16_t interaction) {
    for (const auto& m : sim.person->autonomyMemory)
        if (m.failed && m.object == object && m.interaction == interaction && vm.ticks - m.tick <= kMemoryDuration)
            return true;
    return false;
}

constexpr int16_t kObjectDataTreeTableEntry = 20;
constexpr int16_t kObjectDataUseCount = 62;

// The Sim's age as the engine numbers it (cITSPerson::eSimAge, from the
// Mac binary's cTSPerson::GetAvailabilityBitset and the TTAB data): 0
// adult, 1 child, 2 toddler, 3 teen, 4 elder, 5 baby. It picks the ad table
// (cTSTreeTableEntry::GetAdScoreForHuman) and the availability bit.
int simAge(const Object& sim) {
    switch (curveAgeIndex(sim)) {
    case 0: return 4;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 5: return 5;
    default: return 0;
    }
}

// cTSPerson::GetAvailabilityBitset: the TTAB flag an entry needs for the
// Sim's age (adult 0x40, child 0x10, toddler 0x200, teen 0x800, elder
// 0x400, baby 0x1000). TTABs before version 0x52 store 0x10 and 0x40
// inverted (cTSTreeTableEntry::Read flips them), so in the base game's
// tables a set bit 4/6 means "not for children/adults" (the teen runaway
// entry, 0x850, is for teens only).
bool availableForAge(const Object& sim, const InteractionTable& table, const InteractionTable::Interaction& e) {
    if (!sim.person || sim.person->data[persondata::PersonAge] == 0)
        return true; // not a Sim, or an age nobody set (stand-ins, tests)
    static constexpr uint32_t kBits[6] = {0x40, 0x10, 0x200, 0x800, 0x400, 0x1000};
    uint32_t flags = uint32_t(e.flags) | uint32_t(e.flags2) << 16;
    if (table.version < 0x52)
        flags ^= 0x10 | 0x40;
    return flags & kBits[simAge(sim)];
}

struct Ads {
    std::array<int16_t, kMotiveCount> min{}, range{}, personality{};
};

Ads adsOf(const Object& sim, const InteractionTable::Interaction& e) {
    Ads a;
    if (e.motives.empty())
        return a;
    const auto age = size_t(simAge(sim));
    if (age >= e.motives.size())
        return a;
    const auto& table = e.motives[age];
    for (size_t m = 0; m < table.size() && m < kMotiveCount; ++m) {
        a.min[m] = table[m].minimum;
        a.range[m] = table[m].delta;
        a.personality[m] = int16_t(table[m].personality);
    }
    return a;
}

// The motives a human's interaction score sums (FUN_01be21b4 in the Mac
// binary: every motive but 1, 2, 4, 10, 11 and 12).
constexpr std::array<int16_t, 10> kScoredMotives = {0, 3, 5, 6, 7, 8, 9, 13, 14, 15};

// cTSMotiveEffects::GetInteractionScore - GetCurrentScore (Mac binary):
// the sum over the scored motives of curve(motive + ad / 1000) -
// curve(motive), divided by 10 (the motive count, 16, minus 6). The mask
// only chooses the candidates.
float scoreAds(VM& vm, const Object& sim, const Ads& a) {
    const auto& curves = scoreCurves(vm, sim);
    float score = 0;
    for (int16_t m : kScoredMotives) {
        const auto k = size_t(m);
        if (!a.min[k] && !a.range[k])
            continue;
        const float ad = adValue(sim, a.min[k], a.range[k], uint16_t(a.personality[k])) * 0.001f;
        const float v = sim.person->motives[k];
        score += curves[k].at(v + ad) - curves[k].at(v);
    }
    return score / 10.0f;
}

// The score is multiplied by 1 / (1 + attenuation x distance)
// ((anonymous namespace)::CalcAttenuation; "autonomy attenuation formula" 1
// would use min(1, tuning / (...)) instead, but it is 0).
float attenuationOf(const InteractionTable::Interaction& e) {
    if (e.attenuationCode == 0)
        return e.attenuationValue;
    return e.attenuationCode < std::size(kAttenuation) ? kAttenuation[e.attenuationCode] : 0.0f;
}

// The biases cTSPerson::FBA_ScoreInteraction multiplies the attenuated
// score by (Mac binary, Ghidra headless), as they come out in the base game:
// - ownership, x (1 + GetOwnerShipBias): 0.1 x relationship variable 1 of
//   the interaction leader towards the Sim, when that relationship has more
//   than one variable (the binary also needs bit 0 of a definition field,
//   GetDef()->field 0x19, not identified: not checked here; objects with
//   relationships to Sims are rare in the base game);
// - GetAutonomyBias (the autonomy memory and "interaction is for my
//   family"): only with bit 0x04 of a tuning word that is 1 in the binary's
//   data: 1;
// - GetRelationshipBias: 1 + 0.01 x global constant 0x15B entry 11 for
//   Sims with relationship flag 0x800, but that BCON is not in the base
//   game: 1;
// - CalcWeatherFactor and GetTemperatureBias: weather and temperature
//   (expansion packs; GetTemperatureBias needs an expansion flag): 1.
float ownershipFactor(VM& vm, const Object& sim, ObjectId leader) {
    const auto& rels = vm.relationships;
    auto it = rels.find({vm.relationshipKey(leader), vm.relationshipKey(sim.id)});
    if (it == rels.end() || it->second.size() < 2)
        return 1.0f;
    return 1.0f + 0.1f * float(it->second[1]);
}

float distanceBetween(const Object& a, const Object& b) {
    const float dx = a.position.x - b.position.x, dy = a.position.y - b.position.y;
    return std::sqrt(dx * dx + dy * dy) + kFloorToFloorDistance * float(std::abs(a.position.level - b.position.level));
}

struct Candidate {
    ObjectId object = 0;
    const InteractionTable::Interaction* entry = nullptr;
    std::string name;
    float staticScore = 0;
    float distance = 0;
    float bias = 1; // the FBA_ScoreInteraction biases (ownershipFactor)
};

// Runs a candidate's check tree for autonomy: parameter 0 = 1, the ads in
// the thread for data owners 0x0C/0x1C/0x1D; true if it passes (or there
// is none). `ads` gets what the tree left, `args` the stack object of the
// first action string it added (the action tree's parameter 0).
bool runCheckTree(VM& vm, ObjectId sim, const Candidate& c, Ads& ads, std::array<int16_t, 4>& args,
                  std::vector<std::string>& errors) {
    if (!c.entry->guard)
        return true;
    const int16_t params[1] = {1};
    Thread t;
    try {
        t = vm.start(sim, c.entry->guard, params, c.object, c.object);
    } catch (const VMError&) {
        return false;
    }
    std::vector<ActionString> strings;
    t.actionStrings = &strings;
    t.adMin = ads.min;
    t.adRange = ads.range;
    t.adPersonality = ads.personality;
    if (auto it = vm.threads.find(sim); it != vm.threads.end())
        t.temps = it->second.temps;
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < 100; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    if (Object* s = vm.object(sim))
        s->data[objdata::HideInteraction] = 0;
    if (t.status == Thread::Status::Failed)
        errors.push_back(c.name + ": " + t.error.substr(0, t.error.find('\n')));
    else if (t.status == Thread::Status::Running)
        errors.push_back(c.name + ": still running");
    if (t.status != Thread::Status::Finished || !t.result)
        return false;
    ads.min = t.adMin;
    ads.range = t.adRange;
    ads.personality = t.adPersonality;
    if (!strings.empty())
        args[0] = strings.front().stackObject;
    return true;
}

// Find Best Interaction (0x03): autonomy.hpp.
Exit findBestInteraction(Context& c) {
    VM& vm = c.vm;
    const auto& o = c.instruction.operands;
    const ObjectId simId = c.thread.me;
    Object& sim = c.me();
    if (!sim.person)
        throw VMError("Find Best Interaction: object " + std::to_string(sim.id) + " is not a person");
    uint16_t mask = le16(&o[0]);
    if (mask == 0)
        for (int16_t m : motive::kPhysicalAndMental)
            mask = uint16_t(mask | (1u << m));
    const uint8_t flags = o[3];

    AutonomyDecision decision;
    decision.sim = simId;
    decision.motiveMask = mask;
    auto report = [&](bool found) {
        decision.found = found;
        if (auto& obs = autonomyObserver())
            obs(decision);
        return found ? Exit::True : Exit::False;
    };
    if (flags & 0x08)
        return report(false); // nested interactions only: not done

    const Object* from = &sim;
    if (flags & 0x20)
        if (const Object* x = vm.object(c.thread.temps[1]))
            from = x;
    const int16_t room = c.thread.temps[0];
    const int16_t current = sim.person->data[persondata::CurrentInteractionObject];
    const int16_t autonomyLevel = sim.person->data[persondata::AutonomyLevel];

    // Static scores: the TTABs' own ads.
    std::vector<Candidate> candidates;
    std::set<ObjectId> seen;
    std::vector<ObjectId> ids;
    for (const auto& [id, x] : vm.objects())
        ids.push_back(id);
    for (ObjectId id : ids) {
        const Object* x = vm.object(id);
        if (!x || id == simId || (!x->position.inWorld() && !(flags & 0x04)))
            continue;
        const ObjectId leaderId = interactionLeader(vm, id);
        if (!seen.insert(leaderId).second)
            continue;
        const Object* leader = vm.object(leaderId);
        if (!leader || leaderId == simId)
            continue;
        if ((flags & 0x02) && x->data[objdata::Room] != room)
            continue;
        if ((flags & 0x10) && leaderId != current)
            continue;
        if (useCount(vm, *leader) > 0)
            continue;
        const auto ref = interactionTable(vm, *leader);
        if (!ref)
            continue;
        const float distance = distanceBetween(*from, *leader);
        for (const auto& e : ref->table->interactions) {
            if ((e.flags & 0x80) || int64_t(e.autonomy) > autonomyLevel || !availableForAge(sim, *ref->table, e))
                continue;
            if (rememberedFailure(vm, sim, leaderId, uint16_t(e.stringIndex)))
                continue;
            const Ads ads = adsOf(sim, e);
            bool advertises = false;
            for (size_t m = 0; m < kMotiveCount; ++m)
                advertises |= (mask & (1u << m)) && (ads.min[m] || ads.range[m]);
            if (!advertises)
                continue;
            Candidate cand;
            cand.object = leaderId;
            cand.entry = &e;
            cand.distance = distance;
            cand.bias = ownershipFactor(vm, sim, leaderId);
            cand.staticScore = scoreAds(vm, sim, ads) / (1.0f + attenuationOf(e) * distance) * cand.bias;
            if (ref->strings)
                cand.name = ref->strings->get(e.stringIndex, lang::BrazilianPortuguese);
            candidates.push_back(std::move(cand));
        }
    }
    decision.candidates = candidates.size();
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.staticScore > b.staticScore; });

    // Check trees, best static score first; the ads they leave score again.
    struct Passed {
        const Candidate* cand;
        float score;
        std::array<int16_t, 4> args;
    };
    std::vector<Passed> passed;
    for (const Candidate& cand : candidates) {
        if (decision.checked >= kMaxCheckTrees)
            break;
        if (!vm.object(cand.object) || !vm.object(simId))
            continue;
        Ads ads = adsOf(sim, *cand.entry);
        std::array<int16_t, 4> args{};
        ++decision.checked;
        if (!runCheckTree(vm, simId, cand, ads, args, decision.checkErrors))
            continue;
        Object* s = vm.object(simId);
        if (!s)
            return Exit::False;
        const float score = scoreAds(vm, *s, ads) / (1.0f + attenuationOf(*cand.entry) * cand.distance) * cand.bias;
        if (score > kMinScore)
            passed.push_back({&cand, score, args});
    }
    std::stable_sort(passed.begin(), passed.end(), [](const Passed& a, const Passed& b) { return a.score > b.score; });
    for (const Passed& p : passed)
        decision.best.push_back({p.cand->object, uint16_t(p.cand->entry->stringIndex), p.cand->name, p.score, p.cand->distance});
    Object* s = vm.object(simId);
    if (passed.empty() || !s)
        return report(false);
    const Passed& win = passed.front();
    s->person->data[persondata::FindBestActionObject] = win.cand->object;
    s->data[kObjectDataTreeTableEntry] = int16_t(win.cand->entry->stringIndex);
    s->person->foundActionArgs = win.args;
    c.frame.stackObject = win.cand->object;
    return report(true);
}

// Gosub Found Action (0x1E): u16 at 0 (STR# 0x1FE): 0 the action Find Best
// Interaction found; 1-3 (interrupt, reaction, top user action) are false.
// The interaction becomes the Sim's current one (autonomous priority, first
// in its queue) and its action tree runs on top of this tree; the
// instruction branches on what it returns.
Exit gosubFoundAction(Context& c) {
    const auto& o = c.instruction.operands;
    // cTSPerson::TryGosubAction: no stack object is error 0x15; modes 1-3
    // (interrupt, reaction, top user action) return a code the binary does
    // nothing else with (-1; taken as false; no tree of the base game uses
    // them); others are error 0x0D.
    const auto mode = int16_t(le16(&o[0]));
    if (mode < 0 || mode > 3)
        throw gameError(err::FlagOutOfRange, "Gosub Found Action mode " + std::to_string(mode));
    if (mode != 0)
        return Exit::False;
    Object& sim = c.me();
    if (!sim.person)
        throw VMError("Gosub Found Action: object " + std::to_string(sim.id) + " is not a person");
    const ObjectId object = sim.person->data[persondata::FindBestActionObject];
    auto q = c.vm.object(object) ? makeInteraction(c.vm, object, uint16_t(sim.data[kObjectDataTreeTableEntry]))
                                 : std::nullopt;
    if (!q)
        throw VMError("Last Find Best Action was Not Successful. Quit trying to run gosub.");
    q->priority = kPriorityAutonomous;
    q->pushedBy = sim.id;
    q->args = sim.person->foundActionArgs;
    if (sim.nextInteractionUid == 0)
        ++sim.nextInteractionUid;
    q->uid = sim.nextInteractionUid++;
    const auto at = size_t(sim.activeInteraction + 1);
    const QueuedInteraction item = *q;
    sim.queue.insert(sim.queue.begin() + std::ptrdiff_t(std::min(at, sim.queue.size())), std::move(*q));
    sim.activeInteraction = int(std::min(at, sim.queue.size() - 1));
    sim.person->data[persondata::Priority] = kPriorityAutonomous;
    syncQueueData(sim);
    c.vm.call(c.thread, item.object, item.action, item.args);
    Frame& f = c.thread.stack.back();
    f.stackObject = item.object;
    f.interaction = true;
    f.foundAction = true;
    return Exit::Call;
}

} // namespace

void foundActionReturned(VM& vm, Thread& t, bool result) {
    Object* sim = vm.object(t.me);
    if (!sim || !sim->person)
        return;
    auto& memory = sim->person->autonomyMemory;
    memory.push_back({sim->person->data[persondata::FindBestActionObject], uint16_t(sim->data[kObjectDataTreeTableEntry]),
                      vm.ticks, !result});
    if (memory.size() > kMemoryCount)
        memory.erase(memory.begin());
}

bool availableToAge(const Object& sim, const InteractionTable& table, const InteractionTable::Interaction& e) {
    return availableForAge(sim, table, e);
}

std::function<void(const AutonomyDecision&)>& autonomyObserver() {
    static std::function<void(const AutonomyDecision&)> observer;
    return observer;
}

float adValue(const Object& sim, int16_t min, int16_t range, uint16_t personality) {
    // The Mac binary's table at 0x02734540 (FUN_0049ee38): STR# 0xE8
    // "personality ads" type -> person data, inverted: 1/2 nice (2),
    // 3/4 active (3), 5/6 generous (none: 0), 7/8 playful (5), 9/10 outgoing
    // (6), 11/12 neat (7) (the even ones inverted: 1000 - value), 13
    // cleaning (9), 14 cooking (10), 15 social (11), 16 repair (12), 17
    // gardening (13), 18 music (14), 19 creative (15), 20 literacy (16), 21
    // physical (17), 22 logic (18). Type 0: min + range.
    static constexpr int16_t kData[23] = {-1, 2, 2, 3, 3, -1, -1, 5, 5, 6, 6, 7, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
    if (personality == 0)
        return float(min) + float(range);
    if (personality >= std::size(kData))
        return 0;
    float p = 0;
    if (kData[personality] >= 0 && sim.person)
        p = float(sim.person->data[size_t(kData[personality])]);
    if (personality < 13 && personality % 2 == 0)
        p = 1000.0f - p;
    return float(min) + float(range) * p * 0.001f;
}

std::optional<Exit> idleSimsCall(Context& c, uint8_t call) {
    switch (call) {
    case 43: return Exit::True;
    case 52: {
        const Object& me = c.me();
        if (!c.vm.world || !me.position.inWorld())
            return Exit::False;
        const TilePos here = VM::tileOf(me);
        const auto [dx, dy] = directionStep(c.thread.temps[0]);
        const TilePos next{here.x + dx, here.y + dy, here.level};
        const World& w = *c.vm.world;
        if (!w.contains(next) || (w.lineOfSight(here, next) && !w.diagonalWall(next)))
            return Exit::False;
        const auto on = c.vm.objectsOnTile(next);
        c.thread.temps[1] = on.empty() ? 0 : on.front();
        return Exit::True;
    }
    default: return std::nullopt;
    }
}

void registerAutonomyPrimitives(VM& vm) {
    vm.setPrimitive(0x03, findBestInteraction);
    vm.setPrimitive(0x1E, gosubFoundAction);
}

} // namespace os2::vm
