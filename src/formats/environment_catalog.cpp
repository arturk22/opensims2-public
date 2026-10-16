#include "environment_catalog.hpp"

#include "dbpf.hpp"
#include "log_once.hpp"
#include "property_set.hpp"
#include "resindex.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeCatalogPattern = 0xCCA8E925;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

// The Res folder above the catalog's pattern package.
std::filesystem::path resFolder(ResourceIndex& index) {
    for (const auto& loc : index.ofType(kTypeCatalogPattern))
        for (auto p = loc.package->path().parent_path(); p.has_relative_path(); p = p.parent_path())
            if (p.filename() == "Res")
                return p;
    return {};
}

// The blocks of a catalog script whose first word is `head`: its name and
// its "crapScore" ('#' starts a comment, as in the game's scripts).
void readScript(const std::filesystem::path& file, const std::string& head, std::map<std::string, PatternScore>& out) {
    std::ifstream in(file);
    std::string name;
    for (std::string line; std::getline(in, line);) {
        if (auto h = line.find('#'); h != std::string::npos)
            line.resize(h);
        std::istringstream ss(line);
        std::string w;
        if (!(ss >> w))
            continue;
        if (lower(w) == lower(head)) {
            std::string rest;
            std::getline(ss, rest);
            const size_t a = rest.find('"'), b = rest.rfind('"');
            name = a != std::string::npos && b > a ? rest.substr(a + 1, b - a - 1) : std::string();
            if (!name.empty())
                out[lower(name)] = PatternScore{};
            continue;
        }
        if (name.empty())
            continue;
        if (lower(w) == "crapscore") {
            float v = 0;
            if (ss >> v)
                out[lower(name)].crap = v;
        }
    }
}

PatternScores load(ResourceIndex& index) {
    PatternScores out;
    if (const auto res = resFolder(index); !res.empty()) {
        readScript(res / "Catalog" / "Scripts" / "floorPatterns.txt", "floorPattern", out.floors);
        readScript(res / "Catalog" / "Scripts" / "wallPatterns.txt", "wallPattern", out.walls);
    }
    for (const auto& loc : index.ofType(kTypeCatalogPattern)) {
        try {
            const auto bytes = index.read(loc);
            const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            auto get = [&](const char* key) -> const std::string* {
                auto it = p.find(key);
                return it == p.end() ? nullptr : &it->second;
            };
            const std::string* type = get("type");
            const std::string* guid = get("guid");
            if (!type || !guid || (*type != "floor" && *type != "wall"))
                continue;
            PatternScore s;
            if (const std::string* v = get("cost"))
                s.price = int(std::strtol(v->c_str(), nullptr, 10));
            if (const std::string* v = get("nicenessmultiplier"))
                s.niceness = std::strtof(v->c_str(), nullptr);
            if (const std::string* v = get("crapscore"))
                s.crap = std::strtof(v->c_str(), nullptr);
            char key[16];
            std::snprintf(key, sizeof key, "%lx", std::strtoul(guid->c_str(), nullptr, 10));
            (*type == "floor" ? out.floors : out.walls)[key] = s;
        } catch (const std::exception& err) {
            os2::logOnce("pattern scores", "catalog pattern", err);
        }
    }
    return out;
}

} // namespace

const PatternScore* PatternScores::floor(const std::string& lotName) const {
    auto it = floors.find(lower(lotName));
    return it == floors.end() ? nullptr : &it->second;
}

const PatternScore* PatternScores::wall(const std::string& lotName) const {
    auto it = walls.find(lower(lotName));
    return it == walls.end() ? nullptr : &it->second;
}

const PatternScores& patternScores(ResourceIndex& index) {
    // By the index and its number of resources (a later index may take
    // the address of one gone); a list keeps the tables where they are.
    static std::mutex mutex;
    static std::list<std::pair<std::pair<const ResourceIndex*, size_t>, PatternScores>> cache;
    const std::pair<const ResourceIndex*, size_t> key{&index, index.resourceCount()};
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [k, s] : cache)
        if (k == key)
            return s;
    cache.emplace_back(key, load(index));
    return cache.back().second;
}

} // namespace os2
