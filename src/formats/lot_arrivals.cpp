// Sims of a newly moved-in family put on the lot (lot_arrivals.hpp).

#include "lot_arrivals.hpp"

#include "hood_moves.hpp"
#include "neighborhood.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace os2 {

namespace fs = std::filesystem;

namespace {
constexpr int16_t kPersonObjectType = 2;
constexpr size_t kGlobalHour = 0, kGlobalMinute = 5, kGlobalPaused = 17, kGlobalCurrentFamily = 9, kGlobalBudgetMod = 13,
                 kGlobalBudgetDiv = 14;
constexpr uint32_t kFamilyNewLot = 0x04; // FAMI flags: cTSFamily::SetNewLot
// cTSNeighborhoodConstants "new family start hour" (docs/neighborhood-mac.md §6.1).
constexpr int16_t kNewFamilyStartHour = 7;

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
} // namespace

LotArrivals addArrivingSims(LotObjects& saved, const Lot& world, const fs::path& lotFile, ResourceIndex* index) {
    (void)index;
    LotArrivals out;
    std::error_code ec;
    const fs::path lots = fs::absolute(lotFile).parent_path();
    if (lots.filename() != "Lots")
        return out;
    fs::path package;
    for (const auto& e : fs::directory_iterator(lots.parent_path(), ec))
        if (e.path().filename().string().ends_with("_Neighborhood.package"))
            package = e.path();
    if (package.empty())
        return out;
    hood::Neighborhood h;
    try {
        h = hood::Neighborhood::load(package, true);
    } catch (const std::exception&) {
        return out;
    }
    const hood::Lot* lot = nullptr;
    for (const auto& l : h.lots)
        if (!l.package.empty() && l.package.filename() == lotFile.filename())
            lot = &l;
    if (!lot)
        return out;
    // Saved Sims without a cPerson in the lot (saved by us after they
    // arrived): drawn from their character files, found by GUID.
    {
        std::set<uint32_t> bodied;
        for (const auto& o : world.objects)
            if (o.person)
                bodied.insert(o.instance & 0xFFFF);
        for (const auto& s : saved.objects) {
            if (!s.person || s.x < 0 || bodied.count(uint32_t(uint16_t(s.id))))
                continue;
            const hood::Neighbor* n = h.neighborByGuid(saved.guidOf(s.id));
            if (!n || n->character < 0)
                continue;
            Lot::Object o;
            o.instance = uint32_t(uint16_t(s.id));
            o.definition = h.characters[size_t(n->character)].path.stem().string();
            o.position = {s.x, s.y, world.elevationAt(std::max(0, s.level - 1), s.x, s.y)};
            o.person = true;
            out.sims.push_back(o);
            out.characters.push_back(h.characters[size_t(n->character)].path);
        }
    }
    const hood::Family* fam = h.family(hood::familyOnLot(h, lot->id));
    if (!fam)
        return out;

    std::set<uint32_t> present;
    int16_t nextId = 1;
    for (const auto& s : saved.objects) {
        nextId = std::max<int16_t>(nextId, int16_t(s.id + 1));
        if (s.person)
            present.insert(saved.guidOf(s.id));
    }
    for (const auto& o : world.objects)
        nextId = std::max<int16_t>(nextId, int16_t((o.instance & 0xFFFF) + 1));

    // By the mailbox (inferred), else the middle of the lot by its front.
    float x = float(world.width) / 2, y = 1.5f;
    for (const auto& o : world.objects)
        if (startsWith(o.definition, "Mailbox") || startsWith(o.definition, "mailbox")) {
            x = o.position[0], y = o.position[1] + 1.0f;
            break;
        }
    int k = 0;
    for (uint16_t member : fam->members) {
        const hood::Neighbor* n = h.neighbor(member);
        if (!n || !n->description.guid || present.count(n->description.guid) || n->character < 0)
            continue;
        const hood::Character& c = h.characters[size_t(n->character)];
        // A save type for the character's GUID (only for loading: the save
        // writes the objt record from the definition).
        int16_t type = 0;
        for (const auto& [st, t] : saved.saveTypes)
            if (t.guid == n->description.guid)
                type = st;
        if (!type) {
            for (const auto& [st, _] : saved.saveTypes)
                type = std::max<int16_t>(type, st);
            ++type;
            LotObjects::SaveType t;
            t.guid = t.guid2 = n->description.guid;
            t.objectType = kPersonObjectType;
            t.catalogName = "*" + n->firstName;
            saved.saveTypes[type] = t;
        }
        LotObjects::State s;
        s.id = nextId++;
        s.kind = LotObjects::Kind::Person;
        s.person = true;
        // Side by side along x, half a tile apart.
        s.x = std::clamp(x + 0.75f * float(k) - 0.75f * float(fam->members.size() - 1) / 2, 0.5f,
                         float(world.width) - 0.5f);
        s.y = std::clamp(y, 0.5f, float(world.height) - 0.5f);
        s.level = 1;
        s.data.assign(lotObjectDataCount(saved.version), 0);
        saved.saveTypeOf[s.id] = type;
        saved.pairs.emplace_back(uint32_t(uint16_t(s.id)), type);
        Lot::Object o;
        o.instance = uint32_t(uint16_t(s.id));
        o.definition = c.path.stem().string();
        o.position = {s.x, s.y, world.elevationAt(0, s.x, s.y)};
        o.person = true;
        out.sims.push_back(o);
        out.characters.push_back(c.path);
        saved.objects.push_back(std::move(s));
        ++k;
    }
    // cTSLoadLotController::LoadLot, step 0x1B (BV 0x8804ec): a family
    // moving in (the transition that brought it carries the move, which the
    // family's "new lot" flag stands for here: cTSFamilyManager::MoveIn
    // raised it and only the first day drops it) pays the lot's price, read
    // from the lot file as it is before this load, unless the lot is of type
    // 2; then global 9 = the family and its funds go to the simulator.
    const bool movingIn = (fam->record.flags & kFamilyNewLot) != 0;
    if (movingIn) {
        out.family = fam->id;
        if (lot->info.type != 2)
            out.charge = hood::lotFileInfo(*lot).price;
        const int64_t funds = int64_t(fam->record.funds) - out.charge;
        if (saved.globals.size() > kGlobalBudgetDiv) {
            saved.globals[kGlobalCurrentFamily] = int16_t(fam->id);
            saved.globals[kGlobalBudgetMod] = int16_t(funds % 10000);
            saved.globals[kGlobalBudgetDiv] = int16_t(funds / 10000);
        }
        if (saved.simulator.read)
            saved.simulator.f98 = uint32_t(int32_t(funds)), saved.simulator.f9C = 0;
    }
    // Step 0x1F: cTSNeighborhood::SetupFamilyFirstDay (BV 0x4db0c0) for that
    // family, or whenever the lot info has flag 4: cTSFamily::SetNewLot(0)
    // (the neighborhood save drops the flag, vm/hood_save.hpp),
    // cTSSimulator::SetCurrentTime(new family start hour, 0, -1, -1, -1, -1)
    // (BV 0x49af84: hour and minute; -1 keeps the day of the week, the day,
    // the month and the year), ClearHistory and Resume(true), which clears
    // global 17 "Paused" (the lots for sale keep 1).
    out.firstDay = movingIn || (lot->info.flags & hood::LotInfo::kFlagFirstDay) != 0;
    if (out.firstDay && saved.globals.size() > kGlobalPaused) {
        saved.globals[kGlobalHour] = kNewFamilyStartHour;
        saved.globals[kGlobalMinute] = 0;
        saved.globals[kGlobalPaused] = 0;
        // ClearHistory: the expense reports start over (inferred: the
        // simulator's report values are that history).
        for (auto& r : saved.simulator.reports)
            std::fill(r.values.begin(), r.values.end(), 0u);
    }
    return out;
}

} // namespace os2
