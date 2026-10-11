// Find Best Interaction (0x03) and Gosub Found Action (0x1E): autonomy.hpp.

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
constexpr float kMinScore = 0.0f;

constexpr int16_t kObjectDataTreeTableEntry = 20;
constexpr int16_t kObjectDataUseCount = 62;

// The ad table of a Sim's age: TTABs from version 0x44 have seven, taken to
// follow the age bits of the game's property sets (toddler, child, teen,
// adult, elder, baby, young adult); an entry whose table for the age is
// empty uses the first one (inferred, docs/data-findings.md).
size_t adTableOf(const Object& sim, const InteractionTable::Interaction& e) {
    if (e.motives.size() < 7)
        return 0;
    size_t t = 3;
    switch (curveAgeIndex(sim)) {
    case 0: t = 4; break;
    case 1: t = 3; break;
    case 2: t = 2; break;
    case 3: t = 1; break;
    case 4: t = 0; break;
    case 5: t = 5; break;
    }
    const auto& table = e.motives[t];
    const bool any = std::any_of(table.begin(), table.end(),
                                 [](const InteractionTable::Advertisement& a) { return a.minimum || a.delta; });
    return any ? t : 0;
}

struct Ads {
    std::array<int16_t, kMotiveCount> min{}, range{}, personality{};
};

Ads adsOf(const Object& sim, const InteractionTable::Interaction& e) {
    Ads a;
    if (e.motives.empty())
        return a;
    const auto& table = e.motives[adTableOf(sim, e)];
    for (size_t m = 0; m < table.size() && m < kMotiveCount; ++m) {
        a.min[m] = table[m].minimum;
        a.range[m] = table[m].delta;
        a.personality[m] = int16_t(table[m].personality);
    }
    return a;
}

// Sum of the score curve gains of the considered motives.
float scoreAds(VM& vm, const Object& sim, const Ads& a, uint16_t mask) {
    const auto& curves = scoreCurves(vm, sim);
    float score = 0;
    for (size_t m = 0; m < kMotiveCount; ++m) {
        if (!(mask & (1u << m)) || (!a.min[m] && !a.range[m]))
            continue;
        const float ad = adValue(sim, a.min[m], a.range[m], uint16_t(a.personality[m]));
        const float v = sim.person->motives[m];
        score += curves[m].at(std::clamp(v + ad, -100.0f, 100.0f)) - curves[m].at(v);
    }
    return score;
}

float attenuationOf(const InteractionTable::Interaction& e) {
    if (e.attenuationCode == 0)
        return e.attenuationValue;
    return e.attenuationCode < std::size(kAttenuation) ? kAttenuation[e.attenuationCode] : 0.0f;
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
};

// Runs a candidate's check tree for autonomy: parameter 0 = 1, the ads in
// the thread for data owners 0x0C/0x1C/0x1D; true if it passes (or there
// is none). `ads` gets what the tree left, `args` the stack object of the
// first action string it added (the action tree's parameter 0).
bool runCheckTree(VM& vm, ObjectId sim, const Candidate& c, Ads& ads, std::array<int16_t, 4>& args) {
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
        if (leader->data[kObjectDataUseCount] > 0)
            continue;
        const auto ref = interactionTable(vm, *leader);
        if (!ref)
            continue;
        const float distance = distanceBetween(*from, *leader);
        for (const auto& e : ref->table->interactions) {
            if ((e.flags & 0x80) || int64_t(e.autonomy) > autonomyLevel)
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
            cand.staticScore = scoreAds(vm, sim, ads, mask) / (1.0f + attenuationOf(e) * distance);
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
        if (!runCheckTree(vm, simId, cand, ads, args))
            continue;
        Object* s = vm.object(simId);
        if (!s)
            return Exit::False;
        const float score = scoreAds(vm, *s, ads, mask) / (1.0f + attenuationOf(*cand.entry) * cand.distance);
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
// Interaction found; 1-3 (interrupt, reaction, top user action) are not
// done. The interaction becomes the Sim's current one (autonomous priority,
// first in its queue) and its action tree runs on top of this tree; the
// instruction branches on what it returns.
Exit gosubFoundAction(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t mode = le16(&o[0]);
    if (mode != 0)
        throw VMError("Gosub Found Action: mode " + std::to_string(mode) + " not implemented");
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

std::function<void(const AutonomyDecision&)>& autonomyObserver() {
    static std::function<void(const AutonomyDecision&)> observer;
    return observer;
}

float adValue(const Object& sim, int16_t min, int16_t range, uint16_t personality) {
    // STR# 0xE8 "personality ads": 1/2 nice/grouchy, 3/4 active/lazy, 5/6
    // generous/selfish, 7/8 playful/serious, 9/10 outgoing/shy, 11/12
    // neat/sloppy (the even ones inverted), then the skills. Person data
    // (STR# 0xC8): personality 2-7, skills 9 cleaning, 10 cooking, 11
    // charisma, 12 mechanical, 15 creativity, 17 body, 18 logic;
    // gardening, music and literacy have none.
    static constexpr int16_t kData[23] = {-1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 9, 10, 11, 12, -1, -1, 15, -1, 17, 18};
    float factor = 0;
    if (personality > 0 && personality < std::size(kData) && kData[personality] >= 0 && sim.person) {
        factor = std::clamp(float(sim.person->data[size_t(kData[personality])]) / 1000.0f, 0.0f, 1.0f);
        if (personality < 13 && personality % 2 == 0)
            factor = 1.0f - factor;
    }
    return float(min) + float(range) * factor;
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
