// The lot as viewed: the floor and level shown, the mouse ray on the lot
// (Go Here, picking), the walls cut down, the lot model rebuilt for the
// view or after Build mode, and the fences (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// Page Up / Page Down and the puck's floor buttons ("Subir um Andar (Page
// Up)", "Descer um Andar (Page Down)"): in Buy mode the floor objects go on
// (the camera follows it), else the floor the camera shows.
void changeFloor(int delta) {
    if (gBuy && gBuy->active()) {
        if (gBuy->changeLevel(delta) && gCameraFollow)
            gCameraFollow->setLevel(gBuy->level() - 1);
    } else if (gBuild && gBuild->active() && gCameraFollow) {
        if (gBuild->level() - 1 + delta >= std::min(0, gBuild->level() - 1) && gCameraFollow->setLevel(gBuild->level() - 1 + delta))
            gBuild->setLevel(gCameraFollow->level() + 1);
    } else if (gCameraFollow) {
        // A level shown together with the one below (a foundation's top,
        // WallView::shownLevel) is stepped over.
        // The pool's level (-1) is no floor of the lot: the buttons stop at
        // the ground (base game: no basements; LotCameraFollow::follow keeps
        // a swimming Sim's view there too). Inferred, live QA round 20.
        const int now = gCameraFollow->level(), shown = gWallView.shownLevel(now);
        const int lowest = std::max(gCameraFollow->lowestLevel(), std::min(0, now));
        int to = delta > 0 ? std::min(std::max(now + delta, shown + 1), std::max(gWallView.topLevel(), now)) : now + delta;
        while (delta < 0 && to > lowest && gWallView.shownLevel(to) == shown)
            --to;
        gCameraFollow->setLevel(std::max(to, std::min(lowest, now)));
    }
}

// The camera shows the selected Sim's level (cCameraLive::Update).
void followSelectedSim() {
    if (gCameraSelector.freeCamera()) // the free camera is another controller: no floor following
        return;
    if (gCameraFollow && gSimulation && gPie)
        gCameraFollow->follow(gSimulation->machine(), gPie->sim());
}

// Fence rails and posts (os2::lotFencePieces): one model per CRES, an
// instance per piece; called again after Build mode changed the fences
// (instances are reused, extra ones hidden). Returns the pieces placed.
// Pieces above `level` (the lot's numbering) are left out (see WallView::visible).
size_t placeFencePieces(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                        const os2::PatternCatalog& patterns, std::vector<std::string>* warnings,
                        int level) {
    auto& models = gFenceModels;
    auto& instances = gFenceInstances;
    const auto pieces = os2::lotFencePieces(lot, patterns, warnings);
    size_t used = 0;
    for (const auto& piece : pieces) {
        // (Not the next-story rule of a level shown with the one below:
        // railings stand on decks over any ground, buildLotModel keeps them.)
        if (!os2::WallView::visible(lot, level, piece.transform[12], piece.transform[13], piece.transform[14]))
            continue;
        auto it = models.find(piece.model);
        if (it == models.end()) {
            int id = -1;
            try {
                id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
            } catch (const std::exception& e) {
                if (warnings)
                    warnings->push_back(piece.model + ": " + e.what());
                else // (a rebuild after Build mode: nobody prints the warnings)
                    os2::logOnce("fence piece", piece.model, e);
            }
            it = models.emplace(piece.model, id).first;
        }
        if (it->second < 0)
            continue;
        simd_float4x4 at;
        std::memcpy(&at, piece.transform.data(), sizeof at);
        if (used < instances.size()) {
            // An instance this placed holds a fence's model; anything else is
            // another thing's instance (the ids of an earlier scene's renderer).
            const int held = renderer.instanceModel(instances[used]);
            if (std::none_of(models.begin(), models.end(), [&](const auto& m) { return m.second == held; }))
                std::fprintf(stderr, "opensims2: fence piece instance %d holds model %d, not a fence's\n", instances[used], held);
            renderer.setInstanceModel(instances[used], it->second), renderer.setInstanceTransform(instances[used], at);
        }
        else
            instances.push_back(renderer.addInstance(it->second, at));
        ++used;
    }
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[3] = simd_make_float4(0, 0, -1000, 1);
    for (size_t i = used; i < instances.size(); ++i)
        renderer.setInstanceTransform(instances[i], away);
    return used;
}

// The level the camera shows, whose walls follow the wall mode: Build and
// Buy mode's level, else the camera's (the selected Sim's or the one Page
// Up/Down chose); every level outside the game (a bare --lot view).
int viewLevel() {
    if (!gLevelsHidden)
        return os2::LotViewOptions::kAllLevels;
    if (gBuild && gBuild->active())
        return gWallView.shownLevel(gBuild->level() - 1);
    if (gBuy && gBuy->active())
        return gWallView.shownLevel(gBuy->level() - 1);
    return gCameraFollow ? gWallView.shownLevel(gCameraFollow->level()) : os2::LotViewOptions::kAllLevels;
}

// Whether the floor of `lotLevel` at `at` is drawn when the camera shows
// `top`: not, in a level shown with the one below, the parts a story over
// the ground (os2::storyOverGround), which the next floor up shows.
bool drawnFloor(int top, int lotLevel, simd_float3 at) {
    return lotLevel != top || gWallView.lowestCut(top) >= top || !os2::storyOverGround(*gLotShape, lotLevel, int(std::floor(at.x)), int(std::floor(at.y)));
}

// The ground or floor under the mouse ray, as Go Here takes it
// (cTSUserToolStandardAction::GroundOrWallButNotPondAtGameWindowPoint): the
// first level, from the one the camera shows down, whose floor the ray
// meets where that level has something to stand on (or a pool); the point
// and its VM level.
std::optional<std::pair<simd_float2, int>> groundUnderRay(simd_float3 eye, simd_float3 dir) {
    if (!gSimulation || !gLotShape || std::fabs(dir.z) < 1e-5f || !gSimulation->machine().world)
        return std::nullopt;
    const os2::vm::World& w = *gSimulation->machine().world;
    int top = viewLevel();
    if (top == os2::LotViewOptions::kAllLevels)
        top = gLotShape->minLevel + gLotShape->levelCount - 1;
    for (int lotLevel = top; lotLevel >= gLotShape->minLevel; --lotLevel) {
        const int level = lotLevel + 1;
        float z = gSimulation->floorHeight(level, float(w.width()) / 2, float(w.height()) / 2);
        simd_float3 hit{};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            const float t = (z - eye.z) / dir.z;
            ok = t > 0;
            hit = eye + dir * t;
            z = gSimulation->floorHeight(level, hit.x, hit.y);
        }
        const os2::vm::TilePos tile{int(std::floor(hit.x)), int(std::floor(hit.y)), level};
        if (ok && w.contains(tile) && (w.walkable(tile) || w.pool(tile)) && drawnFloor(top, lotLevel, hit))
            return std::make_pair(simd_make_float2(hit.x, hit.y), level);
    }
    return std::nullopt;
}

// Where the mouse ray first meets a floor of the levels shown (the ray's
// parameter), from the one the camera shows down: a level's floor where
// the tile has one (the ground's terrain everywhere; not the openings over
// stairs). ObjectAtGameWindowPoint stops there: nothing behind it is picked.
std::optional<float> floorUnderRay(simd_float3 eye, simd_float3 dir) {
    if (!gSimulation || !gLotShape || std::fabs(dir.z) < 1e-5f || !gSimulation->machine().world)
        return std::nullopt;
    const os2::vm::World& w = *gSimulation->machine().world;
    int top = viewLevel();
    if (top == os2::LotViewOptions::kAllLevels)
        top = gLotShape->minLevel + gLotShape->levelCount - 1;
    for (int lotLevel = top; lotLevel >= gLotShape->minLevel; --lotLevel) {
        const int level = lotLevel + 1;
        float z = gSimulation->floorHeight(level, float(w.width()) / 2, float(w.height()) / 2);
        simd_float3 hit{};
        float t = 0;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            t = (z - eye.z) / dir.z;
            ok = t > 0;
            hit = eye + dir * t;
            z = gSimulation->floorHeight(level, hit.x, hit.y);
        }
        const os2::vm::TilePos tile{int(std::floor(hit.x)), int(std::floor(hit.y)), level};
        if (ok && w.contains(tile) && w.hasFloor(tile) && drawnFloor(top, lotLevel, hit))
            return t;
    }
    return std::nullopt;
}

// A click on the ground: the selected Sim's pie menu on a go-here
// destination there (vm/go_here.hpp). False when nothing opened.
bool openGoHere(simd_float3 eye, simd_float3 dir, simd_float2 at) {
    if (!gPie || !gPie->sim())
        return false;
    const auto g = groundUnderRay(eye, dir);
    if (!g)
        return false;
    os2::vm::VM& m = gPie->vm();
    const os2::vm::ObjectId dest = os2::vm::createGoHereDestination(m, gPie->sim(), g->first.x, g->first.y, g->second);
    if (!dest)
        return false;
    if (!gPie->open(dest, at)) {
        os2::vm::releaseDestination(m, dest);
        return false;
    }
    if (const os2::vm::Object* d = m.object(dest))
        std::printf("opensims2: go here: destination %d (0x%08X%s) at (%.2f, %.2f, %d)\n", dest,
                    d->definition ? d->definition->guid() : 0u, d->definition && d->definition->fileName.find("Swim") != std::string::npos ? ", swim here" : "", d->position.x, d->position.y,
                    d->position.level);
    return true;
}

// A click that picked object `id` (cTSUserToolStandardAction::MouseEvent):
// its menu; a rug's merges a go-here destination at the floor under the
// mouse (vm::objectClick). Any other object without interactions only
// shows its tooltip, as the binary does (no Go Here under a tree's canopy).
void openClicked(os2::vm::ObjectId id, simd_float3 eye, simd_float3 dir, simd_float2 at) {
    if (!gPie || !gPie->sim())
        return;
    os2::vm::VM& m = gPie->vm();
    if (os2::vm::isGoHereRug(m, id)) {
        std::optional<os2::vm::Position> ground;
        if (const auto g = groundUnderRay(eye, dir))
            ground = os2::vm::Position{g->first.x, g->first.y, g->second};
        bool has = false;
        for (const auto& e : os2::vm::pieMenu(m, gPie->sim(), id))
            has = has || (e.available() && !e.debug);
        const auto menu = os2::vm::objectClick(m, gPie->sim(), id, ground, has);
        if (menu.object && gPie->open(menu.object, at, menu.destination)) {
            std::printf("opensims2: rug %d%s\n", id, menu.destination ? " with a go-here destination" : "");
            return;
        }
        for (const os2::vm::ObjectId d : {menu.object, menu.destination})
            if (d && d != id)
                os2::vm::releaseDestination(m, d);
        return;
    }
    if (!gPie->open(id, at))
        std::printf("opensims2: object %d: no interactions available (its tooltip)\n", id);
}

// Whether an object hangs on a wall the lot model cut down: a cut wall of
// its level within 0.6 tiles, across its front (perpendicular to its +Y).
bool onCutWall(const os2::Xform& at) {
    if (gLotCache.cut.empty() || !gLotShape)
        return false;
    const os2::Vec3 f = at.rotate({0, 1, 0});
    for (const auto& c : gLotCache.cut) {
        const int level = int(c[4]);
        const float floorZ = gLotShape->elevationAt(level, std::clamp(at.t[0], 0.0f, float(gLotShape->width)),
                                                    std::clamp(at.t[1], 0.0f, float(gLotShape->height)));
        if (at.t[2] < floorZ - 0.2f || at.t[2] > floorZ + 2.5f)
            continue;
        float dx = c[2] - c[0], dy = c[3] - c[1];
        const float len = std::hypot(dx, dy);
        if (len < 1e-4f)
            continue;
        dx /= len, dy /= len;
        if (std::abs(dx * f[0] + dy * f[1]) > 0.3f)
            continue;
        const float s = std::clamp((at.t[0] - c[0]) * dx + (at.t[1] - c[1]) * dy, 0.0f, len);
        if (std::hypot(c[0] + dx * s - at.t[0], c[1] + dy * s - at.t[1]) <= 0.6f)
            return true;
    }
    return false;
}

// The selected Sim's point for the cutaway (cWallCutawayManager,
// docs/ui-live-mac.md 8d). SetSelectedSimLocation keeps where the Sim is
// while it is on the level shown; "changed" (CutawayLocationChanged) is
// another room indoors, another tile outdoors (by the room where it was).
// The cutaway is worked out again only when the Sim, the camera or the view
// changed, with the Sim where it is then: between those times the point
// stays. None unless the kept Sim is on the level shown.
std::optional<std::array<float, 3>> cutawaySimPoint(const os2::LotViewOptions& v) {
    auto& c = gScene.cutawaySim;
    const os2::vm::VM& m = gSimulation->machine();
    const int16_t id = m.globals[os2::vm::global::SelectedPersonId];
    bool changed = false;
    if (const auto it = m.objects().find(os2::vm::ObjectId(id)); id > 0 && it != m.objects().end() &&
                                                                  it->second.position.inWorld() &&
                                                                  it->second.position.level - 1 == v.level) {
        const auto& p = it->second.position;
        if (!c.stored) {
            changed = true;
        } else {
            const uint16_t was = os2::lotRoomAt(*gLotShape, c.level, c.x, c.y);
            changed = was ? was != os2::lotRoomAt(*gLotShape, v.level, p.x, p.y)
                          : (std::floor(c.x) != std::floor(p.x) || std::floor(c.y) != std::floor(p.y) ||
                             c.level != v.level);
        }
        c.stored = true, c.level = v.level, c.x = p.x, c.y = p.y;
    }
    const simd_float3 e = gCamera->eye(), f = gCamera->pivot();
    const std::array<float, 6> camera{e.x, e.y, e.z, f.x, f.y, f.z};
    if (changed || camera != c.camera || c.mode != gWallView.mode() || c.viewLevel != v.level || !c.used) {
        c.camera = camera, c.mode = gWallView.mode(), c.viewLevel = v.level;
        if (c.stored)
            c.used = std::array<float, 3>{c.x, c.y, gLotShape->elevationAt(c.level, c.x, c.y)};
    }
    if (!c.stored || c.level != v.level)
        return std::nullopt;
    return c.used;
}

os2::LotViewOptions wantedLotView() {
    if (cinematicPlaying())
        return cinematicLotView();
    std::array<float, 2> look{gLotView.viewDirection};
    std::optional<std::array<float, 3>> eye;
    if (gCamera) {
        const simd_float3 d = gCamera->pivot() - gCamera->eye();
        look = {d.x, d.y};
        const simd_float3 e = gCamera->eye();
        eye = std::array<float, 3>{e.x, e.y, e.z};
    }
    os2::LotViewOptions v = gWallView.options(look, viewLevel());
    v.camera = eye; // cutaway tests the walls against the camera's position
    // cutawayForSelectedSim: the selected Sim on the level shown.
    if (v.walls == os2::LotViewOptions::Walls::Cutaway && gCamera && gSimulation && gLive && gLotShape &&
        v.level != os2::LotViewOptions::kAllLevels)
        v.simPoint = cutawaySimPoint(v);
    return v;
}

// Keeps the lot drawn for the current view (called every frame and before
// headless shots): rebuilds the lot model when the wall mode, the roof, the
// level or the cutaway side of the walls changed (only the geometry: the
// cache keeps the materials and holes, and the light-map atlas is the same
// for every view), hides the Sims above the level shown and keeps their pick
// boxes where they stand.
// The model part runs before the objects are placed (what hangs on cut
// walls hides), the Sims part (`sims`) after they are posed.
// Whether the Sim of object `id` is in the lot's world: it, or what holds
// it (a carried baby, a passenger in a car), stands in it. A Sim at work
// rode off in its carpool, which the car then took out of the world with
// the Sim still in its slot (live QA 15: the plumbob hung at the road's end).
bool simInTheWorld(int16_t id) {
    if (!gSimulation)
        return true;
    os2::vm::VM& m = gSimulation->machine();
    const os2::vm::Object* o = m.object(id);
    for (int depth = 0; o && depth < 8; ++depth) {
        if (o->position.inWorld())
            return true;
        const int16_t c = o->data[os2::vm::objdata::ContainerId];
        if (!c || c == o->id)
            return false;
        o = m.object(c);
    }
    return false;
}

void updateLotView(os2::Renderer& renderer, bool sims) {
    if (!gLotShape || gLotInstance < 0 || gLotModel < 0 || !gLotPatterns || !gIndex)
        return;
    const os2::LotViewOptions want = wantedLotView();
    // The cutaway is worked out every frame (the game does it at each
    // camera move) and the model rebuilt only when what is cut changed
    // (UpdateSceneGraph touches only the differences).
    std::vector<bool> cutaway;
    if (!sims)
        cutaway = os2::lotCutawayWalls(*gLotShape, want);
    if (!sims && (!os2::WallView::sameCut(gLotView, want) || cutaway != gScene.lotCutaway)) {
        const CFTimeInterval start = CACurrentMediaTime();
        os2::LotLightMapLayout layout;
        const os2::ObjectModel model = os2::buildLotModel(*gLotShape, *gIndex, *gLotPatterns, want, &layout, &gLotCache);
        const CFTimeInterval built = CACurrentMediaTime();
        renderer.replaceObject(gLotModel, model, *gIndex);
        if (want.level != gLotView.level)
            placeFencePieces(renderer, *gIndex, *gLotShape, *gLotPatterns, nullptr, want.level);
        // The atlas does not depend on the view; if it did change (it should
        // not), the light maps follow it.
        if (gLightMapLayout && (layout.walls.size() != gLightMapLayout->walls.size() || layout.height != gLightMapLayout->height)) {
            gLightMapLayout = std::move(layout);
            if (gLighting)
                gLighting->setLightMapLayout(*gLightMapLayout);
        }
        gLotView = want;
        gScene.lotCutaway = std::move(cutaway);
        static const bool perf = std::getenv("OS2_PERF") != nullptr;
        if (perf || !gCamera)
            std::printf("opensims2: lot view: walls %s, level %d, roof %s (%.1f ms build, %.1f ms upload)\n",
                        want.walls == os2::LotViewOptions::Walls::Up        ? "up"
                        : want.walls == os2::LotViewOptions::Walls::Cutaway ? "cutaway"
                                                                            : "down",
                        want.level == os2::LotViewOptions::kAllLevels ? -1 : want.level, want.roof ? "on" : "off",
                        (built - start) * 1000, (CACurrentMediaTime() - built) * 1000);
    }
    if (gLive)
        gLive->setWallMode(gWallView.mode());
    if (!sims)
        return;
    // Sims above the level shown: out of sight, and not picked.
    auto& instances = gSimInstances;
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[3] = simd_make_float4(0, 0, -1000, 1);
    for (const auto& s : gLotSims) {
        auto it = instances.find(s.actor);
        if (it == instances.end())
            it = instances.emplace(s.actor, renderer.instanceOf(s.actor->model)).first;
        // A carried Sim is shown where its carrier stands (its belly-high
        // placement would count as the level above).
        const auto carrier = gCarriers.find(s.actor);
        const os2::Vec3 p = carrier != gCarriers.end() ? carrier->second->position
                            : s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                             s.actor->transform->t[2]}
                                                 : s.actor->position;
        bool shown = os2::WallView::visible(*gLotShape, want.level, p[0], p[1], p[2], want.lowestCut);
        // A Sim the VM removed (a carpool or bus driver whose vehicle left)
        // or sent out of the world (at work, gone home) is not drawn: its
        // actor kept its last pose where it was (live QA 15: the drivers
        // stayed sitting in the street after each carpool).
        if (!simInTheWorld(s.id))
            shown = false;
        if (it->second >= 0)
            renderer.setInstanceTransform(it->second, shown ? matrix_identity_float4x4 : away);
        if (!gPie)
            continue;
        // The pick box: the posed bones' extent, a little wider.
        simd_float3 lo = simd_float3(1e9f), hi = simd_float3(-1e9f);
        for (const auto& b : s.actor->bones)
            lo = simd_min(lo, simd_make_float3(b.t[0], b.t[1], b.t[2])), hi = simd_max(hi, simd_make_float3(b.t[0], b.t[1], b.t[2]));
        if (s.actor->bones.empty()) // not posed yet (headless clicks): a standing adult's box
            lo = simd_make_float3(p[0] - 0.15f, p[1] - 0.15f, p[2]), hi = simd_make_float3(p[0] + 0.15f, p[1] + 0.15f, p[2] + 1.7f);
        if (!shown || hi.z < -100) {
            gPie->setMovingBox(os2::vm::ObjectId(s.id), simd_float3(0), simd_float3(0));
            continue;
        }
        lo -= simd_make_float3(0.15f, 0.15f, 0.05f), hi += simd_make_float3(0.15f, 0.15f, 0.15f);
        // Picked by its skinned triangles as drawn (the box is the quick
        // test); before its first pose (headless clicks) by the box.
        static const bool boxes = std::getenv("OS2_PICK_BOXES") != nullptr;
        static const bool simBoxes = std::getenv("OS2_PICK_SIM_BOXES") != nullptr; // only the Sims by box, to compare
        const bool posed = !s.actor->bones.empty() && !s.actor->posed.parts.empty() && !boxes && !simBoxes;
        gPie->setMovingBox(os2::vm::ObjectId(s.id), lo, hi, posed ? &s.actor->posed : nullptr);
    }
    // Objects the VM made after the lot loaded (the bills a mailman leaves,
    // dishes, newspapers, things bought): picked by their model's triangles
    // where the VM view draws them, as the lot's own objects are (they had no
    // pick box, so a click on the bills found nothing: live QA 15).
    if (gPie && gObjectView && gIndex) {
        auto& made = gMadePickBoxes; // (a new lot: none made yet; a new host can reuse the old one's address)
        auto& meshes = gGameData.pickMeshes; // by CRES (null: none; the game's models: for the app's life)
        std::set<int16_t> drawnNow;
        gObjectView->forEachDrawn([&](int16_t id, int, os2::SimActor*) { drawnNow.insert(id); });
        for (auto it = made.begin(); it != made.end();) {
            const bool same = drawnNow.count(it->first) && gObjectView->cresOf(it->first) == it->second;
            if (same) {
                ++it;
                continue;
            }
            gPie->removePickBox(os2::vm::ObjectId(it->first));
            it = made.erase(it);
        }
        for (const int16_t id : drawnNow) {
            if (made.count(id) || gPie->hasPickBox(os2::vm::ObjectId(id)))
                continue;
            const std::string cres = gObjectView->cresOf(id);
            if (cres.empty())
                continue;
            auto m = meshes.find(cres);
            if (m == meshes.end()) {
                std::shared_ptr<const os2::PickMesh> mesh;
                try {
                    mesh = os2::makePickMesh(os2::loadObjectModel(*gIndex, cres, {}), gIndex.get());
                } catch (const std::exception& err) {
                    os2::logOnce("pick mesh", cres, err);
                }
                m = meshes.emplace(cres, std::move(mesh)).first;
            }
            if (!m->second || m->second->corners.empty())
                continue;
            gPie->addFollowingPickBox(os2::vm::ObjectId(id), m->second->lo, m->second->hi, m->second);
            if (std::getenv("OS2_DEBUG_PICK"))
                std::fprintf(stderr, "pick: box made for object %d (%s): %s\n", id, cres.c_str(), gPie->hasPickBox(os2::vm::ObjectId(id)) ? "yes" : "no");
            if (gPie->hasPickBox(os2::vm::ObjectId(id)))
                made[id] = cres;
        }
    }
    // Objects the VM animated (a door open, a chair pulled out): picked by
    // their skinned triangles as drawn, not by their static model's rest pose.
    static const bool staticObjects = std::getenv("OS2_PICK_STATIC_OBJECTS") != nullptr; // the old picking, to compare
    if (gPie && !staticObjects)
        for (const auto& [id, actor] : gObjectActors) {
            if (!actor || actor->posed.parts.empty() || (gObjectView && !gObjectView->shown(id))) {
                gPie->setPosedObject(os2::vm::ObjectId(id), simd_float3(0), simd_float3(0), nullptr);
                continue;
            }
            simd_float3 lo = simd_float3(1e9f), hi = simd_float3(-1e9f);
            for (const auto& part : actor->posed.parts)
                for (const auto& v : part.mesh.positions)
                    lo = simd_min(lo, simd_make_float3(v[0], v[1], v[2])), hi = simd_max(hi, simd_make_float3(v[0], v[1], v[2]));
            if (lo.x > hi.x)
                continue;
            gPie->setPosedObject(os2::vm::ObjectId(id), lo - 0.01f, hi + 0.01f, &actor->posed);
        }
}

// Home / End and the puck's wall fan.
void setWallMode(int mode) {
    gWallView.setMode(mode);
    if (gLive)
        gLive->setWallMode(gWallView.mode());
}

// Redraws the lot after Build mode changed it: the model (walls cut away
// as seen from the camera now, holes for the doors and windows), and the
// light-map atlas and light maps.
void rebuildLotModel(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape || gLotInstance < 0 || !gLotPatterns)
        return;
    gWallView.study(*gLotShape);
    const os2::LotViewOptions view = wantedLotView();
    gLotCache.geometryChanged();
    gLightMapLayout.emplace();
    const os2::ObjectModel model =
        os2::buildLotModel(*gLotShape, index, *gLotPatterns, view, &*gLightMapLayout, &gLotCache);
    renderer.replaceObject(gLotModel, model, index);
    renderer.setGroundClip(os2::lotSkirtBottom(*gLotShape)); // the terrain may have been lowered
    gLotView = view;
    placeFencePieces(renderer, index, *gLotShape, *gLotPatterns, nullptr, view.level);
    if (gLighting) {
        gLighting->setLightMapLayout(*gLightMapLayout);
    } else {
        const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
        if (const auto* state = lighting.state(argument(@"--lighting").value_or("day"))) {
            const os2::RgbaImage lm =
                os2::computeLotLightMap(*gLightMapLayout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            renderer.setLightMap(0, lm);
            renderer.setLightMap(1, lm);
        }
    }
}

} // namespace os2app
