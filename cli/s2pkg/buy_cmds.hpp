#pragma once

#include <string>
#include <utility>
#include <vector>

namespace os2 {

// Buy-mode commands (catalog, buying into a lot); -1 if `cmd` is not one.
int buyCommand(const std::string& cmd, int argc, char** argv);
const char* buyUsage();

namespace vm {
class VM;
}
class ResourceIndex;

// Buy-mode edits on a loaded lot, for s2pkg buy and lot-save: "--buy
// <guid|name>@x,y[,dir[,level]]" buys a catalog object there, "--sell <id>"
// sells a placed object, "--move <id>@x,y[,dir[,level]]" moves one. Prints
// what happened; returns false if one failed.
bool applyBuyEdits(vm::VM& vm, ResourceIndex& index, const std::vector<std::pair<std::string, std::string>>& edits);

} // namespace os2
