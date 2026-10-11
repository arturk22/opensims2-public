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
    for (auto [instance, out] : {std::pair{0x6B96E0A2u, &c.raise_}, std::pair{0x8B96E0CFu, &c.lower_}})
        if (auto b = index.read(Tgi{kTypeUiImage, kGroupTerrain, instance, 0}); b && b->size() >= 18 + 32 * 32) {
            const auto& v = *b;
            const size_t w = size_t(v[12] | v[13] << 8), h = size_t(v[14] | v[15] << 8), at = 18 + v[0];
            if (v[2] == 3 && v[16] == 8 && w == 32 && h == 32 && v.size() >= at + w * h)
                out->assign(v.begin() + long(at), v.begin() + long(at + w * h));
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
