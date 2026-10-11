#pragma once

// The player's own folder: where the game keeps the neighborhoods it plays
// and saves (the game's "EA Games/The Sims 2/Neighborhoods"), never the CD's
// data. Here it is ~/Library/Application Support/opensims2 (--user-dir <dir>
// or $OS2_USER_DIR change it), with
//
//   Neighborhoods/<N001|N002|N003|Tutorial>/   the neighborhoods played
//   Options.txt                                 the options (key = value)
//
// A shipped neighborhood is copied there the first time it is opened (on
// demand, one at a time): on APFS as a clone (clonefile), which costs no disk
// until a file changes; elsewhere as a plain copy. From then on everything
// loads from and saves to the copy; data/game stays read-only. Plain C++.

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class UserData {
public:
    // `gameDir` is data/game (the read-only CD data); `root` the user folder.
    UserData(std::filesystem::path gameDir, std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path neighborhoodsDir() const { return root_ / "Neighborhoods"; }
    // TSData/Res/UserData/Neighborhoods of the game (read-only).
    std::filesystem::path shippedNeighborhoodsDir() const;

    // The neighborhoods the game ships (folders with a <P>_Neighborhood.package),
    // the three towns first (N001, N002, N003, ...), the tutorial last.
    std::vector<std::string> shipped() const;
    // The user's copy of neighborhood `prefix` (a name like "N001"), made
    // now if there is none. Throws if neither exists.
    std::filesystem::path neighborhood(const std::string& prefix);
    // Whether the copy exists already.
    bool hasCopy(const std::string& prefix) const;
    // The folder a neighborhood is shown from without copying it: the
    // user's copy if there is one, else the shipped folder (read-only use:
    // thumbnails, names).
    std::filesystem::path neighborhoodForReading(const std::string& prefix) const;

    // Whether `path` lies in the game's data (where nothing may be written).
    bool inGameData(const std::filesystem::path& path) const;

    // --- Options (Options.txt) -------------------------------------------
    std::optional<std::string> option(const std::string& key) const;
    float optionFloat(const std::string& key, float fallback) const;
    bool optionBool(const std::string& key, bool fallback) const;
    void setOption(const std::string& key, const std::string& value); // saved at once
    void setOption(const std::string& key, float value);
    void setOption(const std::string& key, bool value);

private:
    void loadOptions();
    void saveOptions() const;

    std::filesystem::path game_, root_;
    std::map<std::string, std::string> options_;
};

// The settings the options windows change (the user's Options.txt keys:
// volume.<group>, mute.<group>, edgeScrolling).
struct GameSettings {
    // Voices, sound effects, music, ambience: 0..1.
    std::array<float, 4> volume{1, 1, 1, 1};
    std::array<bool, 4> muted{false, false, false, false};
    bool edgeScrolling = true;

    static GameSettings load(const UserData& user);
    void save(UserData& user) const;
    static const char* groupKey(int group); // "voice", "sfx", "music", "ambience"
};

// ~/Library/Application Support/opensims2 ($HOME based).
std::filesystem::path defaultUserDir();

// Copies a folder tree: a clone of the whole tree on APFS (clonefile), else
// a recursive copy; written under a temporary name and renamed into place.
void cloneTree(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace os2
