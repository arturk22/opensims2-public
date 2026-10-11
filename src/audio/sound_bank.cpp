#include "sound_bank.hpp"

#include "decode.hpp"
#include "formats/binreader.hpp"
#include "formats/property_set.hpp"

#include <set>

namespace os2 {

namespace {

// "TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII" (the s2pkg ls spelling: instanceHi before instance).
std::optional<Tgi> parseKey(std::string_view s) {
    if (s.size() != 35 || s[8] != '-' || s[17] != '-' || s[26] != '-')
        return std::nullopt;
    auto hex = [&](size_t at) -> std::optional<uint32_t> {
        uint32_t v = 0;
        for (size_t i = at; i < at + 8; ++i) {
            const char c = s[i];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0)
                return std::nullopt;
            v = v << 4 | uint32_t(d);
        }
        return v;
    };
    auto t = hex(0), g = hex(9), hi = hex(18), i = hex(27);
    if (!t || !g || !hi || !i)
        return std::nullopt;
    return Tgi{*t, *g, *i, *hi};
}

} // namespace

const char* soundGroupName(uint32_t group) {
    switch (group) {
    case 0xEB8AB356: return "Sfx/Tracks/A_M";
    case 0x0B8AB3CD: return "Sfx/Samples/A_M";
    case 0xADA1F465: return "Sfx/Tracks/N_Z";
    case 0xADA1F468: return "Sfx/Samples/N_Z";
    case 0x4B8AB359: return "Stings/Tracks";
    case 0xEB8AB35D: return "UI/Tracks";
    case 0x8C4E0090: return "footsteps/Tracks";
    case 0xAB8AB3D2: return "Stings/Samples";
    case 0x6B8AB3D7: return "UI/Samples";
    case 0xAC4E009A: return "footsteps/Samples";
    case 0x6B8AB361: return "TrackLogic";
    case 0x1D6962CF: return "HitLabUI";
    case 0x4C1940C5: return "PropertySets";
    case 0x2C3E53D9: return "Ambience/LotDay";
    case 0x4C3E66EE: return "Ambience/LotEvening";
    case 0x6E0A5058: return "Ambience/cemetary";
    case 0x2C3F70D5: return "Ambience/LotMorning";
    case 0x2C3F70D8: return "Ambience/LotNight";
    case 0x2C3F70DB: return "Ambience/LotRain";
    case 0x4C3F70DE: return "Ambience/LotRainAfter";
    case 0x8C4C567D: return "Ambience/LotCoyotes";
    case 0xAC4C5684: return "Ambience/LotFrogs";
    case 0xAC4C5687: return "Ambience/LotOwls";
    case 0xAC4C568B: return "Ambience/NHoodBirds";
    case 0xAC4C568F: return "Ambience/NHoodWind";
    case 0x4CC9E945: return "Ambience/Emptyness";
    case 0xADB5E4C8: return "Effects";
    case 0xADD550A7: return "Sys";
    case 0x0DA1F2CA: return "Vox/Tracks/A_G";
    case 0x2DA1F2EC: return "Vox/Samples/A_G";
    case 0x0DA1F2D2: return "Vox/Tracks/H_P";
    case 0x2DA1F2F0: return "Vox/Samples/H_P";
    case 0x0DA1F2D8: return "Vox/Tracks/R_S";
    case 0x2DA1F2F4: return "Vox/Samples/R_S";
    case 0x2DA1F2DD: return "Vox/Tracks/S_Soc_A_J";
    case 0x2DA1F2F9: return "Vox/Samples/S_Soc_A_J";
    case 0x2DA1F2E2: return "Vox/Tracks/S_Soc_K_Z";
    case 0x4DA1F2FD: return "Vox/Samples/S_Soc_K_Z";
    case 0x2DA1F2E7: return "Vox/Tracks/S_Z";
    case 0x4DA1F302: return "Vox/Samples/S_Z";
    case 0xFF606C7A: return "Music/Build";
    case 0xFF91772B: return "Music/Buy";
    case 0xFF549C7F: return "Music/CAS";
    case 0xFFC4D24B: return "Music/Nhood";
    case 0xFF1A1399: return "Music/hiphop";
    case 0xFFA15803: return "Music/metal";
    case 0xFF23C3BF: return "Music/pop";
    case 0xFF8A8826: return "Music/rnb";
    case 0xFFC8EDCE: return "Music/salsa";
    case 0xFF840CDD: return "Music/techno";
    case 0xFF8DFFC2: return "Music/splash";
    case 0xFF52F7D1: return "Music/credits";
    }
    return nullptr;
}

SoundBank::SoundBank(const std::filesystem::path& dir, std::string_view locale) {
    std::vector<std::filesystem::path> roots;
    if (std::filesystem::is_directory(dir / "Sound")) {
        roots.push_back(dir / "Sound");
        const auto loc = dir / "Locale" / std::string(locale) / "Sound";
        if (!locale.empty() && std::filesystem::is_directory(loc))
            roots.push_back(loc);
    } else {
        roots.push_back(dir);
    }
    for (const auto& r : roots) {
        auto index = std::make_unique<ResourceIndex>(r);
        for (auto type : {type::AUDIO, type::HITLIST, type::TRACK_SETTINGS})
            for (const auto& loc : index->ofType(type)) {
                owner_[loc.package] = index.get();
                const Key k{loc.entry->tgi.instance, loc.entry->tgi.instanceHi};
                // Later roots (the locale) replace earlier ones.
                if (type == type::AUDIO) {
                    samples_.push_back(loc);
                    // Sys INIs, HIT code and UI scripts are AUDIO too; one INI
                    // shares its instance with a pop song, so they stay out
                    // of the name lookup.
                    const uint32_t g = loc.entry->tgi.group;
                    if (g != 0xADD550A7 && g != 0x6B8AB361 && g != 0x1D6962CF && g != 0x4C1940C5)
                        samplesByKey_[k] = loc;
                } else {
                    (type == type::HITLIST ? hitLists_ : settings_)[k] = loc;
                }
            }
        indexes_.push_back(std::move(index));
    }
}

std::vector<SoundBank::Location> SoundBank::ofType(uint32_t type) const {
    std::vector<Location> out;
    for (const auto& i : indexes_)
        for (const auto& loc : i->ofType(type))
            out.push_back(loc);
    return out;
}

size_t SoundBank::packageCount() const {
    size_t n = 0;
    for (const auto& i : indexes_)
        n += i->packageCount();
    return n;
}

SoundBank::Key SoundBank::nameKey(std::string_view name) {
    return {nameInstanceHash(name), nameInstanceHiHash(name)};
}

static std::optional<SoundBank::Location> lookup(const std::map<SoundBank::Key, SoundBank::Location>& m,
                                                 SoundBank::Key k) {
    auto it = m.find(k);
    if (it == m.end())
        return std::nullopt;
    return it->second;
}

std::optional<SoundBank::Location> SoundBank::sample(Key k) const { return lookup(samplesByKey_, k); }
std::optional<SoundBank::Location> SoundBank::hitList(Key k) const { return lookup(hitLists_, k); }
std::optional<SoundBank::Location> SoundBank::trackSettings(Key k) const { return lookup(settings_, k); }

ResourceIndex& SoundBank::indexFor(const Location& loc) {
    auto it = owner_.find(loc.package);
    if (it == owner_.end())
        throw FormatError("sound bank: resource from another index");
    return *it->second;
}

void SoundBank::resolveInto(Key k, std::vector<Location>& out, int depth) const {
    if (depth > 4)
        return;
    if (auto hl = hitList(k)) {
        // The hit list is small; read it straight from its package.
        const bool wasOpen = hl->package->fileOpen();
        auto bytes = hl->package->read(*hl->entry);
        if (!wasOpen)
            hl->package->closeFile();
        for (const auto& s : parseHitList(bytes).sounds) {
            if (auto smp = sample({s.first, s.second}))
                out.push_back(*smp);
            else
                resolveInto({s.first, s.second}, out, depth + 1);
        }
        return;
    }
    if (auto smp = sample(k))
        out.push_back(*smp);
}

std::vector<SoundBank::Location> SoundBank::resolve(std::string_view nameOrKey) const {
    std::vector<Location> out;
    if (auto tgi = parseKey(nameOrKey)) {
        resolveInto({tgi->instance, tgi->instanceHi}, out, 0);
        return out;
    }
    resolveInto(nameKey(nameOrKey), out, 0);
    return out;
}

std::map<std::string, std::string> SoundBank::readTrackSettings(Key k) {
    auto loc = trackSettings(k);
    if (!loc)
        return {};
    return readPropertySet(read(*loc));
}

Pcm SoundBank::decode(const Location& loc) { return decodeSound(read(loc)); }

} // namespace os2
