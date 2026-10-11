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
        // Why the part after the object data could not be read (the fields
        // above stay as far as they got).
        std::string tailError;
    };

    uint32_t version = 0; // of the object module; the states' layout depends on it
    std::map<int16_t, SaveType> saveTypes;
    std::map<int16_t, int16_t> saveTypeOf; // object ID -> save type
    std::vector<State> objects;            // by ID
    std::vector<std::string> warnings;

    static LotObjects load(DbpfPackage& package);
    // One XOBJ/PERS resource, for an object module of `version`.
    static State parseState(std::span<const uint8_t> data, uint32_t version, bool person = false);

    // GUID of the object with this ID, 0 if the object module does not say.
    uint32_t guidOf(int16_t id) const;
};

} // namespace os2
