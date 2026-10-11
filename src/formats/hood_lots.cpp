// Placing, moving and binning lots (hood_lots.hpp).

#include "hood_lots.hpp"

#include "dbpf.hpp"
#include "hood_moves.hpp"
#include "property_set.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace os2::hood {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTypeBaseLotInfo = 0x6C589723, kTypeStr = 0x53545223;
// cTSUserToolPlaceNewLot::Init's size tables (BV 0x2733010 / 0x2732fc0),
// the first ten (the panel's buttons; 10-19 are expansion sizes).
constexpr int kSizeW[kEmptyLotSizes] = {2, 3, 4, 3, 4, 5, 3, 5, 4, 5};
constexpr int kSizeD[kEmptyLotSizes] = {3, 3, 3, 4, 4, 3, 5, 4, 5, 5};
const char* const kSkirt = "lottexture-test-01"; // every shipped lot's

bool under(const fs::path& path, const fs::path& dir) {
    if (dir.empty())
        return false;
    const std::string d = fs::weakly_canonical(fs::absolute(dir)).string();
    const std::string p = fs::weakly_canonical(fs::absolute(path)).string();
    return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
}

void checkFolder(const fs::path& folder, const fs::path& forbidden) {
    if (under(folder, forbidden))
        throw std::runtime_error("refusing to write into the game's data (" + folder.string() + ")");
}

using Key = std::pair<uint32_t, uint32_t>;
void rewritePackage(const fs::path& file, const std::map<Key, std::vector<uint8_t>>& replace, const std::set<Key>& drop,
                    uint32_t addGroup = 0xFFFFFFFF) {
    const fs::path tmp = file.parent_path() / (file.filename().string() + ".saving");
    {
        DbpfPackage pkg(file);
        DbpfWriter w;
        std::set<Key> done;
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == type::DIR)
                continue;
            const Key key{e.tgi.type, e.tgi.instance};
            if (drop.count(key))
                continue;
            if (auto it = replace.find(key); it != replace.end())
                w.add(e.tgi, it->second, e.compressed()), done.insert(key);
            else
                w.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
        }
        for (const auto& [key, bytes] : replace)
            if (!done.count(key))
                w.add(Tgi{key.first, addGroup, key.second, 0}, bytes, true);
        pkg.closeFile();
        w.save(tmp);
    }
    fs::rename(tmp, file);
}

struct RoadTile {
    int x = 0, y = 0;
};

std::vector<RoadTile> roadTiles(const Occupants& o) {
    std::vector<RoadTile> out;
    auto add = [&](const Road& r) {
        float x = INFINITY, y = INFINITY;
        for (const auto& c : r.corners)
            x = std::min(x, c[0]), y = std::min(y, c[1]);
        out.push_back({int(std::floor(x / kHoodCellSize + 0.01f)), int(std::floor(y / kHoodCellSize + 0.01f))});
    };
    for (const auto& r : o.roads)
        add(r);
    for (const auto& b : o.bridges)
        add(b.road);
    return out;
}

bool overlaps(const TileRect& a, const TileRect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

} // namespace

TileRect lotRect(const LotInfo& info) {
    return {int(info.x), int(info.y), int(info.x + info.currentWidth()), int(info.y + info.currentDepth())};
}

EmptyLotSize emptyLotSize(int index) {
    index = std::clamp(index, 0, kEmptyLotSizes - 1);
    return {kSizeW[index], kSizeD[index]};
}

TileRect emptyLotRect(EmptyLotSize size, int x, int y, int rotation) {
    const bool odd = rotation & 1;
    TileRect r{x, y, x + (odd ? size.depth : size.width), y + (odd ? size.width : size.depth)};
    switch (rotation & 3) {
    case 0: --r.x0; break;
    case 1: ++r.y1; break;
    case 2: ++r.x1; break;
    default: --r.y0; break;
    }
    return r;
}

Placement checkPlacement(const PlacementWorld& world, const TileRect& R, uint32_t self) {
    Placement out;
    const Terrain& t = *world.terrain;
    auto fail = [&](const char* why) {
        out.why = why;
        return out;
    };
    // 1. The corners on the terrain.
    if (R.x0 < 0 || R.y0 < 0 || R.x1 > int(t.width) || R.y1 > int(t.height) || R.width() < 1 || R.depth() < 1)
        return fail("off the terrain");
    const auto roads = roadTiles(*world.occupants);
    std::set<std::pair<int, int>> roadSet;
    for (const auto& r : roads)
        roadSet.insert({r.x, r.y});
    // 2. Other lots: none over the rectangle less a tile all round, and no
    // tile of another lot without a road on it.
    const TileRect inner{R.x0 + 1, R.y0 + 1, R.x1 - 1, R.y1 - 1};
    for (const auto& l : world.hood->lots) {
        if (l.id == self)
            continue;
        const TileRect o = lotRect(l.info);
        if (inner.width() > 0 && inner.depth() > 0 && overlaps(o, inner))
            return fail("another lot");
        for (int y = std::max(R.y0, o.y0); y < std::min(R.y1, o.y1); ++y)
            for (int x = std::max(R.x0, o.x0); x < std::min(R.x1, o.x1); ++x)
                if (!roadSet.count({x, y}))
                    return fail("another lot");
    }
    // 3. Flora and props in the way that may not be in a lot nor be removed.
    const float wx0 = float(R.x0) * kHoodCellSize, wy0 = float(R.y0) * kHoodCellSize;
    const float wx1 = float(R.x1) * kHoodCellSize, wy1 = float(R.y1) * kHoodCellSize;
    auto blocks = [&](const OccupantPosition& at, uint32_t guid) {
        if (at.boundsMax[0] <= wx0 || at.boundsMin[0] >= wx1 || at.boundsMax[1] <= wy0 || at.boundsMin[1] >= wy1)
            return false;
        auto it = world.objects.find(guid);
        const bool b = it == world.objects.end() || !(it->second.allowedInLot || it->second.removeOnLotPlop);
        return b;
    };
    for (const auto& f : world.occupants->flora)
        if (blocks(f.at, f.guid))
            return fail("objects in the way");
    for (const auto& p : world.occupants->props)
        if (blocks(p.at, p.guid))
            return fail("objects in the way");
    // 4. Water (not a beach lot).
    for (int y = R.y0; y <= R.y1; ++y)
        for (int x = R.x0; x <= R.x1; ++x)
            if (t.vertex(x, y) < t.seaLevel)
                return fail("water");
    // 5. A road there, and not too steep.
    bool anyRoad = false;
    for (const auto& r : roads)
        anyRoad = anyRoad || (r.x >= R.x0 && r.x < R.x1 && r.y >= R.y0 && r.y < R.y1);
    if (!anyRoad)
        return fail("no road");
    {
        float lo = INFINITY, hi = -INFINITY;
        int lx = 0, ly = 0, hx = 0, hy = 0;
        for (int y = R.y0; y <= R.y1; ++y)
            for (int x = R.x0; x <= R.x1; ++x) {
                const float h = t.vertex(x, y);
                if (h < lo)
                    lo = h, lx = x, ly = y;
                if (h > hi)
                    hi = h, hx = x, hy = y;
            }
        const int dx = hx - lx, dy = hy - ly;
        if ((dx || dy) && (hi - lo) / (std::sqrt(float(dx * dx + dy * dy)) * 10.0f) > world.maxSlope)
            return fail("too steep");
    }
    // 6. The roads in the rectangle: only along its edges; those with two
    // connections or more mark the edge tiles they cover.
    std::array<std::vector<bool>, 4> edge;
    edge[0].assign(size_t(R.depth()), false), edge[2].assign(size_t(R.depth()), false);
    edge[1].assign(size_t(R.width()), false), edge[3].assign(size_t(R.width()), false);
    std::vector<std::pair<int, int>> heights; // road tiles on each edge, for the height
    for (const auto& r : roads) {
        if (r.x < R.x0 || r.x >= R.x1 || r.y < R.y0 || r.y >= R.y1)
            continue;
        const bool ring = r.x + 1 >= R.x1 || r.x <= R.x0 || r.y + 1 >= R.y1 || r.y <= R.y0;
        if (!ring)
            return fail("a road through the lot");
        int connections = 0;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
            connections += roadSet.count({r.x + dx, r.y + dy}) ? 1 : 0;
        if (connections < 2)
            continue;
        if (r.x == R.x0)
            edge[0][size_t(r.y - R.y0)] = true;
        if (r.y + 1 == R.y1)
            edge[1][size_t(r.x - R.x0)] = true;
        if (r.x + 1 == R.x1)
            edge[2][size_t(R.y1 - r.y - 1)] = true;
        if (r.y == R.y0)
            edge[3][size_t(R.x1 - r.x - 1)] = true;
    }
    // FUN_00a27460: an edge fully lined with road is a road edge; on the
    // others only the end tiles may be road, next to a road edge; one road
    // edge at least.
    bool anyFull = false;
    std::array<size_t, 4> count{};
    for (int i = 0; i < 4; ++i) {
        count[size_t(i)] = size_t(std::count(edge[size_t(i)].begin(), edge[size_t(i)].end(), true));
        out.roads[size_t(i)] = count[size_t(i)] == edge[size_t(i)].size() && !edge[size_t(i)].empty();
        anyFull = anyFull || out.roads[size_t(i)];
        if (!out.roads[size_t(i)])
            for (size_t k = 1; k + 1 < edge[size_t(i)].size(); ++k)
                if (edge[size_t(i)][k])
                    return fail("roads beside the lot");
    }
    if (!anyFull)
        return fail("no road along an edge");
    for (int i = 0; i < 4; ++i) {
        const auto& e = edge[size_t(i)];
        if (out.roads[size_t(i)] || !count[size_t(i)])
            continue;
        if ((e.front() && !out.roads[size_t((i + 3) & 3)]) || (e.back() && !out.roads[size_t((i + 1) & 3)]))
            return fail("roads beside the lot");
    }
    for (int i = 0; i < 4; ++i)
        if (out.roads[size_t(i)]) {
            out.front = i;
            break;
        }
    // The front road's height (DetermineFlattenHeightForRoadOccs: here the
    // mean height of its road tiles' corners).
    float sum = 0;
    int n = 0;
    auto edgeTile = [&](const Road& r) {
        float x = INFINITY, y = INFINITY;
        for (const auto& c : r.corners)
            x = std::min(x, c[0]), y = std::min(y, c[1]);
        const int tx = int(std::floor(x / kHoodCellSize + 0.01f)), ty = int(std::floor(y / kHoodCellSize + 0.01f));
        switch (out.front) {
        case 0: return tx == R.x0 && ty >= R.y0 && ty < R.y1;
        case 1: return ty + 1 == R.y1 && tx >= R.x0 && tx < R.x1;
        case 2: return tx + 1 == R.x1 && ty >= R.y0 && ty < R.y1;
        default: return ty == R.y0 && tx >= R.x0 && tx < R.x1;
        }
    };
    for (const auto& r : world.occupants->roads)
        if (edgeTile(r))
            for (const auto& c : r.corners)
                sum += c[2], ++n;
    out.roadHeight = n ? sum / float(n) : t.heightAt((wx0 + wx1) / 2, (wy0 + wy1) / 2);
    out.ok = true;
    return out;
}

bool roadsMatch(const LotInfo& base, int front, const std::array<bool, 4>& place) {
    for (int i = 0; i < 4; ++i) {
        const bool baseRoad = (base.roads >> i) & 1;
        if (baseRoad != place[size_t((i + front - int(base.creationFrontEdge) + 8) & 3)])
            return false;
    }
    return true;
}

std::optional<std::pair<int, Placement>> findFront(const PlacementWorld& world, const LotInfo& base, int x, int y,
                                                    uint32_t self, int preferred) {
    std::vector<int> order;
    if (preferred >= 0)
        order.push_back(preferred & 3);
    for (int k = 0; k < 4; ++k)
        if (std::find(order.begin(), order.end(), (int(base.creationFrontEdge) + k) & 3) == order.end())
            order.push_back((int(base.creationFrontEdge) + k) & 3);
    for (int front : order) {
        LotInfo turned = base;
        turned.frontEdge = uint8_t(front), turned.x = uint32_t(std::max(0, x)), turned.y = uint32_t(std::max(0, y));
        if (x < 0 || y < 0)
            continue;
        const Placement p = checkPlacement(world, lotRect(turned), self);
        if (p.ok && roadsMatch(base, front, p.roads))
            return std::pair{front, p};
    }
    return std::nullopt;
}

std::map<uint32_t, OccupantFlags> loadOccupantFlags(ResourceIndex& index) {
    std::map<uint32_t, OccupantFlags> out;
    for (const auto& loc : index.ofType(hoodtype::ObjectEntry)) {
        const auto bytes = index.read(loc);
        const auto props = parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto g = props.find("guid");
        if (g == props.end())
            continue;
        OccupantFlags f;
        if (auto it = props.find("allowedinlot"); it != props.end())
            f.allowedInLot = it->second == "1" || it->second == "true";
        if (auto it = props.find("removeonlotplop"); it != props.end())
            f.removeOnLotPlop = it->second == "1" || it->second == "true";
        try {
            out[uint32_t(std::stoul(g->second))] = f;
        } catch (const std::exception&) {
        }
    }
    return out;
}

fs::path playerLotCatalog(const fs::path& hoodFolder, const fs::path& gameCatalog) {
    const fs::path abs = fs::absolute(hoodFolder);
    if (abs.parent_path().filename() != "Neighborhoods")
        return {};
    const fs::path dir = abs.parent_path().parent_path() / "LotCatalog";
    std::error_code ec;
    if (!fs::exists(dir)) {
        fs::create_directories(dir, ec);
        if (!gameCatalog.empty() && fs::is_directory(gameCatalog))
            for (const auto& e : fs::directory_iterator(gameCatalog, ec))
                if (e.path().extension() == ".package")
                    fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::skip_existing, ec);
    }
    return dir;
}

std::vector<BinLot> loadLotBin(const fs::path& catalogDir) {
    std::vector<BinLot> out;
    std::error_code ec;
    if (catalogDir.empty() || !fs::is_directory(catalogDir))
        return out;
    for (const auto& e : fs::directory_iterator(catalogDir, ec)) {
        const std::string name = e.path().filename().string();
        if (e.path().extension() != ".package" || name.rfind("cx_", 0) != 0)
            continue;
        try {
            DbpfPackage pkg(e.path());
            for (const auto& entry : pkg.entries())
                if (entry.tgi.type == kTypeBaseLotInfo) {
                    BinLot b;
                    b.file = e.path();
                    b.catalogId = uint32_t(std::stoul(name.substr(3, 8)));
                    b.base = LotInfo::parseBase(pkg.read(entry));
                    pkg.closeFile();
                    Lot l;
                    l.info = b.base;
                    l.package = b.file;
                    b.price = lotFileInfo(l).price;
                    out.push_back(std::move(b));
                    break;
                }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "lot bin: %s: %s\n", name.c_str(), ex.what());
        }
    }
    std::sort(out.begin(), out.end(), [](const BinLot& a, const BinLot& b) { return a.catalogId < b.catalogId; });
    return out;
}

uint32_t nextLotId(const Neighborhood& hood) {
    uint32_t id = 1;
    for (const auto& l : hood.lots)
        id = std::max(id, l.id + 1);
    const fs::path lots = hood.package.parent_path() / "Lots";
    while (fs::exists(lots / (hood.prefix + "_Lot" + std::to_string(id) + ".package")))
        ++id;
    return id;
}

namespace {

LotInfo newLotInfo(const LotInfo& base, uint32_t id, int x, int y, int front, float height) {
    LotInfo info = base;
    info.version = 13;
    if (!info.baseVersion)
        info.baseVersion = 6;
    info.x = uint32_t(x), info.y = uint32_t(y);
    info.heightOffset = height;
    info.lotId = int32_t(id);
    info.frontEdge = uint8_t(front & 3);
    info.skirtTexture = kSkirt;
    info.rest = {0};
    return info;
}

// FUN_00a284cc (cTSUserToolPlaceLotBase): the flora and props over the lot
// whose product says removeonlotplop go; the occupants resource rewritten.
std::pair<std::vector<uint8_t>, size_t> removeOccupantsOnLot(const fs::path& hoodPackage, const PlacementWorld& world,
                                                             const TileRect& R) {
    const float x0 = float(R.x0) * kHoodCellSize, y0 = float(R.y0) * kHoodCellSize;
    const float x1 = float(R.x1) * kHoodCellSize, y1 = float(R.y1) * kHoodCellSize;
    auto over = [&](const OccupantPosition& at, uint32_t guid) {
        auto it = world.objects.find(guid);
        return it != world.objects.end() && it->second.removeOnLotPlop && at.boundsMax[0] > x0 &&
               at.boundsMin[0] < x1 && at.boundsMax[1] > y0 && at.boundsMin[1] < y1;
    };
    std::vector<size_t> flora, props;
    for (size_t i = 0; i < world.occupants->flora.size(); ++i)
        if (over(world.occupants->flora[i].at, world.occupants->flora[i].guid))
            flora.push_back(i);
    for (size_t i = 0; i < world.occupants->props.size(); ++i)
        if (over(world.occupants->props[i].at, world.occupants->props[i].guid))
            props.push_back(i);
    if (flora.empty() && props.empty())
        return {{}, 0};
    DbpfPackage pkg(hoodPackage);
    for (const auto& e : pkg.entries())
        if (e.tgi.type == hoodtype::Occupants) {
            const auto bytes = pkg.read(e);
            const Occupants parsed = Occupants::parse(bytes);
            return {Occupants::removeOccupants(bytes, parsed, flora, props), flora.size() + props.size()};
        }
    return {{}, 0};
}

// The lot info and the occupants without what the lot removes.
void writeLot(const Neighborhood& hood, const PlacementWorld& world, const LotInfo& info, uint32_t id,
              std::vector<std::string>& lines, bool dropName) {
    std::map<Key, std::vector<uint8_t>> replace{{{hoodtype::LotInfo, id}, info.write()}};
    auto [occupants, removed] = removeOccupantsOnLot(hood.package, world, lotRect(info));
    if (removed) {
        replace[{hoodtype::Occupants, 0}] = std::move(occupants);
        lines.push_back(std::to_string(removed) + " trees and props removed from the lot");
    }
    std::set<Key> drop;
    if (dropName)
        drop.insert({kTypeStr, 0x8000 | id});
    rewritePackage(hood.package, replace, drop);
}

} // namespace

LotResult placeEmptyLot(const fs::path& folder, const PlacementWorld& world, EmptyLotSize size, int x, int y,
                        int rotation, const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        const TileRect R = emptyLotRect(size, x, y, rotation);
        const Placement p = checkPlacement(world, R);
        if (!p.ok)
            throw std::runtime_error("cannot place the lot here: " + p.why);
        const Neighborhood& hood = *world.hood;
        LotInfo base;
        base.baseVersion = 6;
        base.width = uint32_t(R.width()), base.depth = uint32_t(R.depth());
        base.type = 0; // residential (the name dialog's zone is not done)
        for (int i = 0; i < 4; ++i)
            if (p.roads[size_t(i)])
                base.roads |= uint8_t(1 << i);
        base.creationFrontEdge = uint8_t(p.front);
        base.flags = 0;
        base.heights.assign(size_t(R.width() + 1) * size_t(R.depth() + 1), 0.0f);
        out.lot = nextLotId(hood);
        const LotInfo info = newLotInfo(base, out.lot, R.x0, R.y0, p.front, p.roadHeight);
        // A name string left by an older lot of this ID goes: the lot info's name is the lot's.
        writeLot(hood, world, info, out.lot, out.lines, true);
        out.lines.push_back("empty lot " + std::to_string(out.lot) + " (" + std::to_string(R.width()) + "x" +
                            std::to_string(R.depth()) + " with the road) placed at (" + std::to_string(R.x0) + ", " +
                            std::to_string(R.y0) + "), front " + std::to_string(p.front));
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

LotResult placeBinLot(const fs::path& folder, const PlacementWorld& world, const BinLot& bin, int x, int y, int front,
                      const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        LotInfo turned = bin.base;
        turned.frontEdge = uint8_t(front & 3), turned.x = uint32_t(x), turned.y = uint32_t(y);
        const Placement p = checkPlacement(world, lotRect(turned));
        if (!p.ok)
            throw std::runtime_error("cannot place the lot here: " + p.why);
        if (!roadsMatch(bin.base, front, p.roads))
            throw std::runtime_error("cannot place the lot here: its roads are not on its road edges");
        const Neighborhood& hood = *world.hood;
        out.lot = nextLotId(hood);
        const fs::path file = folder / "Lots" / (hood.prefix + "_Lot" + std::to_string(out.lot) + ".package");
        fs::create_directories(file.parent_path());
        fs::copy_file(bin.file, file.string() + ".saving", fs::copy_options::overwrite_existing);
        fs::rename(file.string() + ".saving", file);
        const LotInfo info = newLotInfo(bin.base, out.lot, x, y, front, p.roadHeight);
        writeLot(hood, world, info, out.lot, out.lines, true);
        out.lines.push_back("lot " + std::to_string(out.lot) + " \"" + bin.base.name + "\" from the bin (" +
                            bin.file.filename().string() + ") placed at (" + std::to_string(x) + ", " +
                            std::to_string(y) + "), front " + std::to_string(front));
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

LotResult moveLot(const fs::path& folder, const PlacementWorld& world, uint32_t lotId, int x, int y, int front,
                  const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        const Lot* lot = world.hood->lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        LotInfo info = lot->info;
        info.frontEdge = uint8_t(front & 3), info.x = uint32_t(x), info.y = uint32_t(y);
        const Placement p = checkPlacement(world, lotRect(info), lotId);
        if (!p.ok)
            throw std::runtime_error("cannot move the lot here: " + p.why);
        if (!roadsMatch(lot->info, front, p.roads))
            throw std::runtime_error("cannot move the lot here: its roads are not on its road edges");
        info.heightOffset = p.roadHeight;
        writeLot(*world.hood, world, info, lotId, out.lines, false);
        out.lot = lotId;
        out.lines.push_back("lot " + std::to_string(lotId) + " moved to (" + std::to_string(x) + ", " +
                            std::to_string(y) + "), front " + std::to_string(front));
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

LotResult moveLotToBin(const fs::path& folder, uint32_t lotId, const fs::path& catalogDir, const fs::path& forbidden) {
    LotResult out;
    try {
        checkFolder(folder, forbidden);
        if (catalogDir.empty())
            throw std::runtime_error("no Lots & Houses bin for this neighborhood");
        checkFolder(catalogDir, forbidden);
        const Neighborhood hood = Neighborhood::load(folder, false);
        const Lot* lot = hood.lot(lotId);
        if (!lot)
            throw std::runtime_error("no lot " + std::to_string(lotId));
        if (familyOnLot(hood, lotId))
            throw std::runtime_error("moving an occupied lot with its family to the bin is not done");
        if (lot->package.empty() || !fs::exists(lot->package) || !lotFileInfo(*lot).hasHouse)
            throw std::runtime_error("empty lot");
        uint32_t id = 1;
        for (const auto& b : loadLotBin(catalogDir))
            id = std::max(id, b.catalogId + 1);
        char name[32];
        std::snprintf(name, sizeof name, "cx_%08u.package", id);
        const fs::path to = catalogDir / name;
        fs::create_directories(catalogDir);
        fs::copy_file(lot->package, to.string() + ".saving", fs::copy_options::overwrite_existing);
        // The base lot info the bin reads (SaveLot writes it into the lot file).
        std::vector<uint8_t> base(64, 0);
        const auto b = lot->info.writeBase(true);
        base.insert(base.end(), b.begin(), b.end());
        rewritePackage(to.string() + ".saving", {{{kTypeBaseLotInfo, 0}, base}}, {});
        fs::rename(to.string() + ".saving", to);
        rewritePackage(hood.package, {}, {{hoodtype::LotInfo, lotId}, {kTypeStr, 0x8000 | lotId}});
        fs::remove(lot->package);
        out.lot = lotId;
        out.lines.push_back("lot " + std::to_string(lotId) + " moved to the bin as " + to.filename().string());
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace os2::hood
