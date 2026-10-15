// The want simulator (wants.hpp). Addresses are of the BV 1.10 ppc binary.

#include "wants.hpp"

#include "gnu_sort.hpp"
#include "motives.hpp"
#include "formats/log_once.hpp"

#include "engine.hpp"
#include "errors.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/neighborhood.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>

namespace os2::vm {

using wants::SubjectType;

namespace {

constexpr size_t kMaxEvents = 4096;
constexpr int16_t kPdAspirationScore = 160, kPdRewardPoints = 161, kPdAspirationRaw = 162;
constexpr int16_t kPdAspiration = 46, kPdGender = 65;
constexpr int kMood = 3; // motive index

// The library, read once per game data set (493 definitions, 211 trees). Held
// strongly: a run of many lots (lot-run) drops each VM, and with it the
// library, before the next one asks again (it was reread 69 times, ~55% of
// the run). The key is a fingerprint of where the definitions, trees and
// tuning live (package path, offset, size), so another index over other
// packages never gets a stale library.
std::shared_ptr<const wants::Library> libraryFor(ResourceIndex* index) {
    static std::mutex m;
    static std::map<size_t, std::shared_ptr<const wants::Library>> cache;
    std::string fp = std::to_string(index->resourceCount()) + '/' + std::to_string(index->packageCount());
    for (const uint32_t type : {wants::kTypeWantDefinition, wants::kTypeWantTree})
        for (const auto& loc : index->ofType(type))
            fp += loc.package->path().string() + ':' + std::to_string(loc.entry->offset) + ':' +
                  std::to_string(loc.entry->size) + ';';
    if (const auto t = index->find(Tgi{0, wants::kTuningGroup, wants::kTuningInstance, 0}))
        fp += t->package->path().string() + ':' + std::to_string(t->entry->offset) + ':' + std::to_string(t->entry->size);
    const size_t key = std::hash<std::string>()(fp);
    std::lock_guard lock(m);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    auto lib = std::make_shared<const wants::Library>(wants::Library::load(*index));
    cache[key] = lib;
    return lib;
}

void warn(WantSystem& ws, std::string s) {
    if (ws.warnings.size() >= 64)
        ws.warnings.erase(ws.warnings.begin());
    ws.warnings.push_back(std::move(s));
}

void event(VM& vm, WantSystem& ws, WantEvent e) {
    e.tick = vm.ticks;
    if (ws.events.size() >= kMaxEvents)
        ws.events.pop_front();
    ws.events.push_back(e);
}

// --- MetaPerson: a neighbor and its Sim on the lot ---

struct Meta {
    int16_t id = 0;
    Object* sim = nullptr;      // on the lot
    Neighbor* neighbor = nullptr;
    int16_t* data = nullptr;    // the person data in use (the Sim's on the lot)
    explicit operator bool() const { return data != nullptr; }
};

Meta meta(VM& vm, int16_t id) {
    Meta m;
    m.id = id;
    m.neighbor = vm.neighbor(id);
    m.sim = vm.neighborSim(id);
    if (m.sim && m.sim->person)
        m.data = m.sim->person->data.data();
    else if (m.neighbor)
        m.data = m.neighbor->personData.data();
    return m;
}

int16_t pd(const Meta& m, int16_t i) { return m.data && i >= 0 && size_t(i) < kPersonDataCount ? m.data[i] : 0; }
void setPd(const Meta& m, int16_t i, int16_t v) {
    if (m.data && i >= 0 && size_t(i) < kPersonDataCount)
        m.data[i] = v;
}

// --- the personal clock (cTSWantAchievementHistory) ---

// FUN_00aedfd0: the personal clock moves by what the simulator's clock moved.
int32_t personalTime(WantSystem& ws, SimWants& s) {
    const int32_t d = int32_t(ws.clock - s.lastClock);
    s.lastClock = ws.clock;
    s.personal += std::max(0, d);
    return s.personal;
}

SimWants& simState(WantSystem& ws, int16_t id) {
    auto it = ws.sims.find(id);
    if (it == ws.sims.end()) {
        it = ws.sims.emplace(id, SimWants{}).first;
        it->second.lastClock = ws.clock;
    }
    return it->second;
}

const wants::Definition* defOf(const WantSystem& ws, const WantInstance& w) { return ws.library->definition(w.data.guid); }
bool isFear(const WantSystem& ws, const WantInstance& w) {
    const auto* d = defOf(ws, w);
    return d && d->isFear();
}
const wants::Node* nodeOf(const WantSystem& ws, const WantInstance& w) {
    return w.node >= 0 && size_t(w.node) < ws.library->nodes.size() ? &ws.library->nodes[size_t(w.node)] : nullptr;
}

// The 16-bit subject value of Sims and skills is the short the game keeps
// at the subject's +4; GUIDs, categories and careers are whole.
bool shortSubject(SubjectType t) { return t == SubjectType::Sim || t == SubjectType::Skill; }

// The catalog want category of an object definition (OBJD field 0x2F,
// offset 0x9E: 10000..10052 in 689 definitions, the tuning's categories).
int32_t categoryOfGuid(VM& vm, uint32_t guid) {
    if (auto d = vm.code().definitionByGuid(guid))
        if (const ObjectDefinition* od = vm.code().definition(d->first, d->second))
            return od->field(0x9E);
    return 0;
}

bool compareInt(wants::IntegerOperator op, int a, int s) {
    using wants::IntegerOperator;
    switch (op) {
    case IntegerOperator::Equals: return a == s;
    case IntegerOperator::LessThan: return a < s;
    case IntegerOperator::GreaterThan: return s < a;
    case IntegerOperator::LessOrEquals: return a <= s;
    case IntegerOperator::GreaterOrEquals: return s <= a;
    default: return true;
    }
}

// cTSWantInstance vtable 0xc: the same want of the same Sim about the same subject.
bool same(const WantInstance& a, const WantInstance& b) {
    return a.data.owner == b.data.owner && a.data.guid == b.data.guid && a.data.subjectType == b.data.subjectType &&
           a.data.subjectValue == b.data.subjectValue && a.data.subjectData == b.data.subjectData;
}

// vtable 0x10 (FUN_00ae8ab8): does achievement `a` fulfil slot `s`?
bool fulfils(VM& vm, const WantSystem& ws, const WantInstance& s, const WantInstance& a) {
    if (s.data.owner != a.data.owner || s.data.guid != a.data.guid)
        return false;
    const auto* d = defOf(ws, s);
    const auto op = d ? d->integerOperator : wants::IntegerOperator::None;
    const auto sv = s.data.subjectValue, av = a.data.subjectValue;
    const int sd = int16_t(s.data.subjectData), ad = int16_t(a.data.subjectData);
    switch (s.data.subjectType) {
    case SubjectType::None: return compareInt(op, ad, sd);
    case SubjectType::Sim:
        if (sv != 0 && int16_t(av) != int16_t(sv))
            return false;
        return compareInt(op, ad, sd);
    case SubjectType::Guid:
        if (sv != 0 && av != 0 && av != sv)
            return false;
        return compareInt(op, ad, sd);
    case SubjectType::Category:
        if (sv != 0) {
            if (a.data.subjectType == SubjectType::Guid) {
                if (uint32_t(categoryOfGuid(vm, av)) != sv)
                    return false;
            } else if (a.data.subjectType == SubjectType::Category) {
                return av == sv;
            }
        }
        return compareInt(op, ad, sd);
    case SubjectType::Skill:
    case SubjectType::Career: {
        const bool ok = sv == 0 || (s.data.subjectType == SubjectType::Skill ? int16_t(av) == int16_t(sv) : av == sv);
        if (sd == 0)
            return ok;
        return ok && ad == sd;
    }
    }
    return false;
}

// vtable 0x14 (FUN_00ae84fc): is `c` less specific than `s` (the same want
// with a subject or number `s` has and `c` leaves open)?
bool lessSpecific(const WantInstance& c, const WantInstance& s) {
    if (c.data.guid != s.data.guid || c.data.subjectType != s.data.subjectType)
        return false;
    switch (c.data.subjectType) {
    case SubjectType::Sim:
    case SubjectType::Skill:
    case SubjectType::Guid:
    case SubjectType::Category:
    case SubjectType::Career:
        if (c.data.subjectValue == 0 && s.data.subjectValue != 0)
            return true;
        break;
    default: break;
    }
    return c.data.subjectData == 0 && s.data.subjectData != 0;
}

// cTSWantNode vtable 0x94 (FUN_00e4cafc): does an achievement count for a node?
bool nodeMatches(VM& vm, const WantSystem& ws, const wants::Node& n, const WantInstance& a) {
    if (n.wantId != a.data.guid)
        return false;
    const auto* d = ws.library->definition(n.wantId);
    const auto op = d ? d->integerOperator : wants::IntegerOperator::None;
    const int ad = int16_t(a.data.subjectData), nf = int32_t(n.integerField);
    switch (n.subjectType) {
    case SubjectType::None:
    case SubjectType::Sim: return compareInt(op, ad, nf);
    case SubjectType::Guid:
        if (n.objectField != 0 && a.data.subjectValue != 0 && a.data.subjectValue != n.objectField)
            return false;
        return compareInt(op, ad, nf);
    case SubjectType::Category:
        if (n.objectField != 0) {
            if (a.data.subjectType == SubjectType::Guid) {
                if (uint32_t(categoryOfGuid(vm, a.data.subjectValue)) != n.objectField)
                    return false;
            } else if (a.data.subjectType == SubjectType::Category) {
                return a.data.subjectValue == n.objectField;
            }
        }
        return compareInt(op, ad, nf);
    case SubjectType::Skill:
    case SubjectType::Career: {
        const bool ok = n.objectField == 0 || (n.subjectType == SubjectType::Skill
                                                   ? uint32_t(int16_t(a.data.subjectValue)) == n.objectField
                                                   : a.data.subjectValue == n.objectField);
        if (n.integerField == 0)
            return ok;
        return ok && ad == nf;
    }
    }
    return false;
}

bool purged(const WantInstance& a) { return a.data.flags & 0x10; }

// FUN_00aeea94: the history's achievements of a node.
std::vector<WantPtr> achievementsOf(VM& vm, const WantSystem& ws, const SimWants& s, const wants::Node& n) {
    std::vector<WantPtr> out;
    if (auto it = s.history.find(n.wantId); it != s.history.end())
        for (const auto& a : it->second)
            if (!purged(*a) && nodeMatches(vm, ws, n, *a))
                out.push_back(a);
    return out;
}

// FUN_00aeec14: the same, with a subject the achievement must agree with.
std::vector<WantPtr> achievementsOf(VM& vm, const WantSystem& ws, const SimWants& s, const wants::Node& n,
                                    const WantSubject& subject) {
    std::vector<WantPtr> out;
    for (const auto& a : achievementsOf(vm, ws, s, n)) {
        if (a->data.subjectType == subject.type && subject.value != 0) {
            const bool ok = a->data.subjectValue == 0 ? subject.type == SubjectType::Guid
                                                      : a->data.subjectValue == subject.value;
            if (!ok)
                continue;
        }
        out.push_back(a);
    }
    return out;
}

// FUN_00503d0c (PickSubject): a node's subject, inherited from the
// achievements it follows when the node asks for it, or fixed by it.
WantSubject pickSubject(const WantSystem& ws, const WantInstance* from, const wants::Node& n) {
    WantSubject s;
    s.type = n.subjectType;
    auto inherited = [&](auto pred) -> const WantInstance* {
        if (!n.inheritSubject)
            return nullptr;
        for (const WantInstance* p = from; p; p = p->source.get())
            if (pred(*p))
                return p;
        return nullptr;
    };
    switch (n.subjectType) {
    case SubjectType::None: break;
    case SubjectType::Sim:
        if (auto p = inherited([](const WantInstance& w) { return w.data.subjectType == SubjectType::Sim; }))
            s.value = uint16_t(p->data.subjectValue);
        break;
    case SubjectType::Guid:
        if (auto p = inherited([](const WantInstance& w) {
                return w.data.subjectType == SubjectType::Guid && w.data.subjectValue != 0;
            }))
            s.value = p->data.subjectValue;
        if (n.objectField)
            s.value = n.objectField;
        break;
    case SubjectType::Category:
        if (auto p = inherited([&](const WantInstance& w) {
                const auto* pn = nodeOf(ws, w);
                return pn && pn->subjectType == SubjectType::Category;
            }))
            s.value = nodeOf(ws, *p)->objectField;
        if (n.objectField)
            s.value = n.objectField;
        break;
    case SubjectType::Skill:
        if (auto p = inherited([](const WantInstance& w) { return w.data.subjectType == SubjectType::Skill; }))
            s.value = uint16_t(p->data.subjectValue);
        if (n.objectField)
            s.value = uint16_t(n.objectField);
        break;
    case SubjectType::Career:
        if (auto p = inherited([](const WantInstance& w) { return w.data.subjectType == SubjectType::Career; }))
            s.value = p->data.subjectValue;
        if (n.objectField)
            s.value = n.objectField;
        break;
    }
    if (n.integerField)
        s.data = uint16_t(n.integerField);
    return s;
}

int32_t nodeScore(const WantSystem& ws, const wants::Node& n) {
    if (n.scoreOverride)
        return *n.scoreOverride;
    const auto* d = ws.library->definition(n.wantId);
    return d ? d->score : 0;
}

// FUN_00aeb1fc: a new instance (a candidate, or an achievement).
WantPtr newInstance(const WantSystem& ws, int16_t owner, uint32_t guid, const WantSubject& s, int node, WantPtr source) {
    auto w = std::make_shared<WantInstance>();
    w->data.version = 8;
    w->data.owner = uint16_t(owner);
    w->data.guid = guid;
    w->data.subjectType = s.type;
    w->data.subjectValue = s.value;
    w->data.subjectData = s.data;
    w->node = node;
    w->source = std::move(source);
    if (node >= 0) {
        w->data.score = nodeScore(ws, ws.library->nodes[size_t(node)]);
    } else if (const auto* d = ws.library->definition(guid)) {
        w->data.score = d->score;
        w->data.influence = d->influence;
    }
    return w;
}

// FUN_00aeb41c: an instance standing for achievement `a` at `node`.
WantPtr fromAchievement(const WantSystem& ws, const WantPtr& a, int node, WantPtr parent) {
    auto w = std::make_shared<WantInstance>(*a);
    w->node = node;
    w->source = std::move(parent);
    w->priority = 0;
    w->data.score = nodeScore(ws, ws.library->nodes[size_t(node)]);
    w->data.flags = 0;
    return w;
}

// --- priority (CalculateWantPriority, 0x509c54) ---

float ramp(int v, const std::array<int32_t, 2>& r) {
    const int lo = r[0], hi = r[1];
    if (hi < lo) { // falling
        if (v < hi)
            return 1;
        if (v > lo)
            return 0;
        return 1.0f - float(v - hi) / float(lo - hi);
    }
    if (v < lo)
        return 0;
    if (v <= hi) {
        if (lo != hi)
            return float(v - lo) / float(hi - lo);
        return v == hi ? 1.0f : 0.0f;
    }
    return 1;
}

int16_t aspirationScoreOf(const WantSystem& ws, const Meta& m) {
    return normalizedAspirationScore(*ws.library, wants::ageEnum(pd(m, persondata::PersonAge)),
                                     int32_t(pd(m, kPdAspirationRaw)) * 10);
}

int16_t relationshipLtr(VM& vm, int16_t from, int16_t to) {
    auto it = vm.relationships.find({{true, from}, {true, to}});
    if (it == vm.relationships.end() || it->second.size() < 3)
        return 0;
    return it->second[2];
}

int32_t personalNow(WantSystem& ws, int16_t id) { return personalTime(ws, simState(ws, id)); }

float priority(VM& vm, WantSystem& ws, const Meta& m, const WantInstance& c) {
    const wants::Node* n = nodeOf(ws, c);
    if (!n)
        return 0;
    // The time factor: how much of the node's lifetime (or the period of a
    // regenerating node) is left since the achievement it follows.
    float timeFactor = 1;
    if (c.source && c.source->data.time != 0) {
        const int32_t elapsed = personalNow(ws, m.id) - int32_t(c.source->data.time);
        const int32_t life = n->lifetime, freq = n->frequency;
        if (life >= 1 && elapsed > life) {
            timeFactor = 0;
        } else if (freq >= 1) {
            timeFactor = float(std::max(0, freq - elapsed % freq)) / float(freq);
        } else if (life >= 1) {
            timeFactor = float(std::max(0, life - elapsed)) / float(life);
        }
    }
    std::array<float, wants::kFactorCount> f{};
    bool zero = false;
    using namespace wants;
    // Age and aspiration always count.
    const int age = ageEnum(pd(m, persondata::PersonAge));
    f[kAge] = std::find(n->ages.begin(), n->ages.end(), age) != n->ages.end() ? 1.0f : 0.0f;
    if (f[kAge] == 0 && n->superZero[kAge])
        zero = true;
    const int aspiration = pd(m, kPdAspiration);
    if (aspiration != 0)
        f[kAspiration] = std::find(n->aspirations.begin(), n->aspirations.end(), aspiration) != n->aspirations.end();
    if (f[kAspiration] == 0 && n->superZero[kAspiration])
        zero = true;
    // Relationship (lifetime, variable 2) with the subject Sim.
    if (n->useFactor(kRelationship) && c.data.subjectType == SubjectType::Sim && c.data.subjectValue != 0 && m.neighbor &&
        Node::hasRange(n->relationshipRange))
        f[kRelationship] = ramp(relationshipLtr(vm, m.id, int16_t(c.data.subjectValue)), n->relationshipRange);
    if (f[kRelationship] <= 0 && n->superZero[kRelationship])
        zero = true;
    // Mood (motive 3) of the Sim on the lot: its float, truncated
    // (CalculateWantPriority, BV 0x509c54).
    if (n->useFactor(kMood) && Node::hasRange(n->moodRange)) {
        const int mood = m.sim && m.sim->person ? int(motiveNow(*m.sim, kMood)) : 0;
        f[kMood] = ramp(mood, n->moodRange);
    }
    if (f[kMood] <= 0 && n->superZero[kMood])
        zero = true;
    if (n->useFactor(kAspirationScore) && Node::hasRange(n->aspirationScoreRange))
        f[kAspirationScore] = ramp(aspirationScoreOf(ws, m), n->aspirationScoreRange);
    if (f[kAspirationScore] <= 0 && n->superZero[kAspirationScore])
        zero = true;
    if (n->useFactor(kPersonData) && n->personDataIndex >= 1 && n->personDataIndex <= 0xCC) {
        if (Node::hasRange(n->personDataRange))
            f[kPersonData] = ramp(pd(m, int16_t(n->personDataIndex - 1)), n->personDataRange);
    }
    if (f[kPersonData] <= 0 && n->superZero[kPersonData])
        zero = true;
    // Personality: the mean, over the traits the node marks, of how far the
    // Sim leans that way (person data 0..1000, 500 neutral).
    if (n->useFactor(kPersonality) && m.data) {
        static const std::pair<int, int16_t> traits[] = {{2, 2},  {1, 2},  {4, 3},  {3, 3},  {8, 5},
                                                         {7, 5},  {10, 6}, {9, 6},  {12, 7}, {11, 7}};
        float sum = 0;
        int count = 0;
        for (const auto& [t, field] : traits) {
            if (std::find(n->personality.begin(), n->personality.end(), t) == n->personality.end())
                continue;
            const int v = pd(m, field);
            // odd = the high end (nice, active, playful, outgoing, neat).
            float x = (t % 2 ? float(v - 500) : float(500 - v)) / 500.0f;
            x = std::clamp(x, 0.0f, 1.0f);
            sum += x;
            ++count;
        }
        f[kPersonality] = count ? sum / float(count) : 0.0f;
    }
    if (f[kPersonality] <= 0 && n->superZero[kPersonality])
        zero = true;
    // The weighted mean; the game keeps the running sum in an integer.
    int32_t sum = 0, weights = 0;
    for (int k = 0; k < kFactorCount; ++k) {
        sum = int32_t(float(n->weight[size_t(k)]) * f[size_t(k)] + float(sum));
        weights += n->weight[size_t(k)];
    }
    const float mean = weights > 0 ? float(sum) / float(weights) : 1.0f;
    const float p = (mean * float(n->maxScore - n->minScore) + float(n->minScore)) * timeFactor;
    return zero ? 0.0f : p;
}

// --- the want controller and its trees ---

Object* controller(VM& vm) {
    for (const auto& [id, o] : vm.eachObject())
        if (o.guid() == wants::kControllerGuid)
            return vm.object(id);
    return nullptr;
}

// Runs one of the want trees (in the code of "Wants - Check Trees") on the
// controller, to its end: the controller is me and the stack object, and
// its code is that object's while the tree runs (the binary finds the tree
// by name and runs it on the controller). nullopt when the tree is missing.
std::optional<bool> runWantTree(VM& vm, WantSystem& ws, Object& ctl, const std::string& name,
                                std::span<const int16_t> params, std::array<int16_t, kTempCount>* tempsOut = nullptr) {
    if (name.empty())
        return std::nullopt;
    const auto tree = vm.code().behaviorByName(wants::kCheckTreesGroup, name);
    if (!tree)
        return std::nullopt;
    const uint32_t group = ctl.group, semi = ctl.semiGroup;
    ctl.group = wants::kCheckTreesGroup;
    ctl.semiGroup = vm.code().semiGlobalFor(wants::kCheckTreesGroup);
    std::array<int16_t, kTempCount> temps{};
    std::optional<bool> result;
    try {
        result = runOneTickTree(vm, temps, ctl.id, ctl.id, ctl.id, *tree, params);
    } catch (const std::exception& e) {
        warn(ws, name + ": " + e.what());
        os2::logOnce("want check tree", name, e); // (once a name: it runs every check; the app does not print ws.warnings)
        result = false;
    }
    if (Object* again = vm.object(ctl.id)) {
        again->group = group;
        again->semiGroup = semi;
    }
    if (tempsOut)
        *tempsOut = temps;
    return result;
}

// The 8 parameters the check tree gets (GoalPassesEdithCheck), also the
// first 8 of the feedback tree: owner, want GUID (low, high), the subject's
// neighbor ID, its object GUID (low, high), the subject's short and number.
std::array<int16_t, 8> treeParams(int16_t owner, const wants::Instance& w) {
    const uint32_t guid = w.guid;
    const int16_t nid = w.subjectType == SubjectType::Sim ? int16_t(w.subjectValue) : int16_t(0);
    const bool objectGuid = w.subjectType == SubjectType::Guid || w.subjectType == SubjectType::Career;
    const uint32_t sel = objectGuid ? w.subjectValue : 0;
    const int16_t subjectShort =
        shortSubject(w.subjectType) ? int16_t(w.subjectValue) : int16_t(w.subjectValue >> 16);
    return {owner, int16_t(guid & 0xFFFF), int16_t(guid >> 16), nid, int16_t(sel & 0xFFFF), int16_t(sel >> 16),
            subjectShort, int16_t(w.subjectData)};
}

// GoalPassesEdithCheck (0x50d174): the definition's check tree on the
// controller, remembered per instance while a selection runs. No tree, no
// controller or the cheat: it passes.
bool passesCheck(VM& vm, WantSystem& ws, const Meta& m, WantInstance& w) {
    if (ws.skipCheckTrees)
        return true;
    const auto* d = defOf(ws, w);
    Object* ctl = controller(vm);
    if (!d || !ctl || d->checkTree.empty())
        return true;
    if (auto it = ws.checked.find(&w); it != ws.checked.end())
        return it->second;
    const auto params = treeParams(m.id, w.data);
    const auto r = runWantTree(vm, ws, *ctl, d->checkTree, params);
    if (!r)
        return true;
    // vtable 0x98: 1 = checked and passed (|6), 2 = checked and failed.
    if (*r)
        w.data.flags |= 6;
    else
        w.data.flags = uint8_t((w.data.flags & ~4) | 2);
    ws.checked[&w] = *r;
    return *r;
}

// NotifyEdithOfScore (0x504c6c): the feedback tree with 15 parameters.
void notifyScore(VM& vm, WantSystem& ws, const Meta& m, const WantInstance& w, bool achieved, int32_t points,
                 int32_t influence, int16_t objectParam) {
    const auto* d = defOf(ws, w);
    Object* ctl = controller(vm);
    if (!d || !ctl || !m.data || d->feedbackTree.empty())
        return;
    const auto p8 = treeParams(m.id, w.data);
    const int32_t oldRaw = int32_t(pd(m, kPdAspirationRaw)) * 10 - points * (achieved ? 1 : 0);
    const int age = wants::ageEnum(pd(m, persondata::PersonAge));
    std::array<int16_t, 15> params{};
    std::copy(p8.begin(), p8.end(), params.begin());
    params[8] = achieved;
    params[9] = int16_t(points);
    // The definition's vtable 0x24 (0xe4e1e0): the level as its reader
    // numbers it, Power 2, Memorable 1, else 0.
    params[10] = d->level == wants::Level::Power ? 2 : d->level == wants::Level::Memorable ? 1 : 0;
    params[11] = int16_t(oldRaw / 10);
    params[12] = normalizedAspirationScore(*ws.library, age, oldRaw);
    params[13] = objectParam;
    params[14] = int16_t(influence);
    runWantTree(vm, ws, *ctl, d->feedbackTree, params);
}

// --- choosing (WorkOnFindingGoals, PrioritizeGoals, SelectBestWantsAndFears) ---

void findCandidates(VM& vm, WantSystem& ws, int16_t id) {
    ws.candidates.clear();
    SimWants& s = simState(ws, id);
    const auto& lib = *ws.library;
    const int32_t now = personalTime(ws, s);
    for (int root : lib.roots) {
        const wants::Node& rn = lib.nodes[size_t(root)];
        const auto achs = achievementsOf(vm, ws, s, rn);
        if ((achs.empty() || rn.regeneration) && rn.visible)
            ws.candidates.push_back(newInstance(ws, id, rn.wantId, pickSubject(ws, nullptr, rn), root, nullptr));
        // Each achievement of the root opens its children; achieved children
        // (in their lifetime after it) open theirs in turn.
        for (const auto& a : achs) {
            std::vector<WantPtr> open{fromAchievement(ws, a, root, nullptr)};
            for (size_t k = 0; k < open.size(); ++k) {
                const WantPtr q = open[k];
                const wants::Node& qn = lib.nodes[size_t(q->node)];
                for (int child : qn.children) {
                    const wants::Node& cn = lib.nodes[size_t(child)];
                    const WantSubject subj = pickSubject(ws, q.get(), cn);
                    bool achieved = false;
                    for (const auto& ca : achievementsOf(vm, ws, s, cn, subj)) {
                        const int32_t since = int32_t(ca->data.time) - int32_t(q->data.time);
                        if (since > 0 && (cn.lifetime < 1 || since < cn.lifetime)) {
                            open.push_back(fromAchievement(ws, ca, child, q));
                            achieved = true;
                        }
                    }
                    if (!achieved || cn.regeneration) {
                        const int32_t elapsed = now - int32_t(q->data.time);
                        if (elapsed >= 0 && (cn.lifetime < 1 || elapsed < cn.lifetime) && cn.visible)
                            ws.candidates.push_back(newInstance(ws, id, cn.wantId, subj, child, q));
                    }
                }
            }
        }
    }
}

void prioritize(VM& vm, WantSystem& ws, int16_t id) {
    const Meta m = meta(vm, id);
    for (auto& c : ws.candidates)
        c->priority = priority(vm, ws, m, *c);
    // std::sort with the predicate 0x5042fc (higher priority first), as the
    // game's libstdc++ sorts: ties end up where the game's do (gnu_sort.hpp;
    // docs/wants-mac.md §9).
    gnu::sort(ws.candidates.begin(), ws.candidates.end(),
              [](const WantPtr& a, const WantPtr& b) { return b->priority < a->priority; });
}

int indexOf(const std::vector<WantPtr>& v, const WantInstance* w) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].get() == w)
            return int(i);
    return -1;
}

// SelectOneWantOrFear (0x50d57c).
void selectOne(VM& vm, WantSystem& ws, const Meta& m, WantPtr c, std::vector<WantPtr>& slots, int& count,
               const std::vector<WantPtr>& old) {
    const int n = int(slots.size());
    if (n < count)
        return;
    for (const auto& s : slots)
        if (s && lessSpecific(*c, *s))
            return; // a more specific one of it is there
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (slots[size_t(i)] && same(*slots[size_t(i)], *c)) {
            idx = i;
            break;
        }
    const bool pass = passesCheck(vm, ws, m, *c);
    if (count <= n && pass) {
        WantInstance* found = idx >= 0 ? slots[size_t(idx)].get() : nullptr;
        if (!found)
            for (const auto& s : slots)
                if (s && fulfils(vm, ws, *s, *c)) {
                    found = s.get();
                    break;
                }
        if (found) {
            bool replace = lessSpecific(*found, *c);
            if (!replace) {
                const auto* fn = nodeOf(ws, *found);
                const auto* cn = nodeOf(ws, *c);
                replace = fn && cn && fn->tree == cn->tree && fn->depth < cn->depth;
            }
            if (replace) {
                if (found->data.flags & 1)
                    return; // locked
                if (int i = indexOf(slots, found); i >= 0) {
                    slots[size_t(i)] = c;
                    return;
                }
            }
        }
    }
    if (count == n) {
        if (idx < 0 && pass)
            ++count;
        return;
    }
    if (count < n && idx < 0 && pass) {
        // A want that was in a slot before the update goes back to it.
        const int k = std::min(int(old.size()), n);
        for (int i = 0; i < k; ++i) {
            const auto& o = old[size_t(i)];
            if (!o || !same(*o, *c) || o->data.score != c->data.score || o->data.influence != c->data.influence)
                continue;
            if (!slots[size_t(i)]) {
                slots[size_t(i)] = o;
                ++count;
                return;
            }
            WantPtr displaced = slots[size_t(i)];
            slots[size_t(i)] = o;
            c = displaced;
            break;
        }
        for (auto& s : slots)
            if (!s) {
                s = c;
                ++count;
                return;
            }
    }
}

void selectBest(VM& vm, WantSystem& ws, int16_t id, bool replaceWants, bool replaceFears) {
    SimWants& s = simState(ws, id);
    const Meta m = meta(vm, id);
    auto keep = [&](std::vector<WantPtr>& slots, bool replace) {
        int count = 0;
        for (auto& w : slots) {
            if (!w)
                continue;
            if (!(w->data.flags & 1) && (replace || !passesCheck(vm, ws, m, *w)))
                w = nullptr;
            else
                ++count;
        }
        return count;
    };
    int wants = keep(s.wants, replaceWants);
    int fears = keep(s.fears, replaceFears);
    for (const auto& c : ws.candidates) {
        c->data.flags &= uint8_t(~2); // vtable 0x98(0)
        if (!(c->priority > 0))
            continue;
        if (!isFear(ws, *c)) {
            if (wants <= int(s.wants.size()))
                selectOne(vm, ws, m, c, s.wants, wants, s.oldWants);
        } else if (fears <= int(s.fears.size())) {
            selectOne(vm, ws, m, c, s.fears, fears, s.oldFears);
        }
    }
    // vtable 0x94: the slots let go of the achievements they followed.
    for (auto* v : {&s.wants, &s.fears})
        for (auto& w : *v)
            if (w)
                w->source = nullptr;
    ws.checked.clear();
}

// State 4: what changed in the slots, for the hosts.
void reportSlots(VM& vm, WantSystem& ws, int16_t id) {
    SimWants& s = simState(ws, id);
    auto report = [&](const std::vector<WantPtr>& before, const std::vector<WantPtr>& now, bool fear) {
        for (size_t i = 0; i < now.size(); ++i) {
            const WantInstance* b = i < before.size() ? before[i].get() : nullptr;
            const WantInstance* a = now[i].get();
            if (a == b || (a && b && same(*a, *b)))
                continue;
            if (b && !a)
                event(vm, ws, {.kind = WantEvent::Kind::Dropped, .sim = id, .guid = b->data.guid, .subjectType = b->data.subjectType,
                               .subjectValue = b->data.subjectValue, .subjectData = b->data.subjectData,
                               .slot = int(i), .fear = fear});
            if (a)
                event(vm, ws, {.kind = WantEvent::Kind::Rolled, .sim = id, .guid = a->data.guid, .subjectType = a->data.subjectType,
                               .subjectValue = a->data.subjectValue, .subjectData = a->data.subjectData,
                               .slot = int(i), .fear = fear, .points = a->data.score});
        }
    };
    report(ws.beforeWants, s.wants, false);
    report(ws.beforeFears, s.fears, true);
}

// DoWork (0x512100), one state per call; `only` = 0: the next request
// (the selected Sim's first), else that Sim's.
void doWork(VM& vm, WantSystem& ws, int16_t only = 0) {
    switch (ws.state) {
    case 0: {
        if (ws.updates.empty())
            return;
        auto it = ws.updates.end();
        if (only) {
            it = ws.updates.find(only);
            if (it == ws.updates.end())
                return;
        } else {
            if (const Object* sel = vm.object(vm.globals[global::SelectedPersonId]); sel && sel->person)
                it = ws.updates.find(neighborIdOf(*sel));
            if (it == ws.updates.end())
                it = ws.updates.begin();
        }
        ws.current = it->first;
        ws.currentType = it->second;
        ws.updates.erase(it);
        SimWants& s = simState(ws, ws.current);
        ws.beforeWants = s.wants;
        ws.beforeFears = s.fears;
        ws.candidates.clear();
        ws.state = 1;
        return;
    }
    case 1:
        findCandidates(vm, ws, ws.current);
        ws.state = 2;
        return;
    case 2:
        prioritize(vm, ws, ws.current);
        ws.state = 3;
        return;
    case 3:
        selectBest(vm, ws, ws.current, ws.currentType < 2, ws.currentType <= 2);
        ws.state = 4;
        return;
    case 4:
        reportSlots(vm, ws, ws.current);
        ws.candidates.clear();
        ws.current = 0;
        ws.state = 0;
        return;
    default: ws.state = 0; return;
    }
}

// FUN_00aee684: an achievement into the history (timed now if it is not).
void addAchievement(WantSystem& ws, SimWants& s, const WantPtr& a) {
    if (a->data.time == 0)
        a->data.time = uint32_t(personalTime(ws, s));
    else if (s.personal < int32_t(a->data.time))
        s.personal = int32_t(a->data.time);
    s.history[a->data.guid].push_back(a);
}

// ReadPerson (0x515030) for a Sim on the lot: an update of type 3 and the
// "Is Born" achievement if it has none.
void readPerson(VM& vm, WantSystem& ws, int16_t id) {
    SimWants& s = simState(ws, id);
    s.read = true;
    s.lastClock = ws.clock;
    requestWantsUpdate(vm, id, 3);
    bool born = false;
    if (auto it = s.history.find(wants::kIsBornWant); it != s.history.end())
        for (const auto& a : it->second)
            born |= !purged(*a);
    if (!born)
        addAchievement(ws, s, newInstance(ws, id, wants::kIsBornWant, {}, -1, nullptr));
}

WantPtr toRuntime(const wants::Instance& i) {
    auto w = std::make_shared<WantInstance>();
    w->data = i;
    return w;
}

// The game keeps a want's subject Sim by neighbor ID in a value whose
// 16 bits are all that is saved for Sims and skills.
std::string personName(VM& vm, int16_t id) {
    const Neighbor* n = vm.neighbor(id);
    if (!n)
        return "#" + std::to_string(id);
    return n->firstName.empty() ? "#" + std::to_string(id) : n->firstName;
}

} // namespace

WantSystem* wantSystem(VM& vm) { return vm.wants.get(); }

const SimWants* simWants(VM& vm, int16_t id) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return nullptr;
    auto it = ws->sims.find(id);
    return it == ws->sims.end() ? nullptr : &it->second;
}

int16_t neighborIdOf(const Object& sim) { return sim.person ? sim.person->data[persondata::NeighborId] : int16_t(0); }

void loadWants(VM& vm, const hood::Neighborhood& h) {
    vm.wants.reset();
    ResourceIndex* index = vm.code().index();
    if (!index)
        return;
    auto lib = libraryFor(index);
    if (lib->empty())
        return;
    auto ws = std::make_shared<WantSystem>();
    ws->library = lib;
    if (!h.package.empty() && std::filesystem::exists(h.package)) {
        DbpfPackage pkg(h.package);
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type != wants::kTypeSwaf)
                continue;
            const auto id = int16_t(e.tgi.instance);
            try {
                SimWants s;
                s.record = wants::Record::parse(pkg.read(e));
                const auto& r = *s.record;
                s.wants.assign(4, nullptr); // the base game's 4 wants and 3 fears
                s.fears.assign(3, nullptr);
                // Read: the first (slot count) saved, then up to the count.
                for (size_t i = 0; i < r.wants.size() && i < s.wants.size(); ++i)
                    s.wants[i] = toRuntime(r.wants[i]);
                for (size_t i = 0; i < r.fears.size() && i < s.fears.size(); ++i)
                    s.fears[i] = toRuntime(r.fears[i]);
                s.historyVersion = r.history.version;
                s.personal = r.history.clock;
                for (const auto& [guid, list] : r.history.entries) {
                    auto& v = s.history[guid];
                    for (const auto& i : list)
                        v.push_back(toRuntime(i));
                }
                ws->sims[id] = std::move(s);
            } catch (const std::exception& ex) {
                warn(*ws, "SWAF " + std::to_string(id) + ": " + ex.what());
                os2::logOnce("SWAF", std::to_string(id), ex); // (the app does not print ws.warnings)
            }
        }
    }
    vm.wants = std::move(ws);
}

WantSystem& installWants(VM& vm, std::shared_ptr<const wants::Library> library) {
    vm.wants = std::make_shared<WantSystem>();
    vm.wants->library = std::move(library);
    return *vm.wants;
}

void tickWants(VM& vm) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return;
    // OnTick: the clock runs while the simulation does (reset past 2^30).
    ws->clock += 2;
    if (ws->clock > 0x3FFFFFFF) {
        for (auto& [_, s] : ws->sims)
            personalTime(*ws, s);
        ws->clock = 0;
        for (auto& [_, s] : ws->sims)
            s.lastClock = 0;
    }
    // Sims on the lot are read once (ReadPerson).
    for (Object* o : vm.objectTable())
        if (o && o->person && o->position.inWorld())
            if (const int16_t id = neighborIdOf(*o); id > 0 && vm.neighbor(id) && !simState(*ws, id).read)
                readPerson(vm, *ws, id);
    doWork(vm, *ws);
}

void flushWants(VM& vm) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return;
    for (int guard = 0; (ws->state != 0 || !ws->updates.empty()) && guard < 100000; ++guard)
        doWork(vm, *ws);
}

void requestWantsUpdate(VM& vm, int16_t id, int type) {
    WantSystem* ws = wantSystem(vm);
    if (!ws || id == 0)
        return;
    auto it = ws->updates.find(id);
    if (it == ws->updates.end()) {
        ws->updates[id] = type;
        if (type < 2) {
            SimWants& s = simState(*ws, id);
            s.oldWants = s.wants;
            s.oldFears = s.fears;
        }
    } else if (type < it->second) {
        it->second = type;
    }
}

int16_t normalizedAspirationScore(const wants::Library& lib, int age, int32_t raw) {
    static constexpr int kMeter[7] = {-100, -50, 0, 25, 50, 75, 100};
    raw = std::clamp<int32_t>(raw, -0x50000, 0x4FFF6);
    auto it = lib.tuning.levels.find(age);
    if (it == lib.tuning.levels.end())
        return 0;
    const auto& t = it->second;
    size_t i = 0;
    for (size_t k = 1; k <= 5; ++k)
        if (t[k] <= raw)
            i = k;
    float f = float(raw - t[i]) / float(t[i + 1] - t[i]);
    f = std::clamp(f, 0.0f, 1.0f);
    const float x = float(kMeter[i + 1] - kMeter[i]) * f + float(kMeter[i]);
    return int16_t(std::clamp(int(std::nearbyintf(x)), -100, 100)); // the float rounding trick: to nearest, ties even
}

int32_t rawAspirationScore(const wants::Library& lib, int age, int32_t normalized) {
    // RawAspirationScore (BV 0x506efc): the meter's segment of the value,
    // the same fraction of the age's thresholds (fmadds, rounded to
    // nearest even), within the first and last threshold (-1000 and 16000
    // without them).
    static constexpr int kMeter[7] = {-100, -50, 0, 25, 50, 75, 100};
    const int v = std::clamp<int>(normalized, -100, 100);
    size_t i = 0;
    for (size_t k = 1; k <= 5; ++k)
        if (kMeter[k] <= v)
            i = k;
    float f = float(v - kMeter[i]) / float(kMeter[i + 1] - kMeter[i]);
    f = std::clamp(f, 0.0f, 1.0f);
    int32_t raw = 0, lo = -1000, hi = 16000;
    if (auto it = lib.tuning.levels.find(age); it != lib.tuning.levels.end()) {
        const auto& t = it->second;
        raw = int32_t(std::nearbyintf(std::fmaf(float(t[i + 1] - t[i]), f, float(t[i]))));
        lo = t[0], hi = t[6];
    }
    raw = std::clamp(raw, lo, hi);
    return std::clamp<int32_t>(raw, -0x50000, 0x4FFF6);
}

int aspirationLevel(const wants::Library& lib, int age, int32_t raw) {
    auto it = lib.tuning.levels.find(age);
    if (it == lib.tuning.levels.end())
        return 0;
    int level = 0;
    for (int k = 1; k <= 5; ++k)
        if (it->second[size_t(k)] <= raw)
            level = k;
    return level;
}

void changeAspirationScore(VM& vm, int16_t id, int32_t points) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return;
    const Meta m = meta(vm, id);
    if (!m)
        return;
    const int age = wants::ageEnum(pd(m, persondata::PersonAge));
    int32_t lo = -1000, hi = 16000;
    if (auto it = ws->library->tuning.levels.find(age); it != ws->library->tuning.levels.end())
        lo = it->second[0], hi = it->second[6];
    const int16_t before = pd(m, kPdAspirationScore);
    const int32_t raw = std::clamp<int32_t>(points + int32_t(pd(m, kPdAspirationRaw)) * 10, lo, hi);
    setPd(m, kPdAspirationRaw, int16_t(raw / 10));
    setPd(m, kPdAspirationScore, normalizedAspirationScore(*ws->library, age, int32_t(pd(m, kPdAspirationRaw)) * 10));
    setPd(m, kPdRewardPoints, int16_t(std::clamp<int32_t>(points / 10 + pd(m, kPdRewardPoints), 0, 0x7FFF)));
    event(vm, *ws, {.kind = WantEvent::Kind::Score, .sim = id, .points = raw, .before = before,
                    .after = pd(m, kPdAspirationScore)});
}

bool markWantAchieved(VM& vm, int16_t id, uint32_t guid, const WantSubject& subject, int16_t objectParam) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return false;
    const auto* d = ws->library->definition(guid);
    if (!d)
        return false;
    // A definition with a Sim array tree counts for every Sim the tree lists.
    std::vector<int16_t> sims{id};
    if (!d->simArrayTree.empty()) {
        sims.clear();
        if (Object* ctl = controller(vm)) {
            std::array<int16_t, 8> params{};
            params[0] = id;
            std::array<int16_t, kTempCount> temps{};
            if (runWantTree(vm, *ws, *ctl, d->simArrayTree, params, &temps).value_or(false))
                // The list is the object array 0 of the object the tree
                // leaves in the controller's temp 0 (FUN_00508b10: the
                // controller's vtable 0x4c(0), the object of that ID, its
                // array 0 when it has arrays).
                if (Object* holder = vm.object(temps[0]); holder && !holder->arrays.empty())
                    for (int16_t v : holder->arrays[0].values)
                        if (v)
                            sims.push_back(v);
        }
    }
    bool any = false;
    for (int16_t sim : sims) {
        // MarkOneWantAsAchieved (0x513760).
        const Meta m = meta(vm, sim);
        if (!m.neighbor)
            continue;
        SimWants& s = simState(*ws, sim);
        WantPtr a = newInstance(*ws, sim, guid, subject, -1, nullptr);
        addAchievement(*ws, s, a);
        const bool fear = d->isFear();
        auto& slots = fear ? s.fears : s.wants;
        bool matched = false;
        int32_t points = 0, influence = 0;
        for (;;) {
            int found = -1;
            for (size_t i = 0; i < slots.size(); ++i)
                if (slots[i] && fulfils(vm, *ws, *slots[i], *a)) {
                    found = int(i);
                    break;
                }
            if (found < 0)
                break;
            if (!matched && !ws->updates.count(sim)) {
                // SetOldWants: the slots as they are, untimed.
                s.oldWants = s.wants;
                s.oldFears = s.fears;
            }
            matched = true;
            WantPtr w = slots[size_t(found)];
            points += w->data.score;
            influence += w->data.influence;
            a->data.score = w->data.score;
            a->data.influence = w->data.influence;
            a->data.flags = w->data.flags;
            w->data.time = uint32_t(personalTime(*ws, s));
            event(vm, *ws, {.kind = fear ? WantEvent::Kind::FearRealized : WantEvent::Kind::Achieved, .sim = sim,
                            .guid = guid, .subjectType = w->data.subjectType, .subjectValue = w->data.subjectValue,
                            .subjectData = w->data.subjectData, .slot = found, .fear = fear,
                            .points = w->data.score});
            slots[size_t(found)] = nullptr;
        }
        int32_t notifyPoints = points, notifyInfluence = influence;
        if (!matched) {
            notifyPoints = a->data.score;
            notifyInfluence = a->data.influence;
            a->data.score = 0;
            a->data.influence = 0;
            event(vm, *ws, {.kind = WantEvent::Kind::Satisfied, .sim = sim, .guid = guid, .subjectType = subject.type,
                            .subjectValue = subject.value, .subjectData = subject.data, .fear = fear});
        } else {
            changeAspirationScore(vm, sim, points);
            requestWantsUpdate(vm, sim, 3);
        }
        notifyScore(vm, *ws, m, *a, matched, notifyPoints, notifyInfluence, objectParam);
        any |= matched;
    }
    return any;
}

Exit wantsGenericCall(Context& c) {
    VM& vm = c.vm;
    WantSystem* ws = wantSystem(vm);
    const auto& t = c.thread.temps;
    int op = t[0];
    int16_t id = 0, arg = 0;
    if (op < 10) {
        const Object& me = c.me();
        if (!me.person)
            throw gameError(0x1C, "Force Recalc of Wants on an object that is not a Sim");
        id = neighborIdOf(me);
        arg = t[1];
    } else {
        id = t[1];
        if (!vm.neighbor(id))
            throw gameError(0x37, "Force Recalc of Wants: no neighbor " + std::to_string(id));
        arg = t[2];
        op -= 10;
    }
    if (!ws || id == 0)
        return Exit::True;
    switch (op) {
    case 0: requestWantsUpdate(vm, id, arg >= 0 && arg < 4 ? arg : 0); break;
    case 1: { // ClearAchievementHistory
        SimWants& s = simState(*ws, id);
        s.history.clear();
        break;
    }
    case 2:
    case 3: { // SetAspirationScore (simulator vtable 0x9c, BV 0x509634): normalized (2) or raw / 10 (3)
        const Meta m = meta(vm, id);
        if (!m)
            break;
        const int age = wants::ageEnum(pd(m, persondata::PersonAge));
        if (op == 2) {
            // The meter value, clamped, is kept; the raw score is
            // RawAspirationScore's (BV 0x506efc).
            const int16_t v = int16_t(std::clamp<int>(arg, -100, 100));
            setPd(m, kPdAspirationRaw, int16_t(rawAspirationScore(*ws->library, age, v) / 10));
            setPd(m, kPdAspirationScore, v);
        } else {
            const int32_t raw = std::clamp<int32_t>(int32_t(arg) * 10, -0x50000, 0x4FFF6);
            setPd(m, kPdAspirationRaw, int16_t(raw / 10));
            setPd(m, kPdAspirationScore, normalizedAspirationScore(*ws->library, age, raw));
        }
        break;
    }
    case 4: requestWantsUpdate(vm, id, 3); break; // RecheckGoals
    case 5: { // SetPersonalClock (hours)
        if (arg > 0) {
            SimWants& s = simState(*ws, id);
            s.lastClock = ws->clock;
            s.personal = int32_t(arg) * 3600;
        }
        break;
    }
    case 6: { // IncrementPersonalClock (hours)
        SimWants& s = simState(*ws, id);
        s.personal += int32_t(arg) * 3600;
        break;
    }
    case 8: ws->skipCheckTrees = !ws->skipCheckTrees; break;
    default: break; // 7: the lifetime want (BV)
    }
    return Exit::True;
}

Exit wantSatisfyPrimitive(Context& c) {
    // TryWantSatisfy (0xe46e70): the Sim (owner byte 0, data u16 1; byte 13
    // bit 0 = it is a neighbor ID, else an object ID), the want u32 3, the
    // subject (owner 7, data u16 8; byte 13 bit 1 = a Sim subject is a
    // neighbor ID) and the subject's number (owner 10, data u16 11).
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[13];
    auto neighborOf = [&](int16_t v, bool isNeighbor) -> int16_t {
        if (isNeighbor)
            return c.vm.neighbor(v) ? v : int16_t(0);
        const Object* s = c.vm.object(v);
        return s && s->person ? neighborIdOf(*s) : int16_t(0);
    };
    const int16_t simValue = c.get(o[0], int16_t(le16(&o[1])));
    const int16_t id = neighborOf(simValue, flags & 1);
    if (id == 0)
        throw gameError(0x1C, "Want Satisfy");
    const uint32_t guid = le32(&o[3]);
    WantSystem* ws = wantSystem(c.vm);
    if (!ws)
        return Exit::True; // no want data: nothing to satisfy
    const auto* d = ws->library->definition(guid);
    if (!d)
        throw gameError(0x07, "Want Satisfy: no want " + hexString(guid));
    const int16_t value = c.get(o[7], int16_t(le16(&o[8])));
    const int16_t number = c.get(o[10], int16_t(le16(&o[11])));
    WantSubject s;
    s.type = d->objectType;
    int16_t objectParam = 0;
    auto guidOf = [&](int16_t objectId) -> uint32_t {
        const Object* x = c.vm.object(objectId);
        return x ? x->guid() : 0;
    };
    switch (d->objectType) {
    case SubjectType::None: s.data = uint16_t(value); break;
    case SubjectType::Sim:
        s.value = uint16_t(neighborOf(value, flags & 2));
        s.data = uint16_t(number);
        if (s.value == 0)
            throw gameError(0x1C, "Want Satisfy subject");
        break;
    case SubjectType::Guid:
    case SubjectType::Category:
        s.type = SubjectType::Guid; // an object fulfils a category want by its GUID
        objectParam = value;
        s.data = uint16_t(number);
        if (value != 0) {
            s.value = guidOf(value);
            if (s.value == 0)
                throw gameError(0x17, "Want Satisfy object");
        }
        break;
    case SubjectType::Skill:
        s.value = uint16_t(value);
        s.data = uint16_t(number);
        break;
    case SubjectType::Career:
        s.value = guidOf(value);
        if (s.value == 0)
            throw gameError(0x17, "Want Satisfy career object");
        s.data = uint16_t(number);
        break;
    }
    markWantAchieved(c.vm, id, guid, s, objectParam);
    return Exit::True;
}

wants::Record wantRecord(VM& vm, int16_t id) {
    WantSystem* ws = wantSystem(vm);
    wants::Record r;
    if (!ws)
        return r;
    auto it = ws->sims.find(id);
    if (it == ws->sims.end())
        return r;
    SimWants& s = it->second;
    if (s.record) {
        r.version = s.record->version;
        r.lifetime = s.record->lifetime;
        r.locks = s.record->locks;
    }
    // FUN_00aedf4c: a Sim on the lot has its clock brought up to date first.
    if (s.read)
        personalTime(*ws, s);
    for (const auto& w : s.wants)
        if (w)
            r.wants.push_back(w->data);
    for (const auto& w : s.fears)
        if (w)
            r.fears.push_back(w->data);
    r.history.version = s.historyVersion;
    r.history.clock = s.personal;
    for (const auto& [guid, list] : s.history) {
        std::vector<wants::Instance> v;
        for (const auto& a : list)
            v.push_back(a->data);
        r.history.entries.emplace_back(guid, std::move(v));
    }
    return r;
}

bool wantRecordChanged(VM& vm, int16_t id) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return false;
    auto it = ws->sims.find(id);
    if (it == ws->sims.end())
        return false;
    const wants::Record now = wantRecord(vm, id);
    if (!it->second.record)
        return !now.wants.empty() || !now.fears.empty() || !now.history.entries.empty();
    const auto& was = *it->second.record;
    return now.wants != was.wants || now.fears != was.fears || !(now.history == was.history);
}

std::vector<std::string> compareWantState(VM& a, VM& b, size_t limit) {
    std::vector<std::string> out;
    WantSystem* wa = wantSystem(a);
    WantSystem* wb = wantSystem(b);
    if (!wa || !wb) {
        if (bool(wa) != bool(wb))
            out.push_back("want system in only one of them");
        return out;
    }
    size_t more = 0;
    auto diff = [&](const std::string& s) {
        if (out.size() < limit)
            out.push_back(s);
        else
            ++more;
    };
    std::set<int16_t> ids;
    for (const auto& [id, _] : wa->sims)
        ids.insert(id);
    for (const auto& [id, _] : wb->sims)
        ids.insert(id);
    for (int16_t id : ids) {
        const auto ra = wantRecord(a, id), rb = wantRecord(b, id);
        if (ra.wants != rb.wants)
            diff("Sim " + std::to_string(id) + " wants");
        if (ra.fears != rb.fears)
            diff("Sim " + std::to_string(id) + " fears");
        if (ra.history.clock != rb.history.clock)
            diff("Sim " + std::to_string(id) + " personal want clock " + std::to_string(ra.history.clock) + " vs " +
                 std::to_string(rb.history.clock));
        if (ra.history.entries != rb.history.entries)
            diff("Sim " + std::to_string(id) + " achievement history");
    }
    if (more)
        out.push_back("... and " + std::to_string(more) + " more");
    return out;
}

std::string subjectText(VM& vm, const wants::Instance& w) {
    WantSystem* ws = wantSystem(vm);
    switch (w.subjectType) {
    case SubjectType::None: return {};
    case SubjectType::Sim: return w.subjectValue ? personName(vm, int16_t(w.subjectValue)) : std::string();
    case SubjectType::Skill:
        if (ws)
            if (auto it = ws->library->tuning.skills.find(int(w.subjectValue)); it != ws->library->tuning.skills.end())
                return it->second;
        return w.subjectValue ? "skill " + std::to_string(w.subjectValue) : std::string();
    case SubjectType::Career:
        if (ws)
            if (auto it = ws->library->tuning.careers.find(w.subjectValue); it != ws->library->tuning.careers.end())
                return it->second;
        return w.subjectValue ? hexString(w.subjectValue) : std::string();
    case SubjectType::Category:
        if (ws)
            if (const auto* cat = ws->library->category(int32_t(w.subjectValue)))
                return cat->name;
        return w.subjectValue ? std::to_string(w.subjectValue) : std::string();
    case SubjectType::Guid: {
        if (!w.subjectValue)
            return {};
        if (auto d = vm.code().definitionByGuid(w.subjectValue))
            if (const ObjectDefinition* od = vm.code().definition(d->first, d->second))
                return od->fileName;
        return hexString(w.subjectValue);
    }
    }
    return {};
}

std::string wantText(VM& vm, const wants::Instance& w) {
    WantSystem* ws = wantSystem(vm);
    if (!ws)
        return hexString(w.guid);
    const auto* d = ws->library->definition(w.guid);
    if (!d)
        return hexString(w.guid);
    // Entries: 0 with a male subject, 1 with a female one, 2 without one
    // (a want about any Sim, subject 0, reads as that one; inferred).
    size_t entry = 0;
    const bool open = w.subjectType != SubjectType::None && w.subjectValue == 0;
    if (w.subjectType == SubjectType::Sim && w.subjectValue) {
        if (const Neighbor* n = vm.neighbor(int16_t(w.subjectValue)); n && n->personData[kPdGender] == 1)
            entry = 1;
    }
    std::string s = open ? ws->library->text(*d, 2) : std::string();
    if (s.empty())
        s = ws->library->text(*d, entry);
    if (s.empty())
        s = ws->library->text(*d, 2);
    if (s.empty())
        s = d->nodeText.empty() ? hexString(w.guid) : d->nodeText;
    const std::string subject = subjectText(vm, w);
    auto replace = [&](const std::string& token, const std::string& with) {
        for (size_t p; (p = s.find(token)) != std::string::npos;)
            s.replace(p, token.size(), with);
    };
    const int number = int(int16_t(w.subjectData)) * (d->integerMultiplier ? d->integerMultiplier : 1);
    replace("$Money", "§" + std::to_string(number));
    for (const char* tok : {"$ObjectTypePlural", "$ObjectType", "$Object", "$CareerTrack", "$Neighbor", "$Skill",
                            "$Interaction"})
        replace(tok, subject.empty() ? "?" : subject);
    for (const char* tok : {"$Level", "$Value"})
        replace(tok, std::to_string(number));
    return s;
}

} // namespace os2::vm
