// Families moving in and out, bulldozing (hood_moves.hpp).

#include "hood_moves.hpp"

#include "dbpf.hpp"
#include "lot_objects.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace os2::hood {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTypeSimi = 0x53494D49, kTypeStr = 0x53545223;
constexpr size_t kGlobalInhibitMoveIn = 21, kGlobalLotHasHouse = 22;
constexpr size_t kGlobalCurrentFamily = 9, kGlobalBudgetMod = 13, kGlobalBudgetDiv = 14;
// GetLotFileInfo takes the architecture value (SIMI +0xB0, cTSSimulator::
// GetArchValue) at the catalog's refund rate (cTSCatalog::RefundRate, BV
// 0xb2a66c: the float 0.8).
constexpr float kRefundRate = 0.8f;

bool under(const fs::path& path, const fs::path& dir) {
    if (dir.empty())
        return false;
    const std::string d = fs::weakly_canonical(fs::absolute(dir)).string();
    const std::string p = fs::weakly_canonical(fs::absolute(path)).string();
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
}

// Rewrites a package: resources keyed (type, instance) replaced or deleted,
// the rest as stored; written to <file>.saving and renamed over it.
void rewritePackage(const fs::path& file, const std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>>& replace,
                    const std::set<std::pair<uint32_t, uint32_t>>& drop) {
    const fs::path tmp = file.parent_path() / (file.filename().string() + ".saving");
    {
        DbpfPackage pkg(file);
        DbpfWriter w;
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            const std::pair<uint32_t, uint32_t> key{e.tgi.type, e.tgi.instance};
            if (drop.count(key))
                continue;
            if (auto it = replace.find(key); it != replace.end())
                w.add(e.tgi, it->second, e.compressed());
            else
                w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
        pkg.closeFile();
        w.save(tmp);
    }
    fs::rename(tmp, file);
}

void checkFolder(const fs::path& folder, const fs::path& forbidden) {
    if (under(folder, forbidden))
        throw std::runtime_error("refusing to write into the game's data (" + folder.string() + ")");
}

void setFunds(std::vector<int16_t>& globals, int32_t funds) {
    if (globals.size() > kGlobalBudgetDiv) {
        globals[kGlobalBudgetMod] = int16_t(funds % 10000);
        globals[kGlobalBudgetDiv] = int16_t(funds / 10000);
    }
}

} // namespace

int64_t worldLotValue(int width, int depth) {
    // (int)(w * 6.5 * d / 100 + 1.5 * 2^23) - 0x4b400000, which the saved
    // lot values (SIMI +0xA4 of all 63 shipped lots) show rounds down: a
    // 30 x 50 lot is worth 8700, not 8800.
    const float v = float(width) * 6.5f * float(depth) / 100.0f;
    return int64_t(std::floor(v)) * 100 - 1000;
}

LotFileInfo lotFileInfo(const Lot& lot) {
    LotFileInfo out;
    if (!lot.package.empty() && fs::exists(lot.package)) {
        try {
            DbpfPackage pkg(lot.package);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == kTypeSimi && e.tgi.instance == 1) {
                    const LotObjects objects = LotObjects::load(pkg);
                    const auto& m = objects.simulator;
                    if (!m.read)
                        break;
                    out.fromFile = true;
                    out.lotValue = m.fA4, out.lotObjects = m.fA8, out.familyObjects = m.fAC, out.architecture = m.fB0;
                    const int32_t arch = int32_t(double(kRefundRate) * double(float(int32_t(m.fB0))));
                    out.price = int64_t(uint32_t(m.fA4 + m.fA8 + uint32_t(arch)));
                    // Lot flag 8 (evicted) would leave out the family objects;
                    // moveOut zeroes them in the file instead.
                    out.price += m.fAC;
                    const auto& g = objects.globals;
                    out.hasHouse = g.size() > kGlobalLotHasHouse && g[kGlobalLotHasHouse] != 0;
                    out.canMoveIn = !(g.size() > kGlobalInhibitMoveIn && g[kGlobalInhibitMoveIn] != 0);
                    return out;
                }
        } catch (const std::exception&) {
        }
    }
    // The lot's rectangle without its road (lot info vtable 0x34: neighborhood
    // tiles x 10 = lot tiles); the road runs along one edge, bits 1/4 across
    // the width, 2/8 across the depth (checked against the saved values).
    const int w = int(lot.info.width) - ((lot.info.roads & 5) ? 1 : 0);
    const int d = int(lot.info.depth) - ((lot.info.roads & 10) ? 1 : 0);
    out.price = worldLotValue(w * 10, d * 10);
    out.hasHouse = false;
    out.canMoveIn = true;
    return out;
}

uint32_t familyOnLot(const Neighborhood& hood, uint32_t lot) {
    if (lot == 0)
        return 0;
    for (const auto& f : hood.families)
        if (f.id != 0 && f.record.lot == lot)
            return f.id;
    return 0;
}

MoveInCheck canMoveIn(const Neighborhood& hood, uint32_t family, uint32_t lotId) {
    const Lot* lot = hood.lot(lotId);
    if (!lot)
        return MoveInCheck::NoLot;
    const Family* fam = hood.family(family);
    if (!fam || family == 0)
        return MoveInCheck::NoFamily;
    // Lot type (the base lot info's): 1 community; 3 (dorm, with flag 8),
    // 5 and 6 are expansion types.
    if (lot->info.type == 1)
        return MoveInCheck::Community;
    if (familyOnLot(hood, lotId) != 0)
        return MoveInCheck::Occupied;
    if (fam->members.empty() && fam->record.members.empty())
        return MoveInCheck::NoMembers;
    const LotFileInfo info = lotFileInfo(*lot);
    // Not for lots of type 2 (dorms, BV), nor when the lot inhibits move-in.
    if (lot->info.type != 2 && info.canMoveIn && int64_t(fam->record.funds) < info.price)
        return MoveInCheck::CantAfford;
    return MoveInCheck::Ok;
}

const char* moveInCheckName(MoveInCheck c) {
    switch (c) {
    case MoveInCheck::Ok: return "ok";
    case MoveInCheck::NoFamily: return "no such family";
    case MoveInCheck::NoLot: return "no such lot";
    case MoveInCheck::Community: return "community lot";
    case MoveInCheck::Occupied: return "occupied";
    case MoveInCheck::CantAfford: return "cannot afford";
    case MoveInCheck::NoMembers: return "no members";
    }
    return "?";
}

std::vector<uint32_t> homelessFamilies(const Neighborhood& hood) {
    std::vector<uint32_t> out;
    for (const auto& f : hood.families)
        if (f.id >= 1 && f.id < 0x7FD9 && f.record.lot < 1)
            out.push_back(f.id);
    return out;
}

int64_t netWorth(const Neighborhood& hood, uint32_t family) {
    const Family* f = hood.family(family);
    if (!f)
        return 0;
    int64_t worth = f->record.funds;
    if (const Lot* l = f->record.lot ? hood.lot(f->record.lot) : nullptr)
        worth += lotFileInfo(*l).price;
    return worth;
}

MoveResult moveIn(const fs::path& folder, uint32_t family, uint32_t lotId, const fs::path& forbidden) {
    MoveResult out;
    try {
        checkFolder(folder, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const MoveInCheck c = canMoveIn(hood, family, lotId);
        if (c != MoveInCheck::Ok)
            throw std::runtime_error(std::string("cannot move in: ") + moveInCheckName(c));
        const Family& fam = *hood.family(family);
        const Lot& lot = *hood.lot(lotId);
        out.price = lotFileInfo(lot).price;
        out.fundsBefore = out.fundsAfter = fam.record.funds;
        FamilyRecord r = fam.record;
        r.lot = lotId;
        r.flags |= 0x04; // cTSFamily::SetNewLot(1)
        rewritePackage(hood.package, {{{hoodtype::FAMI, family}, r.write()}}, {});
        out.lines.push_back("family " + std::to_string(family) + " \"" + fam.name + "\" moved into lot " +
                            std::to_string(lotId) + " (the price, " + std::to_string(out.price) +
                            ", is charged when the lot loads)");
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

MoveResult moveOut(const fs::path& folder, uint32_t lotId, const fs::path& forbidden) {
    MoveResult out;
    try {
        checkFolder(folder, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const Lot* lot = hood.lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        const uint32_t family = familyOnLot(hood, lotId);
        if (!family)
            throw std::runtime_error("nobody lives on lot " + std::to_string(lotId));
        const Family& fam = *hood.family(family);
        const LotFileInfo info = lotFileInfo(*lot);
        FamilyRecord r = fam.record;
        out.fundsBefore = r.funds;
        // Lot types 2 and 3 (dorms, BV) give nothing back.
        if (lot->info.type != 2 && lot->info.type != 3) {
            out.price = info.price;
            r.funds = int32_t(int64_t(r.funds) + info.price);
        }
        r.lot = 0;
        out.fundsAfter = r.funds;
        rewritePackage(hood.package, {{{hoodtype::FAMI, family}, r.write()}}, {});
        out.lines.push_back("family " + std::to_string(family) + " \"" + fam.name + "\" moved out of lot " +
                            std::to_string(lotId) + " with " + std::to_string(out.price) + ": funds " +
                            std::to_string(out.fundsBefore) + " -> " + std::to_string(out.fundsAfter));
        // CleanupThingsRelatedToEvictedFamily, done now: the family's Sims and
        // the objects deleted by eviction leave the lot file, and the family's
        // objects are worth nothing there any more.
        if (!lot->package.empty() && fs::exists(lot->package)) {
            const fs::path tmp = lot->package.parent_path() / (lot->package.filename().string() + ".saving");
            {
                DbpfPackage pkg(lot->package);
                LotObjects objects = LotObjects::load(pkg);
                std::set<uint32_t> members(fam.record.members.begin(), fam.record.members.end());
                for (uint16_t id : fam.members)
                    if (const Neighbor* n = hood.neighbor(id))
                        members.insert(n->description.guid);
                std::set<int16_t> gone;
                for (const auto& s : objects.objects) {
                    if (s.person) {
                        if (members.count(objects.guidOf(s.id)))
                            gone.insert(s.id), ++out.simsRemoved;
                    }
                }
                // Objects held or contained by the Sims that go, go with them.
                bool more = true;
                while (more) {
                    more = false;
                    for (const auto& s : objects.objects)
                        if (!gone.count(s.id) && s.data.size() > 2 && s.data[2] != 0 && gone.count(s.data[2])) {
                            gone.insert(s.id), ++out.objectsRemoved, more = true;
                        }
                }
                std::erase_if(objects.objects, [&](const LotObjects::State& s) { return gone.count(s.id) != 0; });
                if (objects.simulator.read) {
                    // The furniture stays with the house: what was the
                    // family's is the lot's now (the game recounts the
                    // objects' values when it next loads the lot).
                    objects.simulator.fA8 += objects.simulator.fAC;
                    objects.simulator.fAC = 0;
                    objects.simulator.f98 = 0;
                    objects.simulator.f9C = 0;
                    if (objects.globals.size() > kGlobalCurrentFamily)
                        objects.globals[kGlobalCurrentFamily] = 0;
                    setFunds(objects.globals, 0);
                }
                objects.save(pkg, tmp);
            }
            fs::rename(tmp, lot->package);
            out.lines.push_back("lot file " + lot->package.filename().string() + ": " +
                                std::to_string(out.simsRemoved) + " Sims and " + std::to_string(out.objectsRemoved) +
                                " things they held removed");
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

MoveResult bulldoze(const fs::path& folder, uint32_t lotId, const fs::path& forbidden) {
    MoveResult out;
    try {
        checkFolder(folder, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const Lot* lot = hood.lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        if (uint32_t f = familyOnLot(hood, lotId))
            throw std::runtime_error("the " + hood.family(f)->name + " family lives on lot " + std::to_string(lotId) +
                                     ": move it to the Family Bin first");
        rewritePackage(hood.package, {}, {{hoodtype::LotInfo, lotId}, {kTypeStr, 0x8000 | lotId}});
        out.lines.push_back("lot " + std::to_string(lotId) + " \"" + lot->name + "\" bulldozed");
        if (!lot->package.empty() && fs::exists(lot->package) && !under(lot->package, forbidden)) {
            fs::remove(lot->package);
            out.lines.push_back("removed " + lot->package.filename().string());
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace os2::hood
