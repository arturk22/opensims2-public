// The live mode over the lot's VM: the pie menu, the live panel, the
// tutorial, the Sim selection, Buy and Build mode with their day/night
// button and headless steps (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// The pie menu's constants (ui.package UI script 0x090617B7), the selected
// Sim's portrait (its character file's IMG 0x10) as the head, and the item
// sounds through the lot's audio.
void setupPieMenu(os2::ResourceIndex& index) {
    os2::UiResources ui(index, gameDir() / "TSData/Res");
    if (auto script = ui.script(0x090617B7))
        gPie->setStyle(os2::PieMenuStyle::fromConstants(os2::uiConstants(*script)));
    if (auto p = simPortrait(gPie->sim()))
        gPie->setPortrait(std::move(*p));
    gPie->setResources(index, gameDir() / "TSData/Res"); // the dialog window's scripts
    gPie->setDialogThumbnails(dialogThumbnail);
    setupWardrobe(index);
    setupAspirationWants(index);
    if (gAudio)
        gPie->setSoundPlayer([](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        });
}

// --ui live: the live-mode panel (app/live_ui.hpp) in place of the pie
// menu's debug queue (--debug-queue keeps it). Picking a Sim there selects
// it as --sim-id does (global 3, the pie menu's Sim and head).
void setupLiveUi(os2::ResourceIndex& index) {
    gLive = std::make_unique<os2::LiveUi>(gSimulation->machine(), index, gameDir() / "TSData/Res");
    for (const auto& s : gLotSims)
        if (auto p = simPortrait(s.id))
            gLive->setPortrait(s.id, std::move(*p));
    gLive->setSim(gPie->sim());
    gLive->simAway = [](os2::vm::ObjectId id) { return !simInTheWorld(int16_t(id)); };
    gLive->onSelectSim = [](os2::vm::ObjectId id) {
        gSimulation->machine().globals[os2::vm::global::SelectedPersonId] = id;
        gPie->close();
        gPie->setSim(id);
        if (auto p = simPortrait(id))
            gPie->setPortrait(std::move(*p));
    };
    gLive->onCentreSim = [](os2::vm::ObjectId id) {
        if (gCameraFollow)
            gCameraFollow->centre(gSimulation->machine(), id);
    };
    if (gAudio)
        gLive->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
    // The options panel: "Vizinhança" is the way back to the neighborhood the lot was entered from (as Esc).
    // Leaving asks whether to save and switches scenes in place (game flow, below).
    if (auto hood = argument(@"--hood"); hood && gHoodLot) {
        gLive->setCanLeave(true);
        gLive->onNeighborhood = [] { leaveLot(); };
    }
    gLive->setLiveEnabled(!gLotVacant); // no Sims: the Live button is off (HandleLiveModeButton)
    gLive->onQuit = [] { quitFromLot(); };
    gLive->onSave = [] { saveGameNow(); };
    gLive->onOptionWindow = [](int window) {
        if (gOptions)
            gOptions->toggleWindow(window);
    };
    gLive->onFloor = [](int delta) { changeFloor(delta); };
    gLive->onWallMode = [](int mode) { setWallMode(mode); };
    gLive->setWallMode(gWallView.mode());
    gLive->onZoom = [](int steps) {
        if (gCamera)
            gCamera->zoomBy(steps);
    };
    gLive->onRotate = [](int steps) {
        if (gCamera)
            gCamera->rotateBy(steps);
    };
    // Relationship thumbnails of Sims off the lot: the small portrait of the
    // character file (in the lot's neighborhood) whose OBJD has the GUID.
    if (auto query = lotQuery())
        gLive->neighborPortrait = [dir = lotPath(*query).parent_path().parent_path() / "Characters",
                                   byGuid = std::make_shared<std::map<uint32_t, fs::path>>()](uint32_t guid)
            -> std::optional<os2::RgbaImage> {
            std::error_code ec;
            if (byGuid->empty() && fs::is_directory(dir, ec))
                for (const auto& de : fs::directory_iterator(dir, ec))
                    try {
                        os2::DbpfPackage p(de.path());
                        for (const auto& e : p.entries())
                            if (e.tgi.type == kTypeObjd) {
                                byGuid->emplace(os2::ObjectDefinition::parse(p.read(e)).guid(), de.path());
                                break;
                            }
                    } catch (const std::exception& err) {
                        os2::logOnce("neighbor portraits", de.path().filename().string(), err);
                    }
            const auto it = byGuid->find(guid);
            if (it == byGuid->end())
                return std::nullopt;
            try {
                os2::DbpfPackage character(it->second);
                for (const auto& e : character.entries())
                    if (e.tgi.type == os2::kTypeUiImage && e.tgi.instance < 0x100)
                        return os2::decodeUiImage(character.read(e));
            } catch (const std::exception& err) {
                os2::logOnce("neighbor portrait", it->second.filename().string(), err);
            }
            return std::nullopt;
        };
    // AutotrackSim (BV 0x1ad1f0): centre on the Sim, then track it; no Sim: stop.
    gLive->onFollowSim = [](os2::vm::ObjectId id) {
        if (!gCameraFollow)
            return;
        if (id)
            gCameraFollow->centre(gSimulation->machine(), id);
        gCameraFollow->setTracked(id);
    };
    if (![sceneArgs() containsObject:@"--debug-queue"])
        gPie->setShowQueue(false);
    std::printf("opensims2: live UI: %s, %zu family members\n", gLive->ok() ? "loaded" : "missing scripts",
                gLive->family().size());
}

// A lot of the Tutorial neighborhood: its tutorial controllers (created if
// the lot has none, vm::ensureTutorialControllers) and the tutorial
// manager's UI over the live panel (app/tutorial.hpp).
void setupTutorial(os2::ResourceIndex& index) {
    if (!gSimulation || !gLive || gLotFile.parent_path().parent_path().filename() != "Tutorial")
        return;
    os2::vm::VM& vm = gSimulation->machine();
    const int created = os2::vm::ensureTutorialControllers(vm);
    // The tutorial lots are saved paused (global 17); the episode plays in
    // time (step 10: "this is the only mode in which time passes"), so it
    // starts unpaused and the controller's first press of Buy mode pauses
    // it. The toolbar's saved UI state of the episodes' lots 14, 18 and 19
    // agrees (unpaused, docs/game-flow-mac.md §7.6); lots 1 and 4 have it
    // paused, and kept paused their first window would not come up until
    // the player pressed play (inferred: the game's tutorial start was not
    // read, so they start unpaused too).
    vm.globals[os2::vm::global::Paused] = 0;
    gTutorialRequestsSeen = vm.engineRequests.size();
    gTutorial = std::make_unique<os2::TutorialOverlay>(vm, index, gameDir() / "TSData/Res");
    // The controls: Buy's and Build's panels while they are up, the live
    // panel and the puck.
    gTutorial->locate = [](uint32_t window, uint32_t control) -> std::optional<std::array<float, 4>> {
        // The main window's controls are the puck's (the panels reuse small ids).
        if (window == os2::LiveUi::kMainWindow)
            if (auto r = gLive->controlRect(window, control))
                return r;
        if (gBuy && gBuy->active())
            if (auto r = gBuy->controlRect(window, control))
                return r;
        if (gBuild && gBuild->active())
            if (auto r = gBuild->controlRect(window, control))
                return r;
        return gLive->controlRect(window, control);
    };
    gTutorial->pressControl = [](uint32_t window, uint32_t control) {
        const auto r = gTutorial->locate(window, control);
        if (!r)
            return;
        const simd_float2 p = simd_make_float2(((*r)[0] + (*r)[2]) / 2, ((*r)[1] + (*r)[3]) / 2);
        gTutorial->clicked(p); // as a click: the control's listeners, then the control
        if (gLive->overModeButton(p) && gLive->press(p))
            return;
        if (gBuy && gBuy->active() && gBuy->press(p))
            return;
        if (gBuild && gBuild->active() && gBuild->press(p))
            return;
        gLive->press(p);
    };
    gLive->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    if (gBuy) {
        gBuy->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    }
    if (gBuild) {
        gBuild->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    }
    std::printf("opensims2: tutorial: %d controller(s) created\n", created);
}

// Selecting a Sim of the family other than by its thumbnail: a right click
// on it, or the space bar for the next one (the game's controls, as the
// tutorial's second lesson teaches them). False if it is not one of them.
bool selectFamilySim(os2::vm::ObjectId id) {
    if (!gLive || !id || id == gLive->sim())
        return false;
    const auto family = gLive->family();
    if (std::find(family.begin(), family.end(), id) == family.end())
        return false;
    if (!simInTheWorld(int16_t(id))) // at work: its thumbnail is greyed, no orders
        return false;
    std::printf("opensims2: select Sim %d\n", id);
    if (gLive->onSelectSim)
        gLive->onSelectSim(id);
    gLive->setSim(id);
    std::fflush(stdout);
    return true;
}

// The game selecting a Sim itself (Generic Sims Call 25: TryGenericSimCall
// BV 0xe36834 hands the person to SetSelectedPerson, BV 0x52a55c, which
// tells the UI): the app's Sim (pie menu, live panel, plumbob, camera)
// follows when the VM's global 3 changes. "Script - Pleasantville - Goth"
// selects Cassandra at tick 27 of N001_Lot11; the app kept the Sim it had,
// and the game's PlumbBob (whose "Main - Update My Owner" goes to global
// 3) stayed over her while the app drew its own over the other Sim.
void followVmSelection() {
    if (!gSimulation || !gPie)
        return;
    const int16_t id = gSimulation->machine().globals[os2::vm::global::SelectedPersonId];
    if (id == gScene.vmSelected)
        return;
    gScene.vmSelected = id;
    const os2::vm::Object* sim = gSimulation->machine().object(os2::vm::ObjectId(id));
    if (id <= 0 || id == gPie->sim() || !sim || !sim->person)
        return;
    std::printf("opensims2: the game selects Sim %d\n", id);
    gPie->close();
    gPie->setSim(id);
    if (auto p = simPortrait(id))
        gPie->setPortrait(std::move(*p));
    if (gLive)
        gLive->setSim(os2::vm::ObjectId(id));
    std::fflush(stdout);
}

void selectNextFamilySim() {
    if (!gLive)
        return;
    const auto family = gLive->family();
    if (family.size() < 2)
        return;
    auto it = std::find(family.begin(), family.end(), gLive->sim());
    selectFamilySim(it == family.end() || std::next(it) == family.end() ? family.front() : *std::next(it));
}

// A selected Sim that leaves the world (off to work) hands the selection to
// a family member who is home: its thumbnail is greyed and it takes no
// orders (live QA 15: the Sim at work could be selected and got "Receber
// Correio"). All of them away: the selection stays.
void keepSelectedSimHome() {
    if (!gLive || !gPie || !gSimulation || !gLive->sim() || simInTheWorld(int16_t(gLive->sim())))
        return;
    for (const os2::vm::ObjectId id : gLive->family())
        if (id != gLive->sim() && selectFamilySim(id))
            return;
}

// The tutorial each frame: its UI Effects, and the end of an episode
// (Generic Sims Call 0 "Exit Lot", from "Simulation - Exit Tutorial"): back
// to the main menu it was played from.
void pollTutorial(double seconds) {
    if (!gTutorial || !gSimulation)
        return;
    gTutorial->update(seconds);
    auto& requests = gSimulation->machine().engineRequests;
    if (gTutorialRequestsSeen > requests.size())
        gTutorialRequestsSeen = 0;
    for (; gTutorialRequestsSeen < requests.size(); ++gTutorialRequestsSeen)
        if (requests[gTutorialRequestsSeen].call == 0 && !gPendingScene) {
            const auto& t = requests[gTutorialRequestsSeen].temps;
            std::printf("opensims2: tutorial: episode over (state %d), exit lot%s\n",
                        gSimulation->machine().globals[26], t[2] ? ", saving" : "");
            std::fflush(stdout);
            // Exit Lot's temp 2: save the lot first (its SIMI keeps global 26
            // "Tutorial State", which the episode list reads).
            if (t[2])
                saveGameNow();
            os2::Scene next;
            next.kind = os2::Scene::Kind::MainMenu;
            requestScene(next);
        }
}

// The lot camera shows the controller the selector chose (camera_mode.hpp):
// "Live", or "Build", the top-down view of BuildCamera.txt. The new camera
// takes over the old one's pivot, heading, distance and pitch.
void applyCameraSelection() {
    if (!gCamera)
        return;
    using Controller = os2::CameraSelector::Controller;
    // The free camera ("CameraMan"): starts at the view of the camera it replaces.
    if (gCameraSelector.controller() == Controller::CameraMan) {
        if (!gFreeCamera) {
            gFreeCamera = std::make_unique<os2::FreeCamera>(loadLotCamera("CameraMan"));
            const auto& b = gCamera->bounds();
            if (b)
                gFreeCamera->setBounds(b->first, b->second);
            os2::LotCamera* camera = gCamera;
            gFreeCamera->setFloorSampler([camera](float x, float y) { return camera->floorHeight(x, y); });
            gFreeCamera->start(gCamera->eye(), gCamera->forward(), gCamera->fov());
            gCamera->setFirstPerson(gFreeCamera->pose());
            std::printf("opensims2: camera: CameraMan (free camera) at (%.1f, %.1f, %.1f)\n", gFreeCamera->position().x,
                        gFreeCamera->position().y, gFreeCamera->position().z);
        }
        return;
    }
    // Back from the free camera: Live or Build takes the view over (MatchThisControllerWith).
    const char* want = gCameraSelector.controller() == Controller::Build ? "Build" : "Live";
    std::optional<os2::LotCamera::FirstPerson> from;
    if (gFreeCamera) {
        from = gFreeCamera->pose();
        gFreeCamera.reset();
        gCamera->setFirstPerson(std::nullopt);
    }
    bool changed = from.has_value();
    if (gCamera->definition().name != want) {
        gCamera->setDefinition(loadLotCamera(want));
        changed = true;
    }
    if (from)
        gCamera->matchView(from->eye, from->forward);
    if (changed)
        std::printf("opensims2: camera: %s%s\n", want, gCameraSelector.topDown() ? " (top-down view on)" : "");
}

bool freeCameraActive() { return gFreeCamera && gCameraSelector.freeCamera(); }

// "Cycle Camera Controller" (Tab, Y): in a lot, the free camera and back. Not in the
// neighborhood (its "CameraManNH" is not done) nor in Create-a-Sim.
void cycleCameraController() {
    if (!gCamera || !gSimulation || gHood || gCas)
        return;
    gCameraSelector.cycleController();
    applyCameraSelection();
}

// Escape in the free camera leaves it (cCameraCameraManController::DoMessage).
bool leaveFreeCamera() {
    if (!gCameraSelector.escape())
        return false;
    applyCameraSelection();
    return true;
}

// The play mode changed (cTSMainToolbar::SetMode): Live keeps the Live camera, Buy and
// Build show the top-down one if the player left it on.
void enterPlayMode(os2::CameraSelector::Mode mode) {
    gCameraSelector.setMode(mode);
    applyCameraSelection();
}

// "Vista de cima": the key T or the Buy / Build panel's button (Live has none).
void toggleTopDownView() {
    if (gCameraSelector.toggleTopDown())
        applyCameraSelection();
}

// Buy mode over the lot's VM: the puck's mode buttons switch it (the live
// panel gives way to the Buy panel); --mode buy starts in it; headless,
// --buy <guid|name> picks a catalog item, --hold x,y[,dir[,level]] holds it
// there and --place x,y[,dir[,level]] buys it there (repeatable, in order),
// --grab <object id> picks up a placed object, --sell sells what is held,
// --undo undoes the last change.
void setupBuyMode(os2::Renderer& renderer, os2::ResourceIndex& index) {
    gBuy = std::make_unique<os2::BuyMode>(*gSimulation, *gObjectView, renderer, index, gameDir() / "TSData/Res");
    if (gLive)
        gLive->onMode = [](int mode) {
            if (mode == 1) {
                gBuy->enter();
                gLive->setMode(1);
                enterPlayMode(os2::CameraSelector::Mode::Buy);
            } else if (mode == 0) {
                gBuy->exit();
                releaseDayNight();
                gLive->setMode(0);
                enterPlayMode(os2::CameraSelector::Mode::Live);
            }
        };
    gBuy->pickObject = [](simd_float3 eye, simd_float3 dir) { return gPie ? gPie->pick(eye, dir) : os2::vm::ObjectId(0); };
    gBuy->onDayNight = [] { toggleDayNight(); };
    gBuy->onTopDown = [] { toggleTopDownView(); };
    gBuy->topDown = [] { return gCameraSelector.topDown(); };
    if (gAudio)
        gBuy->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
    if (startMode().value_or("") == "buy") {
        gBuy->enter();
        // ApplySavedUIState sets the floor after the mode: the saved one.
        if (gCameraFollow && gScene.lotUi)
            gBuy->setLevel(gCameraFollow->level() + 1);
        if (gLive)
            gLive->setMode(1);
        enterPlayMode(os2::CameraSelector::Mode::Buy);
    }
}

// "Alternar Dia/Noite" of Buy and Build mode (0xCC607035 / 0xAA): the lot
// lit as at night or by day, whatever the clock says, until the mode is left.
void toggleDayNight() {
    if (!gLighting)
        return;
    const std::string next = gLighting->stateName() == "night" ? "day" : "night";
    gLighting->forceState(next); // the next update() blends to it
    std::printf("opensims2: lighting: %s (day/night toggle)\n", next.c_str());
}

// Leaving Buy or Build mode: the clock's lighting again (or --lighting's).
void releaseDayNight() {
    if (gLighting)
        gLighting->forceState(argument(@"--lighting").value_or(""));
}

// The game's one undo manager over Buy and Build mode (app/undo_order.hpp,
// docs/buy-build-mac.md §7.24): a change in either can be undone from the
// other; entering Live mode empties it.
static os2::UndoOrder gUndoOrder;

// Build mode over the lot's VM: the puck's Build button (mode 2) or --mode
// build; headless steps in BuildMode::runSteps (build_mode.cpp).
void setupBuildMode(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape)
        return;
    gBuild = std::make_unique<os2::BuildMode>(*gSimulation, *gObjectView, renderer, index, gameDir() / "TSData/Res",
                                              *gLotShape);
    gBuild->onGeometryChanged = [&renderer, &index]() { rebuildLotModel(renderer, index); };
    if (gBuy) {
        gBuy->undoOrder = gBuild->undoOrder = &gUndoOrder;
        gUndoOrder.attach(os2::UndoOrder::Buy, {[] { return gBuy->undoOwn(); }, [] { return gBuy->redoOwn(); },
                                                [] { gBuy->dropRedo(); }, [] { gBuy->flushHistory(); }});
        gUndoOrder.attach(os2::UndoOrder::Build, {[] { return gBuild->undoOwn(); }, [] { return gBuild->redoOwn(); },
                                                  [] { gBuild->dropRedo(); }, [] { gBuild->flushHistory(); }});
    }
    gBuild->pickObject = [](simd_float3 eye, simd_float3 dir) { return gPie ? gPie->pick(eye, dir) : os2::vm::ObjectId(0); };
    gBuild->onDayNight = [] { toggleDayNight(); };
    gBuild->onTopDown = [] { toggleTopDownView(); };
    gBuild->topDown = [] { return gCameraSelector.topDown(); };
    gBuild->renderThumb = [&renderer, &index](const std::vector<os2::ThumbnailPart>& parts) {
        return os2::renderPartsThumbnail(renderer, index, parts, 94);
    };
    if (gAudio) { // the ui_build_* sounds and the terrain brush's loops
        gBuild->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
        gBuild->startLoop = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            o.loop = true;
            return uint32_t(gAudio->sound().play(name, o));
        };
        gBuild->stopLoop = [](uint32_t h) { gAudio->sound().stop(os2::SoundSystem::Handle(h)); };
    }
    if (gLive) {
        auto previous = gLive->onMode;
        gLive->onMode = [previous](int mode) {
            if (mode == 2) {
                if (gBuy)
                    gBuy->exit();
                gBuild->enter();
                gLive->setMode(2);
                enterPlayMode(os2::CameraSelector::Mode::Build);
                return;
            }
            gBuild->exit();
            if (previous)
                previous(mode);
            if (mode == 0) // entering Live mode: FlushCommandQueues (BV 0x159ff64)
                gUndoOrder.flush();
        };
    }
    if (startMode().value_or("") == "build") {
        gBuild->enter();
        if (gCameraFollow && gScene.lotUi)
            gBuild->setLevel(gCameraFollow->level() + 1);
        if (gLive)
            gLive->setMode(2);
        enterPlayMode(os2::CameraSelector::Mode::Build);
    }
}

void runBuildSteps() {
    if (!gBuild || !gBuild->active())
        return;
    std::vector<std::string> args;
    for (NSString* a in sceneArgs())
        args.push_back(a.UTF8String);
    gBuild->runSteps(args);
}

// The --buy/--hold/--place/--grab/--sell/--undo steps of a headless run, in order.
void runBuySteps() {
    if (!gBuy || !gBuy->active())
        return;
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i < args.count; ++i) {
        const std::string a = args[i].UTF8String;
        const std::string v = i + 1 < args.count ? args[i + 1].UTF8String : "";
        int x = 0, y = 0, dir = 2, level = 0;
        if (a == "--buy") {
            const os2::CatalogEntry* e = gBuy->catalog().find(v);
            if (!e)
                throw std::runtime_error("--buy: no catalog item \"" + v + "\"");
            gBuy->select(*e);
        } else if ((a == "--hold" || a == "--place") && std::sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &dir, &level) >= 2) {
            if (level > 0)
                gBuy->setLevel(level);
            if (a == "--hold")
                gBuy->holdAt(x, y, dir);
            else
                gBuy->placeAt(x, y, dir);
        } else if (float fx = 0, fy = 0; a == "--buy-drag" && std::sscanf(v.c_str(), "%d,%d,%f,%f", &x, &y, &fx, &fy) == 4) {
            // --buy-drag x,y,fx,fy: the press on tile x,y, a drag to the ground point fx,fy, the release.
            if (gBuy->pressAt(x, y)) {
                gBuy->aimAt(simd_make_float2(fx, fy));
                gBuy->mouseUp();
            }
        } else if (a == "--buy-cancel") { // Esc
            gBuy->cancel();
        } else if (a == "--grab") {
            gBuy->grab(os2::vm::ObjectId(std::stoi(v)));
        } else if (int id = 0; a == "--grab-drag" && std::sscanf(v.c_str(), "%d,%d,%d", &id, &x, &y) == 3) {
            gBuy->grabDrag(os2::vm::ObjectId(id), x, y); // --grab-drag id,x,y: press on it, drag to x,y, release
        } else if (a == "--eyedrop") {
            gBuy->eyedrop(os2::vm::ObjectId(std::stoi(v)));
        } else if (a == "--design") {
            // --design <id>[,row:option...][,accept|cancel]: the Design tool
            // on an object (the dialog by the middle of the screen), swatches
            // picked, then Accept or Cancel.
            std::vector<std::string> f;
            for (size_t k = 0, e; k <= v.size(); k = e + 1) {
                e = v.find(',', k);
                if (e == std::string::npos)
                    e = v.size();
                f.push_back(v.substr(k, e - k));
            }
            gBuy->setTool(os2::BuyMode::Tool::Design);
            // A name instead of an ID: the first object whose file name has it (a part with a model).
            os2::vm::ObjectId target = 0;
            if (!f[0].empty() && !std::isdigit(static_cast<unsigned char>(f[0][0]))) {
                for (const auto& [id, o] : gSimulation->machine().objects())
                    if (o.definition && std::string(o.definition->fileName).find(f[0]) != std::string::npos && !o.person) {
                        target = id;
                        break;
                    }
            } else {
                target = os2::vm::ObjectId(std::stoi(f[0]));
            }
            if (gBuy->design(target, simd_make_float2(560, 420)))
                for (size_t k = 1; k < f.size(); ++k) {
                    int row = 0, option = 0;
                    if (f[k] == "accept")
                        gBuy->designTool().accept();
                    else if (f[k] == "cancel")
                        gBuy->designTool().cancel();
                    else if (std::sscanf(f[k].c_str(), "%d:%d", &row, &option) == 2)
                        gBuy->designTool().choose(row, size_t(option));
                }
        } else if (a == "--day-night") {
            if (gBuy->onDayNight)
                gBuy->onDayNight();
        } else if (a == "--sell") {
            gBuy->deleteHeld();
        } else if (a == "--undo") {
            gBuy->undo();
        }
    }
    gBuy->update();
}

} // namespace os2app
