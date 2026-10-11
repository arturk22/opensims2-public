#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
struct LotObjects {
    struct SaveType {
        uint32_t guid = 0;
        std::array<uint8_t, 20> unknown{}; // init/main tree versions, attribute and array counts, a string set count (OpenTS2)
        int16_t objectType = 0;  // OBJD object type: 2 = person (read_pre_object picks the person class on it)
        std::string catalogName; // "*Trash Can - Outside"; empty for objects not in the catalog
        uint32_t flags = 0;      // bit 0: multi-tile object (saved as XMTO)
        uint32_t guid2 = 0;      // the GUID again (the master's for multi-tile parts?)
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
        // cTSObject::ReadHeader.
        std::array<uint8_t, 64> fileName{};
        std::array<float, 4> headerFloats{};
        float x = -1, y = -1;
        int32_t level = 0;
        uint16_t headerOne = 1;
        float elevation = 0;
        int32_t multiTileGroup = -1; // -1 for single-tile objects
        int16_t header298 = 0;
        float headerD6 = 0; // > 0xD6 (OpenTS2)
        std::vector<int16_t> attributes, semiAttributes;
        std::array<int16_t, 8> temps{};
        std::vector<int16_t> data; // object data, numbered as in STR# 0x8D (as many as the version saves)

        std::array<int16_t, 8> tempTokenFields{};
        struct Token {
            uint32_t guid = 0;
            uint16_t flags = 0;
            std::array<uint8_t, 8> extra{}; // > 0xCA: stack ID, personal inventory instance, category
            std::vector<int16_t> properties;
        } token;
        std::vector<std::vector<int16_t>> arrays; // object arrays
        std::vector<int16_t> shortList;           // the u16 list at +0x194 (not understood)
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
        uint32_t stackUnknown = 0;
        std::vector<Frame> stack;
        std::vector<uint8_t> relationshipTable; // raw
        // Slots (cTSObject::ReadSlots): the objects in each container slot
        // (0 = empty); raw keeps the encoding.
        std::vector<int16_t> slots;
        std::vector<uint8_t> slotsRaw, effectsRaw, overridesRaw;
        // Timers set by On Timer (cTSObject::WriteTimers): seven raw fields
        // and the eight parameters the event tree gets.
        struct Timer {
            uint32_t a = 0, b = 0;
            uint16_t c = 0;
            uint32_t d = 0;
            int32_t e = 0, f = 0;
            std::array<int16_t, 8> params{};
        };
        std::vector<Timer> timers;
        // cEdithMTObject::Read (XMTO): two triples of u32 (offsets of the part
        // in the object? not decoded) and the lead object's ID.
        std::array<uint32_t, 6> multiTile{};
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
    std::vector<uint8_t> simulatorRaw; // the whole SIMI, for writing back
    std::map<int16_t, SaveType> saveTypes;
    std::vector<int16_t> saveTypeOrder; // as in the objt
    std::vector<uint8_t> saveTypesHeader; // objt bytes before the records (name, tag, version, u32)
    std::map<int16_t, int16_t> saveTypeOf; // object ID -> save type
    std::vector<State> objects;            // the ObjM's objects, by ID
    // The lot's SRELs (instance = from << 16 | to), named by object ID, with
    // their values: what the lot's objects and Sims think of each other, and
    // ownership (docs/data-findings.md §7: two values, "to" a neighbor ID).
    struct Relationship {
        int16_t from = 0, to = 0;
        std::vector<int32_t> values;
    };
    std::vector<Relationship> relationships;
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
// Number of person data fields a PERS of `version` holds (cTSPerson::Read).
size_t lotPersonDataCount(uint32_t version);

} // namespace os2
