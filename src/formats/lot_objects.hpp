#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "scene_anim.hpp"

namespace os2 {

class DbpfPackage;

// SimAntics side of the objects saved in a lot package (the game's
// cEdithObjectModule), read and written as the Mac binary does
// (docs/save-mac.md: cEdithObjectModule::SplitLoad/read_pre_object/
// read_post_object/ReadObjectGroups, cTSObject::Read/ReadHeader,
// cEdithMTObject::Read, cTSPerson::Read, cTSInteraction::Read).
//
// - ObjM (0x4F626A4D) instance 1: the module's version, then one (object ID,
//   save type) pair per object of the lot: the game creates exactly these
//   objects, nothing else. After the pairs, in the same resource, the
//   "post object" part (a u32 per object ID, the list of selectable Sims,
//   the lot's name, the message subscribers).
// - ObjM instance 2: the object groups (the modular stairs: each group lists
//   the step objects of one staircase).
// - objt (0x6F626A74): save type -> object GUID, object type (2 = person),
//   catalog name and class flags (bit 0: a multi-tile object).
// - One state per object, the instance being the object ID, whose resource
//   type is the class's GZCLSID: XOBJ (0x584F424A, cTSObject), XMTO
//   (0x584D544F, cEdithMTObject: a part of a multi-tile object; the
//   cTSObject followed by its offsets and its lead object) or PERS
//   (0x50455253, cTSPerson: the cTSObject followed by the person). An object
//   is saved under its class: a person (object type 2) as PERS, a multi-tile
//   one (flags bit 0) as XMTO, the rest as XOBJ.
//
// The packages keep the states of objects that are gone (the persist DB does
// not delete records): those are orphans no pair names, and are not read.
constexpr uint32_t kVmThreadStateType = 0x4F533256; // "OS2V" (vm/thread_state.hpp)

struct LotObjects {
    // A record of the objt (ObjectSaveTypeTable::Read/WriteSelector,
    // docs/save-mac.md §5): what the game checks a definition against when
    // it loads the lot (a changed tree version or count resets the objects).
    struct SaveType {
        uint32_t guid = 0;
        uint32_t initTreeVersion = 0;  // tree version of the OBJf init action's BHAV
        uint32_t mainTreeVersion = 0;  // ... of the main action's (0 without one)
        uint32_t field3A = 0, field3B = 0; // OBJD fields 0x3A and 0x3B (file offsets 0xB4, 0xB6)
        uint32_t stringCount = 0;     // strings in the group's STR# 0x100
        int16_t objectType = 0;  // OBJD field 9, the object type: 2 = person (read_pre_object picks the person class on it)
        std::string catalogName; // "*Trash Can - Outside"; empty for objects not in the catalog
        uint32_t flags = 0;      // 0, 1 (no catalog name: saved as XMTO) or 3
        uint32_t guid2 = 0;      // OBJD field 0xD0
    };

    enum class Kind : uint8_t { Object, MultiTile, Person };
    static uint32_t resourceType(Kind k);

    // A Sim's interaction (cTSInteraction::Read; names from its getters,
    // the rest by offset in the class).
    struct SavedInteraction {
        uint32_t u08 = 0;
        uint32_t uid = 0;           // unique ID (a global counter)
        int16_t u0C = 0;
        int16_t stackObject = 0;    // the object whose interaction it is
        int16_t target = 0;         // the Sim it is for / on
        int16_t initiator = 0;      // who pushed it (> 0xA5)
        uint32_t ttabEntry = 0;     // index in the object's TTAB
        std::array<int16_t, 4> params{};
        uint32_t priority = 0;      // +0x20 (inferred: 20 on the autonomous ones, as Push Interaction gives)
        int16_t u24 = 0;            // flags? (0x1005, 0x2002)
        float u28 = 0;
        uint32_t u54 = 0;           // >= 0x3E
        int16_t u30 = 0;            // > 0x6E
        int16_t u2C = 0, u2E = 0;   // >= 0x72
        uint16_t u40 = 0, u42 = 0;  // >= 0x7C
        std::string name;           // > 0x96: the name shown in the queue ("Joke")
        uint32_t u38 = 0, u3C = 0;  // > 0x96
        int16_t u32 = 0;            // > 0xBA
        // Whether a record of this name and object was actually saved (the
        // fixed slots of a person are read even when empty).
        bool empty() const { return uid == 0 && stackObject == 0 && name.empty(); }
    };

    // Saved state of one object instance. Position is in tiles (the centre of
    // the tile: 9.5, 13.5); objects out of the world (controllers, contained or
    // carried ones, ...) are at (-1, -1). Levels count from 1 = the ground
    // (Lot's level 0); a ground floor on a foundation is level 2.
    struct State {
        int16_t id = 0;
        Kind kind = Kind::Object;
        bool person = false; // kind == Person
        // cTSObject::ReadHeader/WriteHeader (BV 0xb13dd4/0xb07790; docs/
        // save-mac.md §8): 4 f32 the game writes 0 and skips, the position,
        // the level, a u16 it writes 1 and skips, the elevation, the
        // multi-tile group and the step-over portal.
        std::array<uint8_t, 64> fileName{};
        float x = -1, y = -1;
        int32_t level = 0;
        float elevation = 0;
        int32_t multiTileGroup = -1; // -1 for single-tile objects
        // The object of its step-over portal (+0x298, UpdateStepOverPortal);
        // 0 for none.
        int16_t stepOverPortal = 0;
        float headerD6 = 0; // > 0xD6 (OpenTS2; not in the BV's reader)
        std::vector<int16_t> attributes, semiAttributes;
        std::array<int16_t, 8> temps{};
        std::vector<int16_t> data; // object data, numbered as in STR# 0x8D (as many as the version saves)

        // The temporary token's fields 0-7 as data owner 0x27 sees them
        // (cTSObject +0x1A0, InterpValue BV 0xb15f2c): inventory kind,
        // inventory ID, counted, index, GUID low and high words, flags,
        // property count. 4-7 are the game's raw copies of the token's, made
        // when a Manage Inventory reads one.
        std::array<int16_t, 8> tempTokenFields{};
        struct Token {
            uint32_t guid = 0;
            uint16_t flags = 0;
            std::array<uint8_t, 8> extra{}; // > 0xCA: stack ID, personal inventory instance, category
            std::vector<int16_t> properties;
        } token;
        std::vector<std::vector<int16_t>> arrays; // object arrays
        // Each array's iterator index (+0x194: data owner 0x29 reads it, 0x2B
        // the element at it; -1 in every array never walked).
        std::vector<int16_t> arrayIterators;
        uint32_t extra8E = 0;                     // 0x8E-0x8F only
        int32_t ownership = 0;
        std::vector<std::string> materialSubstitutes;
        uint16_t persistentFlag = 0;
        // The tree stack (cTSTreeStack), the root first: tree, node, locals,
        // parameters and the objects it runs as/on (>= 0xA5; before, the
        // owner of the stack), with the BHAV's save type.
        struct Frame {
            uint16_t objectId = 0;
            uint16_t treeId = 0;
            uint16_t node = 0;
            uint16_t runningObject = 0, runningOn = 0;
            std::vector<int16_t> params, locals;
            int32_t primitiveState = 0;
            uint16_t behaviorSaveType = 0;
        };
        std::vector<Frame> stack;
        // The object's relationship table (cTSRelationshipTable::Write/Read,
        // Mac BV 0xac6f04/0xac7738, docs/relationships-mac.md §1): i32
        // version (3), u32 count and the keys ("to": object IDs or, for
        // ownership, neighbor IDs), whose values are the lot's SRELs
        // (instance = id << 16 | key). Before version 0 (an old format) the
        // entries are inline: per entry i32 present, then u32 key, i32 count
        // and the values. Raw is written back as read.
        std::vector<uint8_t> relationshipTable; // raw
        int32_t relationshipVersion = 0;
        std::vector<uint32_t> relationshipKeys;
        std::map<uint32_t, std::vector<int32_t>> relationshipInline; // version < 0
        // Slots (cTSObject::ReadSlots): the objects in each container slot
        // (0 = empty); raw keeps the encoding.
        std::vector<int16_t> slots;
        std::vector<uint8_t> slotsRaw, effectsRaw, overridesRaw;
        // The effects running on it (cTSObject::ReadEffects): name, the
        // bone or slot node, 8 bytes (not decoded).
        struct Effect {
            std::string name, node;
            std::array<uint8_t, 8> data{};
        };
        std::vector<Effect> effects;
        // Timers set by On Timer (cTSObject::cObjectTimerDetail, written by
        // cTSObject::WriteTimers BV 0xb042b8 in this order; docs/save-mac.md
        // §8): the timer's ID (+0x10: from the simulator's GUID counter,
        // global 36), the event tree (+4), the object that set it (+0x14:
        // the event tree's stack object), flags (+0xC: bit 0 repeat), the
        // period (+8), the ticks left (+0) and the eight parameters (+0x16).
        struct Timer {
            uint32_t id = 0;
            uint32_t tree = 0;
            uint16_t object = 0;
            uint32_t flags = 0;
            int32_t period = 0, ticksLeft = 0;
            std::array<int16_t, 8> params{};
        };
        std::vector<Timer> timers;
        // cEdithMTObject::Read (XMTO, BV 0xaf2b8c): the part's offset from
        // the lead (x, y, level; SetOffset) and its normalized offset
        // (SetNormalizedOffset), then the lead object's ID (AddToList).
        std::array<int32_t, 3> offset{}, normalizedOffset{};
        int16_t leadObject = 0;

        // cTSPerson::Read (PERS): the animator (a 0xDEADBEEF tag and a
        // version, 2 in the game's lots: nothing else then), two sets of the 16
        // motives as floats (numbered as STR# 0x86: Mood 3 ... Fun 15; the
        // second set is clamped to 100 and lags the first slightly) and the
        // person data (170 fields in the base game, STR# 0xC8).
        bool personRead = false;
        uint32_t personTag = 0, personVersion = 0;
        std::array<float, 16> motives{}, motives2{};
        std::vector<int16_t> personData;
        uint32_t personU0C = 0;
        // The current action (+0x5D4, GetCurrentAction; tree table entry -1
        // when none), the last one (+0x5D8, GetLastAction) and the queue of
        // top actions (+0x584).
        SavedInteraction slotA, slotB;
        std::vector<SavedInteraction> queue;
        struct ObjectRecord {
            int16_t object = 0;
            uint32_t value = 0;
            uint8_t flag = 0;
        };
        std::vector<ObjectRecord> objectRecords; // cTSPerson::cObjectRecord
        struct MotiveInc {
            uint32_t motive = 0;
            float a = 0, b = 0;
        };
        std::vector<MotiveInc> motiveIncs; // cTSPerson::cMotiveInc
        uint16_t personU644 = 0, personU646 = 0;
        std::optional<SavedInteraction> pending; // the current sub-action (+0x5DC)
        std::vector<SavedInteraction> list5AC;   // the sub-action queue (+0x5AC)
        std::string personName; // +0x900 (>= 0x9D)
        std::array<uint32_t, 3> personU90C{};   // the last Find Best Action (GetLastFBAIconData)
        std::array<int16_t, 8> personU918{};
        uint32_t personU930 = 0;
        std::vector<SavedInteraction> list5E0;
        uint32_t personU938 = 0, personU93C = 0;

        // The animation requests (kind 1: a Sim's Animate Sim, the
        // overlays, Animate Object's) in the object's scene graph record's
        // animation queue (scene_anim.hpp; not part of this resource).
        std::vector<SceneAnimRequest> animations;
        // The VM's own state of the object's thread (vm/thread_state.hpp:
        // routing frames, the route walked), a record of the VM's
        // (kVmThreadStateType) beside the game's; empty when there is none.
        std::vector<uint8_t> vmThreadState;

        // Bytes after what was read (none when the state reads through).
        std::vector<uint8_t> rest;
        // When the part after the object data does not read through: all of
        // it, written back as it was (only the header, attributes, temps and
        // object data can change then).
        std::vector<uint8_t> unparsedTail;
        // Why the state could not be read through (the fields above stay as
        // far as they got).
        std::string tailError;
    };

    uint32_t version = 0; // of the object module; the states' layout depends on it
    // The request counter of the Sims' animation queues (the largest; the
    // game's is one global, DAT_02779c34, each queue saves it).
    uint32_t animationCounter = 0;
    // The VM's own lot state (vm/thread_state.hpp, instance 0 of
    // kVmThreadStateType); empty when the lot has none.
    std::vector<uint8_t> vmLotState;
    std::array<uint8_t, 64> moduleFileName{};
    uint32_t moduleZero = 0;
    // The ObjM pairs in their order: object ID (the u32 as saved), save type.
    std::vector<std::pair<uint32_t, int32_t>> pairs;
    // read_post_object: a u32 per object ID - 1 (flag 0x10000 on selectable
    // Sims), a count, the selectable Sims (object IDs), a u32, the lot's name
    // and the message subscribers (raw).
    std::vector<uint32_t> postTable;
    uint32_t postCount = 0;
    std::vector<uint32_t> postList;
    uint32_t postZero = 0;
    std::string postName;
    std::vector<uint8_t> postRest;
    // ObjM 2 (ReadObjectGroups): version and the groups (key, object IDs).
    struct Group {
        uint32_t key = 0;
        std::vector<int16_t> objects;
    };
    std::array<uint8_t, 64> groupsFileName{};
    uint32_t groupsVersion = 0;
    std::vector<Group> groups;
    std::vector<uint8_t> groupsRest;
    bool haveGroups = false;

    // The simulator's globals when the lot was saved (SIMI, 0x53494D49,
    // instance 1: tag, version, a u32, then the 54 globals of STR# 0x81:
    // hour, day, ..., selected person, current family, current house, the
    // family's funds as % and / 10000; they agree with the neighborhood).
    // Empty if the lot has none.
    std::vector<int16_t> globals;
    std::vector<uint8_t> simulatorRaw; // the whole SIMI as read
    // The rest of the SIMI (cTSSimulator::Read/Write, docs/save-mac.md §5),
    // named by the simulator's field offsets; the number of globals is
    // simulatorGlobalCount(version) (54 in the base game's lots 0xAB-0xAD,
    // 46 in 0x94/0x9F, 58 in the BV's 0xCE).
    struct Simulator {
        bool read = false;
        std::array<uint8_t, 64> fileName{};
        uint32_t version = 0, zero = 0;
        uint32_t fA0 = 0;
        uint16_t f11C = 0;
        uint32_t f98 = 0;  // > 1
        uint32_t fB4 = 0, service = 0; // > 0xD
        uint32_t f9C = 0;  // >= 0xF
        uint32_t fA4 = 0, fAC = 0, fA8 = 0, fB0 = 0; // >= 0x25 (fA8 >= 0xAD)
        // > 0x25: the current expense report and 5 more (u32 kind: 0 has 7
        // values, 1 has 8, others 10; the BV writes kind 2).
        struct ExpenseReport {
            uint32_t kind = 1;
            std::vector<uint32_t> values;
        };
        std::vector<ExpenseReport> reports;
        std::vector<uint8_t> rest; // bytes after what was read
    } simulator;
    std::map<int16_t, SaveType> saveTypes;
    std::vector<int16_t> saveTypeOrder; // as in the objt
    std::vector<uint8_t> saveTypesHeader; // objt bytes before the records (name, tag, version, u32)
    std::map<int16_t, int16_t> saveTypeOf; // object ID -> save type
    std::vector<State> objects;            // the ObjM's objects, by ID
    // The lot's SRELs (instance = from << 16 | to): the entries of the
    // objects' relationship tables (cTSRelationshipEntry, BV 0xac6be0: u32
    // version, only 2 read, u32 count and the values; bytes after them are
    // ignored). One counts only if the table of object `from` lists `to`.
    struct Relationship {
        int16_t from = 0, to = 0;
        uint32_t version = 0;
        std::vector<int32_t> values;
    };
    std::vector<Relationship> relationships;
    // The table bytes the game writes for these keys (version 3).
    static std::vector<uint8_t> relationshipTableBytes(const std::vector<uint32_t>& keys);
    // Other records a save writes over the source's (same key) or adds:
    // scene graph objects, routing states, relationships (vm/lot_records.hpp).
    struct Record {
        uint32_t type = 0, group = 0, instance = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<Record> otherRecords;
    std::vector<Record> deletedRecords; // keys the save leaves out
    // States no ObjM pair names (stale records), by resource type.
    std::map<uint32_t, size_t> orphans;
    std::vector<std::string> warnings;

    static LotObjects load(DbpfPackage& package);
    // Only the save type table (objt) of a package, by GUID: for objects a
    // lot's own table lacks (the game writes one record per definition).
    static std::map<uint32_t, SaveType> loadSaveTypes(DbpfPackage& package);
    // One XOBJ/XMTO/PERS resource, for an object module of `version`.
    static State parseState(std::span<const uint8_t> data, uint32_t version, Kind kind = Kind::Object);
    static State parseState(std::span<const uint8_t> data, uint32_t version, bool person) {
        return parseState(data, version, person ? Kind::Person : Kind::Object);
    }
    // The resource bytes of a state, as the game writes them for `version`
    // (lot_objects_write.cpp). A state read without errors writes back the
    // bytes it was read from.
    static std::vector<uint8_t> writeState(const State& s, uint32_t version);
    std::vector<uint8_t> writeModule() const;     // ObjM 1
    std::vector<uint8_t> writeGroups() const;     // ObjM 2
    std::vector<uint8_t> writeSimulator() const;  // SIMI with `globals`
    std::vector<uint8_t> writeSaveTypes() const;  // objt

    // Writes a lot package: every resource of `source` as it is, except the
    // object module, the save types, the simulator and the states, which come
    // from this (orphan states are dropped). `out` must not be `source`.
    void save(DbpfPackage& source, const std::filesystem::path& out) const;

    // GUID of the object with this ID, 0 if the object module does not say.
    uint32_t guidOf(int16_t id) const;
    const SaveType* saveTypeOfObject(int16_t id) const;
    const State* state(int16_t id) const;
};

// Number of object data fields a state of `version` holds (cTSObject::Read).
size_t lotObjectDataCount(uint32_t version);
// Number of globals a SIMI of `version` holds (cTSSimulator::Read).
size_t simulatorGlobalCount(uint32_t version);
// Number of person data fields a PERS of `version` holds (cTSPerson::Read).
size_t lotPersonDataCount(uint32_t version);

} // namespace os2
