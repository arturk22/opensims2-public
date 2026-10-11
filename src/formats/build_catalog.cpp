#include "build_catalog.hpp"

#include "property_set.hpp"
#include "resindex.hpp"
#include "strings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kTypeCatalogPattern = 0xCCA8E925, kTypeStr = 0x53545223;

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

} // namespace

std::string BuildPattern::lotName() const {
    char b[16];
    std::snprintf(b, sizeof b, "%x", guid);
    return b;
}

BuildCatalog BuildCatalog::load(ResourceIndex& index) {
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
    for (auto* v : {&c.floors_, &c.walls_})
        std::stable_sort(v->begin(), v->end(), [](const BuildPattern& a, const BuildPattern& b) {
            return a.price != b.price ? a.price < b.price : a.name < b.name;
        });
    return c;
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

std::vector<const BuildPattern*> BuildCatalog::bySubsort(bool floor, const std::string& subsort) const {
    std::vector<const BuildPattern*> out;
    for (const auto& b : floor ? floors_ : walls_)
        if (subsort.empty() || b.subsort == subsort)
            out.push_back(&b);
    return out;
}

} // namespace os2
