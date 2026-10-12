#pragma once

#include "vm.hpp"

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace os2 {
struct Lot;
struct LotObjects;
class DbpfPackage;
class ResourceIndex;
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
        std::string cObject; // the cObject with this ID when it names another object (a leftover)
    };
    std::vector<Entry> objects; // in ID order
    size_t created = 0;
    size_t relationships = 0; // from the lot's SRELs
    size_t stairs = 0, doors = 0; // in the world: staircases, door parts that opened a wall
};

// The cObjects (by instance) that are the current scenegraph objects of the
// lot's objects: the cObject's instance is the object's ID and it names the
// object's OBJD (only the lead part of a multi-tile object has one), or it
// is the cPerson of a Sim of the object module. The other cObjects are
// leftovers the package keeps (N001_Lot11 has 834 cObjects for 627 objects:
// six old toilets, two espresso machines, three pianos, stair steps, a
// second Don, ...).
std::set<uint32_t> currentCObjects(CodeLibrary& code, const Lot& lot, const LotObjects& saved);
// Removes the leftover cObjects from `lot` (for drawing it), reading the
// object module from the lot's package. Returns how many were removed.
size_t dropLeftoverCObjects(Lot& lot, DbpfPackage& lotPackage, ResourceIndex& index);

// The VM's rooms (VM::rooms) from the lot's room array: level, area,
// outside and pool (loadLot; Build mode after changing walls or floors).
void setLotRooms(VM& vm, const Lot& lot);

// `charactersDir` is where the Sims' character files are (empty: Sims are
// left out with a reason).
// Every neighbor has its definition, as in the game: a GUID no loaded OBJD
// has is looked up (once) among the neighbourhood's character files, whose
// code is then added (CodeLibrary::setGuidFallback). Returns the lookup
// (GUID -> character file, empty if none). loadLot calls it.
std::function<std::filesystem::path(uint32_t)> useCharacterFiles(CodeLibrary& code,
                                                                  const std::filesystem::path& charactersDir);
LotLoad loadLot(VM& vm, const Lot& lot, const LotObjects& saved, const std::filesystem::path& charactersDir);

// cTSNeighborhood::SetupFamilyFirstDay's last step: cEdithObjectModule::
// BroadcastMessage("reset lot") (BV 0x525b2c) runs the tree of that name of
// every object that has one (here at once, as a one-tick tree, with the
// object as its stack object and four zero parameters). The Controller -
// Age's "reset lot" -> "Sub - Set Age - All" -> "Sub - Generate Family"
// makes the arriving family's Sims with Create New Object Instance (the
// neighbor in the stack object) by the mailbox or the outside trash can
// (docs/neighborhood-mac.md §11.3). `characters` are the arriving Sims'
// character files: their OBJDs join the code first so that their GUIDs
// resolve. Returns the Sims it made.
struct ResetLotResult {
    size_t trees = 0;                // objects that ran a "reset lot"
    std::vector<ObjectId> sims;      // Sims that did not exist before
    std::vector<std::filesystem::path> simFiles; // each one's character file (empty: not one of `characters`)
    std::vector<std::string> errors; // trees that failed
};
ResetLotResult runResetLot(VM& vm, const std::vector<std::filesystem::path>& characters);

// The lot is ready (message 0x4C8E7905, after the loading screen) while the
// game is in the lot state (0x0BEA28C2): the base game's
// cTSGameStateController runs the global tree "CT - Lot Loaded" on the first
// object of the objects list (Mac 1.0 0x864e0c; BV runs "CT - Move In
// Cinematic - Prep" on the first global sim object instead). The tree finds
// the "Controller - Lot Load - Curtain Pull-Down" (updateSimObjects makes
// it) and, when its "reset lot" marked a family moving in (attribute 1),
// plays the moveIn cinematic and runs the curtain's move-in trees. Run here
// as a one-tick tree on the lowest object ID (inferred: the list's order).
// False when the tree or an object is missing, or the tree failed (`error`).
bool runLotLoaded(VM& vm, std::string* error = nullptr);

// The neighbourhood's character directory of a lot package
// (N001/Lots/N001_Lot11.package -> N001/Characters), empty if there is none.
std::filesystem::path charactersDirOf(const std::filesystem::path& lotPackage);

} // namespace os2::vm
