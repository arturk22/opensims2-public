#include "build_catalog.hpp"

#include "property_set.hpp"
#include "resindex.hpp"
#include "strings.hpp"

#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace os2 {

namespace {

constexpr uint32_t kTypeCatalogPattern = 0xCCA8E925, kTypeCatalogFence = 0x2CB230B8, kTypeStr = 0x53545223,
                   kTypeCatalogTerrainPaint = 0x4DCADB7E, kTypeCatalogRoof = 0xACA8EA06, kTypeUiImage = 0x856DDBAC, kGroupTerrain = 0x4B72DA28;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(uint8_t(c)));
    return s;
}

uint32_t number(const std::map<std::string, std::string>& p, const char* key, uint32_t fallback = 0) {
    auto it = p.find(key);
    if (it == p.end())
        return fallback;
    try {
        return uint32_t(std::stoul(it->second));
    } catch (const std::logic_error&) {
        return fallback;
    }
}

std::string text(const std::map<std::string, std::string>& p, const char* key) {
    auto it = p.find(key);
    return it == p.end() ? std::string() : it->second;
}

// The game's Res folder: the one above the package of a catalog text.
// The catalog texts of Text/Build.package (group 0xFFFFFFFF: the package's own).
bool buildTextPackage(const ResourceIndex::Location& loc) {
    return loc.package->path().filename() == "Build.package" && loc.package->path().parent_path().filename() == "Text";
}

std::filesystem::path resFolder(ResourceIndex& index) {
    for (const auto& loc : index.ofType(kTypeStr)) {
        if (!buildTextPackage(loc))
            continue;
        for (auto p = loc.package->path().parent_path(); p.has_relative_path(); p = p.parent_path())
            if (p.filename() == "Res")
                return p;
    }
    return {};
}

std::optional<StringTable> buildText(ResourceIndex& index, uint32_t instance) {
    // The player's language first (Locale packages come first in the index).
    for (const auto& loc : index.ofType(kTypeStr))
        if (loc.entry->tgi.instance == instance && buildTextPackage(loc))
            try {
                return StringTable::parse(index.read(loc));
            } catch (const std::exception&) {
            }
    return std::nullopt;
}

// The leading number of a catalog price string ("70 -- Price - DO NOT TRANSLATE").
int priceText(const std::string& s) { return std::atoi(s.c_str()); }

// The blocks of a catalog script (Catalog/Scripts/*.txt): each starts at a
// line whose first word is `head`; the lines of a block as words.
std::vector<std::vector<std::vector<std::string>>> scriptBlocks(const std::filesystem::path& file, const std::string& head) {
    std::vector<std::vector<std::vector<std::string>>> out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (auto h = line.find('#'); h != std::string::npos)
            line.resize(h);
        std::istringstream ss(line);
        std::vector<std::string> words;
        for (std::string w; ss >> w;)
            words.push_back(w);
        if (words.empty())
            continue;
        if (lower(words[0]) == lower(head))
            out.emplace_back();
        if (!out.empty())
            out.back().push_back(std::move(words));
    }
    return out;
}

} // namespace

std::string BuildPattern::lotName() const {
    char b[16];
    std::snprintf(b, sizeof b, "%x", guid);
    return b;
}

std::set<uint32_t> walkOverFenceGuids(ResourceIndex& index) {
    std::set<uint32_t> out;
    for (const auto& loc : index.ofType(kTypeCatalogFence)) {
        const auto bytes = index.read(loc);
        const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (number(p, "maywalkover") != 0)
            out.insert(number(p, "guid"));
    }
    return out;
}

int railingToolPrice(ResourceIndex& index) {
    // userToolProxies.txt "userToolProxy 0x2c7bf690"
    // (kGZCLSID_cTSUserToolModularStairsRailing): catalog text "build" 139
    // at its catalogTextIndex.
    const auto res = resFolder(index);
    if (res.empty())
        return 0;
    for (const auto& block : scriptBlocks(res / "Catalog" / "Scripts" / "userToolProxies.txt", "userToolProxy")) {
        if (block.empty() || block[0].size() < 2 || std::strtoul(block[0][1].c_str(), nullptr, 16) != kRailingToolProduct)
            continue;
        for (const auto& w : block)
            if (lower(w[0]) == "catalogtextindex" && w.size() > 1)
                if (const auto text = buildText(index, 139)) {
                    const size_t i = 3 * size_t(std::atoi(w[1].c_str()));
                    const int p = priceText(text->get(i));
                    return p ? p : priceText(text->get(i, lang::EnglishUS));
                }
    }
    return 0;
}

BuildCatalog BuildCatalog::load(ResourceIndex& index) {
    // Kept for the next load of the same index (every Build-mode setup asks).
    static std::mutex mutex;
    static std::vector<std::pair<std::pair<const ResourceIndex*, size_t>, BuildCatalog>> cache;
    const std::pair<const ResourceIndex*, size_t> key{&index, index.ofType(kTypeCatalogPattern).size()};
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& c : cache)
            if (c.first == key)
                return c.second;
    }
    BuildCatalog c = loadUncached(index);
    std::lock_guard<std::mutex> lock(mutex);
    cache.emplace_back(key, c);
    return c;
}

BuildCatalog BuildCatalog::loadUncached(ResourceIndex& index) {
    BuildCatalog c;
    for (const auto& loc : index.ofType(kTypeCatalogPattern)) {
        const auto bytes = index.read(loc);
        const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        const std::string type = text(p, "type");
        if ((type != "floor" && type != "wall") || number(p, "showincatalog", 1) == 0 || number(p, "deprecated") != 0)
            continue;
        BuildPattern b;
        b.guid = number(p, "guid");
        b.floor = type == "floor";
        b.material = text(p, "material");
        b.subsort = lower(text(p, "subsort"));
        b.price = int(number(p, "cost"));
        b.name = text(p, "name");
        b.description = text(p, "description");
        // The catalog strings in the player's language: name 0, description 1.
        if (auto s = index.read(Tgi{kTypeStr, number(p, "stringsetgroupid"), number(p, "stringsetid"), 0})) {
            try {
                const StringTable t = StringTable::parse(*s);
                if (const std::string n = t.get(0); !n.empty())
                    b.name = n;
                if (const std::string d = t.get(1); !d.empty())
                    b.description = d;
            } catch (const std::exception&) {
            }
        }
        if (!b.guid || b.material.empty())
            continue;
        (b.floor ? c.floors_ : c.walls_).push_back(std::move(b));
    }
    for (const auto& loc : index.ofType(kTypeCatalogFence)) {
        const auto bytes = index.read(loc);
        const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (number(p, "showincatalog", 1) == 0 || number(p, "deprecated") != 0)
            continue;
        BuildFence f;
        f.guid = number(p, "guid");
        f.price = int(number(p, "cost"));
        f.name = text(p, "name");
        f.description = text(p, "description");
        f.rail = text(p, "rail"), f.diagonalRail = text(p, "diagrail"), f.post = text(p, "post");
        f.walkOver = number(p, "maywalkover") != 0;
        if (auto s = index.read(Tgi{kTypeStr, number(p, "stringsetgroupid"), number(p, "stringsetid"), 0})) {
            try {
                const StringTable t = StringTable::parse(*s);
                if (const std::string n = t.get(0); !n.empty())
                    f.name = n;
                if (const std::string d = t.get(1); !d.empty())
                    f.description = d;
            } catch (const std::exception&) {
            }
        }
        if (f.guid && !f.rail.empty())
            c.fences_.push_back(std::move(f));
    }
    for (const auto& loc : index.ofType(kTypeCatalogTerrainPaint)) {
        const auto bytes = index.read(loc);
        const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (number(p, "showincatalog", 1) == 0 || number(p, "deprecated") != 0)
            continue;
        BuildTerrainPaint t;
        t.guid = number(p, "guid");
        t.texture = text(p, "texturetname");
        t.name = text(p, "name");
        t.description = text(p, "description");
        if (auto s = index.read(Tgi{kTypeStr, number(p, "stringsetgroupid"), number(p, "stringsetid"), 0})) {
            try {
                const StringTable st = StringTable::parse(*s);
                if (const std::string n = st.get(0); !n.empty())
                    t.name = n;
                if (const std::string d = st.get(1); !d.empty())
                    t.description = d;
            } catch (const std::exception&) {
            }
        }
        if (!t.texture.empty())
            c.paints_.push_back(std::move(t));
    }
    for (const auto& loc : index.ofType(kTypeCatalogRoof)) {
        const auto bytes = index.read(loc);
        const auto p = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (number(p, "showincatalog", 1) == 0 || number(p, "deprecated") != 0)
            continue;
        BuildRoofPattern t;
        t.guid = number(p, "guid");
        t.texture = text(p, "texturetop");
        t.name = text(p, "name");
        t.description = text(p, "description");
        if (auto s = index.read(Tgi{kTypeStr, number(p, "stringsetgroupid"), number(p, "stringsetid"), 0})) {
            try {
                const StringTable st = StringTable::parse(*s);
                if (const std::string n = st.get(0); !n.empty())
                    t.name = n;
                if (const std::string d = st.get(1); !d.empty())
                    t.description = d;
            } catch (const std::exception&) {
            }
        }
        if (t.guid)
            c.roofs_.push_back(std::move(t));
    }
    std::sort(c.roofs_.begin(), c.roofs_.end(),
              [](const BuildRoofPattern& a, const BuildRoofPattern& b) { return a.name < b.name; });
    std::sort(c.paints_.begin(), c.paints_.end(),
              [](const BuildTerrainPaint& a, const BuildTerrainPaint& b) { return a.name < b.name; });
    // The brush profiles: uncompressed 8-bit grey TGAs (image type 3).
    for (auto [instance, out] : {std::pair{0x6B96E0A2u, &c.raise_}, std::pair{0x8B96E0CFu, &c.lower_}, std::pair{0x8B731811u, &c.water_}})
        if (auto b = index.read(Tgi{0x856DDBACu, kGroupTerrain, instance, 0}); b && b->size() >= 18 + 32 * 32) {
            const auto& v = *b;
            const size_t w = size_t(v[12] | v[13] << 8), h = size_t(v[14] | v[15] << 8), at = 18 + v[0];
            if (v[2] == 3 && v[16] == 8 && w == 32 && h == 32 && v.size() >= at + w * h)
                out->assign(v.begin() + long(at), v.begin() + long(at + w * h));
        }
    // modularstairs.txt (catalog text "build" 139) and roofs.txt (140).
    if (const auto res = resFolder(index); !res.empty()) {
        const auto scripts = res / "Catalog" / "Scripts";
        const auto stairText = buildText(index, 139);
        for (const auto& block : scriptBlocks(scripts / "modularstairs.txt", "modularStairs")) {
            BuildStairs b;
            int text = -1;
            for (const auto& w : block) {
                const std::string k = lower(w[0]);
                if (k == "modularstairs" && w.size() > 1)
                    b.id = std::atoi(w[1].c_str());
                else if (k == "catalogtextindex" && w.size() > 1)
                    text = std::atoi(w[1].c_str());
                else if (k == "objectid" && w.size() > 2) {
                    static const char* const slots[5] = {"landing", "placeholder", "step", "topstep", "bottomstep"};
                    for (int i = 0; i < 5; ++i)
                        if (lower(w[1]) == slots[i])
                            b.objects[size_t(i)] = uint32_t(std::strtoul(w[2].c_str(), nullptr, 16));
                }
            }
            if (text < 0) // notInCatalog
                continue;
            if (stairText) {
                // The price strings are "DO NOT TRANSLATE": US English when a
                // translation leaves one out (Portuguese: Holy Smoke, WroughtWright).
                b.price = priceText(stairText->get(size_t(3 * text)));
                if (!b.price)
                    b.price = priceText(stairText->get(size_t(3 * text), lang::EnglishUS));
                b.name = stairText->get(size_t(3 * text + 1));
                b.description = stairText->get(size_t(3 * text + 2));
            }
            c.stairs_.push_back(std::move(b));
        }
        c.railingPrice_ = railingToolPrice(index);
        const auto archText = buildText(index, 143);
        for (const auto& block : scriptBlocks(scripts / "fencearches.txt", "fenceArch")) {
            BuildFenceArch a;
            int text = -1;
            for (const auto& w : block) {
                const std::string k = lower(w[0]);
                std::string v = w.size() > 1 ? w[1] : "";
                std::erase(v, '"');
                if (k == "fencearch")
                    a.id = std::atoi(v.c_str());
                else if (k == "catalogtextindex")
                    text = std::atoi(v.c_str());
                else if (k == "archcentermodel")
                    a.center = v;
                else if (k == "archeastmodel")
                    a.east = v;
                else if (k == "archwestmodel")
                    a.west = v;
                else if (k == "archpostmodel")
                    a.post = v;
            }
            if (archText && text >= 0) {
                a.price = priceText(archText->get(size_t(3 * text)));
                if (!a.price)
                    a.price = priceText(archText->get(size_t(3 * text), lang::EnglishUS));
                a.name = archText->get(size_t(3 * text + 1));
                a.description = archText->get(size_t(3 * text + 2));
            }
            c.arches_.push_back(std::move(a));
        }
        const auto roofText = buildText(index, 140);
        for (const auto& block : scriptBlocks(scripts / "roofs.txt", "roof")) {
            BuildRoofProduct r;
            int text = -1;
            for (const auto& w : block) {
                const std::string k = lower(w[0]);
                if (k == "roof" && w.size() > 1)
                    r.id = std::atoi(w[1].c_str());
                else if (k == "catalogtextindex" && w.size() > 1)
                    text = std::atoi(w[1].c_str());
                else if (k == "defaultroofpattern" && w.size() > 1) {
                    r.defaultPattern = w[1];
                    std::erase(r.defaultPattern, '"');
                } else if (k == "thumbnailresource" && w.size() > 2)
                    r.thumbnail = uint32_t(std::strtoul(w[2].c_str(), nullptr, 16));
            }
            if (roofText && text >= 0) {
                r.price = priceText(roofText->get(size_t(3 * text)));
                if (!r.price)
                    r.price = priceText(roofText->get(size_t(3 * text), lang::EnglishUS));
                r.name = roofText->get(size_t(3 * text + 1));
                r.description = roofText->get(size_t(3 * text + 2));
            }
            c.roofProducts_.push_back(std::move(r));
        }
    }
    std::stable_sort(c.fences_.begin(), c.fences_.end(), [](const BuildFence& a, const BuildFence& b) {
        return a.price != b.price ? a.price < b.price : a.name < b.name;
    });
    for (auto* v : {&c.floors_, &c.walls_})
        std::stable_sort(v->begin(), v->end(), [](const BuildPattern& a, const BuildPattern& b) {
            return a.price != b.price ? a.price < b.price : a.name < b.name;
        });
    return c;
}

const BuildStairs* BuildCatalog::stairsById(int id) const {
    for (const auto& b : stairs_)
        if (b.id == id)
            return &b;
    return nullptr;
}

const BuildRoofProduct* BuildCatalog::roofProduct(int id) const {
    for (const auto& r : roofProducts_)
        if (r.id == id)
            return &r;
    return nullptr;
}

const BuildPattern* BuildCatalog::byGuid(uint32_t guid) const {
    for (const auto* v : {&floors_, &walls_})
        for (const auto& b : *v)
            if (b.guid == guid)
                return &b;
    return nullptr;
}

const BuildPattern* BuildCatalog::find(const std::string& key, bool floor) const {
    const auto& v = floor ? floors_ : walls_;
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        const uint32_t guid = uint32_t(std::strtoul(key.c_str(), nullptr, 16));
        for (const auto& b : v)
            if (b.guid == guid)
                return &b;
        return nullptr;
    }
    const std::string k = lower(key);
    for (const auto& b : v)
        if (lower(b.material) == k || lower(b.name) == k || b.lotName() == k)
            return &b;
    for (const auto& b : v)
        if (lower(b.material).find(k) != std::string::npos || lower(b.name).find(k) != std::string::npos)
            return &b;
    return nullptr;
}

const BuildFence* BuildCatalog::fenceByGuid(uint32_t guid) const {
    for (const auto& f : fences_)
        if (f.guid == guid)
            return &f;
    return nullptr;
}

const BuildFence* BuildCatalog::findFence(const std::string& key) const {
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
        return fenceByGuid(uint32_t(std::strtoul(key.c_str(), nullptr, 16)));
    const std::string k = lower(key);
    for (const auto& f : fences_)
        if (lower(f.name) == k || lower(f.rail) == k)
            return &f;
    for (const auto& f : fences_)
        if (lower(f.name).find(k) != std::string::npos || lower(f.rail).find(k) != std::string::npos)
            return &f;
    return nullptr;
}

const BuildRoofPattern* BuildCatalog::findRoofPattern(const std::string& key) const {
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        const uint32_t guid = uint32_t(std::strtoul(key.c_str(), nullptr, 16));
        for (const auto& t : roofs_)
            if (t.guid == guid)
                return &t;
        return nullptr;
    }
    const std::string k = lower(key);
    for (const auto& t : roofs_)
        if (lower(t.texture).find(k) != std::string::npos || lower(t.name).find(k) != std::string::npos)
            return &t;
    return nullptr;
}

const BuildTerrainPaint* BuildCatalog::findTerrainPaint(const std::string& key) const {
    const std::string k = lower(key);
    for (const auto& t : paints_)
        if (lower(t.texture) == k || lower(t.name) == k)
            return &t;
    for (const auto& t : paints_)
        if (lower(t.texture).find(k) != std::string::npos || lower(t.name).find(k) != std::string::npos)
            return &t;
    return nullptr;
}

std::vector<const BuildPattern*> BuildCatalog::bySubsort(bool floor, const std::string& subsort) const {
    std::vector<const BuildPattern*> out;
    for (const auto& b : floor ? floors_ : walls_)
        if (subsort.empty() || b.subsort == subsort)
            out.push_back(&b);
    return out;
}

} // namespace os2
