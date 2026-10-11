#pragma once

// The in-game save (the options panel's "Salvar", and "Salvar?" when leaving
// a lot): the lot the VM is running written back to its own package, and what
// the lot changes in the neighborhood (FAMI, SDSC, SREL, NGBH) to the
// neighborhood package beside it, through the same writers as `s2pkg
// lot-save`/`hood-save` (vm/lot_save.hpp, vm/lot_records.hpp,
// vm/hood_save.hpp). Each file is written under a temporary name and renamed
// over the old one, so an interrupted save leaves the previous one.
//
// Only files in the user's folder are written (app/user_data.hpp): a lot
// inside the game's data is refused. Plain C++.

#include <filesystem>
#include <string>
#include <vector>

namespace os2 {

namespace vm {
class VM;
}

struct GameSaveResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> lines; // what was written, for the log
};

// `lotFile` is the package the VM's lot was loaded from (<hood>/Lots/...);
// `gameDir` is data/game (refused as a destination; its shipped lots give
// the cObjects of objects created during play).
GameSaveResult saveGame(vm::VM& vm, const std::filesystem::path& lotFile, const std::filesystem::path& gameDir);

// Where this session keeps the character files it rewrote (a Sim grown up:
// the game saves the character on aging, docs/cas-mac.md §17.1); saveGame
// copies each over the neighborhood's file of the same name.
std::filesystem::path sessionCharactersDir();

} // namespace os2
