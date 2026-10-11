#pragma once

#include "vm.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace os2 {
struct Lot;
struct LotObjects;
} // namespace os2

namespace os2::vm {

// Puts the objects saved in a lot package into a VM, without rendering.
//
// The objects are exactly the object module's (LotObjects: its ObjM pairs,
// as the game's read_pre_object creates them), each a VM object with its
// saved ID, of the OBJD its save type's GUID names (objt). Sims come from
// their character files (Neighborhoods/N###/Characters/N###_User#####.package,
// named by the cObject "N001_User00011 - Don_objt", else found by the GUID),
// whose code is added to the library under a group of their own. The objects
// keep their saved position, level, direction, container, slot and room
// (object data 1-3 and 29), and the parts of a multi-tile object (XMTO) their
// lead object (object data 19); everything else is restored by
// resumeLotObjects (lot_resume.hpp) or starts fresh, for Init to fill in.
// The packages' leftovers (cObjects and states no ObjM pair names) are not
// loaded.
struct LotLoad {
    struct Entry {
        int16_t id = 0;
        std::string name; // OBJD file name (cObject name without "_objt"), or the catalog name
        bool person = false;
        bool created = false;
        std::string error; // why it was not created
    };
    std::vector<Entry> objects; // in ID order
    size_t created = 0;
    size_t relationships = 0; // from the lot's SRELs
};

// `charactersDir` is where the Sims' character files are (empty: Sims are
// left out with a reason).
LotLoad loadLot(VM& vm, const Lot& lot, const LotObjects& saved, const std::filesystem::path& charactersDir);

// The neighbourhood's character directory of a lot package
// (N001/Lots/N001_Lot11.package -> N001/Characters), empty if there is none.
std::filesystem::path charactersDirOf(const std::filesystem::path& lotPackage);

} // namespace os2::vm
