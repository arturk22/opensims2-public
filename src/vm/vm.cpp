#include "engine.hpp"
#include "vm.hpp"

#include "hood_bridge.hpp"
#include "interactions.hpp"
#include "kill.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>

namespace os2::vm {

namespace {

uint64_t key(uint32_t group, uint16_t instance) { return uint64_t(group) << 32 | instance; }

template <class T>
const T* loadCached(std::unordered_map<uint64_t, std::unique_ptr<T>>& cache, ResourceIndex* index, uint32_t type,
                    uint32_t group, uint16_t instance) {
    const auto k = key(group, instance);
    if (auto it = cache.find(k); it != cache.end())
        return it->second.get();
    std::unique_ptr<T> value;
    if (index)
        if (auto data = index->read(Tgi{type, group, instance, 0}))
            value = std::make_unique<T>(T::parse(*data));
    return cache.emplace(k, std::move(value)).first->second.get();
}

std::string treeName(const Frame& f) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "0x%08X/0x%04X", f.group, f.instance);
    return std::string(buf) + " \"" + f.behavior->fileName + "\"";
}

template <class V>
auto& slot(V& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= values.size())
        throw VMError(std::string(what) + " " + std::to_string(idx) + " out of range (" +
                      std::to_string(values.size()) + ")");
    return values[size_t(idx)];
}

// Attributes and semi-attributes: an index past the end grows the array.
int16_t& growing(std::vector<int16_t>& values, int idx, const char* what) {
    if (idx < 0 || size_t(idx) >= kMaxAttributes)
        throw VMError(std::string(what) + " " + std::to_string(idx) + " out of range");
    if (size_t(idx) >= values.size())
        values.resize(size_t(idx) + 1);
    return values[size_t(idx)];
}

Person& personOf(Object& o) {
    if (!o.person)
        throw VMError("object " + std::to_string(o.id) + " is not a person");
    return *o.person;
}

// Field of the editor's OBJD numbering: 0-1 are the u32 version, i >= 2 the
// field at file offset 0x40 + 2i.
int16_t definitionField(const ObjectDefinition& d, int idx) {
    if (idx < 0)
        throw VMError("definition field " + std::to_string(idx) + " out of range");
    if (idx < 2)
        return int16_t(d.version >> (16 * idx));
    return int16_t(d.field(0x40 + 2 * size_t(idx)));
}

int16_t definitionField(const Object& o, int idx) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    return definitionField(*o.definition, idx);
}

} // namespace

uint32_t semiGlobalGroup(std::string_view name) { return (nameInstanceHash(name) & 0x00FFFFFF) | 0x7F000000; }

const Behavior* CodeLibrary::behavior(uint32_t group, uint16_t instance) {
    return loadCached(behaviors_, index_, kTypeBhav, group, instance);
}

const Constants* CodeLibrary::constants(uint32_t group, uint16_t instance) {
    return loadCached(constants_, index_, kTypeBcon, group, instance);
}

const ObjectDefinition* CodeLibrary::definition(uint32_t group, uint16_t instance) {
    return loadCached(definitions_, index_, kTypeObjd, group, instance);
}

const StringTable* CodeLibrary::strings(uint32_t group, uint16_t instance) {
    return loadCached(strings_, index_, kTypeStr, group, instance);
}

const Slots* CodeLibrary::slots(uint32_t group, uint16_t instance) {
    return loadCached(slots_, index_, kTypeSlot, group, instance);
}

const InteractionTable* CodeLibrary::interactions(uint32_t group, uint16_t instance) {
    try {
        return loadCached(interactions_, index_, kTypeTtab, group, instance);
    } catch (const std::exception&) {
        // A few TTABs carry bytes the parser does not explain: no interactions.
        return interactions_.emplace(key(group, instance), nullptr).first->second.get();
    }
}

const StringTable* CodeLibrary::interactionStrings(uint32_t group, uint16_t instance) {
    return loadCached(interactionStrings_, index_, kTypeTtas, group, instance);
}

void CodeLibrary::add(uint32_t group, uint16_t instance, InteractionTable t) {
    interactions_[key(group, instance)] = std::make_unique<InteractionTable>(std::move(t));
}

void CodeLibrary::addInteractionStrings(uint32_t group, uint16_t instance, StringTable t) {
    interactionStrings_[key(group, instance)] = std::make_unique<StringTable>(std::move(t));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Slots sl) {
    slots_[key(group, instance)] = std::make_unique<Slots>(std::move(sl));
}

std::optional<uint16_t> CodeLibrary::behaviorByName(uint32_t group, const std::string& name) {
    if (!namesScanned_ && index_) {
        namesScanned_ = true;
        for (const auto& loc : index_->ofType(kTypeBhav)) {
            const auto& tgi = loc.entry->tgi;
            const auto data = index_->read(loc);
            // The file name is the first field, a NUL-padded 64-byte string.
            const auto end = std::find(data.begin(), data.begin() + std::min<size_t>(data.size(), 64), 0);
            byName_[tgi.group].emplace(std::string(data.begin(), end), uint16_t(tgi.instance));
        }
    }
    auto g = byName_.find(group);
    if (g == byName_.end())
        return std::nullopt;
    auto it = g->second.find(name);
    return it == g->second.end() ? std::nullopt : std::optional<uint16_t>(it->second);
}

// Group of the resources a package keeps for itself (Sims' character files,
// lots): the index cannot address them, since they all share it.
constexpr uint32_t kPackageLocalGroup = 0xFFFFFFFF;

void CodeLibrary::indexDefinition(uint32_t group, uint16_t instance, const ObjectDefinition& d) {
    // Keeps the first OBJD of a GUID or name, like the rest of the index.
    byGuid_.emplace(d.guid(), std::pair{group, instance});
    byFileName_.emplace(d.fileName, std::pair{group, instance});
    if (d.masterId() && d.subIndex() == 0xFFFF)
        masters_.emplace(std::pair{group, d.masterId()}, instance);
    else if (d.masterId()) {
        auto& p = parts_[{group, d.masterId()}];
        if (std::find(p.begin(), p.end(), instance) == p.end()) {
            p.push_back(instance);
            std::sort(p.begin(), p.end());
        }
    }
}

void CodeLibrary::scanDefinitions() {
    if (guidsScanned_ || !index_)
        return;
    guidsScanned_ = true;
    for (const auto& loc : index_->ofType(kTypeObjd)) {
        const auto& tgi = loc.entry->tgi;
        if (tgi.group == kPackageLocalGroup)
            continue;
        indexDefinition(tgi.group, uint16_t(tgi.instance), ObjectDefinition::parse(index_->read(loc)));
    }
}

std::vector<uint16_t> CodeLibrary::parts(uint32_t group, uint16_t masterId) {
    scanDefinitions();
    auto it = parts_.find({group, masterId});
    return it == parts_.end() ? std::vector<uint16_t>{} : it->second;
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::definitionByName(const std::string& fileName) {
    scanDefinitions();
    if (auto it = byFileName_.find(fileName); it != byFileName_.end())
        return it->second;
    return std::nullopt;
}

std::vector<uint16_t> CodeLibrary::addPackage(DbpfPackage& pkg, uint32_t group) {
    // Once per group: objects already made from it point at its definitions.
    if (auto it = packages_.find(group); it != packages_.end())
        return it->second;
    scanDefinitions(); // first, so the package's OBJDs take over their GUIDs
    std::vector<uint16_t> added;
    uint32_t semi = 0;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.group != kPackageLocalGroup)
            continue;
        const auto instance = uint16_t(e.tgi.instance);
        switch (e.tgi.type) {
        case kTypeBhav: add(group, instance, Behavior::parse(pkg.read(e))); break;
        case kTypeBcon: add(group, instance, Constants::parse(pkg.read(e))); break;
        case kTypeStr: add(group, instance, StringTable::parse(pkg.read(e))); break;
        case kTypeObjf: add(group, instance, ObjectFunctions::parse(pkg.read(e))); break;
        case kTypeSlot: add(group, instance, Slots::parse(pkg.read(e))); break;
        case kTypeGlob: semi = semiGlobalGroup(SemiGlobal::parse(pkg.read(e)).semiGlobalName); break;
        case kTypeObjd: {
            auto d = ObjectDefinition::parse(pkg.read(e));
            byGuid_[d.guid()] = {group, instance};
            byFileName_[d.fileName] = {group, instance};
            add(group, instance, std::move(d));
            added.push_back(instance);
            break;
        }
        default: break;
        }
    }
    setSemiGlobal(group, semi);
    size_t labels = 0;
    if (const StringTable* t = strings(group, 0x100))
        for (const auto& e : t->entries)
            labels += e.language == lang::EnglishUS;
    setAttributeLabelCount(group, labels);
    packages_[group] = added;
    return added;
}

std::optional<uint16_t> CodeLibrary::masterDefinition(uint32_t group, uint16_t masterId) {
    scanDefinitions();
    if (auto it = masters_.find({group, masterId}); it != masters_.end())
        return it->second;
    return std::nullopt;
}

const ObjectFunctions* CodeLibrary::functions(uint32_t group, uint16_t instance) {
    return loadCached(functions_, index_, kTypeObjf, group, instance);
}

void CodeLibrary::add(uint32_t group, uint16_t instance, ObjectFunctions f) {
    functions_[key(group, instance)] = std::make_unique<ObjectFunctions>(std::move(f));
}

std::optional<std::pair<uint32_t, uint16_t>> CodeLibrary::definitionByGuid(uint32_t guid) {
    scanDefinitions();
    if (auto it = byGuid_.find(guid); it != byGuid_.end())
        return it->second;
    return std::nullopt;
}

size_t CodeLibrary::attributeLabelCount(uint32_t group) {
    if (auto it = labelCounts_.find(group); it != labelCounts_.end())
        return it->second;
    size_t n = 0;
    if (index_)
        if (auto data = index_->read(Tgi{kTypeStr, group, 0x100, 0}))
            for (const auto& e : StringTable::parse(*data).entries)
                n += e.language == lang::EnglishUS;
    return labelCounts_[group] = n;
}

uint32_t CodeLibrary::semiGlobalFor(uint32_t group) {
    if (auto it = semiGlobals_.find(group); it != semiGlobals_.end())
        return it->second;
    uint32_t semi = 0;
    if (index_)
        for (uint32_t instance : {0x01u, 0x80u})
            if (auto data = index_->read(Tgi{kTypeGlob, group, instance, 0})) {
                semi = semiGlobalGroup(SemiGlobal::parse(*data).semiGlobalName);
                break;
            }
    semiGlobals_[group] = semi;
    return semi;
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Behavior b) {
    byName_[group][b.fileName] = instance;
    behaviors_[key(group, instance)] = std::make_unique<Behavior>(std::move(b));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, Constants c) {
    constants_[key(group, instance)] = std::make_unique<Constants>(std::move(c));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, StringTable t) {
    strings_[key(group, instance)] = std::make_unique<StringTable>(std::move(t));
}

void CodeLibrary::add(uint32_t group, uint16_t instance, ObjectDefinition d) {
    indexDefinition(group, instance, d);
    definitions_[key(group, instance)] = std::make_unique<ObjectDefinition>(std::move(d));
}

Object::Array& Object::array(int idx) {
    if (idx < 0 || size_t(idx) >= kMaxObjectArrays)
        throw VMError("object array " + std::to_string(idx) + " out of range");
    if (size_t(idx) >= arrays.size())
        arrays.resize(size_t(idx) + 1);
    return arrays[size_t(idx)];
}

// --- Context ---------------------------------------------------------------

Object& Context::me() const {
    auto* o = vm.object(thread.me);
    if (!o)
        throw VMError("thread object " + std::to_string(thread.me) + " does not exist");
    return *o;
}

Object& Context::stackObject() const {
    auto* o = vm.object(frame.stackObject);
    if (!o)
        throw VMError("stack object " + std::to_string(frame.stackObject) + " does not exist");
    return *o;
}

// The neighbor whose ID is in the stack object (data owners 0x18, 0x20, 0x26);
// the message is the game's own (Sims2.exe).
static Neighbor& stackNeighbor(const Context& c) {
    Neighbor* n = c.vm.neighbor(c.frame.stackObject);
    if (!n)
        throw VMError("Missing neighbor for data access. (neighbor " + std::to_string(c.frame.stackObject) + ")");
    return *n;
}

// Person data of a neighbor: its Sim's while it is on the lot, else the neighborhood's copy.
static std::array<int16_t, kPersonDataCount>& neighborPersonData(const Context& c) {
    Neighbor& n = stackNeighbor(c);
    if (Object* sim = c.vm.neighborSim(c.frame.stackObject))
        return personOf(*sim).data;
    return n.personData;
}

// Neighbor in stack object (0x18), fields labelled in STR# 0xDD. Only those
// that do not need the neighborhood are implemented; "person age" is read
// from person data "Person Age" (inferred).
static int16_t neighborField(const Context& c, int16_t field) {
    Neighbor& n = stackNeighbor(c);
    switch (field) {
    case 0: { // person instance id: the Sim's object, 0 when it is not on the lot
        const Object* sim = c.vm.neighborSim(c.frame.stackObject);
        return sim ? sim->id : 0;
    }
    case 2: return neighborPersonData(c)[persondata::PersonAge];
    case 6: return n.house;
    default: return neighborhoodNeighborField(c.vm, c.frame.stackObject, n, field);
    }
}

// Definition of a multi-tile object's master (the OBJD of its group with the
// same master ID and sub index -1); an object that is not multi-tile, or whose
// master is not found, is its own master (as FreeSO does).
static const ObjectDefinition& masterDefinitionOf(VM& vm, const Object& o) {
    if (!o.definition)
        throw VMError("object " + std::to_string(o.id) + " has no definition");
    if (const uint16_t master = o.definition->masterId())
        if (auto instance = vm.code().masterDefinition(o.group, master))
            if (const ObjectDefinition* d = vm.code().definition(o.group, *instance))
                return *d;
    return *o.definition;
}

// Type attributes of an object's type: keyed by its master definition's GUID
// (inferred; the OBJD's "type attr guid" fields are labelled unused in The
// Sims 2), starting with "num type attributes" (field 90) entries.
static std::vector<int16_t>& typeAttributesOf(VM& vm, const Object& o) {
    const ObjectDefinition& d = masterDefinitionOf(vm, o);
    auto [it, added] = vm.typeAttributes.try_emplace(d.guid());
    if (added)
        it->second.resize(std::min<size_t>(uint16_t(definitionField(d, 90)), kMaxAttributes));
    return it->second;
}

constexpr int16_t kObjectDataUseCount = 62;

// Storage behind a data owner, or null for the computed/read-only ones.
// Owners whose index comes from another variable read it first.
static int16_t* storage(const Context& c, uint8_t owner, int16_t data) {
    auto& t = c.thread;
    switch (owner) {
    case 0x00: return &growing(c.me().attributes, data, "attribute");
    case 0x01: return &growing(c.stackObject().attributes, data, "stack object attribute");
    case 0x02: return &growing(c.me().semiAttributes, data, "semi-attribute");
    case 0x03: return &slot(c.me().data, data, "object data");
    case 0x04: return &slot(c.stackObject().data, data, "stack object data");
    case 0x05: return &growing(c.stackObject().semiAttributes, data, "stack object semi-attribute");
    case 0x06: return &slot(c.vm.globals, data, "global");
    case 0x08: return &slot(t.temps, data, "temp");
    case 0x09: return &slot(c.frame.params, data, "param");
    case 0x0B: return &slot(t.temps, slot(t.temps, data, "temp"), "temp[temp]");
    case 0x0D: { // stack object's temps: the temps belong to the object in the game (cTSObject +0x8A),
                 // here to its thread: the running one if it is me, else its Main's
        const ObjectId so = c.stackObject().id;
        if (so == t.me)
            return &slot(t.temps, data, "stack object temp");
        auto it = c.vm.threads.find(so);
        if (it == c.vm.threads.end())
            throw VMError("stack object " + std::to_string(so) + " has no thread for its temps");
        return &slot(it->second.temps, data, "stack object temp");
    }
    case 0x0C: return &slot(t.adRange, data, "check tree ad range");
    case 0x1C: return &slot(t.adPersonality, data, "check tree ad personality var");
    case 0x1D: return &slot(t.adMin, data, "check tree ad min");
    case 0x0E: return &slot(personOf(c.me()).motives, data, "motive");
    case 0x0F: return &slot(personOf(c.stackObject()).motives, data, "stack object motive");
    case 0x11: return &slot(personOf(c.stackObject()).motives, slot(t.temps, data, "temp"), "stack object motive[temp]");
    case 0x12: return &slot(personOf(c.me()).data, data, "person data");
    case 0x13: return &slot(personOf(c.stackObject()).data, data, "stack object person data");
    case 0x16:
        return &growing(c.stackObject().attributes, slot(c.frame.params, data, "param"), "stack object attribute[param]");
    case 0x19: return &slot(c.frame.locals, data, "local");
    case 0x22: return &slot(c.vm.neighborhoodData, data, "neighborhood data");
    case 0x24: return &growing(typeAttributesOf(c.vm, c.me()), data, "type attribute");
    case 0x25: return &growing(typeAttributesOf(c.vm, c.stackObject()), data, "stack object type attribute");
    case 0x20: return &slot(neighborPersonData(c), data, "neighbor's person data");
    case 0x1E: return &slot(personOf(c.me()).data, slot(t.temps, data, "temp"), "person data[temp]");
    case 0x1F:
        return &slot(personOf(c.stackObject()).data, slot(t.temps, data, "temp"), "stack object person data[temp]");
    case 0x32:
        return &growing(c.stackObject().semiAttributes, slot(c.frame.params, data, "param"),
                        "stack object semi-attribute[param]");
    // Object arrays: `data` is the array; the element is the one at its
    // iterator index or at temp 0.
    case 0x29: return &c.me().array(data).iterator;
    case 0x2A: return &c.stackObject().array(data).iterator;
    case 0x2B: {
        auto& a = c.me().array(data);
        return &slot(a.values, a.iterator, "my object array element");
    }
    case 0x2C: {
        auto& a = c.stackObject().array(data);
        return &slot(a.values, a.iterator, "stack object's object array element");
    }
    case 0x2D: return &slot(c.me().array(data).values, t.temps[0], "my object array element");
    case 0x2E: return &slot(c.stackObject().array(data).values, t.temps[0], "stack object's object array element");
    default: return nullptr;
    }
}

// Fields of a temporary token (data owners 0x27/0x28), numbered as in TempToken.
static int16_t& tokenProperty(TempToken& t) {
    return slot(t.token.properties, t.propertyIndex, "temporary token property");
}

static int16_t tokenField(TempToken& t, int16_t field) {
    switch (field) {
    case 0: return int16_t(t.kind);
    case 1: return t.inventoryId;
    case 2: return t.counted;
    case 3: return t.index;
    case 4: return int16_t(t.token.guid & 0xFFFF);
    case 5: return int16_t(t.token.guid >> 16);
    case 6: return t.token.flags;
    case 7: return int16_t(t.counted ? t.token.count : t.token.properties.size());
    case 8: return t.propertyIndex;
    case 9: return tokenProperty(t);
    default: throw VMError("temporary token field " + std::to_string(field) + " out of range");
    }
}

static void setTokenField(TempToken& t, int16_t field, int16_t value) {
    switch (field) {
    case 0:
        if (value < 0 || value > 3)
            throw VMError("temporary token: inventory kind " + std::to_string(value) + " out of range");
        t.kind = InventoryKind(value);
        return;
    case 1: t.inventoryId = value; return;
    case 2: t.counted = value != 0; return;
    case 3: t.index = value; return;
    case 4: t.token.guid = (t.token.guid & 0xFFFF0000) | uint16_t(value); return;
    case 5: t.token.guid = (t.token.guid & 0xFFFF) | uint32_t(uint16_t(value)) << 16; return;
    case 6: t.token.flags = value; return;
    case 7:
        if (t.counted)
            t.token.count = value;
        else if (value < 0 || size_t(value) > kMaxTokenProperties)
            throw VMError("temporary token: property count " + std::to_string(value) + " out of range");
        else
            t.token.properties.resize(size_t(value));
        return;
    case 8: t.propertyIndex = value; return;
    case 9: tokenProperty(t) = value; return;
    default: throw VMError("temporary token field " + std::to_string(field) + " out of range");
    }
}

// Room values, as STR# 0xDB labels them.
static std::string roomValueName(int16_t v) {
    static const char* const names[] = {"ambient light", "outside", "level", "area", "pool", "number of windows"};
    return v >= 0 && size_t(v) < std::size(names) ? names[v] : "value " + std::to_string(v);
}

static int16_t slotIndex(int16_t slot) {
    if (slot < 0)
        throw VMError("slot " + std::to_string(slot) + " out of range");
    return slot;
}

int16_t Context::get(uint8_t owner, int16_t data) const {
    if ((owner == 0x03 || owner == 0x04) && data == kObjectDataUseCount)
        return useCount(vm, owner == 0x03 ? me() : stackObject()); // computed (engine.hpp)
    if (int16_t* v = storage(*this, owner, data))
        return *v;
    switch (owner) {
    case 0x07: return data; // literal
    case 0x0A: return frame.stackObject;
    case 0x27: return tokenField(me().tempToken, data);
    case 0x28: return tokenField(stackObject().tempToken, data);
    case 0x15: return definitionField(stackObject(), data);
    case 0x33: return definitionField(masterDefinitionOf(vm, stackObject()), data);
    case 0x17: { // room [temp 0]
        auto it = vm.rooms.find(thread.temps[0]);
        // Room 0 is a tile that no room covers (fence lamps stand on such
        // tiles; "Is Objects's Room Outside?" asks about it): outside, with
        // nothing else known to be there (inferred; 370 failures in lot-run).
        if (it == vm.rooms.end() && thread.temps[0] == 0) {
            if (data == 1)
                return 1;
            if (data >= 0 && size_t(data) < VM::kRoomValueCount)
                return 0;
        }
        if (it == vm.rooms.end())
            throw VMError("room " + std::to_string(thread.temps[0]) + " does not exist");
        const int16_t& v = slot(it->second.values, data, "room value");
        if (!(it->second.known & (1u << data)))
            throw VMError("room " + std::to_string(thread.temps[0]) + ": " + roomValueName(data) + " not known");
        return v;
    }
    case 0x23: { // stack object's function: the action tree of its OBJf entry, 0 if none
        const Object& o = stackObject();
        const ObjectFunctions* f = vm.code().functions(o.group, o.definitionInstance);
        if (data < 0)
            throw VMError("object function " + std::to_string(data) + " out of range");
        return f && size_t(data) < f->functions.size() ? int16_t(f->functions[size_t(data)].action) : int16_t(0);
    }
    case 0x2F: {
        // Constants [temp]: bits 13-14 pick private, semi-global or global
        // tables (as for 0x1A), bits 3-12 the table and bits 0-2 the temp
        // holding the entry ("G - Logarithms Table" 0x140/0x141 are 0x4200 and
        // 0x4208 in "Get in Temp 0 - Logarithmic Modifier").
        auto* owner = vm.object(frame.codeOwner);
        if (!owner)
            throw VMError("code owner does not exist");
        const int scope = uint16_t(data) >> 13;
        if (scope > 2)
            throw VMError("constant [temp] " + hexString(uint16_t(data)) + " out of range");
        const auto& c = vm.resolveConstants(*owner, scope, uint16_t((data & 0x1FFF) >> 3));
        return slot(c.values, thread.temps[size_t(data & 7)], "constant [temp]");
    }
    // Slots: the object in a slot of mine / of the stack object, 0 if the slot
    // is empty; the slot is `data` or the temp it names. FreeSO reads slots the
    // same way, and also gives 0 past the object's last slot.
    case 0x10: return vm.containedIn(stackObject().id, slotIndex(data));
    case 0x14: return vm.containedIn(me().id, slotIndex(data));
    case 0x30: return vm.containedIn(me().id, slotIndex(slot(thread.temps, data, "temp")));
    case 0x31: return vm.containedIn(stackObject().id, slotIndex(slot(thread.temps, data, "temp")));
    case 0x18: return neighborField(*this, data);
    case 0x26: {
        auto def = vm.code().definitionByGuid(stackNeighbor(*this).guid);
        const ObjectDefinition* d = def ? vm.code().definition(def->first, def->second) : nullptr;
        if (!d)
            throw VMError("neighbor " + std::to_string(frame.stackObject) + " has no definition");
        return definitionField(*d, data);
    }
    case 0x1A: {
        // High bits pick the BCON (private, semi-global or global), low 7 bits the entry.
        auto* owner = vm.object(frame.codeOwner);
        if (!owner)
            throw VMError("code owner does not exist");
        const auto& c = vm.resolveConstants(*owner, uint16_t(data) >> 7);
        return slot(c.values, data & 0x7F, "constant");
    }
    default:
        ++vm.missingOwners[owner];
        throw VMError("data owner " + hexString(owner) + " not implemented");
    }
}

void Context::set(uint8_t owner, int16_t data, int16_t value) const {
    if (int16_t* v = storage(*this, owner, data)) {
        *v = value;
        return;
    }
    switch (owner) {
    case 0x0A: frame.stackObject = value; return;
    case 0x27: setTokenField(me().tempToken, data, value); return;
    case 0x28: setTokenField(stackObject().tempToken, data, value); return;
    case 0x07:
    case 0x10: // slots change by moving objects, not by writing to them
    case 0x14:
    case 0x30:
    case 0x31:
    case 0x15: // the editor marks only some definition fields read-only; writes are not supported yet
    case 0x17:
    case 0x18:
    case 0x1A:
    case 0x23:
    case 0x26:
    case 0x2F:
    case 0x33: throw VMError("data owner " + hexString(owner) + " is read-only");
    default:
        ++vm.missingOwners[owner];
        throw VMError("data owner " + hexString(owner) + " not implemented");
    }
}

// --- VM --------------------------------------------------------------------

VM::VM(CodeLibrary& code) : code_(code) { registerPrimitives(*this); }

Object& VM::createObject(uint32_t group, size_t attributeCount, ObjectId id) {
    if (id <= 0) {
        id = 1;
        while (objects_.count(id))
            ++id;
    } else if (objects_.count(id)) {
        throw VMError("object " + std::to_string(id) + " already exists");
    }
    Object& o = objects_[id];
    o.id = id;
    o.group = group;
    o.semiGroup = code_.semiGlobalFor(group);
    o.attributes.resize(attributeCount);
    if (o.semiGroup)
        o.semiAttributes.resize(code_.attributeLabelCount(o.semiGroup));
    o.data[objdata::ObjectId] = id;
    o.data[objdata::LeadTileObjectId] = id;
    o.data[objdata::SlotNumber] = -1;
    o.data[objdata::SemiAttributeCount] = int16_t(o.semiAttributes.size());
    return o;
}

Object& VM::instantiate(uint32_t group, uint16_t definitionInstance) {
    const ObjectDefinition* def = code_.definition(group, definitionInstance);
    if (!def)
        throw VMError("OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " not found");
    if (!def->masterId() || def->subIndex() != 0xFFFF)
        return instantiatePart(group, definitionInstance);
    const auto parts = code_.parts(group, def->masterId());
    if (parts.empty())
        throw VMError("multi-tile OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " has no parts");
    // The lead is the part with sub index 0, else the first.
    uint16_t lead = parts.front();
    for (uint16_t p : parts)
        if (const ObjectDefinition* d = code_.definition(group, p); d && d->subIndex() == 0) {
            lead = p;
            break;
        }
    Object& first = instantiatePart(group, lead);
    const ObjectId leadId = first.id;
    for (uint16_t p : parts)
        if (p != lead)
            instantiatePart(group, p).data[objdata::LeadTileObjectId] = leadId;
    return *object(leadId);
}

Object& VM::instantiatePart(uint32_t group, uint16_t definitionInstance, ObjectId id) {
    const ObjectDefinition* def = code_.definition(group, definitionInstance);
    if (!def)
        throw VMError("OBJD " + hexString(group) + "/" + hexString(definitionInstance) + " not found");
    Object& o = createObject(group, std::max<size_t>(def->numAttributes(), code_.attributeLabelCount(group)), id);
    o.definition = def;
    o.definitionInstance = definitionInstance;
    // Engine-maintained, as the lots' saved states show (600 of the 665 objects
    // of N001_Lot11; the rest are multi-tile parts and leftovers): the
    // number of container slots (type 0) of the object's SLOT.
    if (const Slots* sl = code_.slots(group, def->slotId()))
        o.data[objdata::SlotCount] = int16_t(std::count_if(sl->slots.begin(), sl->slots.end(),
                                                           [](const Slots::Slot& x) { return x.type == 0; }));
    // Object data 58 "object version" starts as the definition's (OBJD label
    // 49): a saved object whose version differs runs its load tree
    // (lot_resume.cpp). Inferred: the lots' states hold the definition's.
    o.data[58] = int16_t(def->field(0x40 + 2 * 49));
    if (def->objectType() == kObjectTypePerson)
        o.person.emplace();
    return o;
}

Object* VM::instantiateGuid(uint32_t guid) {
    auto def = code_.definitionByGuid(guid);
    return def ? &instantiate(def->first, def->second) : nullptr;
}

VM::RelationshipKey VM::relationshipKey(ObjectId id) {
    const Object* o = object(id);
    if (!o)
        throw VMError("object " + std::to_string(id) + " does not exist");
    if (o->person && o->person->data[persondata::NeighborId])
        return {true, o->person->data[persondata::NeighborId]};
    return {false, id};
}

Object* VM::neighborSim(int16_t id) {
    if (id == 0)
        return nullptr;
    for (auto& [_, o] : objects_)
        if (o.person && o.person->data[persondata::NeighborId] == id)
            return &o;
    return nullptr;
}

ObjectId VM::containedIn(ObjectId container, int16_t slot) const {
    if (container == 0 || slot < 0)
        return 0;
    for (const auto& [id, o] : objects_)
        if (o.data[objdata::ContainerId] == container && o.data[objdata::SlotNumber] == slot && id != container)
            return id;
    return 0;
}

uint16_t VM::functionTree(const Object& o, size_t function) {
    const ObjectFunctions* f = code_.functions(o.group, o.definitionInstance);
    return f && function < f->functions.size() ? f->functions[function].action : 0;
}

std::optional<Thread> VM::runFunction(ObjectId id, size_t function, size_t maxTurns) {
    const Object* o = object(id);
    if (!o)
        throw VMError("object " + std::to_string(id) + " does not exist");
    const uint16_t tree = functionTree(*o, function);
    if (!tree)
        return std::nullopt;
    Thread t = start(id, tree);
    for (size_t turn = 0; t.status == Thread::Status::Running && turn < maxTurns; ++turn)
        run(t, instructionsPerTurn);
    return t;
}

void VM::initialize(ObjectId id, const std::vector<ObjectId>& existing) {
    constexpr int kMaxInitDepth = 8;
    if (initDepth_ >= kMaxInitDepth)
        throw VMError("objects created by Inits nest too deep");
    const Object* o = object(id);
    if (!o)
        return;
    const ObjectId lead = o->data[objdata::LeadTileObjectId] ? o->data[objdata::LeadTileObjectId] : id;
    std::vector<ObjectId> parts;
    for (const auto& [other, x] : objects_)
        if ((other == id || x.data[objdata::LeadTileObjectId] == lead) &&
            !std::binary_search(existing.begin(), existing.end(), other))
            parts.push_back(other);
    ++initDepth_;
    try {
        for (ObjectId p : parts)
            if (auto t = runFunction(p, 0))
                createdInits.push_back(std::move(*t));
    } catch (...) {
        --initDepth_;
        throw;
    }
    --initDepth_;
    for (ObjectId p : parts)
        startMain(p);
}

bool VM::startMain(ObjectId id) {
    const Object* o = object(id);
    const uint16_t tree = o ? functionTree(*o, 1) : 0;
    if (!tree)
        return false;
    threads.insert_or_assign(id, start(id, tree));
    return true;
}

void VM::setClockGlobals() {
    globals[global::Hour] = int16_t(clock.hour);
    globals[global::Minute] = int16_t(clock.minute);
    globals[global::Second] = int16_t(clock.tick * 60 / std::max(clock.ticksPerMinute, 1));
    globals[global::DayOfMonth] = int16_t(clock.day);
    globals[global::Month] = int16_t(clock.month);
    globals[global::Year] = int16_t(clock.year);
    globals[global::TimeOfDay] = timeOfDay(clock.hour);
}

// cTSSimulator's time of day for an hour (Mac binary, FUN_004984d4, STR#
// 0xE1 "times of day"): 0 day 7-17, 1 dusk 18, 2 night 19-5, 3 dawn 6. The
// simulator sets global 4 from it whenever the hour changes.
int16_t timeOfDay(int hour) {
    if (hour == 6)
        return 3;
    if (hour >= 7 && hour < 18)
        return 0;
    if (hour == 18)
        return 1;
    return 2;
}

void VM::tick() {
    // The calendar is 30-day months, as FreeSO's clock (not confirmed).
    if (++clock.tick >= clock.ticksPerMinute) {
        clock.tick = 0;
        if (++clock.minute == 60) {
            clock.minute = 0;
            if (++clock.hour == 24) {
                // A new day (cTSSimulator::SimulateOneTick): Days Running
                // counts up, Day Of Week = Days Running % 7, and a new month
                // sets Maximum Day Number to 30.
                clock.hour = 0;
                ++globals[global::DaysRunning];
                globals[global::DayOfWeek] = int16_t(globals[global::DaysRunning] % 7);
                if (++clock.day > 30) {
                    clock.day = 1;
                    globals[global::MaxDayNumber] = 30;
                    if (++clock.month > 12)
                        clock.month = 1, ++clock.year;
                }
            }
        }
    }
    setClockGlobals();
    ++ticks;
    tickMotives(*this); // motives.cpp
    handleTimers();
    // Objects created or removed during the tick: the IDs are read first;
    // new objects get their turn from the next tick on.
    std::vector<ObjectId> ids;
    for (const auto& [id, _] : threads)
        ids.push_back(id);
    for (ObjectId id : ids) {
        auto it = threads.find(id);
        if (it == threads.end())
            continue;
        Thread& t = it->second;
        if (t.status == Thread::Status::Finished) {
            const Object* o = object(id);
            const uint16_t tree = o ? functionTree(*o, 1) : 0;
            if (!tree)
                continue;
            const size_t done = t.instructions;
            t = start(id, tree);
            t.instructions = done;
        }
        if (t.status == Thread::Status::Running)
            run(t, instructionsPerTurn);
    }
    flushKillQueue(*this); // kill.hpp
    followContainers();
}

void VM::followContainers() {
    // Contained objects are where their container is (as the saved lots keep
    // them and Snap Into puts them): what a Sim carries goes along with it.
    for (auto& [id, o] : objects_) {
        const ObjectId in = o.data[objdata::ContainerId];
        if (!in || in == id)
            continue;
        auto c = objects_.find(in);
        if (c == objects_.end())
            continue;
        const Object* top = &c->second;
        for (int depth = 0; depth < 8 && top->data[objdata::ContainerId]; ++depth) {
            auto up = objects_.find(top->data[objdata::ContainerId]);
            if (up == objects_.end() || &up->second == top)
                break;
            top = &up->second;
        }
        o.position = top->position;
        o.data[objdata::Room] = top->data[objdata::Room];
    }
}

Object* VM::object(ObjectId id) {
    auto it = objects_.find(id);
    return it == objects_.end() ? nullptr : &it->second;
}

std::pair<uint32_t, const Behavior*> VM::resolveTree(const Object& owner, uint16_t treeId) {
    uint32_t group = 0;
    if (treeId >= 0x100 && treeId < 0x1000)
        group = kGlobalGroup;
    else if (treeId >= 0x1000 && treeId < 0x2000)
        group = owner.group;
    else if (treeId >= 0x2000)
        group = owner.semiGroup;
    const Behavior* b = group ? code_.behavior(group, treeId) : nullptr;
    return {b ? group : 0, b};
}

const Constants& VM::resolveConstants(const Object& owner, uint16_t table) {
    if (table < 64)
        return resolveConstants(owner, 0, table);
    if (table < 128)
        return resolveConstants(owner, 1, uint16_t(table - 64));
    return resolveConstants(owner, 2, uint16_t(table - 128));
}

const Constants& VM::resolveConstants(const Object& owner, int scope, uint16_t table) {
    // Private and semi-global tables resolve against the code owner, also
    // when the running code is semi-global or global.
    uint32_t group;
    uint16_t instance;
    if (scope == 0)
        group = owner.group, instance = uint16_t(0x1000 + table);
    else if (scope == 1)
        group = owner.semiGroup, instance = uint16_t(0x2000 + table);
    else
        group = kGlobalGroup, instance = uint16_t(0x100 + table);
    const Constants* c = group ? code_.constants(group, instance) : nullptr;
    if (!c)
        throw VMError("BCON " + hexString(group) + "/" + hexString(instance) + " not found");
    return *c;
}

Thread VM::start(ObjectId me, uint16_t treeId, std::span<const int16_t> args, ObjectId stackObject,
                 ObjectId codeOwner) {
    Thread t;
    t.me = me;
    std::array<int16_t, 4> a{};
    std::copy_n(args.begin(), std::min<size_t>(args.size(), 4), a.begin());
    try {
        push(t, codeOwner ? codeOwner : me, treeId, a);
        t.stack.back().stackObject = stackObject;
    } catch (const std::exception& e) {
        fail(t, e.what());
    }
    return t;
}

void VM::push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args) {
    const Object* owner = object(codeOwner);
    if (!owner)
        throw VMError("code owner " + std::to_string(codeOwner) + " does not exist");
    auto [group, behavior] = resolveTree(*owner, treeId);
    if (!behavior)
        throw VMError("tree " + hexString(treeId) + " not found");
    if (t.stack.size() >= kMaxStackDepth)
        throw VMError("stack overflow");
    Frame f;
    f.behavior = behavior;
    f.group = group;
    f.instance = treeId;
    f.codeOwner = codeOwner;
    if (!t.stack.empty())
        f.stackObject = t.stack.back().stackObject;
    f.params.assign(std::max<size_t>(behavior->argCount, args.size()), 0);
    std::copy(args.begin(), args.end(), f.params.begin());
    f.locals.assign(behavior->localCount, 0);
    if (behavior->instructions.empty())
        throw VMError("tree " + hexString(treeId) + " has no instructions");
    t.stack.push_back(std::move(f));
}

void VM::fail(Thread& t, const std::string& message) {
    t.status = Thread::Status::Failed;
    t.error = message;
    constexpr size_t kShownFrames = 8;
    for (size_t i = 0; i < t.stack.size() && i < kShownFrames; ++i) {
        const Frame& f = t.stack[t.stack.size() - 1 - i];
        t.error += "\n  at " + treeName(f) + " #" + std::to_string(f.pc);
    }
    if (t.stack.size() > kShownFrames)
        t.error += "\n  ... " + std::to_string(t.stack.size() - kShownFrames) + " more frames";
}

bool VM::branch(Thread& t, Behavior::Target target) {
    for (;;) {
        switch (target.kind) {
        case Behavior::TargetKind::Instruction:
            t.stack.back().pc = target.index;
            return true;
        case Behavior::TargetKind::Error:
            fail(t, "error branch taken");
            return false;
        case Behavior::TargetKind::True:
        case Behavior::TargetKind::False: {
            const bool value = target.kind == Behavior::TargetKind::True;
            const bool interaction = t.stack.back().interaction;
            const bool foundAction = t.stack.back().foundAction;
            const bool subInteraction = t.stack.back().subInteraction;
            const std::optional<bool> forced = t.stack.back().callerResult;
            const bool rerun = t.stack.back().rerunCaller;
            t.stack.pop_back();
            if (t.stack.empty()) {
                t.status = Thread::Status::Finished;
                t.result = value;
                return false;
            }
            if (rerun) { // the primitive that ran it goes on
                t.rerunResult = value;
                return true;
            }
            if (subInteraction)
                endSubInteraction(*this, t); // and its Idle for Input branches on the result
            if (interaction) {
                // A queued interaction's action tree: the Idle for Input that ran it runs again.
                endInteraction(*this, t);
                if (!foundAction)
                    return true;
                t.interactionReturned = 0; // Gosub Found Action branches instead
                foundActionReturned(*this, t, value);
            }
            // The caller's call instruction branches on the returned value.
            const Frame& caller = t.stack.back();
            const auto& call = caller.behavior->instructions[caller.pc];
            target = forced.value_or(value) ? call.onTrue : call.onFalse;
            break;
        }
        }
    }
}

void VM::run(Thread& t, size_t maxInstructions) {
    for (size_t budget = maxInstructions; t.status == Thread::Status::Running;) {
        if (budget-- == 0) {
            fail(t, "instruction budget exhausted (" + std::to_string(maxInstructions) + ")");
            return;
        }
        if (!object(t.me)) {
            t.status = Thread::Status::Killed;
            return;
        }
        Frame& frame = t.stack.back();
        const auto& in = frame.behavior->instructions[frame.pc];
        if (trace)
            trace(t, frame, in);
        ++t.instructions;
        try {
            Context ctx{*this, t, frame, in};
            if (in.opcode >= 0x100) {
                // Call: four arguments, either literals, the temps 0-3 (all
                // literals 0xFFFF) or, with bit 0 of byte 12, (owner u8, data
                // u16) pairs. Node version 0 keeps only that bit of byte 12
                // (RunPrivateTreeParam::Read in the Mac binary; the rest is
                // stale: "Run Accepter Anim" has 0x69 there).
                const auto& o = in.operands;
                std::array<int16_t, 4> args{};
                if (o[12] & 1) {
                    const Object* owner = object(frame.codeOwner);
                    const Behavior* callee = owner ? resolveTree(*owner, in.opcode).second : nullptr;
                    const size_t n = callee ? std::min<size_t>(callee->argCount, 4) : 0;
                    for (size_t k = 0; k < n; ++k)
                        args[k] = ctx.get(o[k * 3], int16_t(le16(&o[k * 3 + 1])));
                } else if (std::all_of(o.begin(), o.begin() + 8, [](uint8_t b) { return b == 0xFF; })) {
                    std::copy_n(t.temps.begin(), 4, args.begin());
                } else {
                    for (size_t k = 0; k < 4; ++k)
                        args[k] = int16_t(le16(&o[k * 2]));
                }
                push(t, frame.codeOwner, in.opcode, args);
                continue;
            }
            auto it = primitives_.find(in.opcode);
            if (it == primitives_.end()) {
                ++missingPrimitives[in.opcode];
                fail(t, "primitive " + hexString(in.opcode) + " (" + primitiveName(in.opcode) + ") not implemented");
                return;
            }
            switch (it->second(ctx)) {
            case Exit::Yield: return;
            case Exit::Call: break;
            case Exit::Error: fail(t, "primitive " + primitiveName(in.opcode) + " returned error"); return;
            case Exit::True: branch(t, in.onTrue); break;
            case Exit::False: branch(t, in.onFalse); break;
            }
        } catch (const std::exception& e) { // VMError, or a resource that failed to parse
            fail(t, e.what());
        }
    }
}

} // namespace os2::vm
