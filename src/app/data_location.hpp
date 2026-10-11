// Where the game data (the extracted data/game folder) lives when the app is
// started without --data / $OS2_DATA: a folder remembered in the user defaults
// (key "GameDataPath"), else next to the app or in ~/Library/Application
// Support/opensims2, else one the user picks in a folder dialog.
#pragma once
#include <filesystem>
#include <optional>

namespace os2 {

// `relativeCheck` is a file that must exist under the game folder (it tells a
// real data/game apart from any other directory).
// Remembered folder, then the default places; nullopt when none has the data.
std::optional<std::filesystem::path> locateGameData(const std::filesystem::path& executable,
                                                    const char* relativeCheck);

// Shows a folder picker (the folder itself or the folder holding data/game is
// accepted), remembers a valid choice in the user defaults and returns it.
std::optional<std::filesystem::path> askForGameData(const char* relativeCheck);

} // namespace os2
