// Loading a scene's things into the renderer: the lot (terrain, walls,
// objects, Sims), the test objects, the skinned Sim of --sim / --character /
// --skinned with --anim / --overlay / --walk, --ui and --lighting
// (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// Places the lot's objects that have a model and stand in the world. Models
// are uploaded once per CRES and design choice. Returns the instances placed.
size_t loadLotObjects(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                      const std::string& lotName) {
    std::map<std::string, int> models; // CRES + design -> model id, -1 if it failed
    std::map<std::string, std::pair<simd_float3, simd_float3>> bounds; // for picking
    std::map<std::string, std::shared_ptr<const os2::PickMesh>> meshes;
    std::map<std::string, os2::Skeleton> skeletons; // CRES + design -> the model's skeleton
    // Some models were renamed after the lots were saved (old modular stairs);
    // then the object's OBJD, found by file name, names the current one.
    std::map<std::string, os2::Tgi> definitions; // lowercase OBJD file name -> OBJD
    auto lower = [](std::string s) {
        for (auto& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto cresOf = [&](const os2::Lot::Object& o) {
        if (index.findByName(o.model + "_cres", kTypeCres))
            return o.model;
        if (definitions.empty())
            for (const auto& loc : index.ofType(kTypeObjd))
                definitions.emplace(lower(os2::ObjectDefinition::parse(index.read(loc)).fileName), loc.entry->tgi);
        std::string name = o.definition.ends_with("_objt") ? o.definition.substr(0, o.definition.size() - 5)
                                                           : o.definition;
        auto it = definitions.find(lower(name));
        std::string cres = it == definitions.end() ? std::string() : os2::objectCresName(index, it->second);
        return cres.empty() ? o.model : cres;
    };
    size_t placed = 0;
    for (const auto& o : lot.objects) {
        if (o.model.empty() || o.model == "nullModel" || o.model == "noModel" || o.model == "null" ||
            o.model == "placeholder" || o.person)
            continue; // ("placeholder": the controllers' marker box, app/vm_objects.cpp; "null": "Routing - Look At")
        // Objects in a slot are saved at (0, 0): drawn only where the VM's
        // containment puts them (app/vm_objects.hpp).
        const bool contained = o.position[0] == 0 && o.position[1] == 0;
        std::string key = o.model;
        for (const auto& [subset, material] : o.materials)
            key += "|" + subset + "=" + material;
        auto it = models.find(key);
        if (it == models.end()) {
            int id = -1;
            try {
                const auto model = os2::loadObjectModel(index, cresOf(o), o.materials);
                for (const auto& w : model.warnings)
                    std::fprintf(stderr, "opensims2: %s: %s: %s\n", lotName.c_str(), model.name.c_str(), w.c_str());
                id = renderer.addObject(model, index);
                bounds[key] = os2::modelBounds(model);
                meshes[key] = os2::makePickMesh(model, &index);
                skeletons[key] = model.skeleton;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: %s: %s (%s): %s\n", lotName.c_str(), o.model.c_str(),
                             o.definition.c_str(), e.what());
            }
            it = models.emplace(key, id).first;
        }
        if (it->second < 0)
            continue;
        // Drawn as the lot saved its bones (nTSSG::cObject's transform
        // nodes): a dining chair pushed in under its table (its push-in's
        // last frame) rather than its model's rest pose, pulled out.
        std::vector<os2::Xform> pose;
        if (!o.nodes.empty())
            if (auto p = os2::savedPose(skeletons[key], o.nodes)) {
                pose = std::move(*p);
                const std::string posedKey = key + "@" + os2::poseKey(pose);
                auto pit = models.find(posedKey);
                if (pit == models.end()) {
                    int id = -1;
                    try {
                        const auto model = os2::posedModel(os2::loadObjectModel(index, cresOf(o), o.materials), pose);
                        id = renderer.addObject(model, index);
                        bounds[posedKey] = os2::modelBounds(model);
                        meshes[posedKey] = os2::makePickMesh(model, &index);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "opensims2: %s: %s posed: %s\n", lotName.c_str(), o.model.c_str(), e.what());
                    }
                    pit = models.emplace(posedKey, id).first;
                }
                if (pit->second >= 0) {
                    it = pit;
                    key = posedKey;
                } else {
                    pose.clear();
                }
            }
        const auto& q = o.rotation;
        simd_float4x4 at = simd_matrix4x4(simd_quaternion(q[0], q[1], q[2], q[3]));
        at.columns[3] = simd_make_float4(o.position[0], o.position[1], o.position[2], 1);
        if (contained)
            at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
        const int instance = renderer.addInstance(it->second, at);
        if (const char* f = std::getenv("OS2_DEBUG_OBJECTS"); f && o.definition.find(f) != std::string::npos)
            std::fprintf(stderr, "object %u (%s) at %.2f %.2f %.2f%s\n", o.instance & 0xFFFF, o.definition.c_str(),
                         o.position[0], o.position[1], o.position[2], contained ? " contained" : "");
        // Stale cObjects share IDs with live ones: the first (or one in the
        // world) keeps the ID; app/vm_objects.cpp checks it against the VM.
        const int16_t id = int16_t(o.instance & 0xFFFF);
        if (!gPlacedObjects.count(id) || (gPlacedObjects[id].contained && !contained))
            gPlacedObjects[id] = {cresOf(o), o.materials, o.position, o.rotation, o.definition, instance, contained, pose};
        if (contained) {
            gPickBoxes.push_back({true, o.instance, bounds[key].first, bounds[key].second, at, meshes[key]});
            continue;
        }
        gPickBoxes.push_back({false, o.instance, bounds[key].first, bounds[key].second, at, meshes[key]});
        ++placed;
    }
    return placed;
}

// A community lot's loading screen shows the catalog image of one of the
// lot's objects (docs/cinematics-mac.md §2.1: bit 0x20 of OBJD field 0x5E,
// the function subsort), when the screen asked for it.
void offerCommunityLotImage(os2::ResourceIndex& index, const os2::Lot& lot) {
    if (!os2::loadingIconWanted())
        return;
    try {
        os2::Catalog catalog = os2::Catalog::load(index);
        catalog.setThumbnailPackage(gameDir() / "TSData/Res/UserData/Thumbnails/ObjectThumbnails.package");
        for (const auto& o : lot.objects) {
            std::string name = o.definition;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, "_objt") == 0)
                name.resize(name.size() - 5);
            const os2::CatalogEntry* e = catalog.find(name);
            if (!e || !(e->functionSubSort & 0x20))
                continue;
            if (const os2::RgbaImage* img = catalog.thumbnail(*e)) {
                os2::offerLoadingIcon(*img);
                return;
            }
        }
    } catch (const std::exception& err) {
        os2::logOnce("loading screen icon", "catalog", err);
    }
}

// Loads --lot into the renderer in place of the test floor. Returns the middle
// of the lot at ground level, for the camera.
// --walls up|cutaway|down picks the wall mode; cutaway uses `viewDirection`.
std::optional<simd_float3> loadLot(os2::Renderer& renderer, os2::ResourceIndex& index, simd_float2 viewDirection) {
    const auto query = lotQuery();
    if (!query)
        return std::nullopt;
    CFTimeInterval start = CACurrentMediaTime();
    gLotFile = lotPath(*query);
    os2::DbpfPackage pkg(lotPath(*query));
    perfLap("before the lot");
    os2::Lot lot = os2::Lot::load(pkg);
    perfLap("Lot::load");
    const size_t leftovers = os2::vm::dropLeftoverCObjects(lot, pkg, index); // stale copies the package keeps
    std::printf("opensims2: %zu leftover cObjects not drawn\n", leftovers);
    perfLap("leftover cObjects");
    const os2::PatternCatalog patterns(index, gameDir() / "TSData/Res");
    perfLap("PatternCatalog");
    offerCommunityLotImage(index, lot);
    // The game enters a lot with walls cutaway and the levels above the
    // camera's hidden (--ui live, --mode); a bare --lot view shows every
    // level with walls up and the roof. --walls down|cutaway|up|roof picks
    // the game's wall mode 0-3 (up shows the roof too unless --roof off;
    // roof is mode 3, the only one with the roofs).
    gLevelsHidden = liveUiRequested() || startMode().has_value();
    const std::string walls = argument(@"--walls").value_or(gLevelsHidden ? "cutaway" : "up");
    const bool roofOff = argument(@"--roof").value_or("on") == "off";
    if (walls == "cutaway")
        gWallView.setMode(1);
    else if (walls == "down")
        gWallView.setMode(0);
    else if (walls == "up" || walls == "roof")
        gWallView.setMode(roofOff ? 2 : 3);
    else
        throw std::runtime_error("--walls: expected down, cutaway, up or roof");
    gWallView.study(lot);
    gLotCache = {};
    const os2::LotViewOptions view = gWallView.options(
        {viewDirection.x, viewDirection.y}, gLevelsHidden ? gWallView.shownLevel(0) : os2::LotViewOptions::kAllLevels);
    gLightMapLayout.emplace();
    const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view, &*gLightMapLayout, &gLotCache);
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("buildLotModel");
    renderer.hideTestScene();
    gLotModel = renderer.addObject(model, index);
    gLotInstance = renderer.addInstance(gLotModel, matrix_identity_float4x4);
    gLotShape = std::make_unique<os2::Lot>(lot);
    // Nothing under the lot's earth skirt is drawn (lot_model.hpp).
    renderer.setGroundClip(os2::lotSkirtBottom(lot));
    gLotPatterns = std::make_unique<os2::PatternCatalog>(patterns);
    gLotView = view;
    perfLap("lot model to the GPU");
    for (const auto& w : lot.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    const size_t objects = loadLotObjects(renderer, index, lot, *query);
    perfLap("loadLotObjects");
    const size_t sims = loadLotSims(renderer, index, lot, pkg, lotPath(*query));
    perfLap("loadLotSims");
    NSArray<NSString*>* args = sceneArgs();
    startAudio(index, lot);
    perfLap("startAudio");
    if (argument(@"--run-vm") || argument(@"--pie") || argument(@"--click") || [args containsObject:@"--vm"] ||
        liveUiRequested())
        startSimulation(renderer, index, lot, pkg, lotPath(*query));
    perfLap("startSimulation");
    // Without the VM (no lamps): the light maps of the --lighting state.
    if (!gLighting) {
        const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
        if (const auto* state = lighting.state(argument(@"--lighting").value_or("day"))) {
            const os2::RgbaImage lm = os2::computeLotLightMap(*gLightMapLayout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            if (const char* d = std::getenv("OS2_LIGHTMAP_DUMP")) // the atlas, as LotLighting writes it
                os2::writePng(d, lm);
            renderer.setLightMap(0, lm);
            renderer.setLightMap(1, lm);
            auto global = [&](const char* name, float fallback) {
                auto it = lighting.globals.find(os2::lowerAscii(name));
                return it == lighting.globals.end() ? fallback : std::stof(it->second);
            };
            gStaticShadowLight = os2::simShadowLight(*state, global("hmSunLightTransitionZone", 1), global("shadowStrengthLimit", 1));
        }
    }
    // Fence rails and posts: one model per CRES, an instance per piece.
    std::vector<std::string> fenceWarnings;
    const size_t fences = placeFencePieces(renderer, index, lot, patterns, &fenceWarnings, view.level);
    for (const auto& w : fenceWarnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("fences");
    std::printf("opensims2: lot %s: %ux%u tiles, levels %d..%d, %zu walls, %zu paints (%zu parts), %zu roofs, "
                "%zu fence pieces, %zu of %zu objects and %zu Sims placed in %.2f s\n",
                query->c_str(), lot.width, lot.height, lot.minLevel, lot.minLevel + lot.levelCount - 1,
                lot.walls.size(), lot.paints.size(), model.parts.size(), lot.roofs.size(), fences, objects,
                lot.objects.size(), sims, CACurrentMediaTime() - start);
    if (gCamera)
        gCameraFollow = std::make_unique<os2::LotCameraFollow>(*gCamera, lot);
    const int cx = int(lot.width / 2), cy = int(lot.height / 2);
    return simd_make_float3(float(lot.width) / 2, float(lot.height) / 2,
                            lot.elevation(os2::LotCameraFollow::startLevel(lot), cx, cy));
}

// Loads the objects into the renderer on a grid, four per row, three tiles
// apart, starting at tile (2, -3). Returns the middle of the grid, for the camera.
simd_float3 loadObjects(os2::Renderer& renderer, os2::ResourceIndex& index) {
    CFTimeInterval start = CACurrentMediaTime();
    int slot = 0;
    for (const auto& query : objectQueries()) {
        try {
            std::string match;
            const auto model = os2::loadObjectModel(index, os2::resolveObjectCres(index, query, &match));
            for (const auto& w : model.warnings)
                std::fprintf(stderr, "opensims2: %s: %s\n", model.name.c_str(), w.c_str());
            const int id = renderer.addObject(model, index);
            simd_float4x4 at = matrix_identity_float4x4;
            at.columns[3] = simd_make_float4(2.5f + 3.0f * float(slot % 4), -2.5f - 3.0f * float(slot / 4), 0, 1);
            ++slot;
            renderer.addInstance(id, at);
            std::printf("opensims2: %s (%zu parts)\n", match.c_str(), model.parts.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: object %s: %s\n", query.c_str(), e.what());
        }
    }
    if (slot)
        std::printf("opensims2: loaded in %.2f s\n", CACurrentMediaTime() - start);
    std::fflush(stdout);
    const int cols = std::clamp(slot, 1, 4), rows = (std::max(slot, 1) + 3) / 4;
    return simd_make_float3(2.5f + 1.5f * float(cols - 1), -2.5f - 1.5f * float(rows - 1), 0);
}


os2::Skeleton loadSkeleton(os2::ResourceIndex& index, const std::string& cres) {
    auto loc = index.findByName(cres + "_cres", kTypeCres);
    if (!loc)
        throw std::runtime_error("skeleton not found: " + cres);
    const auto bytes = index.read(*loc);
    return os2::Skeleton::fromCres(os2::Rcol(bytes));
}

// A Sim's actor from its character file, standing at `position` facing
// `heading`, its idle started `phase` seconds early; added to gLotSims.
bool spawnSimActor(os2::Renderer& renderer, os2::ResourceIndex& index, const fs::path& file, int16_t id,
                   simd_float3 position, float heading, double phase, const std::string& label) {
    const std::string idle = argument(@"--sim-anim").value_or("a-idle-neutral-armsSide-wtC-breatheLoop");
    const std::string stem = file.stem().string();
    try {
        perfLap("  loadSimParts");
        auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, simParts(index), file));
        perfLap("  loadCharacterSim");
        for (const auto& w : actor->skin.model.warnings)
            std::fprintf(stderr, "opensims2: %s: %s\n", stem.c_str(), w.c_str());
        actor->position = {position.x, position.y, position.z};
        actor->heading = heading;
        // Start the idles at different moments so the Sims do not breathe in step.
        // Children and toddlers have their own skeletons and idles.
        std::string anim = idle;
        if (!argument(@"--sim-anim")) {
            const std::string sk = actor->skin.skeleton.name;
            if (sk.size() > 2 && std::tolower(static_cast<unsigned char>(sk[0])) == 'c')
                anim = "c-idle-neutral-armsSide-wtC-breatheLoop50";
            else if (sk.size() > 2 && std::tolower(static_cast<unsigned char>(sk[0])) == 'p')
                anim = "p-idle-neutral-lookAround";
        }
        actor->cue(-phase, actor->animation(index, anim));
        perfLap("  Sim animation");
        actor->model = renderer.addObject(actor->skin.model, index, true);
        renderer.addInstance(actor->model, matrix_identity_float4x4);
        perfLap("  Sim to the GPU");
        std::printf("opensims2: Sim %s (%s) at (%.2f, %.2f, %.2f), %zu parts\n", stem.c_str(), label.c_str(), position.x,
                    position.y, position.z, actor->skin.model.parts.size());
        gLotSims.push_back({actor.get(), id, file, anim});
        gActors.push_back(std::move(actor));
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: Sim %s: %s\n", label.c_str(), e.what());
        return false;
    }
}

// The VM runs the lot (--vm, --pie, --click, the live UI): the arriving
// Sims of a family moving in come from its "reset lot" broadcast
// (LotSimulation), not from formats/lot_arrivals.
bool lotVmWillRun() {
    return argument(@"--run-vm") || argument(@"--pie") || argument(@"--click") ||
           [sceneArgs() containsObject:@"--vm"] || liveUiRequested();
}

// The Sims on a lot: each cPerson with a saved state in the world, built from
// its character file (N001/Characters/<N001_User#####>.package, named by the
// cObject's "N001_User00011 - Don_objt"), standing where it was saved and
// playing an idle animation (--sim-anim <name> picks another).
size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile) {
    os2::LotObjects saved = os2::LotObjects::load(pkg);
    // A family that just moved in: its Sims arrive (formats/lot_arrivals.hpp)
    // when no VM runs; with the VM the game's tree brings them.
    const os2::LotArrivals arrivals = os2::addArrivingSims(saved, lot, lotFile, &index, !lotVmWillRun());
    std::set<uint32_t> inWorld; // object IDs of the Sims standing on the lot
    for (const auto& st : saved.objects)
        if (st.person && st.x >= 0)
            inWorld.insert(uint32_t(uint16_t(st.id)));
    size_t placed = 0;
    std::vector<os2::Lot::Object> people = lot.objects;
    people.insert(people.end(), arrivals.sims.begin(), arrivals.sims.end());
    for (const auto& o : people) {
        if (!o.person || !inWorld.count(o.instance & 0xFFFF))
            continue;
        const std::string stem = o.definition.substr(0, o.definition.find(" - "));
        const fs::path file = lotFile.parent_path().parent_path() / "Characters" / (stem + ".package");
        if (spawnSimActor(renderer, index, file, int16_t(o.instance & 0xFFFF),
                          simd_make_float3(o.position[0], o.position[1], o.position[2]),
                          2 * std::atan2(o.rotation[2], o.rotation[3]), 0.37 * double(placed), o.definition))
            ++placed;
    }
    return placed;
}

// --sim [body,hair,tone]: an adult Sim assembled from catalog parts; or
// --skinned: the listed CRES on the adult skeleton. Placed at tile (2.5, -2.5).
std::optional<simd_float3> loadSkinned(os2::Renderer& renderer, os2::ResourceIndex& index) {
    const auto list = argument(@"--skinned");
    const auto sim = argument(@"--sim");
    const auto character = argument(@"--character");
    if (!list && !sim && !character)
        return std::nullopt;
    os2::SkinnedModel skin;
    if (character) {
        skin = os2::loadCharacterSim(index, simParts(index), characterPath(*character));
    } else if (sim) {
        os2::SimLook look;
        std::vector<std::string> fields;
        for (size_t start = 0; start <= sim->size();) {
            size_t end = std::min(sim->find(',', start), sim->size());
            fields.push_back(sim->substr(start, end - start));
            start = end + 1;
        }
        if (fields.size() > 0 && !fields[0].empty() && fields[0] != "default")
            look.body = fields[0];
        if (fields.size() > 1 && !fields[1].empty())
            look.hair = fields[1];
        if (fields.size() > 2 && !fields[2].empty())
            look.skinTone = std::stoi(fields[2]);
        look.prefix = look.body.substr(0, 2);
        skin = os2::loadSim(index, simParts(index), look);
    } else {
        skin = os2::SkinnedModel(loadSkeleton(index, "auskel"));
    }
    auto actor = std::make_unique<os2::SimActor>(std::move(skin));
    for (size_t start = 0; list && start <= list->size();) {
        size_t end = list->find(',', start);
        if (end == std::string::npos)
            end = list->size();
        const std::string cres = list->substr(start, end - start);
        start = end + 1;
        if (cres.empty())
            continue;
        actor->skin.add(os2::loadObjectModel(index, cres));
    }
    for (const auto& w : actor->skin.model.warnings)
        std::fprintf(stderr, "opensims2: %s\n", w.c_str());
    actor->position = {2.5f, -2.5f, 0};
    std::string playing;
    if (auto h = argument(@"--heading")) // degrees about Z
        actor->heading = std::stof(*h) * 3.14159265f / 180;
    // The idle and walk of the skeleton's age (child cuskel, toddler puskel,
    // baby buskel; auskel for the rest); --walk-anim names another walk.
    const char age = char(std::tolower(static_cast<unsigned char>(actor->skin.skeleton.name.empty() ? 'a' : actor->skin.skeleton.name[0])));
    const char* ageIdle = age == 'c'   ? "c-idle-neutral-armsSide-wtC-breatheLoop50"
                          : age == 'p' ? "p-idle-standing-loopBreathe"
                          : age == 'b' ? "b-idle-carryBaby-breathe"
                                       : "a-idle-neutral-armsSide-wtC-breatheLoop";
    const char* ageWalk = age == 'c' ? "c-walk-alt0-normal" : age == 'p' ? "p-walk" : "a-male-walk-alt0-normal";
    if (auto walk = argument(@"--walk")) {
        // Idle, then walk straight ahead for <seconds>, then idle again;
        // the walk's locomotion velocity moves the Sim.
        const double seconds = std::stod(*walk);
        const auto& idle = actor->animation(index, argument(@"--anim").value_or(ageIdle));
        const auto& walkAnim = actor->animation(index, argument(@"--walk-anim").value_or(ageWalk));
        actor->cue(0, idle);
        actor->cue(0.5, walkAnim, {.fade = 0.3f});
        actor->cue(0.5 + seconds, idle, {.fade = 0.3f});
        actor->rootMotion = true;
        playing = "idle, walk, idle";
    } else if (auto name = argument(@"--anim"); name || character || sim) {
        actor->cue(0, actor->animation(index, name.value_or(ageIdle)));
        playing = actor->animations.back()->resource.fileName;
    }
    // --overlay <anim>[@bone]: a second animation over the bones under <bone>
    // (default spine0), with its own priority.
    if (auto overlay = argument(@"--overlay")) {
        std::string name = *overlay, root = "spine0";
        if (auto at = name.find('@'); at != std::string::npos)
            root = name.substr(at + 1), name.resize(at);
        const auto& anim = actor->animation(index, name);
        actor->cue(0, anim, {.fade = 0, .mask = os2::boneMask(actor->skin.skeleton, root), .priority = 100});
        playing += " + " + anim.resource.fileName + "@" + root;
    }
    actor->model = renderer.addObject(actor->skin.model, index, true);
    renderer.addInstance(actor->model, matrix_identity_float4x4); // placement is in the vertices
    std::printf("opensims2: skinned %s (%zu parts, %zu bones)%s%s\n", actor->skin.model.name.c_str(), actor->skin.model.parts.size(),
                actor->skin.skeleton.bones.size(), playing.empty() ? "" : ", playing ", playing.c_str());
    gActors.push_back(std::move(actor));
    return simd_make_float3(2.5f, -2.5f, 0.9f);
}

std::unique_ptr<os2::UiLayer>& gUi = gScene.ui;

// --ui <instance-hex>: lays out that UI script and hands it to the renderer as
// an overlay. Returns the canvas size, for the screenshot.
std::optional<std::pair<uint32_t, uint32_t>> loadUi(os2::Renderer& renderer, os2::ResourceIndex& index, MTKView* view) {
    const auto id = argument(@"--ui");
    if (!id || *id == "live")
        return std::nullopt;
    const fs::path res = gameDir() / "TSData/Res";
    os2::UiResources ui(index, res);
    auto script = ui.script(uint32_t(std::stoul(*id, nullptr, 16)));
    if (!script)
        throw std::runtime_error("no UI script " + *id);
    os2::UiLayoutOptions opt;
    if (auto show = argument(@"--ui-show"))
        for (const auto& w : os2::uiList("(" + *show + ")"))
            opt.show.insert(uint32_t(std::stoul(w, nullptr, 16)));
    os2::FontLibrary fonts(res / "UI/Fonts");
    const os2::UiScreen screen = os2::layoutUi(*script, ui, fonts, opt);
    for (const auto& w : screen.warnings)
        std::fprintf(stderr, "opensims2: ui %s: %s\n", id->c_str(), w.c_str());
    gUi = std::make_unique<os2::UiLayer>((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                         uint32_t(view.depthStencilPixelFormat));
    gUi->setScreen(screen);
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) { gUi->encode(enc, w, h); });
    std::printf("opensims2: ui %s: %zu windows, %zu drawn, %zu texts, %zu quads\n", id->c_str(), screen.windows,
                screen.drawn, screen.texts, screen.quads.size());
    return std::make_pair(uint32_t(screen.width), uint32_t(screen.height));
}

// Lighting state from the game's Lighting.txt: --lighting <name>, else "day".
void applyLighting(os2::Renderer& renderer) {
    const std::string name = argument(@"--lighting").value_or("day");
    const fs::path path = gameDir() / kLightingConfig;
    const os2::LightingConfig cfg = os2::loadLightingConfig(path);
    const os2::LightingState* state = cfg.state(name);
    if (!state)
        throw std::runtime_error("no lighting state \"" + name + "\" in " + path.string());
    renderer.setLighting(*state);
}

} // namespace os2app
