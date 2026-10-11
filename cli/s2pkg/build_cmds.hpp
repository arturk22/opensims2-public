#pragma once

#include <string>
#include <utility>
#include <vector>

namespace os2 {

// Build-mode commands (lot geometry round trip, rooms, edits); -1 if `cmd`
// is not one.
int buildCommand(const std::string& cmd, int argc, char** argv);
const char* buildUsage();

struct Lot;
class ResourceIndex;
namespace vm {
class VM;
}

// Build-mode edits on a lot loaded into a VM, for lot-save (vm/build_edit.hpp;
// levels are the VM's, 1 = the ground, and default to 1):
//   --wall x0,y0,x1,y1[,level]          walls along a run (axis or 45°)
//   --room x0,y0,x1,y1[,level]          walls around a rectangle
//   --delete-wall x0,y0,x1,y1[,level]
//   --fence x0,y0,x1,y1,fence[,level]   a fence along a run (catalog GUID or name)
//   --delete-fence x0,y0,x1,y1[,level]
//   --foundation / --delete-foundation x0,y0,x1,y1  ground tiles (corners inclusive)
//   --floor x0,y0,x1,y1,pattern[,level[,rotation]] floor the tiles of a rectangle ("none" erases)
//   --floor-click px,py,pattern[,level[,rotation]] one click: the quarters under a point
//   --wallpaper x0,y0,x1,y1,side,pattern[,level]  side r, l or b (seen from x0,y0)
//   --wallpaper-room x,y,pattern[,level]
//   --door <guid|name>@x,y[,dir[,level]]  a door or window (object tool rules)
//   --build-undo / --build-redo (value ignored)
// Patterns are catalog GUIDs (0x...) or names/materials. Prints what
// happened; false if one failed.
bool isBuildEdit(const std::string& flag);
bool applyBuildEdits(vm::VM& vm, Lot& lot, ResourceIndex& index,
                     const std::vector<std::pair<std::string, std::string>>& edits);
// Differences between the build-mode geometry of two lots (walls, coverings,
// floors, rooms, pattern names), one line each.
std::vector<std::string> compareLotGeometry(const Lot& a, const Lot& b, size_t limit = 20);

} // namespace os2
