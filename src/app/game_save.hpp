#pragma once

#include "formats/lot_ui_state.hpp"

#include <optional>

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
// `sessionDir` holds the character files written in play (sessionCharactersDir()).
// `ui`, when given, is the UI state written with the lot (cTSMainToolbar::
// Write: resource 0x8B0C79D6, formats/lot_ui_state.hpp).
GameSaveResult saveGame(vm::VM& vm, const std::filesystem::path& lotFile, const std::filesystem::path& gameDir,
                        const std::filesystem::path& sessionDir, const std::optional<LotUiState>& ui = std::nullopt);

// Copies the character files of `sessionDir` (and its NPCs folder) that
// belong in the neighborhood `charactersDir` (<prefix>_User files, and
// rewritten ones it has) there, each with its neighbor's current names;
// the lines say what was copied.
std::vector<std::string> copySessionCharacters(vm::VM& vm, const std::filesystem::path& sessionDir,
                                               const std::filesystem::path& charactersDir, const std::string& prefix);

// Where this session keeps the character files it rewrote (a Sim grown up:
// the game saves the character on aging, docs/cas-mac.md §17.1) or made (a
// baby; an NPC, in its NPCs folder); saveGame copies them to the
// neighborhood's Characters. A folder of the user's temporary directory
// named after the process; removed when the process exits, and the first
// call removes those of sessions no longer running (crashed ones).
std::filesystem::path sessionCharactersDir();

// Removes the session folders in `tempDir` whose process is not running;
// returns how many.
size_t removeStaleSessionCharacters(const std::filesystem::path& tempDir);

} // namespace os2
