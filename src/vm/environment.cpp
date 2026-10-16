// The environment motive (environment.hpp; docs/motives-mac.md §8).

#include "environment.hpp"

#include "motives.hpp"
#include "formats/environment_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// The game's float arithmetic, operation by operation: no contraction.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace os2::vm {

namespace {

// The global STR# with the curve (InitTuningCurves, base 0xc83698: text 1,
// counting from 1, of "Environment Score Curves" in the global group).
constexpr uint16_t kEnvironmentCurveStrings = 0x2BC;

const MotiveCurve& environmentCurve(VM& vm) {
    // Read once per string table (the tests make tables of their own, and a
    // later table may take a freed one's address: the text is compared).
    struct Entry {
        std::string text;
        MotiveCurve curve;
    };
    static std::map<const StringTable*, Entry> cache;
    const StringTable* t = vm.code().strings(kGlobalGroup, kEnvironmentCurveStrings);
    const std::string text = t ? t->get(0, lang::EnglishUS) : std::string();
    auto [it, added] = cache.try_emplace(t);
    if (added || it->second.text != text) {
        it->second.text = text;
        it->second.curve = MotiveCurve::parse(text, 32);
    }
    return it->second.curve;
}

// The base's tent (0xc8338c): u = t + 1; 0 up to 0, u up to 1, 2 - u up to 2, else 0.
float tent(float t) {
    const float u = t + 1.0f;
    if (u <= 0.0f)
        return 0.0f;
    if (u <= 1.0f)
        return u;
    if (u <= 2.0f)
        return 2.0f - u;
    return 0.0f;
}

// cTSObject's IsOnLot of the position's tile (cWorldDB vtable 0xf8): the
// tile and the level within the lot (the VM numbers levels from 1).
bool onLot(const VM& vm, const Position& p) {
    if (!p.inWorld())
        return false;
    if (!vm.world)
        return true;
    const auto& w = *vm.world;
    return p.x < float(w.width()) && p.y < float(w.height());
}

// ShouldProcessObject(object, false, false) (base 0xc851bc): in the world,
// with a model, not a person (OBJD type 2) nor type 6, and of a multi-tile
// object only the lead. HasModel (cTSObject +0x1d2, set when the scene
// attaches the object's model) is taken as true for every object here
// (inferred: the base game's objects without a model are controllers, worth
// nothing to the score).
bool counts(const VM& vm, const Object& o) {
    if (o.person || !o.definition)
        return false;
    const uint16_t type = o.definition->objectType();
    if (type == kObjectTypePerson || type == 6)
        return false;
    const ObjectId lead = o.data[objdata::LeadTileObjectId];
    if (lead && lead != o.id)
        return false;
    return onLot(vm, o.position);
}

// GetPrice (base vtable 0xbc: the definition's field 0x12, a short); a
// multi-tile object's (cEdithMTObject::GetPrice, BV 0xaf04ec) is its
// group's master's, the catalog's (the parts' OBJDs have 0: the hot tub of
// N001_Lot18 gives its room the most the objects can).
int16_t objectPrice(VM& vm, const Object& o) {
    const ObjectDefinition* d = o.definition;
    if (const uint16_t master = d->masterId())
        if (auto instance = vm.code().masterDefinition(o.group, master))
            if (const ObjectDefinition* md = vm.code().definition(o.group, *instance))
                d = md;
    return int16_t(d->price());
}

// UpdateEnvScoreForRoom (base 0xc85364).
void updateScores(EnvironmentRoom& r, const MotiveCurve& curve) {
    using namespace envtuning;
    float f = 0.0f;
    if (0.0f < r.floorArea) {
        const float size = float(std::sqrt(double(r.floorArea)));
        const float most = curve.points.empty() ? 0.0f : curve.at(size);
        f = 0.0f < most ? r.objectNice / most : 0.0f;
        if (f < 0.0f)
            f = 0.0f;
        if (1.0f < f)
            f = 1.0f;
        f = f * ((1.0f - FloorWeight) - WallWeight) * 100.0f;
        if (f < 0.0f)
            f = 0.0f;
    }
    r.objectScore = f;
    f = 0.0f;
    if (0.0f < r.floorArea) {
        f = (r.floorNice / r.floorArea) / FloorNicenessMax;
        if (1.0f < f)
            f = 1.0f;
        f = f * FloorWeight * 100.0f;
        if (f < 0.0f)
            f = 0.0f;
    }
    r.floorScore = f;
    f = 0.0f;
    if (0.0f < r.wallCount) {
        f = (r.wallNice / r.wallCount) / WallNicenessMax;
        if (1.0f < f)
            f = 1.0f;
        f = f * WallWeight * 100.0f;
        if (f < 0.0f)
            f = 0.0f;
    }
    r.wallScore = f;
}

} // namespace

int32_t environmentRoomId(int16_t room, float x, float y) {
    if (room == -1)
        return -1;
    if (room != 0)
        return int32_t(room) + kEnvironmentIndoorBase;
    // The lot's origin in the world (cWorldDB 0xdc/0xe0: the lot's first
    // tile x the tile's size) is 0 on a lot. fctiwz truncates.
    const float cell = std::max(envtuning::CellSize, 1.0f);
    auto index = [&](float v) {
        const float q = v / cell;
        int i = std::isfinite(q) ? int(std::clamp(q, -2.0e9f, 2.0e9f)) : 0;
        if (i < 0)
            i = 0;
        else if (i >= 0x100)
            i = 0xFF;
        return i;
    };
    return index(x) + (index(y) << 8);
}

void setLotEnvironment(VM& vm, const Lot& lot) {
    auto& out = vm.environmentArchitecture;
    out.clear();
    vm.environmentKnown = true;
    static const PatternScores none;
    ResourceIndex* index = vm.code().index();
    const PatternScores& scores = index ? patternScores(*index) : none;

    // UpdateWallScores (base 0xc84840): every partition of wall graph 5 (the
    // placement graph: walls and fences, not deck skirts), both sides. Side
    // A is the one on the right of a -> b, the wall locator of the ends
    // (patterns[0], roomRight), its point the middle + 0.05 (dy, -dx); side
    // B the opposite locator's. A side whose covering is no catalog wall
    // product does not count.
    for (const auto& g : lot.wallGraphs) {
        if (g.instance != 5)
            continue;
        std::map<uint32_t, const Lot::WallGraph::Vertex*> vertices;
        for (const auto& v : g.vertices)
            vertices[v.id] = &v;
        std::map<uint32_t, const Lot::Wall*> walls;
        for (const auto& w : lot.walls)
            walls[w.id] = &w;
        for (const auto& line : g.lines) {
            auto ia = vertices.find(line.a), ib = vertices.find(line.b);
            if (ia == vertices.end() || ib == vertices.end())
                continue;
            const auto& a = *ia->second;
            const auto& b = *ib->second;
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float mx = 0.5f * (a.x + b.x), my = 0.5f * (a.y + b.y);
            const auto wit = walls.find(line.id);
            for (int side = 0; side < 2; ++side) {
                const int32_t room = side == 0 ? line.roomRight : line.roomLeft;
                const float px = float(double(side == 0 ? 0.05f : -0.05f) * double(dy) + double(mx));
                const float py = float(double(side == 0 ? -0.05f : 0.05f) * double(dx) + double(my));
                if (wit == walls.end())
                    continue;
                const uint16_t ref = wit->second->patterns[size_t(side)];
                auto name = lot.wallPatterns.find(ref);
                if (ref == 0xFFFF || name == lot.wallPatterns.end())
                    continue;
                const PatternScore* s = scores.wall(name->second);
                if (!s)
                    continue;
                const int32_t key = environmentRoomId(int16_t(room), px, py);
                if (key < 0)
                    continue; // the game's entry -1, never read
                auto& r = out[key];
                r.wallNice = float(double(s->niceness) * double(float(s->price)) + double(r.wallNice));
                r.wallCrap = r.wallCrap + s->crap;
                r.wallCount = r.wallCount + 1.0f;
            }
        }
    }

    // UpdateFloorScores (base 0xc84d8c): every room of the placement map
    // (3ARY 0x09), its tiles (the room manager's lists, BuildRoomTileListMap:
    // level by level, x outer, y inner; a tile is in the list of each room
    // one of its quarters has, with the mask of those quarters). The tile's
    // pattern is its first quarter's of the mask (inferred: cWorldDB::
    // FloorPattern of a masked tile); a pattern that is no catalog floor
    // product (no floor) gives nothing, but the area counts every tile.
    const uint32_t W = lot.width, H = lot.height;
    if (!lot.rooms.empty() && lot.rooms.size() >= size_t(lot.levelCount) * W * H) {
        struct Tile {
            int level;
            uint32_t x, y;
            uint8_t mask;
        };
        std::map<uint16_t, std::vector<Tile>> lists;
        for (int li = 0; li < lot.levelCount; ++li)
            for (uint32_t x = 0; x < W; ++x)
                for (uint32_t y = 0; y < H; ++y) {
                    const auto& q = lot.rooms[(size_t(li) * W + x) * H + y];
                    std::map<uint16_t, uint8_t> masks;
                    for (size_t k = 0; k < 4; ++k)
                        masks[q[k]] = uint8_t(masks[q[k]] | (1u << k));
                    for (const auto& [room, mask] : masks)
                        lists[room].push_back({li, x, y, mask});
                }
        for (const auto& [room, tiles] : lists) {
            if (room >= 0x8000)
                continue; // no room (-1): not one of the manager's
            for (const Tile& t : tiles) {
                const int32_t key = environmentRoomId(int16_t(room), float(t.x) + 0.5f, float(t.y) + 0.5f);
                if (key < 0)
                    continue;
                float nice = 0.0f, crap = 0.0f;
                if (!lot.floors.empty()) {
                    const auto& f = lot.floors[(size_t(t.level) * W + t.x) * H + t.y];
                    size_t k = 0;
                    while (k < 4 && !(t.mask & (1u << k)))
                        ++k;
                    if (k < 4 && f[k]) {
                        auto name = lot.floorPatterns.find(f[k]);
                        if (name != lot.floorPatterns.end())
                            if (const PatternScore* s = scores.floor(name->second)) {
                                nice = s->niceness * float(s->price);
                                crap = s->crap;
                            }
                    }
                }
                auto& r = out[key];
                r.floorNice = r.floorNice + nice;
                r.floorCrap = r.floorCrap + crap;
                r.floorArea = r.floorArea + 1.0f;
            }
        }
    }
}

EnvironmentRooms environmentRooms(VM& vm) {
    EnvironmentRooms rooms;
    for (const auto& [key, a] : vm.environmentArchitecture) {
        EnvironmentRoom& r = rooms[key];
        r.wallNice = a.wallNice, r.wallCrap = a.wallCrap, r.wallCount = a.wallCount;
        r.floorNice = a.floorNice, r.floorCrap = a.floorCrap, r.floorArea = a.floorArea;
    }
    // UpdateObjectScores (base 0xc8504c) and AddObjectsContributionToRoomInfo
    // (0xc83830): at the object's environment room (cTSObject +0x288, the
    // room and its place when the room was set).
    for (const auto& [id, o] : vm.eachObject()) {
        if (!counts(vm, o))
            continue;
        const int32_t key = environmentRoomId(o.data[objdata::Room], o.position.x, o.position.y);
        if (key < 0)
            continue; // the game's entry -1, never read
        EnvironmentRoom& r = rooms[key];
        float niceness = float(o.data[7]) / 100.0f; // "Niceness Score" (fdivs)
        float crap = float(o.data[86]);             // "Crapness Score"
        if (niceness < 0.0f)
            niceness = 0.0f; // the game logs "negative niceness multiplier"
        if (crap < 0.0f)
            crap = 0.0f;
        const float price = float(objectPrice(vm, o));
        r.objectNice = std::fmaf(price, niceness, r.objectNice);
        r.objectCrap = r.objectCrap + crap;
        r.objectCount = r.objectCount + 1.0f;
        const int16_t category = o.data[objdata::Category];
        if (category == 0x16)
            ++r.windows;
        if (category == 0x17)
            ++r.category17;
    }
    const MotiveCurve& curve = environmentCurve(vm);
    for (auto& [key, r] : rooms)
        updateScores(r, curve);
    return rooms;
}

float baseEnvironmentScore(const EnvironmentRooms& rooms, int32_t key) {
    if (key < 0)
        return 0.0f;
    auto it = rooms.find(key);
    if (it == rooms.end())
        return 0.0f;
    const EnvironmentRoom& r = it->second;
    const float nice = r.wallScore + r.objectScore + r.floorScore;
    float kept = 100.0f;
    if (nice <= 100.0f) {
        kept = nice;
        if (nice < -100.0f)
            kept = -100.0f;
    }
    return kept - (r.wallCrap + r.objectCrap + r.floorCrap);
}

float environmentLightTerm(float light) {
    using namespace envtuning;
    float t = 0.5f;
    if (LightBright != LightDark)
        t = (light - LightDark) / (LightBright - LightDark);
    float c = 0.0f;
    if (0.0f <= t) {
        c = t;
        if (1.0f < t)
            c = 1.0f;
    }
    return (c - 1.0f) * LightWeight * 100.0f;
}

float environmentScore(VM& vm, const Object& sim, const EnvironmentRooms& rooms) {
    // A person out of the world (at work, in no room) has room -1.
    const int16_t room = sim.position.inWorld() ? sim.data[objdata::Room] : int16_t(-1);
    const int32_t key = environmentRoomId(room, sim.position.x, sim.position.y);
    if (key < 0)
        return 0.0f;
    // AggregateIllumination at the Sim's place: the host's light maps (the
    // floor under the Sim); without them the light's term is left out.
    std::optional<float> light;
    if (vm.floorIllumination)
        light = vm.floorIllumination(sim.position.level, sim.position.x, sim.position.y);
    float score = 0.0f;
    if (key < kEnvironmentIndoorBase) {
        // Outdoors: the cells around, by a tent two cells wide each way,
        // not normalized (the weights add up to 4).
        const float cell = std::max(envtuning::CellSize, 1.0f);
        const float fx = sim.position.x / cell - 0.5f;
        const float fy = sim.position.y / cell - 0.5f;
        const int ix = int(std::floor(double(fx))), iy = int(std::floor(double(fy)));
        for (int yy = iy - 2; yy <= iy + 2; ++yy) {
            if (yy < 0 || yy >= 0x100)
                continue;
            const float dy = fy - float(yy);
            for (int xx = ix - 2; xx <= ix + 2; ++xx) {
                if (xx < 0 || xx >= 0x100)
                    continue;
                const float w = tent((fx - float(xx)) * 0.5f) * tent(dy * 0.5f);
                if (w != 0.0f)
                    score = std::fmaf(w, baseEnvironmentScore(rooms, xx + (yy << 8)), score);
            }
        }
    } else {
        score = baseEnvironmentScore(rooms, key);
    }
    const float total = score + (light ? environmentLightTerm(*light) : 0.0f);
    if (100.0f < total)
        return 100.0f;
    if (total < -100.0f)
        return -100.0f;
    return total;
}

float environmentScore(VM& vm, const Object& sim) { return environmentScore(vm, sim, environmentRooms(vm)); }

} // namespace os2::vm
