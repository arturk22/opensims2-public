#pragma once

#include "formats/bhav.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"
#include "animation.hpp"
#include "world.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace os2 {
class DbpfPackage;
class ResourceIndex;
}

namespace os2::vm {

// Group of the global BHAVs/BCONs (opcodes 0x100-0xFFF, in objects.package).
constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
constexpr uint32_t kTypeBhav = 0x42484156;
constexpr uint32_t kTypeBcon = 0x42434F4E;
constexpr uint32_t kTypeGlob = 0x474C4F42;
constexpr uint32_t kTypeTtab = 0x54544142;
constexpr uint32_t kTypeObjf = 0x4F424A66;

// Group of the semi-global library a GLOB resource names: CRC24 of the
// lowercased name with the high byte 0x7F (holds for every GLOB in the game).
uint32_t semiGlobalGroup(std::string_view name);

constexpr uint32_t kTypeObjd = 0x4F424A44;
constexpr uint32_t kTypeStr = 0x53545223;
constexpr uint32_t kTypeSlot = 0x534C4F54;

// STR# with the tree names Run Tree by Name refers to ("Call Named Tree prim
// string set"), in object, semi-global and global groups.
constexpr uint16_t kNamedTreeStrings = 0x12F;
// STR# of an object group with its mesh group and material names.
constexpr uint16_t kMeshGroupStrings = 0x87;
constexpr uint16_t kMaterialStrings = 0x88;

// Sizes of the engine-defined variable sets, from the labels of the game's own
// SimAntics editor (objects.package, group 0x7FE59FD0: STR# 0x81 global labels,
// 0x8D object data, 0xC8 person data, 0x86 motives). They match the highest
// index any BHAV uses.
constexpr size_t kTempCount = 20;
constexpr size_t kObjectDataCount = 88;
constexpr size_t kGlobalCount = 54;
constexpr size_t kPersonDataCount = 170;
constexpr size_t kMotiveCount = 16;
constexpr size_t kMaxStackDepth = 128;
// Attributes and semi-attributes grow when written past their initial size;
// this bounds that growth (the BHAVs use at most 61 and 35).
constexpr size_t kMaxAttributes = 256;

// Object data fields the engine maintains or reads (names as in STR# 0x8D).
namespace objdata {
constexpr int16_t Direction = 1;
constexpr int16_t ContainerId = 2;
constexpr int16_t SlotNumber = 3;
constexpr int16_t Flags = 8; // bit 0 can be stepped over, 2 allow object and person intersection, 4 allow person intersection (STR# 0x8E)
constexpr int16_t ObjectId = 11;
constexpr int16_t LeadTileObjectId = 19;
constexpr int16_t Room = 29;
constexpr int16_t Category = 59;
constexpr int16_t SlotCount = 67;
constexpr int16_t SemiAttributeCount = 79;
} // namespace objdata

// Person data fields the engine reads (names as in STR# 0xC8).
namespace persondata {
constexpr int16_t NeighborId = 31;
constexpr int16_t PersonAge = 58;
constexpr int16_t FamilyNumber = 61;
} // namespace persondata

// Globals the engine reads or writes (names as in STR# 0x81).
namespace global {
constexpr int16_t Hour = 0;
constexpr int16_t DayOfMonth = 1;
constexpr int16_t Minute = 5;
constexpr int16_t Second = 6;
constexpr int16_t Month = 7;
constexpr int16_t Year = 8;
constexpr int16_t SelectedPersonId = 3;
constexpr int16_t CurrentFamily = 9;
constexpr int16_t Speed = 16;
constexpr int16_t Paused = 17;
constexpr int16_t HouseRadioStation = 31;
constexpr int16_t CurrentHouse = 10;
constexpr int16_t BudgetMod10000 = 13; // the current family's funds % 10000
constexpr int16_t BudgetDiv10000 = 14; // and / 10000
constexpr int16_t CurrentNeighborhoodId = 40;
constexpr int16_t LotZoning = 41;
} // namespace global

// Families hold at most 8 Sims (the game's limit; Generic Sims Call "add to
// family" fails past it).
constexpr size_t kMaxFamilySize = 8;

// A Generic Sims Call (or other primitive) that asks the engine for something
// outside the simulation: camera, UI, loading, lot transitions, ... It is
// recorded for the host to act on, with the state the call reads its
// arguments from (STR# 0xDC documents them in the temps).
struct EngineRequest {
    uint8_t call = 0; // Generic Sims Call index (STR# 0xDC)
    int16_t me = 0;
    int16_t stackObject = 0;
    std::array<int16_t, kTempCount> temps{};
};
// Requests the host has not taken yet; the oldest are dropped past this.
constexpr size_t kMaxEngineRequests = 1024;

// Object arrays (Array Operation, data owners 0x29-0x2E) are created on
// demand; this bounds their number and length.
constexpr size_t kMaxObjectArrays = 32;
constexpr size_t kMaxArrayLength = 4096;

// Relationship variables grow when written past their end; this bounds that
// (the trees use indices up to 11).
constexpr size_t kMaxRelationshipVars = 32;

// Object types (OBJD "type", STR# 0xF6).
constexpr uint16_t kObjectTypePerson = 2;
constexpr uint16_t kObjectTypeDoor = 8;
constexpr uint16_t kObjectTypeWindow = 9;
constexpr uint16_t kObjectTypeStairs = 10;
constexpr uint16_t kObjectTypeModularStairs = 11;
constexpr uint16_t kObjectTypeModularStairsPortal = 12;
constexpr uint16_t kObjectTypeVehicle = 13;

// A Sim of the neighborhood, on this lot or not, known by its neighbor ID.
// The neighborhood keeps a copy of its person data (cTSNeighbor has a person
// data array); while the Sim is on the lot, its object's person data is used.
struct Neighbor {
    uint32_t guid = 0; // of the Sim's OBJD
    int16_t house = 0; // house (lot) number; 0 = none
    std::array<int16_t, kPersonDataCount> personData{};
    // Outfit keys of the Sim's character file (category bit << 16 | part;
    // see hood_bridge.hpp), for Generic Sims Call 32.
    std::vector<uint32_t> outfits;
};

// Inventories (Manage Inventory) and their tokens grow on demand; this bounds them.
constexpr size_t kMaxInventoryTokens = 4096;
constexpr size_t kMaxTokenProperties = 256;

// An inventory token: memories, skills, wants, food units, ... are singular
// tokens of an object GUID with flags and a list of properties; counted tokens
// (gifts, outfits, games) have a category and a count instead.
struct InventoryToken {
    uint32_t guid = 0;
    int16_t flags = 0;
    std::vector<int16_t> properties;
    int16_t category = 0; // counted
    int16_t count = 0;    // counted
};

// Inventories are global, of a lot ("house"), of a family or of a neighbor
// (the order of "Which Inventory(G = 0, H, F, N)" in the editor's labels,
// STR# 0xFC), each identified by an ID except the global one.
enum class InventoryKind : uint8_t { Global, Lot, Family, Neighbor };

struct Inventory {
    std::vector<InventoryToken> singular;
    std::vector<InventoryToken> counted;
};

// The token Manage Inventory reads from an inventory, with where it came from,
// so its properties can be changed and saved back. Fields as data owners
// 0x27/0x28 see them, labelled in STR# 0xFC: 0 inventory kind, 1 inventory ID,
// 2 singular (0) or counted, 3 token index, 4-5 GUID low/high word, 6 flags,
// 7 property count, 8 property index, 9 property value (at that index).
struct TempToken {
    InventoryToken token;
    bool loaded = false;
    InventoryKind kind = InventoryKind::Global;
    int16_t inventoryId = 0;
    bool counted = false;
    int16_t index = 0; // 1-based, as in the trees
    int16_t propertyIndex = 0;
};

struct VMError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// BHAVs and BCONs by (group, instance), read on demand from a ResourceIndex
// and/or added directly (tests). Returned pointers stay valid.
class CodeLibrary {
public:
    CodeLibrary() = default;
    explicit CodeLibrary(ResourceIndex* index) : index_(index) {}
    ResourceIndex* index() const { return index_; }

    const Behavior* behavior(uint32_t group, uint16_t instance);
    const Constants* constants(uint32_t group, uint16_t instance);
    const ObjectDefinition* definition(uint32_t group, uint16_t instance);
    const StringTable* strings(uint32_t group, uint16_t instance);
    // SLOT (group, instance): an object's slots, named by its OBJD "slots id".
    const Slots* slots(uint32_t group, uint16_t instance);
    // Instance of the BHAV of `group` with this file name; the first call reads
    // the name of every BHAV.
    std::optional<uint16_t> behaviorByName(uint32_t group, const std::string& name);
    // OBJD (group, instance) with this GUID; the first scan reads every OBJD.
    std::optional<std::pair<uint32_t, uint16_t>> definitionByGuid(uint32_t guid);
    // OBJf with the same (group, instance) as an OBJD: the object's function table.
    const ObjectFunctions* functions(uint32_t group, uint16_t instance);
    // OBJD instance of the master of a multi-tile object of `group` (the OBJD
    // with that master ID and sub index -1), read with the GUID scan.
    std::optional<uint16_t> masterDefinition(uint32_t group, uint16_t masterId);
    // OBJD instances of the parts of that multi-tile object (same master ID,
    // sub index other than -1), by instance.
    std::vector<uint16_t> parts(uint32_t group, uint16_t masterId);
    // OBJD (group, instance) with this file name ("Trash Can - Outside"); the
    // first OBJD of a name, as for GUIDs.
    std::optional<std::pair<uint32_t, uint16_t>> definitionByName(const std::string& fileName);
    // Semi-global group the objects of `group` use (from their GLOB), 0 if none.
    uint32_t semiGlobalFor(uint32_t group);
    // Number of (English) labels in the group's STR# 0x100: the attribute
    // labels of an object group, the semi-attribute labels of a semi-global one.
    size_t attributeLabelCount(uint32_t group);

    void add(uint32_t group, uint16_t instance, Behavior b);
    void add(uint32_t group, uint16_t instance, Constants c);
    void add(uint32_t group, uint16_t instance, ObjectDefinition d);
    void add(uint32_t group, uint16_t instance, StringTable t);
    void add(uint32_t group, uint16_t instance, Slots s);
    void add(uint32_t group, uint16_t instance, ObjectFunctions f);
    void setSemiGlobal(uint32_t group, uint32_t semiGroup) { semiGlobals_[group] = semiGroup; }
    void setAttributeLabelCount(uint32_t group, size_t n) { labelCounts_[group] = n; }
    // Adds the code and definitions of a package's own group (0xFFFFFFFF, as
    // in a Sim's character file) as `group`: the index cannot tell those
    // apart, since every such package uses the same group. Its OBJDs take
    // over their GUIDs. Returns the OBJD instances it added.
    std::vector<uint16_t> addPackage(DbpfPackage& package, uint32_t group);

private:
    // A null entry records a resource known to be missing.
    template <class T>
    using Cache = std::unordered_map<uint64_t, std::unique_ptr<T>>;

    ResourceIndex* index_ = nullptr;
    Cache<Behavior> behaviors_;
    Cache<Constants> constants_;
    Cache<ObjectDefinition> definitions_;
    Cache<StringTable> strings_;
    Cache<Slots> slots_;
    Cache<ObjectFunctions> functions_;
    std::map<std::pair<uint32_t, uint16_t>, uint16_t> masters_;
    std::map<std::pair<uint32_t, uint16_t>, std::vector<uint16_t>> parts_;
    std::unordered_map<std::string, std::pair<uint32_t, uint16_t>> byFileName_;
    std::map<uint32_t, std::vector<uint16_t>> packages_; // addPackage
    void indexDefinition(uint32_t group, uint16_t instance, const ObjectDefinition& d);
    void scanDefinitions();
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint16_t>> byName_;
    bool namesScanned_ = false;
    std::unordered_map<uint32_t, uint32_t> semiGlobals_;
    std::unordered_map<uint32_t, size_t> labelCounts_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint16_t>> byGuid_;
    bool guidsScanned_ = false;
};

using ObjectId = int16_t;

// An interaction pushed onto an object's queue (Push Interaction). There is
// no queue processing yet: the record keeps what the operand says, for the
// scheduler to come.
struct QueuedInteraction {
    ObjectId object = 0;      // whose interaction (TTAB) it is
    uint16_t interaction = 0; // index into that object's interactions
    uint8_t priority = 0;     // STR# 0xE0: inherited, maximum, autonomous, user driven
    uint8_t flags = 0;        // byte 3, not understood
    ObjectId pushedBy = 0;    // the object whose thread pushed it
    std::array<uint8_t, 16> operands{}; // the whole operand, for what is not understood yet
};
constexpr size_t kMaxQueuedInteractions = 64;

// State only Sims have.
struct Person {
    std::array<int16_t, kPersonDataCount> data{};
    std::array<int16_t, kMotiveCount> motives{};
};

// Where an object is in the lot, as the lot's saved state has it: the tile
// position (tile centres at .5), the level (1 = the ground) and the
// direction in object data 1. Objects out of the world (controllers, ones
// in a slot or an inventory) are at (-1, -1).
struct Position {
    float x = -1, y = -1;
    int level = 0;
    bool inWorld() const { return x >= 0 && y >= 0; }
};

struct Object {
    ObjectId id = 0;
    uint32_t group = 0;     // private BHAVs/BCONs (the group of the object's OBJD)
    uint32_t semiGroup = 0; // semi-global BHAVs/BCONs, 0 if none
    const ObjectDefinition* definition = nullptr; // shared by every instance; may be null (tests)
    uint16_t definitionInstance = 0;              // of the OBJD (and of its OBJf)
    // Private to each instance: attributes are declared by the object, semi-attributes
    // by its semi-global library (both labelled in STR# 0x100 of their group).
    std::vector<int16_t> attributes;
    std::vector<int16_t> semiAttributes;
    std::array<int16_t, kObjectDataCount> data{};
    std::optional<Person> person;
    // Object arrays, each with an iterator index.
    struct Array {
        std::vector<int16_t> values;
        int16_t iterator = 0;
    };
    std::vector<Array> arrays;
    Array& array(int idx); // created on demand; throws VMError past kMaxObjectArrays
    // Materials set by Change Material: mesh group name ("" = every group) -> material name.
    std::map<std::string, std::string> materials;
    // Data owners 0x27 (mine) and 0x28 (the stack object's).
    TempToken tempToken;
    // Interactions pushed onto this object (a Sim, usually), oldest first.
    std::vector<QueuedInteraction> queue;
    Position position;
    // The route the object is walking (Go To Routing Slot, Go To Relative
    // Position), while it walks: the points still ahead (the next first; tile
    // centres, then the goal point) on its level, the facing at the end (-1:
    // as it arrives) and the object it walks to. The instruction that walks
    // it keeps yielding until the last point is reached; it is kept here for
    // the renderer to animate the walk.
    struct Route {
        std::vector<std::array<float, 2>> points;
        int level = 0;
        int facing = -1;
        ObjectId target = 0;
        // The instruction walking it: its thread's stack depth, tree and node.
        size_t depth = 0;
        const Behavior* tree = nullptr;
        uint16_t node = 0;
    };
    std::optional<Route> route;
    // What the animation primitives play on it (animation.hpp).
    AnimationState animation;

    uint32_t guid() const { return definition ? definition->guid() : 0; }
};

// Multi-tile objects: the parts point at the lead tile's object (object data
// "lead tile object ID"); the lead itself may hold 0 (inferred).
inline ObjectId multiTileGroup(const Object& x) {
    const ObjectId lead = x.data[objdata::LeadTileObjectId];
    return lead ? lead : x.id;
}

struct Frame {
    const Behavior* behavior = nullptr;
    uint32_t group = 0;  // where the BHAV was found
    uint16_t instance = 0;
    ObjectId codeOwner = 0; // object whose private/semi-global code and constants are in scope
    uint16_t pc = 0;
    // Starts as the caller's; changing it does not change the caller's (subroutines
    // that look an object up return its ID in a temp).
    ObjectId stackObject = 0;
    std::vector<int16_t> params;
    std::vector<int16_t> locals;
};

// Temps belong to the thread, so subroutines can return values in them;
// parameters, locals and the stack object belong to each frame.
struct Thread {
    // Killed: the object running the thread was removed.
    enum class Status { Running, Finished, Failed, Killed };

    ObjectId me = 0;
    std::array<int16_t, kTempCount> temps{};
    std::vector<Frame> stack;

    Status status = Status::Running;
    bool result = false;   // return value of the root tree once Finished
    std::string error;     // once Failed: the message and the stack at the failure
    size_t instructions = 0; // total executed
};

// Outcome of a primitive. Yield keeps the thread on the same instruction,
// which runs again on the next call to VM::run. Call: the primitive pushed a
// tree (VM::call); the instruction branches on its result when it returns.
enum class Exit : uint8_t { True, False, Error, Yield, Call };

class VM;

// What a primitive sees while it runs.
struct Context {
    VM& vm;
    Thread& thread;
    Frame& frame;
    const Behavior::Instruction& instruction;

    Object& me() const;
    Object& stackObject() const; // throws VMError if there is none

    // Variable access by data owner ("scope") and data value, as in the operands
    // (owner names in STR# 0x84 of the editor labels).
    int16_t get(uint8_t owner, int16_t data) const;
    void set(uint8_t owner, int16_t data, int16_t value) const;
};

using Primitive = std::function<Exit(Context&)>;

class VM {
public:
    explicit VM(CodeLibrary& code);

    CodeLibrary& code() { return code_; }

    // Objects get the lowest free id from 1 up, or `id` (a lot's saved
    // objects keep theirs; throws if it is taken). Attributes start with
    // `attributeCount` entries, semi-attributes with one per label of the
    // semi-global library. Object data "object id" and "lead tile object ID"
    // are the new id (a lot's saved single-tile objects lead themselves).
    Object& createObject(uint32_t group, size_t attributeCount = 0, ObjectId id = 0);
    // An instance of the OBJD (group, instance): attributes sized by the larger of
    // its "num attributes" and the group's attribute labels; Sims get a Person.
    // A multi-tile master (master ID set, sub index -1) is not an object
    // itself: each of its parts is created, they all lead to the first one
    // (sub index 0, else the lowest), and that one is returned.
    Object& instantiate(uint32_t group, uint16_t definitionInstance);
    // One instance of exactly that OBJD (a part stays a single part).
    Object& instantiatePart(uint32_t group, uint16_t definitionInstance, ObjectId id = 0);
    // An instance of the object with this GUID, or null if no OBJD has it.
    Object* instantiateGuid(uint32_t guid);
    Object* object(ObjectId id);
    // --- World ---------------------------------------------------------------
    // The lot's walls, floors and rooms (vm::loadLot builds it); without one
    // (single trees, tests) positions mean nothing to the primitives that
    // need them, which then fail with the reason.
    std::optional<World> world;
    // Walking speed in tiles per tick: 1.37 m/s, the speed of the adult walk
    // animation, with a tile taken as a metre and a tick as 1/30 s (30 ticks
    // a game minute, a game minute a second at normal speed; inferred).
    float walkTilesPerTick = 1.37f / 30.0f;
    // Lengths of the animations the animation primitives play (animation.cpp).
    AnimationLibrary animations;
    // The tile an object stands on (its position rounded down), whether or
    // not it is in the world.
    static TilePos tileOf(const Object& o) {
        return {int(std::floor(o.position.x)), int(std::floor(o.position.y)), o.position.level};
    }
    // Whether an object stands in the way of Sims walking: placed in the world
    // (not in a slot), not a Sim, not a door, window or stair (stairs join
    // levels, which routing does not do yet), and none of the object data
    // "flags" that let people through (can be stepped over, allow object and
    // person intersection, allow person intersection). Inferred from what the
    // Inits set: flowers, paintings, emitters and doors set them, furniture
    // does not. Wall-mounted lamps and mirrors set none, so they block the
    // tile in front of their wall too.
    static bool blocksRouting(const Object& o);
    // The blocked tiles of a level, leaving out the objects in `ignore` (and
    // the other parts of their multi-tile objects). Needs a world.
    BlockedGrid blockedTiles(int level, std::span<const ObjectId> ignore = {});
    // A tile is in the world, has a floor and nothing that blocks stands on it.
    bool tileFree(TilePos t, std::span<const ObjectId> ignore = {});
    // Objects placed on a tile (in the world and not in a slot), by ID.
    std::vector<ObjectId> objectsOnTile(TilePos t) const;

    // Where a walker may stand to use a routing slot of `target`: goal
    // tiles with their costs, the exact point and facing of each (facing -1:
    // any). See routingGoals in routing.cpp for how the SLOT fields are read.
    struct RouteGoals {
        int level = 0;
        std::vector<PathGoal> goals;
        std::vector<std::array<float, 2>> points;
        std::vector<int> facings;
        bool onTarget = false; // the goal is on the target's own tile (its parts do not block it)
    };
    RouteGoals routingGoals(const Slots::Slot& slot, const Object& target);

    // Threads of a removed object are killed the next time they run.
    void removeObject(ObjectId id) { objects_.erase(id); }
    const std::map<ObjectId, Object>& objects() const { return objects_; }

    // Containment: an object is in slot "slot number" (object data 3) of the
    // object "container id" (object data 2); outside of any slot those are 0
    // and -1, as in the lot's saved state. The object in that slot, or 0.
    // Data owners 0x10/0x14/0x30/0x31 read slots this way.
    ObjectId containedIn(ObjectId container, int16_t slot) const;

    // --- Scheduler -----------------------------------------------------------
    // Each object has one thread of its own. Its Init (OBJf function 0) runs
    // to completion when the object is initialized, as a check tree: sleeps
    // count down without waiting for ticks (as FreeSO runs entry points).
    // Then its Main (function 1) runs a turn every tick, in object ID order,
    // until it yields; a Main that returns starts again on the next tick
    // (as in FreeSO).

    // Action tree of an object's OBJf function, 0 if it has none.
    uint16_t functionTree(const Object& o, size_t function);
    // Runs an OBJf function of `id` to completion in a thread of its own and
    // returns that thread (Finished, Failed, Killed; Running if it was still
    // going after `maxTurns` yields), or nothing if the object has no such function.
    std::optional<Thread> runFunction(ObjectId id, size_t function, size_t maxTurns = 1000);
    // Starts the object's Main in its thread; false if it has none.
    bool startMain(ObjectId id);
    std::map<ObjectId, Thread> threads; // the Main threads
    // Instructions a thread may run in one turn before it fails as an
    // infinite loop.
    size_t instructionsPerTurn = 100000;
    // Create New Object Instance initializes the new object (runs its Init
    // and, with the scheduler running, starts its Main), as FreeSO does; off
    // for single trees (bhav-sweep), where nothing else would run.
    bool initCreatedObjects = false;
    // Init threads of objects created while running (initCreatedObjects), for the host to look at.
    std::vector<Thread> createdInits;
    // Runs the Init of `id` and of the other parts of its multi-tile object
    // that are not in `existing` (the IDs before it was created), then starts
    // their Mains. Objects created by Inits that create objects nest; past a
    // few levels that is an error.
    void initialize(ObjectId id, const std::vector<ObjectId>& existing);

    // Game time, kept in the globals Hour, Minute, Second, Day of Month,
    // Month and Year. A minute is `ticksPerMinute` ticks (30, as in FreeSO's
    // clock for The Sims 1; not confirmed for The Sims 2).
    struct Clock {
        int ticksPerMinute = 30;
        int tick = 0; // within the minute
        int minute = 0, hour = 8, day = 1, month = 1, year = 1;
    } clock;
    void setClockGlobals();
    uint64_t ticks = 0;
    // One tick: the clock, then a turn of every Main thread.
    void tick();

    std::array<int16_t, kGlobalCount> globals{};

    // Relationship variables from one Sim or object to another. Sims are keyed
    // by neighbor ID (person data), so both ways of naming them meet; other
    // objects by object ID.
    struct RelationshipKey {
        bool neighbor = false;
        int16_t id = 0;
        auto operator<=>(const RelationshipKey&) const = default;
    };
    std::map<std::pair<RelationshipKey, RelationshipKey>, std::vector<int16_t>> relationships;
    // Key of an object (its neighbor ID if it is a Sim that has one).
    RelationshipKey relationshipKey(ObjectId id);

    // The neighborhood's families (funds, house, flags; members are the
    // neighbors whose person data "family number" is the family), lots
    // (zoning, position) and "neighborhood data" (data owner 0x22, labelled in
    // STR# 0xF9: level complete, tutorial progress, tutorial house number,
    // hide arrow). vm::loadNeighborhood (hood_bridge.hpp) fills them in.
    struct Family {
        int32_t funds = 0;
        int16_t house = 0;
        uint32_t flags = 0; // FAMI flags: bit 0 telephone, bit 3 computer (inferred)
        int16_t friends = 0;
    };
    std::map<int16_t, Family> families;
    struct LotRecord {
        int16_t zoning = 0; // 0 residential, 1 community
        float x = 0, y = 0; // neighborhood position
    };
    std::map<int16_t, LotRecord> lots;
    std::array<int16_t, 4> neighborhoodData{};

    // Neighbors by neighbor ID. Without a neighborhood they are only
    // the ones added here (tests, stand-ins).
    std::map<int16_t, Neighbor> neighbors;

    // The family tree by neighbor ID ("Family Relationship Array"), built by
    // Generic Sims Call 21-23 and searched by Set to Next 21-23 and 31. The
    // neighborhood's family ties (loadNeighborhood) fill it in first.
    struct FamilyTies {
        std::set<int16_t> parents, children, spouses;
    };
    std::map<int16_t, FamilyTies> familyTree;

    // Type attributes (data owners 0x24/0x25): shared by every object of a
    // type, keyed by the GUID of its (master) definition; they grow on demand.
    std::map<uint32_t, std::vector<int16_t>> typeAttributes;

    // Rooms of the lot by room ID, with the values labelled in STR# 0xDB
    // (0 ambient light 0-100, 1 outside, 2 level, 3 area, 4 pool, 5 number of
    // windows), read by data owner 0x17 with the room in temp 0. A lot fills
    // in the ones it can tell (vm::loadLot: level and area); reading a value
    // that is not known is an error.
    static constexpr size_t kRoomValueCount = 6;
    struct Room {
        std::array<int16_t, kRoomValueCount> values{};
        uint8_t known = 0x3F; // bit per value
    };
    std::map<int16_t, Room> rooms;
    Neighbor* neighbor(int16_t id) {
        auto it = neighbors.find(id);
        return it == neighbors.end() ? nullptr : &it->second;
    }
    // The Sim on the lot with this neighbor ID, or null.
    Object* neighborSim(int16_t id);

    // Inventories by kind and ID (0 for the global one): the neighborhood's
    // (loadNeighborhood), and others created on demand.
    std::map<std::pair<InventoryKind, int16_t>, Inventory> inventories;
    Inventory& inventory(InventoryKind kind, int16_t id) {
        return inventories[{kind, kind == InventoryKind::Global ? int16_t(0) : id}];
    }

    std::mt19937 random{0x5132};

    // Engine requests not yet taken by the host (oldest first), and the
    // switches Generic Sims Call flips: build/buy buttons and game state
    // transitions (saving, quitting, lot transitions).
    std::deque<EngineRequest> engineRequests;
    void request(EngineRequest r) {
        if (engineRequests.size() >= kMaxEngineRequests)
            engineRequests.pop_front();
        engineRequests.push_back(r);
    }
    bool buildBuyEnabled = true;
    bool gameStateTransitionsEnabled = true;

    void setPrimitive(uint16_t opcode, Primitive p) { primitives_[opcode] = std::move(p); }

    // A thread for `me` that starts in tree `treeId` (a call opcode: global,
    // private or semi-global) with this stack object. The tree and its calls
    // resolve against the code of `codeOwner` (0: me); interactions run an
    // object's tree in the Sim's thread.
    Thread start(ObjectId me, uint16_t treeId, std::span<const int16_t> args = {}, ObjectId stackObject = 0,
                 ObjectId codeOwner = 0);
    // Runs until the thread yields, finishes or fails. Exceeding the budget
    // fails the thread (an infinite loop, as far as the game is concerned).
    void run(Thread& thread, size_t maxInstructions = 100000);

    // Called before each instruction executes.
    std::function<void(const Thread&, const Frame&, const Behavior::Instruction&)> trace;

    // Opcodes and data owners that were needed but are not implemented.
    std::map<uint16_t, size_t> missingPrimitives;
    std::map<uint8_t, size_t> missingOwners;

    // Pushes tree `treeId` of `codeOwner`'s code as a subroutine of the thread.
    void call(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args) {
        push(t, codeOwner, treeId, args);
    }

    // Resolves a call opcode for code owned by `owner`: {group, BHAV} or {0, null}.
    std::pair<uint32_t, const Behavior*> resolveTree(const Object& owner, uint16_t treeId);
    const Constants& resolveConstants(const Object& owner, uint16_t table);
    // A BCON by where it is (0 private 0x1000+, 1 semi-global 0x2000+, 2 global
    // 0x100+) and its number there.
    const Constants& resolveConstants(const Object& owner, int scope, uint16_t table);

private:
    CodeLibrary& code_;
    std::map<ObjectId, Object> objects_;
    std::unordered_map<uint16_t, Primitive> primitives_;

    int initDepth_ = 0;
    void push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args);
    // Follows a branch; returns false when the thread finished or failed.
    bool branch(Thread& t, Behavior::Target target);
    void fail(Thread& t, const std::string& message);
};

// Registers the primitives implemented so far.
void registerPrimitives(VM& vm);
// The primitives that place and move objects in the world (routing.cpp).
void registerRoutingPrimitives(VM& vm);
// The primitives that need the neighborhood (hood_bridge.cpp).
void registerNeighborhoodPrimitives(VM& vm);

} // namespace os2::vm
