// Sims of a newly moved-in family put on the lot (lot_arrivals.hpp).

#include "lot_arrivals.hpp"

#include "hood_moves.hpp"
#include "lot_evict.hpp"
#include "resindex.hpp"
#include "simantics.hpp"
#include "neighborhood.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace os2 {

namespace fs = std::filesystem;

namespace {
constexpr int16_t kPersonObjectType = 2;
constexpr uint32_t kTypeObjd = 0x4F424A44;
constexpr size_t kGlobalHour = 0, kGlobalMinute = 5, kGlobalPaused = 17, kGlobalCurrentFamily = 9, kGlobalBudgetMod = 13,
                 kGlobalBudgetDiv = 14;
constexpr uint32_t kFamilyNewLot = 0x04; // FAMI flags: cTSFamily::SetNewLot
// cTSNeighborhoodConstants "new family start hour" (docs/neighborhood-mac.md §6.1).
constexpr int16_t kNewFamilyStartHour = 7;

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
} // namespace

LotArrivals addArrivingSims(LotObjects& saved, const Lot& world, const fs::path& lotFile, ResourceIndex* index,
                            bool placeSims) {
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
    // A lot whose family was evicted: its things go first
    // (CleanupThingsRelatedToEvictedFamily, formats/lot_evict.hpp).
    if ((lot->info.flags & hood::LotInfo::kFlagEvicted) && index) {
        std::map<uint32_t, EvictObjd> objds;
        for (const auto& loc : index->ofType(kTypeObjd)) {
            try {
                const ObjectDefinition d = ObjectDefinition::parse(index->read(loc));
                objds.emplace(d.guid(), EvictObjd{d.field(0x52), d.field(0xCA), d.field(0x94)});
            } catch (const std::exception&) {
            }
        }
        const EvictCleanup c = cleanupEvictedFamily(
            saved,
            [&](uint32_t guid) -> std::optional<EvictObjd> {
                auto it = objds.find(guid);
                return it == objds.end() ? std::nullopt : std::optional(it->second);
            },
            lot->info.type);
        out.evictedSims = c.sims, out.evictedObjects = c.objects;
    }
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
    const uint32_t famId = hood::familyOnLot(h, lot->id); // 0: nobody (family 0 is not one)
    const hood::Family* fam = famId ? h.family(famId) : nullptr;
    if (!fam) {
        // LoadLot sets global 9 to the lot's family, none here (a lot whose
        // family was evicted still names it, with its funds).
        if (saved.globals.size() > kGlobalBudgetDiv && saved.globals[kGlobalCurrentFamily] != 0) {
            saved.globals[kGlobalCurrentFamily] = 0;
            saved.globals[kGlobalBudgetMod] = saved.globals[kGlobalBudgetDiv] = 0;
            if (saved.simulator.read)
                saved.simulator.f98 = saved.simulator.f9C = 0;
        }
        return out;
    }

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
    // "Sub - Generate Family" (docs/neighborhood-mac.md §11.3): next to the
    // mailbox, else the outside trash can.
    bool found = false;
    for (const char* name : {"Mailbox", "mailbox", "Trash Can - Outside", "TrashCan - Outside"}) {
        for (const auto& o : world.objects)
            if (startsWith(o.definition, name)) {
                x = o.position[0], y = o.position[1] + 1.0f;
                found = true;
                break;
            }
        if (found)
            break;
    }
    int k = 0;
    for (uint16_t member : fam->members) {
        const hood::Neighbor* n = h.neighbor(member);
        if (!n || !n->description.guid || present.count(n->description.guid) || n->character < 0)
            continue;
        const hood::Character& c = h.characters[size_t(n->character)];
        out.arriving.push_back(c.path);
        if (!placeSims)
            continue;
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
        // As a new object starts (cTSObject's constructor; vm::VM::
        // createObject): object data 11 "object id" and 19 "lead tile object
        // ID" are its own ID, 3 "slot number" -1. With 19 at 0, "CT - Start
        // Routing Look At" made object 0 the stack object of "Look At -
        // Allowed Object?" when it met an arriving Sim, and the Main of the
        // Sim that walked past died ("stack object 0 does not exist").
        constexpr size_t kSlotNumber = 3, kObjectId = 11, kLeadTileObjectId = 19;
        if (s.data.size() > kLeadTileObjectId) {
            s.data[kSlotNumber] = -1;
            s.data[kObjectId] = s.id;
            s.data[kLeadTileObjectId] = s.id;
        }
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
