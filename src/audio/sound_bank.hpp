#pragma once

#include "formats/audio.hpp"
#include "formats/resindex.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// The game's sound resources: samples (AUDIO 0x2026960B), tracks (track
// settings 0x0B9EB87E + hit list 0x7B1ACFCD, same group/instance) and the
// other AUDIO payloads. Names hash like scenegraph names (instance =
// CRC24|0xFF000000, instanceHi = CRC32 of the lowercased name: "FileName
// AsInstanceID" in the game's TSAudio INI); groups say which folder a
// resource came from (Sfx/Tracks/A_M, Vox/Samples/H_P, ...). Lookups ignore
// the group, since tracks and samples live in different ones.
// Folder a sound group comes from, after the [DBSegments] section of the
// game's TSAudio INI (Misc.package, group 0xADD550A7): "Sfx/Samples/A_M",
// "Music/pop", ...; nullptr for groups it does not list.
const char* soundGroupName(uint32_t group);

class SoundBank {
public:
    using Location = ResourceIndex::Location;
    using Key = std::pair<uint32_t, uint32_t>; // (instance, instanceHi)

    // `dir` is TSData/Res (indexes Sound/ and Locale/<locale>/Sound/, the
    // latter taking precedence) or any directory of sound packages.
    explicit SoundBank(const std::filesystem::path& dir, std::string_view locale = defaultLocale());

    static Key nameKey(std::string_view name);

    const std::vector<Location>& samples() const { return samples_; }
    const std::map<Key, Location>& tracks() const { return hitLists_; }

    std::optional<Location> sample(Key k) const;
    std::optional<Location> hitList(Key k) const;
    std::optional<Location> trackSettings(Key k) const;
    // Every resource of a type in the sound packages (e.g. the sound
    // property sets, type::SOUND_PROPERTIES).
    std::vector<Location> ofType(uint32_t type) const;

    // The samples a track name, sample name or "TTTTTTTT-GGGGGGGG-HHHHHHHH-
    // IIIIIIII" key stands for (a track gives every entry of its hit list,
    // following nested hit lists). Empty when nothing matches.
    std::vector<Location> resolve(std::string_view nameOrKey) const;

    std::vector<uint8_t> read(const Location& loc) { return loc.package ? indexFor(loc).read(loc) : std::vector<uint8_t>{}; }
    // Track settings as key -> value text (keys are hashes, e.g. "0xff099db2" = sample rate).
    std::map<std::string, std::string> readTrackSettings(Key k);
    Pcm decode(const Location& loc);

    size_t packageCount() const;

private:
    std::vector<std::unique_ptr<ResourceIndex>> indexes_;
    std::vector<Location> samples_;
    std::map<Key, Location> samplesByKey_, hitLists_, settings_;
    std::map<const DbpfPackage*, ResourceIndex*> owner_;

    ResourceIndex& indexFor(const Location& loc);
    void resolveInto(Key k, std::vector<Location>& out, int depth) const;
};

} // namespace os2
