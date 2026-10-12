#pragma once

#include "formats/bhav.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"
#include "formats/xform.hpp"
#include "animation.hpp"
#include "locomotion.hpp"
#include "dialog.hpp"
#include "path_planner.hpp"
#include "tutorial.hpp"
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

struct WantSystem; // wants.hpp

// Group of the global BHAVs/BCONs (opcodes 0x100-0xFFF, in objects.package).
constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
constexpr uint32_t kTypeBhav = 0x42484156;
constexpr uint32_t kTypeBcon = 0x42434F4E;
constexpr uint32_t kTypeGlob = 0x474C4F42;
constexpr uint32_t kTypeTtab = 0x54544142;
constexpr uint32_t kTypeObjf = 0x4F424A66;
constexpr uint32_t kTypeTtas = 0x54544173;

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
constexpr int16_t HideInteraction = 50;
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
constexpr int16_t TimeOfDay = 4;     // 0 day, 1 dusk, 2 night, 3 dawn (STR# 0xE1), from the hour
constexpr int16_t DaysRunning = 28;
constexpr int16_t MaxDayNumber = 29;
constexpr int16_t DayOfWeek = 34;    // Days Running % 7
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
constexpr int16_t InteractionGuid = 36; // "Current Interaction GUID": the next interaction ID (links.cpp)
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

// A sound the trees asked for (Play / Stop Sound Event, Stop ALL Sounds;
// vm/sound.hpp), for the host to play from `source`'s position.
struct SoundRequest {
    enum class Kind : uint8_t { Play, Stop, StopAll } kind = Kind::Play;
    int16_t source = 0;  // object the sound comes from
    std::string event;   // name from the Sound ID Table (STR# 0x4132)
    bool loop = false;
    float volume = 1;
    uint8_t flags = 0;   // operand byte 4, as is
};
constexpr size_t kMaxSoundRequests = 256;

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
    std::string firstName, lastName; // for the dialogs' $ tokens
    // The SDNA property set (genetics.hpp): genes a baby inherits.
    std::map<std::string, std::string> dna;
    // A Sim born in play (Make New Character): its parents' neighbor IDs,
    // for the family tree the save writes (hood_save.hpp).
    int16_t parentA = 0, parentB = 0;
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
    // The OBJDs of type 7 with field 42 "is global sim object" & mask set, in
    // the scan's order, with their field 26 "Ignore GlobalSim field in CAS
    // Lot" (cEdithObjectModule::UpdateSimObjects, docs/game-flow-mac.md §7.1).
    struct GlobalSimObject {
        uint32_t group = 0;
        uint16_t instance = 0;
        uint32_t guid = 0;
        bool notInCas = false;
    };
    std::vector<GlobalSimObject> globalSimObjects(uint16_t mask = 1);
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
    // TTAB (group, instance), null if missing or unreadable, and its strings
    // (the TTAs with the same group and instance).
    const InteractionTable* interactions(uint32_t group, uint16_t instance);
    const StringTable* interactionStrings(uint32_t group, uint16_t instance);
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
    void add(uint32_t group, uint16_t instance, InteractionTable t);
    void addInteractionStrings(uint32_t group, uint16_t instance, StringTable t);
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
    Cache<InteractionTable> interactions_;
    Cache<StringTable> interactionStrings_;
    std::vector<std::pair<GlobalSimObject, uint16_t>> globalSims_; // with field 42
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

// An interaction in a Sim's queue: pushed by Push Interaction or picked
// from the pie menu (interactions.hpp has the queue logic). The entry is
// resolved from the TTAB of the object whose interaction it is when it is
// queued.
struct QueuedInteraction {
    ObjectId object = 0;      // whose interaction (TTAB) it is (the callee; also the stack object and code owner)
    uint16_t interaction = 0; // TTA index of the TTAB entry: the number Push Interaction and the pie menu use
    int16_t priority = 0;     // kPriority* (interactions.hpp)
    uint8_t flags = 0;        // Push Interaction's flags byte (0 from the pie menu)
    ObjectId pushedBy = 0;    // the object whose thread pushed it (the Sim itself from the pie menu)
    // The interaction's target (cTSInteraction +0x10, GetTargetObject): the
    // object, unless Push Interaction's flag 0x01 named the object in a local
    // (also the icon); the other Sim of a Sim-to-Sim animation (0 = the object).
    ObjectId target = 0;
    std::array<uint8_t, 16> operands{}; // Push Interaction's whole operand, for what is not understood yet
    // From the TTAB entry.
    uint16_t action = 0, guard = 0; // trees, resolved against the object's code
    uint32_t ttabFlags = 0;         // flags | flags2 << 16
    std::string name;               // TTAs string (Brazilian Portuguese), "Sub.../Item" for submenus
    std::array<int16_t, 4> args{};  // the action tree's parameters
    uint16_t uid = 0;               // unique across Sims (links.cpp; global 36), for cancelling a specific entry; saved at +0x40
    uint16_t link = 0;              // the interaction it is linked to (Push Interaction flag 0x40), or 0
    // Cancelled by the user or by something more important queued behind it:
    // Idle for Input in its action tree returns at once (FreeSO's NotifyIdle).
    bool cancelled = false;
};
constexpr size_t kMaxQueuedInteractions = 64;

// A pie menu entry a check tree adds (Add/Change the Action String), with
// the stack object at that moment: the action tree gets it as parameter 0.
// As the Mac binary makes it (cEdithObjectTestSim::sMakeNewMenuItem), the
// parameters are {stack object, temp 0} and, for autonomy, the check tree
// ads at that moment (Bon Voyage can also pass temps 4 and 5).
struct ActionString {
    std::string name;
    ObjectId stackObject = 0;
    std::array<int16_t, 4> params{};
    std::array<int16_t, kMotiveCount> adMin{}, adRange{}, adPersonality{};
};

// A motive change Set Motive Change (0x1D) put on a Sim: the motive moves
// by `perTick` each tick until it reaches `limit` (motives.hpp).
struct MotiveChange {
    int16_t motive = 0;
    float perTick = 0;
    float limit = 0;
};

// State only Sims have.
struct Person {
    std::array<int16_t, kPersonDataCount> data{};
    std::array<int16_t, kMotiveCount> motives{};
    // The motive engine (motives.cpp): the changes running, and the motives
    // with their fractions (the game keeps them as floats) as last written.
    std::vector<MotiveChange> motiveChanges;
    std::array<float, kMotiveCount> exactMotives{};
    std::array<int16_t, kMotiveCount> writtenMotives{};
    bool exactMotivesValid = false;
    // The last Find Best Interaction's choice, for Gosub Found Action
    // (autonomy.cpp); its object and entry are also in person data 53 and
    // object data 20.
    std::array<int16_t, 4> foundActionArgs{};
    std::string foundActionName; // a dynamic interaction's own name, or empty
    // What autonomy did lately (autonomy.cpp): the interactions Gosub Found
    // Action ran, newest last, and whether their action tree returned false.
    struct AutonomyMemory {
        ObjectId object = 0;
        uint16_t interaction = 0;
        uint64_t tick = 0;
        bool failed = false;
    };
    std::vector<AutonomyMemory> autonomyMemory;
};

// Where an object is in the lot, as the lot's saved state has it: the tile
// position (tile centres at .5), the level (1 = the ground) and the
// direction in object data 1. Objects out of the world (controllers, ones
// in a slot or an inventory) are at (-1, -1).
struct Position {
    float x = -1, y = -1;
    int level = 0;
    // Set by Build mode for what it puts off the floor (the modular stairs'
    // steps, docs/buy-build-mac.md §7.12): the height of the object in the
    // lot (tiles, as the elevations) and the saved state's "elevation";
    // kNoHeight leaves both to the floor (and the state as it was read).
    static constexpr float kNoHeight = -1000.0f;
    float height = kNoHeight;
    float elevation = 0;
    bool inWorld() const { return x >= 0 && y >= 0; }
    bool hasHeight() const { return height > kNoHeight; }
};

// A timer On Timer (0x7A) set on an object (cTSObject::AddTimer; the saved
// state keeps them, cTSObject::WriteTimers): after `ticks` ticks the event
// tree runs on the object, with these parameters. Looping timers start
// over with `period`.
struct ObjectTimer {
    int16_t id = 0;    // On Timer's timer ID (the saved state's sequence number)
    uint16_t tree = 0; // call opcode, resolved against the object's code
    int32_t ticks = 0; // left
    int32_t period = 0;
    bool looping = false;
    std::array<int16_t, 8> params{};
};
constexpr size_t kMaxTimersPerObject = 64;

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
    // Kept next to `data`: the scans over every object read them (cache lines).
    Position position;
    // On Timer's timers, in the order they were set.
    std::vector<ObjectTimer> timers;
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
    // The design choice the Design tool gave it (subset -> material, the
    // cObject's list, formats/design_catalog.hpp); none: the saved one.
    std::optional<std::map<std::string, std::string>> design;
    // Data owners 0x27 (mine) and 0x28 (the stack object's).
    TempToken tempToken;
    // Interactions queued for this object (a Sim, usually), in the order they
    // will run; `activeInteraction` is the index of the one running (-1: none).
    std::vector<QueuedInteraction> queue;
    int activeInteraction = -1;
    // Nested interactions (TTAB flag 0x8000) wait in the sub-queue until an
    // Idle for Input in "Handle Sub Queue Interactions" mode runs them as the
    // current sub-action (queue.cpp).
    std::vector<QueuedInteraction> subQueue;
    std::optional<QueuedInteraction> subAction;
    // Notify the Stack Object out of Idle: its next Idle for Input returns at once.
    bool idleInterrupt = false;
    // The route the object is walking (Go To Routing Slot, Go To Relative
    // Position), while it walks: the points still ahead (the next first; tile
    // centres, then the goal point) on its level, the facing at the end (-1:
    // as it arrives) and the object it walks to. The instruction that walks
    // it keeps yielding until the last point is reached; it is kept here for
    // the renderer to animate the walk.
    //
    // On stairs the route holds the climb's phases: points where the walker
    // is put (the animation's anchor, `z` high) to play a stair animation
    // for `ticks` ticks, as the game snaps a Sim into each step in turn
    // (Stair, world.hpp); `climb` is the phase playing.
    struct Route {
        struct Point {
            float x = 0, y = 0;
            int level = 0;
            // A climb phase (ticks > 0).
            int ticks = 0;
            float z = 0;
            int direction = -1;
            std::string animation; // empty with ticks > 0: a wait (for a door to open)
            ObjectId door = 0;     // the door whose portal this point goes through
            // The portal tree (OBJf 15 of `portalOwner`) runs on the walker's
            // thread here, with `portal` as its stack object (routing.cpp);
            // `viaPortal` points are the engine's own way through, skipped
            // when the tree did it.
            ObjectId portal = 0, portalOwner = 0;
            std::array<int16_t, 10> portalParams{};
            bool viaPortal = false;
            // A turn in place (ticks > 0): 45-degree steps, counter-clockwise
            // positive; the animation played mirrored (routing.cpp turnPoint).
            int turnSteps = 0;
            bool mirror = false;
        };
        std::vector<Point> points;
        int level = 0; // of the goal
        int facing = -1;
        ObjectId target = 0;
        // For planning again when the way is blocked (routing.cpp): the goal
        // points (x, y, cost) with their facings, the objects whose
        // footprints do not count, how long the walker has been blocked, how
        // often it planned again, and whether it stands waiting (a door, a
        // Sim in the way: no walk animation).
        std::vector<std::array<float, 3>> goals;
        std::vector<int> facings;
        std::vector<int16_t> multi; // the goals' multi slot indices (person data 38), when any
        std::vector<ObjectId> ignore;
        int blockedTicks = 0, replans = 0;
        // Where the Sim in the way stood at the last try (routing.cpp).
        float blockerX = 0, blockerY = 0;
        bool waiting = false;
        bool failureTrees = true; // "CT - Route Failure Feedback" when it fails
        ObjectId blocker = 0;     // the Sim in the way
        // DoBlockedGoalStillAvailable (routing.cpp): the walker steps aside
        // itself on a way of its own (TryGetSelfOutOfWay, once a route), then
        // plans its route again; a pause ("CT - Route Blocked Animation")
        // or "CT - Route Blocked" tree runs; the pauses so far.
        bool aside = false, asideTried = false, pausing = false, blockedTree = false;
        int pauses = 0;
        bool inPortal = false;    // a portal tree runs (routing.cpp)
        // The goal check / along route trees (routing.cpp): which one runs
        // (1 goal check, 2 along route), the tick of the last check and
        // how many path points were walked by then.
        int check = 0;
        uint64_t lastCheck = 0;
        struct Climb {
            std::string animation; // empty between phases
            uint32_t serial = 0;   // bumped as each phase starts
            float z = 0;
            int ticksLeft = 0;
            int turnSteps = 0; // a turn in place (not a stair: on the floor)
            bool mirror = false;
        } climb;
        bool endTurned = false; // the turn to the final facing was added
        // The locomotion along the floor (routing.cpp locoStep, after
        // cLocomotable::FollowOneStep, docs/routing-mac.md §6.1): a start
        // ("turnAndGo", or a step on a short way), the walk cycle, a stop
        // ("turnAndStop") at the end. A frame moves along the route (the
        // start and the stop over their clip's distance in its length, the
        // walk at the gait's speed) and the walker stands where the clip's
        // root is, offset from it.
        struct Loco {
            enum Kind : uint8_t { None, Start, Walk, Stop };
            Kind kind = None;
            bool moving = false;     // walking on (no start when it goes on)
            std::string animation;   // the clip playing
            uint32_t serial = 0;     // bumped as each clip starts
            bool mirror = false;
            // walkShort's weight in the walk, the start and the stop (the
            // stride blend, blendGaits/strideWeight).
            float strideWeight = 0;
            // How early the next phase starts, overlapping the one ending
            // (FollowOneStep: 5 ms when the way's beginning, measured up to
            // 0.75, is at most 0.25 long, else 100 ms).
            float overlap = 0.1f;
            bool sidestep = false;   // stepping without turning (CreateStepCycle)
            int tick = 0, ticks = 0; // in a start or a stop
            float distance = 0;      // its way along the route
            float speed = 0;         // the walk's, tiles per tick
            float fx = 0, fy = 0;    // the frame
            float heading = 0;       // which way the frame faces (radians, 0 = +Y)
            float turn = 0;          // how far the clip turns the body (radians, + counter-clockwise)
            float endTurn = 0;       // where a stop or a step leaves the body, off the frame's way
            float cycle = 0;         // walk cycle portion (0-1)
            float cycleStart = 0;    // the portion the walk clip started at
            float endX = 0, endY = 0; // a stop's last point
            // What plays, blended by weight (the first is `animation`): a
            // start's or a stop's clip and its short one blended by stride,
            // a sidestep's clips, or the weighed walk cycles
            // (docs/routing-mac.md §6.1).
            std::vector<LocoLayer> layers;
            // The frame on the route's curve: `segment` of it, `arc` along
            // that segment, `taken` of the route points it was built
            // through passed.
            std::shared_ptr<const LocoCurve> curve;
            size_t segment = 0, taken = 0;
            float arc = 0;
            bool onCurve = false;
            // Stopping where it is because the way is blocked (the walk
            // interrupted: cActiveAnimLoco::StopLocomotion), by `halted`.
            bool halting = false, haltDone = false;
            ObjectId halted = 0;
        } loco;
        // The instruction walking it: its thread's stack depth, tree and node.
        size_t depth = 0;
        const Behavior* tree = nullptr;
        uint16_t node = 0;
    };
    std::optional<Route> route;
    // The walker's "Routing - Eye Attractor" (routing.cpp): the object the
    // along route tree's parameter 5 names, kept on the way ahead.
    ObjectId eyeAttractor = 0;
    // A routing slot 0 the engine gave the object (the synthetic slot of a
    // move-out-of-way destination, routing.cpp), instead of its SLOT's.
    std::optional<Slots::Slot> syntheticSlot;
    // A destination the engine made for a Sim (go_here.hpp): flushed once no
    // Sim has it queued, unless held (a pie menu is open on it).
    bool managedDestination = false, destinationHeld = false;
    // What the animation primitives play on it (animation.hpp).
    AnimationState animation;
    // Where the head looks (Look At, view.cpp): the target object, from which
    // VM tick and for how many (0: until changed).
    struct LookAt {
        ObjectId target = 0;
        uint64_t since = 0;
        int32_t ticks = 0;
    };
    std::optional<LookAt> lookAt;
    // Reach/Put in progress (reach.cpp): the instruction running it (stack
    // depth, tree, node), the reach animation and its ticks, the tick of its
    // event 0 (when the object changes hands), what moves where, and the
    // grasp/carry overlays it adds on a pick up.
    struct Reach {
        size_t depth = 0;
        const Behavior* tree = nullptr;
        uint16_t node = 0;
        uint8_t mode = 0; // 0 pick up, 1 drop onto an object, 2 drop onto the floor
        std::string animation;
        int tick = 0, transferAt = 0, end = 0;
        bool transferred = false;
        ObjectId object = 0, target = 0;
        int hand = 0, slot = -1;
        TilePos floor{};
        std::vector<std::string> overlays;
    };
    std::optional<Reach> reach;
    // The grasp/carry overlays of what a Sim holds, taken off when it puts it down.
    std::vector<std::string> carryOverlays;
    // The model's bones as last posed (bone name, local transform): the
    // transform nodes the lot saved for it (nTSSG::cObject), then where each
    // Animate Object animation left them (VM::holdObjectPose). Its bone
    // slots are found in this pose, under what plays on it now.
    std::vector<std::pair<std::string, Xform>> pose;
    // Lights as Change Light set them (dialog.hpp).
    ObjectLights lights;

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
    // Root of a queued interaction's action tree (pushed by Idle for Input):
    // when it returns, the interaction ends and the Idle for Input that ran
    // it runs again instead of branching (interactions.hpp).
    bool interaction = false;
    // An interaction run by Gosub Found Action (autonomy.cpp): it ends the
    // same way, but the Gosub Found Action instruction branches on its result.
    bool foundAction = false;
    // A sub-queue interaction's action tree: when it returns, the sub-action
    // ends and the Idle for Input that ran it branches on its result.
    bool subInteraction = false;
    // When this frame returns, the caller's instruction branches on this
    // instead of the returned value (a primitive that ran a tree for its
    // own ends: the route failure feedback; routing.cpp).
    std::optional<bool> callerResult;
    // When this frame returns, the caller's instruction runs again (with
    // the returned value in Thread::rerunResult): a primitive that runs a
    // tree in the middle of its work (the routing's waiting animations).
    bool rerunCaller = false;
    // A route of this frame's instruction failed with this result and its
    // failure feedback tree runs (routing.cpp); -1: none.
    int16_t pendingRouteResult = -1;
    // The walker's pre-route tree ran for this frame's routing instruction,
    // which keeps the routing slot it read before (the tree may change the
    // temps it came from).
    bool preRouteRan = false;
    int16_t routeSlot = -1;
    // The walker's post-route tree runs for this frame's routing instruction
    // (1 after an arrival, 2 after a failure; routing.cpp).
    uint8_t postRoute = 0;
    // A route callback tree (pre/post-route, goal check, along route,
    // portal) the routing pushed: routes inside it are not base routes.
    bool routeCallback = false;
};

// Temps belong to the thread, so subroutines can return values in them;
// parameters, locals and the stack object belong to each frame.
struct Thread {
    // Killed: the object running the thread was removed.
    enum class Status { Running, Finished, Failed, Killed };

    ObjectId me = 0;
    std::array<int16_t, kTempCount> temps{};
    std::vector<Frame> stack;
    // What the tree a primitive ran returned, for the primitive running
    // again (Frame::rerunCaller); it reads and clears it.
    std::optional<bool> rerunResult;

    Status status = Status::Running;
    bool result = false;   // return value of the root tree once Finished
    std::string error;     // once Failed: the message and the stack at the failure
    size_t instructions = 0; // total executed
    // Set when a queued interaction's action tree returned to the Idle for
    // Input that ran it (1, or 2 when it had been cancelled), which runs
    // again at once (queue.cpp).
    uint8_t interactionReturned = 0;
    // Set while a check tree runs for the pie menu: Add/Change the Action
    // String adds entries here instead of renaming the running interaction.
    std::vector<ActionString>* actionStrings = nullptr;
    // The motive ads of the TTAB entry whose check tree runs for Find Best
    // Interaction, by motive: data owners 0x1D "check tree ad min", 0x0C
    // "check tree ad range" and 0x1C "check tree ad personality var" (autonomy.cpp).
    std::array<int16_t, kMotiveCount> adMin{}, adRange{}, adPersonality{};
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
    VM(const VM&) = delete; // byId_ points into objects_

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
    // The path planner of `world` (path_planner.hpp), built on first use
    // (routing.cpp); reset it after changing the world.
    std::shared_ptr<PathPlanner> planner;
    // Walking speed in tiles per tick of walkers without a walk animation
    // (Sims walk at their walk animation's speed, animation.hpp): 2 m/s, the
    // normal adult walk, with a tile taken as a metre and a tick as 1/30 s
    // (30 ticks a game minute, a game minute a second at normal speed; inferred).
    float walkTilesPerTick = 2.0f / 30.0f;
    // Lengths of the animations the animation primitives play (animation.cpp).
    AnimationLibrary animations;
    // The gaits and locomotion clips already looked up (locomotion.cpp), by
    // what they depend on.
    struct LocoCache {
        std::unordered_map<std::string, Gait> gaits;
        std::unordered_map<std::string, std::optional<LocoPick>> clips;
    } locoCache;
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
        std::vector<int16_t> multi; // each goal's "multi slot index" (its bone in a bone slot), when any
        // With `evaluate`: the route result of the last candidate turned down
        // (EvalTileForGoal, routing.cpp), for when none is left; 0 if none was.
        int16_t refused = 0;
    };
    RouteGoals routingGoals(const Slots::Slot& slot, const Object& target, bool evaluate = false, ObjectId router = 0);
    // The model bones a routing slot names, in the object's model space, each
    // with its facing relative to the object's direction (slot_bones.cpp);
    // none without the game's resources. slotBone: the first one.
    struct SlotBone {
        float x = 0, y = 0, z = 0;
        int facing = 0;
        int16_t index = 0; // in the slot's list: the goal's "multi slot index" (person data 38)
    };
    std::vector<SlotBone> slotBones(const Slots::Slot& slot, const Object& target);
    std::optional<std::array<float, 3>> slotBone(const Slots::Slot& slot, const Object& target);
    // The object's model as posed now (local transforms of its skeleton's
    // bones; empty without one): the rest pose with Object::pose, under the
    // Animate Object animations playing on it at their time (held at their
    // last frame). holdObjectPose keeps that pose in Object::pose, for an
    // animation about to be stopped or replaced (the game's objects stay
    // where the last frame left them).
    std::vector<Xform> objectPose(const Object& target);
    void holdObjectPose(Object& target);

    // Threads of a removed object are killed the next time they run.
    void removeObject(ObjectId id) {
        objects_.erase(id);
        if (size_t(uint16_t(id)) < byId_.size())
            byId_[uint16_t(id)] = nullptr;
        ++tableGeneration_;
    }
    // Remove Object Instance queues objects to die at the end of the tick (kill.hpp).
    std::vector<ObjectId> killQueue;
    std::set<ObjectId> dyingObjects;
    const std::map<ObjectId, Object>& objects() const { return objects_; }
    // The same objects in the same (ID) order, as a table indexed by ID with
    // null holes: scanning it is cheaper than walking the map. Objects must
    // not be created or removed while a loop over it runs with a reference.
    const std::vector<Object*>& objectTable() const { return byId_; }
    // The IDs of the people (objects with person data), in ID order, as of
    // this tick and the objects there are (the walkers' collision tests
    // look at them every step).
    const std::vector<ObjectId>& people() const {
        if (peopleTick_ != ticks || peopleGeneration_ != tableGeneration_) {
            people_.clear();
            for (const Object* o : byId_)
                if (o && o->person)
                    people_.push_back(o->id);
            peopleTick_ = ticks, peopleGeneration_ = tableGeneration_;
        }
        return people_;
    }
    // `for (const auto& [id, o] : vm.eachObject())`: the same walk as over
    // objects() (ID order, an object made during the loop is met when its ID
    // is later, one removed is skipped) through the table, which is much
    // cheaper than the map's nodes; safe against creation and removal.
    struct EachObject {
        const VM* vm;
        struct It {
            const VM* vm;
            size_t i;
            void skip() {
                while (i < vm->byId_.size() && !vm->byId_[i])
                    ++i;
            }
            std::pair<ObjectId, const Object&> operator*() const { return {vm->byId_[i]->id, *vm->byId_[i]}; }
            It& operator++() {
                ++i;
                skip();
                return *this;
            }
            bool operator!=(const It& o) const { return i != o.i; }
        };
        It begin() const {
            It it{vm, 0};
            it.skip();
            return it;
        }
        It end() const { return {vm, vm->byId_.size()}; }
    };
    EachObject eachObject() const { return {this}; }

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
    // `args` are its parameters (the engine's RunTree passes some: the
    // stairs' "Set Model by Type", entry 47, takes the railing model).
    std::optional<Thread> runFunction(ObjectId id, size_t function, size_t maxTurns = 1000,
                                      std::span<const int16_t> args = {});
    // Starts the object's Main in its thread; false if it has none.
    bool startMain(ObjectId id);
    void resetAfterError(ObjectId id, Thread& t);
    std::map<ObjectId, Thread> threads; // the Main threads
    // Instructions a thread may run in one turn before it fails as an
    // infinite loop.
    size_t instructionsPerTurn = 100000;
    // Create New Object Instance initializes the new object (runs its Init
    // and, with the scheduler running, starts its Main), as FreeSO does; off
    // for single trees (bhav-sweep), where nothing else would run.
    bool initCreatedObjects = false;
    // Doors and stairs run their portal trees (OBJf entry 15: TEST while
    // planning, the action on arrival; routing.cpp); off: the engine opens
    // the way itself (waits at doors, climbs the stairs with its own phases).
    bool portalTrees = true;
    // An object whose Main fails is reset, as the binary does outside the
    // debugger (cTSObject::Error flags it, HandleError resets it: the tree
    // sim starts its Main again; the game writes an ObjectError log): its
    // queue goes and its Main starts over. On (the game) unless a test or
    // tool wants failed Mains kept. What each reset was, for the host:
    bool resetOnError = true;
    struct ErrorReset {
        size_t count = 0;
        std::string first, last; // the failures' messages with their stacks
        uint64_t lastTick = 0;
    };
    std::map<ObjectId, ErrorReset> errorResets;
    // The global sim objects the lot load made (vm::updateSimObjects): a
    // reload makes them again, so lot-save --check does not compare them.
    std::set<ObjectId> globalsMadeAtLoad;
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
        // Real seconds to ticks at normal speed, for animation and routing
        // durations. The game runs a tick per 42 ms of its scaled clock (about
        // 23.8 a second, docs/ui-live-mac.md §1.2); 30 here by default (tests,
        // s2pkg), the app sets the game's rate.
        double ticksPerSecond = 30;
        int tick = 0; // within the minute
        int minute = 0, hour = 8, day = 1, month = 1, year = 1;
    } clock;
    void setClockGlobals(); // also Time Of Day (global 4) from the hour
    uint64_t ticks = 0;
    // One tick: the clock, the objects' timers, then a turn of every Main
    // thread, then contained objects move to their (outermost) container's position.
    void tick();
    // Counts down every object's timers and runs the event trees of the
    // ones that are due (timer.cpp).
    void handleTimers();
    // Event trees of timers that failed (the latest; for the host to look at).
    std::deque<Thread> failedTimerEvents;
    void followContainers();

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
    // The neighbor of a Sim made from its definition: the one with this
    // OBJD GUID, or a new one (the next ID) when the neighborhood has none;
    // 0 without a neighborhood (inferred, Create New Object Instance).
    int16_t neighborForGuid(uint32_t guid);

    // Inventories by kind and ID (0 for the global one): the neighborhood's
    // (loadNeighborhood), and others created on demand.
    std::map<std::pair<InventoryKind, int16_t>, Inventory> inventories;
    Inventory& inventory(InventoryKind kind, int16_t id) {
        return inventories[{kind, kind == InventoryKind::Global ? int16_t(0) : id}];
    }

    std::mt19937 random{0x5132};

    // Wants, fears and aspiration (wants.hpp); null without want data.
    std::shared_ptr<WantSystem> wants;

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

    // Sounds not yet taken by the host (oldest first), and the loops each
    // object has playing (a loop is not started twice).
    std::deque<SoundRequest> soundRequests;
    std::map<int16_t, std::set<std::string>> loopingSounds;
    std::map<int, size_t> missingSounds; // sound IDs with no name, and how often
    void requestSound(SoundRequest r) {
        if (soundRequests.size() >= kMaxSoundRequests)
            soundRequests.pop_front();
        soundRequests.push_back(std::move(r));
    }

    // Dialogs (dialog.hpp): the blocking ones waiting for an answer, by the
    // object whose thread waits; the text notifications shown (latest last);
    // the answer given at once to every blocking dialog (-1: wait for the
    // host; headless hosts set a button).
    std::map<ObjectId, Dialog> dialogs;
    std::deque<Dialog> notifications;
    // The text of the last text-entry dialog (type 3) answered with OK:
    // TryDialog hands GetEnteredText to the simulator (cTSObject + 0x48,
    // vtable 0x68). Nothing in the base game's trees puts a type 3 up, and
    // what reads it back was not found (The Sims 1 named the next Make New
    // Character with it; the BV TryMakeNewCharacter does not).
    std::string enteredText;
    // The tutorial's progress window and UI Effects (tutorial.hpp).
    TutorialState tutorial;
    int autoDialogAnswer = -1;
    // Dialogs time out as the game's (autoTimeOutDialogs in dialog.hpp); on
    // game time in VM::tick when `dialogClockIsTicks` (headless), else the
    // host calls it with real time.
    bool dialogTimeouts = true;
    bool dialogClockIsTicks = true;
    // The simulator's values (SIMI +0xA4 lot, +0xA8 lot objects, +0xAC
    // family objects), as cTSLot::ComputeAndStoreLotData leaves them
    // (placement.hpp computeLotData); `known` once read or computed.
    struct LotValues {
        int32_t lot = 0, lotObjects = 0, familyObjects = 0;
        bool known = false;
    } lotValues;
    // The Railing Tool's price (BuildCatalog::railingPrice, product
    // 0x2C7BF690), what each side of a staircase's railing is worth
    // (placement.hpp railingCost); 0 until a catalog sets it.
    int railingPrice = 0;
    // Ours, not the binary's: Find Best Interaction leaves out what the Sim
    // ran autonomously and failed in the last 9999 ticks (autonomy.cpp). Off
    // by default: the binary's autonomy memory changes nothing in the base game.
    bool autonomySkipsFailures = false;
    uint32_t nextDialogId = 1;

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
    // id -> object (map nodes never move); kept by createObject/removeObject,
    // the only places that add or remove entries of objects_.
    std::vector<Object*> byId_;
    uint64_t tableGeneration_ = 0;
    mutable std::vector<ObjectId> people_;
    mutable uint64_t peopleTick_ = ~uint64_t(0), peopleGeneration_ = ~uint64_t(0);
    std::unordered_map<uint16_t, Primitive> primitives_;

    int initDepth_ = 0;
    void push(Thread& t, ObjectId codeOwner, uint16_t treeId, std::array<int16_t, 4> args);
    // Follows a branch; returns false when the thread finished or failed.
    bool branch(Thread& t, Behavior::Target target);
    void fail(Thread& t, const std::string& message);
};

// Global 4 "Time Of Day" for an hour (0 day, 1 dusk, 2 night, 3 dawn).
int16_t timeOfDay(int hour);
// Registers the primitives implemented so far.
void registerPrimitives(VM& vm);
// The primitives that place and move objects in the world (routing.cpp).
void registerRoutingPrimitives(VM& vm);
// The primitives that need the neighborhood (hood_bridge.cpp).
void registerNeighborhoodPrimitives(VM& vm);
// Get Distance To and Get Direction To (measure.cpp).
void registerMeasurePrimitives(VM& vm);
// Set to Next 24: the next object (by ID, after `current`) that `me` sees
// within `distance` tiles (view.cpp).
std::optional<ObjectId> nextVisibleObject(VM& vm, const Object& me, ObjectId current, int16_t distance);
// On Timer (timer.cpp).
void registerTimerPrimitives(VM& vm);
// Look At (view.cpp).
void registerViewPrimitives(VM& vm);
// Find Best Object for Function, Run Functional Tree and Reach/Put (reach.cpp).
void registerReachPrimitives(VM& vm);
// The motive engine's tick, Set Motive Change and Find 5 Worst Motives (motives.cpp).
void tickMotives(VM& vm);
void tickWants(VM& vm); // wants.cpp
void registerMotivePrimitives(VM& vm);
// Find Best Interaction and Gosub Found Action (autonomy.cpp).
void registerAutonomyPrimitives(VM& vm);
// Called when an interaction Gosub Found Action ran returns `result`.
void foundActionReturned(VM& vm, Thread& t, bool result);
// The rooms a Sim in `room` can walk to (cTSPerson::GetAccessibleRooms,
// routing.cpp; PathPlanner::accessibleRooms); null without a world or when
// the room has no free floor (nothing is filtered then).
const std::vector<int16_t>* accessibleRooms(VM& vm, int16_t room);

} // namespace os2::vm
