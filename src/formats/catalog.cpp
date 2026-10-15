// The Buy-mode catalog (catalog.hpp).

#include "catalog.hpp"
#include "formats/log_once.hpp"

#include "dbpf.hpp"
#include "resindex.hpp"
#include "strings.hpp"
#include "ui_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kTypeObjd = 0x4F424A44, kTypeCtss = 0x43545353, kTypeThumbnail = 0xAC2950C1;
constexpr uint16_t kObjectTypePerson = 2;
constexpr uint32_t kTypeStr = 0x53545223;
constexpr uint16_t kModelNames = 0x85;
constexpr size_t kModelIndex = 0xD0; // ObjectDefinition::modelIndex

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

CatalogEntry catalogTwin(const CatalogEntry& e, ResourceIndex& index, uint32_t group, uint16_t instance) {
    CatalogEntry twin = e;
    const auto bytes = index.read(Tgi{kTypeObjd, group, instance, 0});
    if (!bytes)
        return twin;
    try {
        const ObjectDefinition d = ObjectDefinition::parse(*bytes);
        twin.group = group, twin.instance = instance;
        twin.guid = d.guid();
        twin.fileName = d.fileName;
        twin.model.clear();
        if (auto names = index.read(Tgi{kTypeStr, group, kModelNames, 0}))
            twin.model = StringTable::parse(*names).get(d.field(kModelIndex), lang::EnglishUS);
    } catch (const std::exception& err) {
        os2::logOnce("the catalog twin of", e.fileName, err);
    }
    return twin;
}

uint32_t thumbnailKey(uint32_t group, const std::string& model) {
    return nameInstanceHiHash(std::to_string(group) + model);
}

const char* functionSortName(int bit) {
    static const char* const names[kFunctionSortCount] = {"Seating",    "Surfaces", "Appliances",
                                                          "Electronics", "Plumbing", "Decorative",
                                                          "Miscellaneous", "Lighting", "Hobbies"};
    return bit >= 0 && bit < kFunctionSortCount ? names[bit] : "?";
}

const char* roomSortName(int bit) {
    static const char* const names[kRoomSortCount] = {"Kitchen", "Bedroom", "Bathroom", "Living Room", "Outside",
                                                      "Dining Room", "Miscellaneous", "Study", "Kids"};
    return bit >= 0 && bit < kRoomSortCount ? names[bit] : "?";
}

namespace {
// What Catalog::load read, kept for the next load of the same index (Buy and
// Build mode both ask, and every scene entry again; each read is ~85 ms).
struct LoadedCatalog {
    const ResourceIndex* index = nullptr;
    size_t objdCount = 0;
    bool build = false;
    std::vector<CatalogEntry> entries;
    std::map<uint32_t, size_t> byGuid;
};
std::mutex gCatalogMutex;
std::vector<LoadedCatalog> gCatalogCache;
} // namespace

Catalog Catalog::load(ResourceIndex& index, bool build) {
    const size_t objdCount = index.ofType(kTypeObjd).size();
    {
        std::lock_guard<std::mutex> lock(gCatalogMutex);
        for (const auto& l : gCatalogCache)
            if (l.index == &index && l.build == build && l.objdCount == objdCount) {
                Catalog hit;
                hit.entries_ = l.entries;
                hit.byGuid_ = l.byGuid;
                return hit;
            }
    }
    // One pass over the OBJDs makes both catalogs (Buy and Build mode ask one
    // after the other), and both are kept.
    std::array<Catalog, 2> both = loadBoth(index);
    std::lock_guard<std::mutex> lock(gCatalogMutex);
    for (int b = 0; b < 2; ++b)
        gCatalogCache.push_back({&index, objdCount, b == 1, both[b].entries_, both[b].byGuid_});
    return std::move(both[build ? 1 : 0]);
}

std::array<Catalog, 2> Catalog::loadBoth(ResourceIndex& index) {
    std::array<Catalog, 2> out; // [0] Buy mode, [1] Build mode
    std::array<std::set<uint32_t>, 2> guids;
    for (const auto& loc : index.ofType(kTypeObjd)) {
        const Tgi& tgi = loc.entry->tgi;
        if (tgi.group == 0xFFFFFFFF)
            continue;
        ObjectDefinition d;
        try {
            d = ObjectDefinition::parse(index.read(loc));
        } catch (const std::exception& err) {
            os2::logOnce("catalog OBJD", std::to_string(tgi.group) + ":" + std::to_string(tgi.instance), err);
            continue;
        }
        const uint16_t function = d.field(objd::FunctionSort), room = d.field(objd::RoomSort);
        const int price = d.field(objd::Price);
        // What cTSWinProcCatalog accepts on a residential lot
        // (docs/buy-build-mac.md §1.3): room sort ≠ 0, not a Sim, and not
        // an unlockable reward (field 16; the unlocked lists are not kept).
        if (d.field(objd::ObjectType) == kObjectTypePerson || d.field(objd::Unlockable) != 0)
            continue;
        const std::array<bool, 2> wanted = {room != 0, d.field(objd::BuildModeType) != 0};
        if (!wanted[0] && !wanted[1])
            continue;
        // A multi-tile part (master ID set, sub index other than -1) is
        // bought through its master.
        const bool multi = d.field(objd::MasterId) != 0;
        if (multi && d.field(objd::SubIndex) != 0xFFFF)
            continue;
        std::array<bool, 2> take{};
        for (int b = 0; b < 2; ++b)
            take[b] = wanted[b] && guids[b].insert(d.guid()).second;
        if (!take[0] && !take[1])
            continue;
        CatalogEntry e;
        e.group = tgi.group;
        e.instance = uint16_t(tgi.instance);
        e.guid = d.guid();
        e.fileName = d.fileName;
        if (auto bytes = index.read(Tgi{kTypeStr, tgi.group, kModelNames, 0})) {
            try {
                e.model = StringTable::parse(*bytes).get(d.field(kModelIndex), lang::EnglishUS);
            } catch (const std::exception& err) {
                os2::logOnce("catalog model names", std::to_string(tgi.group), err);
            }
        }
        e.price = price;
        e.functionSort = function;
        e.roomSort = room;
        e.functionSubSort = d.field(objd::FunctionSubSort);
        e.communitySort = d.field(objd::CommunitySort);
        e.buildModeType = d.field(objd::BuildModeType);
        e.buildModeSubsort = d.field(objd::BuildModeSubsort);
        e.catalogStrings = d.field(objd::CatalogStrings);
        e.objectType = d.field(objd::ObjectType);
        e.multiTile = multi;
        e.initialDepreciation = d.field(objd::InitialDepreciation);
        e.dailyDepreciation = d.field(objd::DailyDepreciation);
        e.depreciationLimit = d.field(objd::DepreciationLimit);
        e.selfDepreciating = d.field(objd::SelfDepreciating) != 0;
        for (size_t i = 0; i < e.ratings.size(); ++i)
            e.ratings[i] = int16_t(d.field(objd::RatingHunger + 2 * i));
        e.skillFlags = d.field(objd::RatingSkillFlags);
        e.placementFlags = d.field(objd::PlacementFlags);
        e.wallPlacementFlags = d.field(objd::WallPlacementFlags);
        e.wallAdjacentFlags = d.field(objd::WallAdjacentFlags);
        e.allowedHeightFlags = d.field(objd::AllowedHeightFlags);
        if (auto bytes = index.read(Tgi{kTypeCtss, tgi.group, e.catalogStrings, 0})) {
            try {
                const StringTable t = StringTable::parse(*bytes);
                e.name = t.get(0);
                e.description = t.get(1);
            } catch (const std::exception& err) {
                os2::logOnce("catalog strings", std::to_string(tgi.group), err);
            }
        }
        if (e.name.empty())
            e.name = d.title.empty() ? d.fileName : d.title;
        if (take[0] && take[1])
            out[0].entries_.push_back(e), out[1].entries_.push_back(std::move(e));
        else
            out[take[1] ? 1 : 0].entries_.push_back(std::move(e));
    }
    // By price, as CatalogSortPred (§1.1; the game's std::sort leaves equal
    // prices in no set order: by name here).
    for (Catalog& c : out) {
        std::stable_sort(c.entries_.begin(), c.entries_.end(), [](const CatalogEntry& a, const CatalogEntry& b) {
            if (a.price != b.price)
                return a.price < b.price;
            return a.name < b.name;
        });
        for (size_t i = 0; i < c.entries_.size(); ++i)
            c.byGuid_[c.entries_[i].guid] = i;
    }
    return out;
}

const CatalogEntry* Catalog::byGuid(uint32_t guid) const {
    auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? nullptr : &entries_[it->second];
}

const CatalogEntry* Catalog::find(const std::string& key) const {
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(key.c_str(), &end, 16);
        if (end && *end == 0)
            return byGuid(uint32_t(v));
    }
    const std::string k = lower(key);
    for (const auto& e : entries_)
        if (lower(e.fileName) == k)
            return &e;
    for (const auto& e : entries_)
        if (lower(e.name) == k)
            return &e;
    for (const auto& e : entries_)
        if (lower(e.fileName).find(k) != std::string::npos || lower(e.name).find(k) != std::string::npos)
            return &e;
    return nullptr;
}

std::vector<const CatalogEntry*> Catalog::byFunction(int bit, int subBit) const {
    std::vector<const CatalogEntry*> out;
    for (const auto& e : entries_)
        if (e.inFunction(bit) && (subBit < 0 || (e.functionSubSort & (1u << subBit))))
            out.push_back(&e);
    return out;
}

std::vector<const CatalogEntry*> Catalog::byRoom(int bit, int functionBit) const {
    std::vector<const CatalogEntry*> out;
    for (const auto& e : entries_)
        if (e.inRoom(bit) && (functionBit < 0 || e.inFunction(functionBit)))
            out.push_back(&e);
    return out;
}

void Catalog::setThumbnailPackage(const std::filesystem::path& path) {
    thumbs_.reset();
    thumbIndex_.clear();
    thumbCache_.clear();
    try {
        thumbs_ = std::make_unique<DbpfPackage>(path);
    } catch (const std::exception& err) {
        os2::logOnce("catalog thumbnails", path.string(), err);
        return;
    }
    const auto& entries = thumbs_->entries();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].tgi.type == kTypeThumbnail)
            thumbIndex_.emplace(entries[i].tgi.instance, uint32_t(i));
}

size_t Catalog::thumbnailCount() const {
    size_t n = 0;
    for (const auto& e : entries_)
        n += thumbIndex_.count(thumbnailKey(e.group, e.model));
    return n;
}

const RgbaImage* Catalog::thumbnail(const CatalogEntry& e) {
    if (auto it = thumbCache_.find(e.guid); it != thumbCache_.end())
        return it->second.get();
    std::shared_ptr<RgbaImage> img;
    if (auto it = thumbIndex_.find(thumbnailKey(e.group, e.model)); thumbs_ && it != thumbIndex_.end())
        if (auto d = decodeUiImage(thumbs_->read(thumbs_->entries()[it->second])))
            img = std::make_shared<RgbaImage>(std::move(*d));
    return thumbCache_.emplace(e.guid, std::move(img)).first->second.get();
}

} // namespace os2
