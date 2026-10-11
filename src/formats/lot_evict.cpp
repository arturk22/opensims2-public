// The evicted family's things leave the lot (lot_evict.hpp).

#include "lot_evict.hpp"

#include <algorithm>
#include <set>

namespace os2 {

namespace {
constexpr size_t kDataContainer = 2, kDataLeadTile = 19, kDataMovementFlags = 43, kDataCategory = 59;
constexpr int16_t kCategoryPerson = 0x67;
constexpr uint16_t kMovementStaysAfterEvict = 0x10;
constexpr uint16_t kObjdTypePerson = 2;
constexpr uint32_t kGuidPlumbBob = 0xF37E5AE0;
constexpr size_t kGlobalObjectsValueThousands = 48, kGlobalObjectsValueRest = 49;

int16_t dataAt(const LotObjects::State& s, size_t i) { return i < s.data.size() ? s.data[i] : 0; }
} // namespace

EvictCleanup cleanupEvictedFamily(LotObjects& saved, const EvictObjdLookup& objd, int lotType) {
    EvictCleanup out;
    std::set<int16_t> gone;
    for (const auto& s : saved.objects) {
        const uint32_t guid = saved.guidOf(s.id);
        const std::optional<EvictObjd> d = objd ? objd(guid) : std::nullopt;
        const bool person = s.person || dataAt(s, kDataCategory) == kCategoryPerson;
        if (dataAt(s, kDataContainer) != 0 && !person)
            continue;
        const bool global = d && (d->field2A & 1);
        const bool deletedByEvict =
            person || (d && (d->type == kObjdTypePerson ||
                             (d->buildModeType == 0 &&
                              !(uint16_t(dataAt(s, kDataMovementFlags)) & kMovementStaysAfterEvict))));
        const bool residential = lotType != 2 && lotType != 3;
        if (!(person || global || (deletedByEvict && residential)))
            continue;
        if (guid == kGuidPlumbBob)
            continue;
        // Only a multi-tile object's lead part decides.
        if (s.kind == LotObjects::Kind::MultiTile) {
            const int16_t lead = s.leadObject ? s.leadObject : dataAt(s, kDataLeadTile);
            if (lead && lead != s.id)
                continue;
        }
        gone.insert(s.id);
        s.person ? ++out.sims : ++out.objects;
    }
    // The lead parts take their other parts, and what is in their slots goes too.
    for (bool more = true; more;) {
        more = false;
        for (const auto& s : saved.objects) {
            if (gone.count(s.id))
                continue;
            const int16_t lead = s.kind == LotObjects::Kind::MultiTile
                                     ? (s.leadObject ? s.leadObject : dataAt(s, kDataLeadTile))
                                     : int16_t(0);
            const int16_t container = dataAt(s, kDataContainer);
            if ((lead && gone.count(lead)) || (container && gone.count(container))) {
                gone.insert(s.id);
                s.person ? ++out.sims : ++out.objects;
                more = true;
            }
        }
    }
    std::erase_if(saved.objects, [&](const LotObjects::State& s) { return gone.count(s.id) != 0; });
    std::erase_if(saved.pairs, [&](const auto& p) { return gone.count(int16_t(p.first)) != 0; });
    if (saved.simulator.read) {
        saved.simulator.fAC = 0;
        const uint32_t total = saved.simulator.fA8;
        if (saved.globals.size() > kGlobalObjectsValueRest) {
            saved.globals[kGlobalObjectsValueThousands] = int16_t(total / 1000);
            saved.globals[kGlobalObjectsValueRest] = int16_t(total % 1000);
        }
    }
    return out;
}

} // namespace os2
