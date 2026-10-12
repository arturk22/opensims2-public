// Playing the neighborhood (formats/hood_moves.hpp) on a hand-built
// neighborhood: lot prices without a lot file, the move-in checks, moving in
// and out (the family's funds and lot in its FAMI), bulldozing.
#include "formats/dbpf.hpp"
#include "formats/hood_lots.hpp"
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

// The lot tools' geometry (formats/hood_lots.hpp).
static void lotTools() {
    const EmptyLotSize s = emptyLotSize(2); // button 0xD003, "3x4": 4 wide, 3 deep
    check(s.width == 4 && s.depth == 3, "empty lot size 2 is 4 x 3");
    const TileRect r0 = emptyLotRect(s, 10, 20, 0), r1 = emptyLotRect(s, 10, 20, 1);
    check(r0.x0 == 9 && r0.x1 == 14 && r0.y0 == 20 && r0.y1 == 23, "rotation 0: the road strip on x-min");
    check(r1.x0 == 10 && r1.x1 == 13 && r1.y0 == 20 && r1.y1 == 25, "rotation 1: turned, the road strip on y-max");
    LotInfo base;
    base.roads = 2, base.creationFrontEdge = 1; // a road on edge 1 when it was made
    check(roadsMatch(base, 1, {false, true, false, false}), "front = creation front: the road stays on edge 1");
    check(roadsMatch(base, 3, {false, false, false, true}), "turned twice: the road on edge 3");
    check(!roadsMatch(base, 3, {false, true, false, false}), "turned twice, the road on edge 1 does not match");
    LotInfo li;
    li.x = 5, li.y = 6, li.width = 3, li.depth = 5, li.creationFrontEdge = 1, li.frontEdge = 2;
    const TileRect lr = lotRect(li);
    check(lr.x1 - lr.x0 == 5 && lr.y1 - lr.y0 == 3, "a lot turned a quarter: its size swapped");
    // RotateLot: the drag's direction from the press picks the front edge.
    check(!frontFromDrag(100, 100, 110, 105), "drag: under 16 units (|dx| + |dy|) turns nothing");
    check(frontFromDrag(100, 100, 130, 100) == 2, "drag towards +x: front 2 (x-max)");
    check(frontFromDrag(100, 100, 100, 130) == 1, "drag towards +y: front 1 (y-max)");
    check(frontFromDrag(100, 100, 70, 100) == 0, "drag towards -x: front 0 (x-min)");
    check(frontFromDrag(100, 100, 100, 70) == 3, "drag towards -y: front 3 (y-min)");
    check(frontFromDrag(100, 100, 130, 120) == 2 && frontFromDrag(100, 100, 120, 130) == 1,
          "drag: quarters centred on the axes");
}

// Adding and removing decorations in the occupants resource (Occupants::addOccupant).
static void decorations() {
    auto record = [](Writer& w, bool flora, float x, float y, uint32_t guid, float rot) {
        w.u8(2).f32(x).f32(y).f32(5).f32(x - 3).f32(y - 3).f32(x + 3).f32(y + 3).u8(8);
        if (flora)
            w.f32(rot).u32(guid);
        else
            w.u32(guid).f32(rot);
    };
    Writer w;
    w.u32(111).u16(8).u32(1);
    record(w, true, 105, 105, 70, 90);
    w.u16(3).u32(0).u16(3).u32(0).u16(8).u32(1);
    record(w, false, 205, 205, 49, 0);
    const auto base = Occupants::parse(w.b);
    check(base.flora.size() == 1 && base.props.size() == 1, "occupants: one flora and one prop parse");
    OccupantPosition at;
    at.position = {315, 325, 12};
    at.boundsMin = {312, 322};
    at.boundsMax = {318, 328};
    const auto withFlora = Occupants::addOccupant(w.b, base, true, at, 66, 180);
    const auto parsed1 = Occupants::parse(withFlora);
    check(parsed1.flora.size() == 2 && parsed1.props.size() == 1, "occupants: a flora added to the flora list");
    check(parsed1.flora[1].guid == 66 && parsed1.flora[1].rotation == 180 && parsed1.flora[1].at.position[0] == 315,
          "occupants: the added flora's fields");
    const auto withProp = Occupants::addOccupant(withFlora, parsed1, false, at, 7, 90);
    const auto parsed2 = Occupants::parse(withProp);
    check(parsed2.flora.size() == 2 && parsed2.props.size() == 2 && parsed2.props[1].guid == 7 &&
              parsed2.props[1].rotation == 90 && parsed2.props[1].at.boundsMax[1] == 328,
          "occupants: a prop added to the prop list");
    const auto back = Occupants::removeOccupants(withProp, parsed2, {1}, {1});
    check(back == w.b, "occupants: removing what was added gives the original bytes");
}

// FlattenAbuttingRoadsIfPossible (docs/neighborhood-mac.md 15.2) on a hand-made neighborhood.
static void flattenRoads() {
    Terrain t;
    t.width = t.height = 20, t.gridWidth = t.gridHeight = 21;
    t.heights.assign(21 * 21, 0.0f);
    auto set = [&](int x, int y, float h) { t.heights[size_t(y) * 21 + size_t(x)] = h; };
    set(10, 7, 0.5f);
    Occupants o;
    auto road = [&](int x, int y) {
        Road r;
        r.at.position = {float(x) * 10 + 5, float(y) * 10 + 5, 0};
        r.at.boundsMin = {float(x) * 10, float(y) * 10};
        r.at.boundsMax = {float(x) * 10 + 10, float(y) * 10 + 10};
        r.corners = {{{float(x) * 10, float(y) * 10, 0.4f}, {float(x) * 10 + 10, float(y) * 10, 0.4f},
                      {float(x) * 10 + 10, float(y) * 10 + 10, 0.4f}, {float(x) * 10, float(y) * 10 + 10, 0.4f}}};
        o.roads.push_back(r);
    };
    road(4, 7);  // edge 0 strip: touches the front road strip
    road(10, 7); // edge 2 strip: free
    road(8, 4);  // edge 3 strip: touches the front strip, but shares vertices with a lot at 2.0
    Neighborhood hood;
    Lot other;
    other.id = 9;
    other.info.x = 8, other.info.y = 0, other.info.width = 4, other.info.depth = 4;
    hood.lots.push_back(other);
    for (int x = 8; x <= 12; ++x)
        set(x, 4, 2.0f);
    set(9, 4, 2.5f); // the lot's vertices under the road differ: no single height
    PlacementWorld world;
    world.hood = &hood, world.terrain = &t, world.occupants = &o;
    const TileRect R{5, 5, 10, 10};
    const RoadFlatten f = flattenAbuttingRoads(world, R, 0, 1.0f);
    auto at = [&](int x, int y) {
        auto it = f.heights.find({x, y});
        return it == f.heights.end() ? -100.0f : it->second;
    };
    check(at(4, 7) == 0.9375f && at(5, 8) == 0.9375f, "flatten: a strip on the front road takes its height, in 0.1875 steps");
    check(at(10, 7) == 0.5625f && at(11, 8) == 0.5625f, "flatten: a free strip takes its first road's corner");
    check(at(8, 4) == -100.0f, "flatten: a strip whose road meets lot vertices of two heights stays");
    check(f.roads.size() == 2 && f.roadHeights.count(0) && f.roadHeights.at(0)[0] == 0.9375f + 0.4f,
          "flatten: the levelled roads are laid 0.4 over the new terrain");
}

// IsOKToPlaceObjectAtCurrentLocation's errors (docs/neighborhood-mac.md 15.4).
static void decorationRules() {
    Terrain t;
    t.width = t.height = 10, t.gridWidth = t.gridHeight = 11, t.seaLevel = 5;
    t.heights.assign(11 * 11, 10.0f);
    for (int y = 0; y <= 11 - 1; ++y)
        for (int x = 7; x <= 10; ++x)
            t.heights[size_t(y) * 11 + size_t(x)] = 0; // a sea from x = 7
    Occupants o;
    Road r;
    r.at.boundsMin = {20, 50}, r.at.boundsMax = {30, 60};
    o.roads.push_back(r);
    Neighborhood hood;
    Lot lot;
    lot.id = 1, lot.info.x = 0, lot.info.y = 0, lot.info.width = 3, lot.info.depth = 3;
    hood.lots.push_back(lot);
    PlacementWorld world;
    world.hood = &hood, world.terrain = &t, world.occupants = &o;
    auto at = [](int tx, int ty) {
        OccupantPosition p;
        p.position = {float(tx) * 10 + 5, float(ty) * 10 + 5, 0};
        p.boundsMin = {float(tx) * 10 + 2, float(ty) * 10 + 2};
        p.boundsMax = {float(tx) * 10 + 8, float(ty) * 10 + 8};
        return p;
    };
    DecorProduct land;
    land.surface = 0, land.inLot = true;
    check(decorationError(world, land, at(4, 4)) == DecorOk, "decor: land product on land");
    check(decorationError(world, land, at(1, 1)) == DecorOk, "decor: allowed in a lot");
    check(decorationError(world, land, at(6, 4)) == DecorNeedsLand, "decor: a tile touching the sea is not all land");
    check(decorationError(world, land, at(2, 5)) == DecorOnRoad, "decor: on a road");
    land.removeOnLotPlop = true;
    check(decorationError(world, land, at(1, 1)) == DecorInLot, "decor: removed by a lot placement, so not in one");
    DecorProduct boat;
    boat.surface = 1;
    check(decorationError(world, boat, at(8, 4)) == DecorOk, "decor: water product on the sea");
    check(decorationError(world, boat, at(4, 4)) == DecorNeedsWater, "decor: water product on land");
}

int main() {
    flattenRoads();
    decorationRules();
    decorations();
    evictCleanup();
    lotTools();
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
    hood.add({0x43545353, G, 1, 0}, strings({"Test Town", "Old story"})); // the neighborhood's name and story
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

    // The story editor's save: string 1 of the CTSS, the name untouched.
    {
        check(Neighborhood::load(dir, false).description == "Old story", "story: read from the CTSS");
        const MoveResult d = setDescription(dir, "A new story\nof two lines", dir.parent_path() / "elsewhere");
        check(d.ok, "story: written");
        const Neighborhood h = Neighborhood::load(dir, false);
        check(h.description == "A new story\nof two lines" && h.name == "Test Town", "story: read back, the name kept");
        check(h.family(1) != nullptr && h.lot(11) != nullptr, "story: the rest of the package kept");
        check(!setDescription(dir, "x", dir.parent_path()).ok, "story: nothing is written under the forbidden folder");
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
