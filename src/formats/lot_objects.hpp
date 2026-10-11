#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;

// SimAntics side of the objects saved in a lot package (the game's
// cEdithObjectModule): which object each object ID is and the state of each
// object instance. Layouts from OpenTS2 (`ObjectModuleCodec`,
// `ObjectSaveTypeTableCodec`, `SimsObjectCodec`), checked on the game's lots.
//
// - ObjM (0x4F626A4D, instance 1): version, then (object ID, save type) pairs.
//   It does not list every object: in N001_Lot11 627 of the 745 objects with a
//   state are in it (the rest, stairs, chairs, Sims, ..., are found by the
//   OBJD name of their cObject).
// - objt (0x6F626A74): save type -> object GUID and catalog name.
// - XOBJ (0x584F424A, cTSObject) and PERS (0x50455253, cTSPerson, the same
//   header followed by the Sim's own data): one per object, the instance is
//   the object ID (object data 11 holds it too).
struct LotObjects {
    struct SaveType {
        uint32_t guid = 0;
        std::string catalogName; // "*Trash Can - Outside"; empty for objects not in the catalog
        uint32_t flags = 0;      // bit 0: single tile, bit 1: multi-tile master (OpenTS2)
    };

    // Saved state of one object instance. Position is in tiles (the centre of
    // the tile: 9.5, 13.5); objects out of the world (controllers, contained or
    // carried ones, ...) are at (-1, -1). Levels count from 1 = the ground
    // (Lot's level 0); a ground floor on a foundation is level 2.
    struct State {
        int16_t id = 0;
        bool person = false; // from a PERS
        float x = -1, y = -1;
        int32_t level = 0;
        float elevation = 0;
        int32_t multiTileGroup = -1; // -1 for single-tile objects
        std::vector<int16_t> attributes, semiAttributes;
        std::array<int16_t, 8> temps{};
        std::vector<int16_t> data; // object data, numbered as in STR# 0x8D

        // The rest of a cTSObject, in OpenTS2's layout (SimsObjectCodec).
        std::array<int16_t, 8> tempTokenFields{};
        struct Token {
            uint32_t guid = 0;
            uint16_t flags = 0;
            std::vector<int16_t> properties;
        } token;
        std::vector<std::vector<int16_t>> arrays; // object arrays
        int32_t ownership = 0;
        std::vector<std::string> materialSubstitutes;
        // The tree stack (cTSTreeStack), the root first: tree, node, locals,
        // parameters and the objects it runs as/on, with the BHAV's save type.
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
        // Bytes after the timers: in a PERS, the Sim's own data (cTSPerson).
        std::vector<uint8_t> rest;
        // The start of a cTSPerson (PERS), read from `rest`: the tag
        // 0xDEADBEEF, a version (2 in the game's lots), two sets of the 16
        // motives as floats (numbered as STR# 0x86: Mood 3 ... Fun 15; the
        // second set is clamped to 100 and lags the first slightly: which
        // one the game simulates is not known) and the 170 person data
        // fields (STR# 0xC8: personality, skills, neighbor ID 31, age 58,
        // family 61, decay rates, interests, ... all check out against the
        // neighborhood's Sims). What follows is not read yet.
        bool personRead = false;
        uint32_t personVersion = 0;
        std::array<float, 16> motives{}, motives2{};
        std::vector<int16_t> personData;
        // The Sim's interaction queue (cTSInteraction records), the running
        // one first. Layout inferred from the lots (no code read): u32
        // unique ID, u16 the initiator (0 for self interactions), u16 the
        // object whose interaction it is, 8 i16 parameters (the first four
        // are the action tree's), i32 priority, 2 bytes, a float
        // ("attenuation"?), 10 bytes of flags, two u16 IDs, the name (u8
        // length + English TTA string, "Entertain.../Joke") and 12 bytes.
        // Where the list starts is not understood either (8 bytes after
        // the person data in most PERS, elsewhere in others): it is found
        // as the first well-formed record naming this Sim near the end of
        // the person data, and read while records stay well-formed with a
        // non-zero ID. In the 405 PERS of the game's lots this finds the
        // running interaction (object = person data 80) of 38 Sims, and
        // the next one (person data 79) where there is one.
        struct SavedInteraction {
            uint32_t uid = 0;
            int16_t initiator = 0, object = 0;
            std::array<int16_t, 8> params{};
            int32_t priority = 0;
            std::string name;
        };
        std::vector<SavedInteraction> interactions;
        // Why the part after the object data could not be read (the fields
        // above stay as far as they got).
        std::string tailError;
    };

    uint32_t version = 0; // of the object module; the states' layout depends on it
    // The simulator's globals when the lot was saved (SIMI, 0x53494D49,
    // instance 1: tag, version, a u32, then the 54 globals of STR# 0x81:
    // hour, day, ..., selected person, current family, current house, the
    // family's funds as % and / 10000; they agree with the neighborhood).
    // Empty if the lot has none.
    std::vector<int16_t> globals;
    std::map<int16_t, SaveType> saveTypes;
    std::map<int16_t, int16_t> saveTypeOf; // object ID -> save type
    std::vector<State> objects;            // by ID
    // The lot's SRELs (instance = from << 16 | to), named by object ID, with
    // their values: what the lot's objects and Sims think of each other, and
    // ownership (docs/data-findings.md §7: two values, "to" a neighbor ID).
    struct Relationship {
        int16_t from = 0, to = 0;
        std::vector<int32_t> values;
    };
    std::vector<Relationship> relationships;
    std::vector<std::string> warnings;

    static LotObjects load(DbpfPackage& package);
    // One XOBJ/PERS resource, for an object module of `version`.
    static State parseState(std::span<const uint8_t> data, uint32_t version, bool person = false);

    // GUID of the object with this ID, 0 if the object module does not say.
    uint32_t guidOf(int16_t id) const;
};

} // namespace os2
