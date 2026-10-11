#include "app/user_data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <sys/clonefile.h>

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
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / "Library/Application Support/opensims2";
}

void cloneTree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    const fs::path tmp = to.parent_path() / (to.filename().string() + ".copying");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (clonefile(from.c_str(), tmp.c_str(), 0) != 0) {
        // Not APFS (or another volume): a plain copy.
        fs::remove_all(tmp, ec);
        fs::copy(from, tmp, fs::copy_options::recursive);
    }
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
    if (prefix.empty() || prefix.find('/') != std::string::npos || prefix == "." || prefix == "..")
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
    const std::string game = fs::weakly_canonical(fs::absolute(game_)).string();
    const std::string p = fs::weakly_canonical(fs::absolute(path)).string();
    return p == game || (p.size() > game.size() && p.compare(0, game.size(), game) == 0 && p[game.size()] == '/');
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
    } catch (const std::exception&) {
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
    return s;
}

void GameSettings::save(UserData& user) const {
    for (int g = 0; g < 4; ++g) {
        user.setOption(std::string("volume.") + groupKey(g), volume[size_t(g)]);
        user.setOption(std::string("mute.") + groupKey(g), muted[size_t(g)]);
    }
    user.setOption("edgeScrolling", edgeScrolling);
}

} // namespace os2
