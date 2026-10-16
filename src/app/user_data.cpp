#include "app/user_data.hpp"
#include "formats/log_once.hpp"

#include "formats/graphics_options.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif

namespace os2 {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    const auto e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// A neighborhood folder: has <name>_Neighborhood.package.
bool isNeighborhood(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / (dir.filename().string() + "_Neighborhood.package"), ec);
}

} // namespace

fs::path defaultUserDir() {
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base)
        base = std::getenv("USERPROFILE");
    return fs::path(base ? base : ".") / "opensims2";
#else
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / "Library/Application Support/opensims2";
#endif
}

void cloneTree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    const fs::path tmp = to.parent_path() / (to.filename().string() + ".copying");
    std::error_code ec;
    fs::remove_all(tmp, ec);
#if defined(__APPLE__)
    if (clonefile(from.c_str(), tmp.c_str(), 0) != 0) {
        // Not APFS (or another volume): a plain copy.
        fs::remove_all(tmp, ec);
        fs::copy(from, tmp, fs::copy_options::recursive);
    }
#else
    fs::copy(from, tmp, fs::copy_options::recursive);
#endif
    fs::rename(tmp, to);
}

UserData::UserData(fs::path gameDir, fs::path root) : game_(std::move(gameDir)), root_(std::move(root)) {
    fs::create_directories(neighborhoodsDir());
    if (inGameData(root_))
        throw std::runtime_error("the user folder " + root_.string() + " is inside the game's data");
    loadOptions();
}

fs::path UserData::shippedNeighborhoodsDir() const { return game_ / "TSData/Res/UserData/Neighborhoods"; }

std::vector<std::string> UserData::shipped() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(shippedNeighborhoodsDir(), ec))
        if (e.is_directory() && isNeighborhood(e.path()))
            out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const bool ta = a == "Tutorial", tb = b == "Tutorial";
        return ta != tb ? tb : a < b;
    });
    return out;
}

bool UserData::hasCopy(const std::string& prefix) const { return isNeighborhood(neighborhoodsDir() / prefix); }

fs::path UserData::neighborhoodForReading(const std::string& prefix) const {
    return hasCopy(prefix) ? neighborhoodsDir() / prefix : shippedNeighborhoodsDir() / prefix;
}

fs::path UserData::neighborhood(const std::string& prefix) {
    if (prefix.empty() || prefix.find('/') != std::string::npos || prefix.find('\\') != std::string::npos ||
        prefix == "." || prefix == "..")
        throw std::runtime_error("bad neighborhood name \"" + prefix + "\"");
    const fs::path mine = neighborhoodsDir() / prefix;
    if (isNeighborhood(mine))
        return mine;
    const fs::path shipped = shippedNeighborhoodsDir() / prefix;
    if (!isNeighborhood(shipped))
        throw std::runtime_error("no neighborhood \"" + prefix + "\" in " + shippedNeighborhoodsDir().string());
    std::error_code ec;
    fs::remove_all(mine, ec); // a broken copy
    cloneTree(shipped, mine);
    std::printf("opensims2: copied neighborhood %s to %s\n", prefix.c_str(), mine.string().c_str());
    std::fflush(stdout);
    return mine;
}

bool UserData::inGameData(const fs::path& path) const {
    std::error_code ec;
    const fs::path game = fs::weakly_canonical(fs::absolute(game_), ec);
    const fs::path p = fs::weakly_canonical(fs::absolute(path), ec);
    auto gi = game.begin();
    auto pi = p.begin();
    for (; gi != game.end(); ++gi, ++pi)
        if (pi == p.end() || *pi != *gi)
            return false;
    return true;
}

void UserData::loadOptions() {
    std::ifstream in(root_ / "Options.txt");
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        options_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

void UserData::saveOptions() const {
    const fs::path file = root_ / "Options.txt", tmp = root_ / "Options.txt.tmp";
    {
        std::ofstream out(tmp);
        out << "# opensims2 options\n";
        for (const auto& [k, v] : options_)
            out << k << " = " << v << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
}

std::optional<std::string> UserData::option(const std::string& key) const {
    const auto it = options_.find(key);
    return it == options_.end() ? std::nullopt : std::optional(it->second);
}

float UserData::optionFloat(const std::string& key, float fallback) const {
    const auto v = option(key);
    if (!v)
        return fallback;
    try {
        return std::stof(*v);
    } catch (const std::logic_error& err) { // not a number (std::stof): the fallback
        os2::logOnce("option", key, err);
        return fallback;
    }
}

bool UserData::optionBool(const std::string& key, bool fallback) const {
    const auto v = option(key);
    if (!v)
        return fallback;
    return *v == "1" || *v == "true" || *v == "on" || *v == "yes";
}

void UserData::setOption(const std::string& key, const std::string& value) {
    options_[key] = value;
    saveOptions();
}

void UserData::setOption(const std::string& key, float value) {
    std::ostringstream s;
    s << value;
    setOption(key, s.str());
}

void UserData::setOption(const std::string& key, bool value) { setOption(key, std::string(value ? "1" : "0")); }

namespace {
// The options panel's radio groups kept in Options.txt (app/options_panel.cpp names them).
const char* const kPickedOptions[] = {"Shadows",        "LightingQuality", "EffectsQuality",  "ObjectDetail",
                                      "MaterialDetail", "UITransparency",  "SquarePixels",
                                      "ObjectHiding",   "LivePIP",         "SpecialEventCamera",
                                      "NeighborhoodCameraDrift", "SnapshotSize", "SnapshotQuality", "VideoSize",
                                      "VideoSound",     "VideoQuality"};
} // namespace

const char* GameSettings::groupKey(int g) {
    static const char* const k[] = {"voice", "sfx", "music", "ambience"};
    return k[std::clamp(g, 0, 3)];
}

GameSettings GameSettings::load(const UserData& user) {
    GameSettings s;
    for (int g = 0; g < 4; ++g) {
        s.volume[size_t(g)] = std::clamp(user.optionFloat(std::string("volume.") + groupKey(g), 1.0f), 0.0f, 1.0f);
        s.muted[size_t(g)] = user.optionBool(std::string("mute.") + groupKey(g), false);
    }
    s.edgeScrolling = user.optionBool("edgeScrolling", true);
    s.reflections = user.optionBool("reflection", true);
    for (const char* option : kPickedOptions) {
        const float v = user.optionFloat(std::string("option.") + option, 0.0f);
        if (v >= 1)
            s.picks[option] = int(v);
    }
    s.picks["Reflection"] = s.reflections ? 2 : 1; // kept as "reflection"
    s.screenWidth = std::max(0, int(user.optionFloat("screenWidth", 0)));
    s.screenHeight = std::max(0, int(user.optionFloat("screenHeight", 0)));
    s.refreshRate = std::max(0, int(user.optionFloat("refreshRate", 0)));
    return s;
}

void GameSettings::applyGraphics() const {
    GraphicsOptions& g = graphicsOptions();
    const int shadows = pick("Shadows", 3); // Graphics Rules.sgr: $Low, $Medium, $High
    g.guob = g.heightMapShadows = shadows >= 2;
    g.simShadows = shadows >= 3;
    g.reflectionWithExtraViewer = g.nhoodWaterReflection = reflections;
    g.specHighlights = pick("LightingQuality", 3) >= 2;
}

void GameSettings::save(UserData& user) const {
    for (int g = 0; g < 4; ++g) {
        user.setOption(std::string("volume.") + groupKey(g), volume[size_t(g)]);
        user.setOption(std::string("mute.") + groupKey(g), muted[size_t(g)]);
    }
    user.setOption("edgeScrolling", edgeScrolling);
    user.setOption("reflection", reflections);
    for (const auto& [option, v] : picks)
        if (option != "Reflection")
            user.setOption(std::string("option.") + option, float(v));
    if (screenWidth > 0 && screenHeight > 0) {
        user.setOption("screenWidth", float(screenWidth));
        user.setOption("screenHeight", float(screenHeight));
    }
    if (refreshRate > 0)
        user.setOption("refreshRate", float(refreshRate));
}

} // namespace os2
