// Playing the neighborhood (formats/hood_moves.hpp) on a hand-built
// neighborhood: lot prices without a lot file, the move-in checks, moving in
// and out (the family's funds and lot in its FAMI), bulldozing.
#include "formats/dbpf.hpp"
#include "formats/hood_moves.hpp"
#include "formats/lot_evict.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace os2;
using namespace os2::hood;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Writer {
    std::vector<uint8_t> b;
    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& u16(uint16_t v) { return u8(uint8_t(v)).u8(uint8_t(v >> 8)); }
    Writer& u32(uint32_t v) { return u16(uint16_t(v)).u16(uint16_t(v >> 16)); }
    Writer& f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u32(u); }
    Writer& str32(const std::string& s) { u32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); return *this; }
    Writer& cstr(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return u8(0); }
    Writer& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

static std::vector<uint8_t> makeSdsc(uint16_t id, uint32_t guid, int16_t family) {
    Writer w;
    w.u32(0x70).u32(0x20).u32(0x20);
    for (size_t i = 0; i < kSdscPersonDataCount; ++i)
        w.u16(uint16_t(i == sdsc::Age ? lifestage::Adult : i == sdsc::FamilyNumber ? family : 0));
    w.u16(id).u32(guid).u32(3).u32(0).u8(1);
    return w.b;
}

static std::vector<uint8_t> fami(uint32_t lot, int32_t funds, std::vector<uint32_t> members) {
    Writer w;
    w.u32(hoodtype::FAMI).u32(0x4E).u32(0).u32(lot).u32(1).u32(uint32_t(funds)).u32(2).u32(lot ? 1 : 0);
    w.u32(uint32_t(members.size()));
    for (auto g : members)
        w.u32(g);
    w.u32(0);
    return w.b;
}

// A lot info: w x d neighborhood tiles, type, road edges.
static std::vector<uint8_t> lotInfo(int32_t id, uint32_t w, uint32_t d, uint8_t type, uint8_t roads) {
    Writer o;
    o.u16(13).u16(6).u32(w).u32(d).u8(type).u8(roads).u8(0).u32(0).str32("Lot").str32("");
    o.u32(0); // no heights
    o.u32(10).u32(10).f32(320).u32(uint32_t(id)).u8(0).str32("lottexture-test-01").u8(1);
    return o.b;
}

static std::vector<uint8_t> strings(std::initializer_list<std::string> values) {
    Writer w;
    w.zeros(64).u16(0xFFFD).u16(uint16_t(values.size()));
    for (const auto& v : values)
        w.u8(1).cstr(v).cstr("");
    return w.b;
}

// The eviction cleanup (formats/lot_evict.hpp) on a hand-made object module.
static void evictCleanup() {
    LotObjects lot;
    auto add = [&](int16_t id, uint32_t guid, LotObjects::Kind kind, int16_t container = 0, int16_t movement = 0,
                   int16_t lead = 0) {
        LotObjects::State s;
        s.id = id;
        s.kind = kind;
        s.person = kind == LotObjects::Kind::Person;
        s.leadObject = lead;
        s.data.assign(80, 0);
        s.data[2] = container;
        s.data[43] = movement;
        lot.objects.push_back(s);
        LotObjects::SaveType t;
        t.guid = guid;
        lot.saveTypes[id] = t;
        lot.saveTypeOf[id] = id;
        lot.pairs.emplace_back(uint32_t(id), id);
    };
    add(1, 0x100, LotObjects::Kind::Person);                // a Sim
    add(2, 0x200, LotObjects::Kind::Object);                // a sofa (Buy mode)
    add(3, 0x300, LotObjects::Kind::Object);                // a door (Build mode)
    add(4, 0x200, LotObjects::Kind::Object, 0, 0x10);       // a sofa that stays after evict
    add(5, 0x400, LotObjects::Kind::Object, 2);             // a plate on the sofa
    add(6, 0xF37E5AE0, LotObjects::Kind::Object);           // the PlumbBob
    add(7, 0x500, LotObjects::Kind::Object);                // a TV channel (field 0x2A bit 0, Build type)
    add(8, 0x200, LotObjects::Kind::MultiTile, 0, 0, 8);    // a bed's lead part
    add(9, 0x200, LotObjects::Kind::MultiTile, 0, 0, 8);    // its other part
    lot.simulator.read = true;
    lot.simulator.fA8 = 1500, lot.simulator.fAC = 9000;
    lot.globals.assign(54, 0);
    const EvictCleanup c = cleanupEvictedFamily(lot, [](uint32_t guid) -> std::optional<EvictObjd> {
        if (guid == 0x300) return EvictObjd{8, 1, 0};
        if (guid == 0x500) return EvictObjd{0, 3, 1};
        return EvictObjd{4, 0, 0};
    });
    std::vector<int16_t> left;
    for (const auto& s : lot.objects)
        left.push_back(s.id);
    check(left == std::vector<int16_t>({3, 4, 6}), "evicted: the door, the sofa that stays and the PlumbBob are left");
    check(c.sims == 1 && c.objects == 5, "evicted: one Sim and five objects go");
    check(lot.simulator.fAC == 0 && lot.globals[48] == 1 && lot.globals[49] == 500, "evicted: no family objects value");
}

int main() {
    evictCleanup();
    // nTSWorld::LotValue, rounding down as the saved values show.
    check(worldLotValue(20, 30) == 2900, "3x3 lot (road off) is worth 2900");
    check(worldLotValue(20, 20) == 1600, "3x2 lot is worth 1600");
    check(worldLotValue(30, 30) == 4800, "4x3 lot is worth 4800 (58.5 rounds down)");
    check(worldLotValue(30, 50) == 8700, "30 x 50 is worth 8700");
    check(worldLotValue(50, 50) == 15200, "50 x 50 is worth 15200");

    // The lot info writer is the parser's inverse; the size as the lot is
    // turned (GetCurrentSize).
    {
        Writer o;
        o.u16(13).u16(6).u32(3).u32(5).u8(0).u8(2).u8(1).u32(3).str32("51 Road").str32("A lot");
        o.u32(24);
        for (int i = 0; i < 24; ++i)
            o.f32(i == 7 ? -0.375f : 0.0f);
        o.u32(52).u32(55).f32(357.19f).u32(22).u8(1).str32("lottexture-test-01").u8(1);
        const LotInfo li = LotInfo::parse(o.b);
        check(li.write() == o.b, "lot info: write(parse(x)) == x");
        check(!li.turned() && li.currentWidth() == 3 && li.currentDepth() == 5, "front 1 = creation front: not turned");
        LotInfo t = li;
        t.frontEdge = 2;
        check(t.turned() && t.currentWidth() == 5 && t.currentDepth() == 3, "one quarter from creation: turned");
    }

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "os2_test_hood_moves" / "N998";
    fs::remove_all(dir.parent_path());
    fs::create_directories(dir / "Lots");
    fs::create_directories(dir / "Characters");
    constexpr uint32_t G = 0xFFFFFFFF;
    DbpfWriter hood;
    hood.add({hoodtype::SDSC, G, 1, 0}, makeSdsc(1, 0x1111, 1));
    hood.add({hoodtype::SDSC, G, 2, 0}, makeSdsc(2, 0x2222, 2));
    hood.add({hoodtype::SDSC, G, 3, 0}, makeSdsc(3, 0x3333, 3));
    hood.add({hoodtype::FAMI, G, 1, 0}, fami(0, 20000, {0x1111})); // in the bin, rich enough
    hood.add({hoodtype::FAMI, G, 2, 0}, fami(0, 1000, {0x2222}));  // in the bin, poor
    hood.add({hoodtype::FAMI, G, 3, 0}, fami(12, 500, {0x3333}));  // living on lot 12
    hood.add({hoodtype::LotInfo, G, 11, 0}, lotInfo(11, 3, 3, 0, 4)); // 20 x 30 without the road: 2900
    hood.add({hoodtype::LotInfo, G, 12, 0}, lotInfo(12, 4, 3, 0, 1)); // 30 x 30: 4800
    hood.add({hoodtype::LotInfo, G, 13, 0}, lotInfo(13, 3, 3, 1, 4)); // community
    hood.add({0x53545223, G, 1, 0}, strings({"Rich", ""}));
    hood.add({0x53545223, G, 2, 0}, strings({"Poor", ""}));
    hood.add({0x53545223, G, 3, 0}, strings({"Home", ""}));
    hood.add({0x53545223, G, 0x800B, 0}, strings({"11 Test Lane", ""}));
    hood.save(dir / "N998_Neighborhood.package");

    {
        const Neighborhood h = Neighborhood::load(dir, false);
        check(lotFileInfo(*h.lot(11)).price == 2900, "price without a lot file: the world value off the road");
        check(homelessFamilies(h) == std::vector<uint32_t>({1, 2}), "the family bin: families 1 and 2");
        check(familyOnLot(h, 12) == 3, "family 3 lives on lot 12");
        check(canMoveIn(h, 1, 11) == MoveInCheck::Ok, "family 1 can move into lot 11");
        check(canMoveIn(h, 2, 11) == MoveInCheck::CantAfford, "family 2 cannot afford lot 11");
        check(canMoveIn(h, 1, 12) == MoveInCheck::Occupied, "lot 12 is occupied");
        check(canMoveIn(h, 1, 13) == MoveInCheck::Community, "lot 13 is a community lot");
        check(canMoveIn(h, 1, 99) == MoveInCheck::NoLot, "no lot 99");
        check(canMoveIn(h, 9, 11) == MoveInCheck::NoFamily, "no family 9");
        check(netWorth(h, 3) == 500 + 4800, "net worth: funds + the lot's price");
    }

    const MoveResult in = moveIn(dir, 1, 11);
    check(in.ok && in.price == 2900 && in.fundsAfter == 20000, "moving in charges nothing yet (the lot load does)");
    {
        const Neighborhood h = Neighborhood::load(dir, false);
        check(h.family(1)->record.lot == 11, "family 1 lives on lot 11");
        check((h.family(1)->record.flags & 4) != 0, "the new lot flag");
        check(h.family(1)->record.funds == 20000, "funds unchanged");
        check(homelessFamilies(h) == std::vector<uint32_t>({2}), "the bin has family 2 left");
    }
    check(!moveIn(dir, 2, 11).ok, "a second family cannot move into lot 11");
    check(!bulldoze(dir, 11).ok, "an occupied lot cannot be bulldozed");

    const MoveResult out = moveOut(dir, 11);
    check(out.ok && out.price == 2900 && out.fundsAfter == 22900, "moving out gives the price back");
    {
        const Neighborhood h = Neighborhood::load(dir, false);
        check(h.family(1)->record.lot == 0, "family 1 is back in the bin");
        check((h.lot(11)->info.flags & LotInfo::kFlagEvicted) != 0, "the lot is flagged evicted (8)");
    }
    check(!moveIn(dir, 1, 11, dir.parent_path()).ok, "nothing is written under the forbidden folder");

    check(bulldoze(dir, 11).ok, "an empty lot is bulldozed");
    {
        const Neighborhood h = Neighborhood::load(dir, false);
        check(h.lot(11) == nullptr, "lot 11 is gone");
        check(h.lot(12) != nullptr, "lot 12 stays");
    }
    fs::remove_all(dir.parent_path());

    if (failures)
        std::fprintf(stderr, "%d failures\n", failures);
    else
        std::printf("hood moves: ok\n");
    return failures ? 1 : 0;
}
