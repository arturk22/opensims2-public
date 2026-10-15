// The app delegate's lot scene: prepareLot (the camera, the test objects,
// the skinned Sim, --ui, the lot and the pivot; may run on the loading
// worker) and finishLot on the main thread, in its phases: the panels
// attached, --pie, the headless steps, OS2_PICK_MAP, the Buy/Build steps
// and --drag/--hover, --cinematic, the test aids' lists and the screenshots.
// A category of OS2AppDelegate (app_delegate.h).

#import "app/app_delegate.h"

using namespace os2app;

@implementation OS2AppDelegate (LotScene)

// A lot (--lot, or --hood <N> --hood-lot <id>), or the test objects and Sims.
- (void)prepareLot {
    OS2View* view = _view;
    // (--camera-scale <k>: every zoom distance times k, close-ups of Sims below the closest level.)
    os2::CameraDef liveCamera = loadLiveCamera();
    [view setCamera:std::make_unique<os2::LotCamera>(liveCamera)];
    gCamera = view.camera;
    os2::ResourceIndex& index = *gIndex;
    { // (indented as it was inside the start-up's try block)
        const simd_float3 objects = loadObjects(*_viewDelegate.renderer, index);
        perfLap("loadObjects");
        const auto skinned = loadSkinned(*_viewDelegate.renderer, index);
        perfLap("loadSkinned");
        const auto uiCanvas = loadUi(*_viewDelegate.renderer, index, view);
        perfLap("loadUi");
        // --zoom <levels> / --orientation <steps>: start the camera that many steps from its default.
        if (auto z = argument(@"--zoom"))
            view.camera->zoomBy(std::stoi(*z));
        if (auto o = argument(@"--orientation"))
            view.camera->rotateBy(std::stoi(*o));
        // --tilt <degrees>: the pitch moved by that much, inside the zoom
        // level's range (negative: lower, toward a side view).
        if (auto t = argument(@"--tilt"))
            view.camera->tilt(std::stof(*t));
        view.camera->snap();
        // The lot's cutaway walls follow the starting view direction.
        const simd_float3 look = view.camera->pivot() - view.camera->eye();
        simd_float3 pivot = loadLot(*_viewDelegate.renderer, index, simd_make_float2(look.x, look.y))
                                .value_or(skinned.value_or(objects));
        // --pivot <x,y[,z]>: look at that point instead (tiles; at the same
        // height, or z: the floor the camera is on follows it).
        float px, py, pz;
        if (auto p = argument(@"--pivot")) {
            const int n = std::sscanf(p->c_str(), "%f,%f,%f", &px, &py, &pz);
            if (n >= 2)
                pivot.x = px, pivot.y = py;
            if (n == 3)
                pivot.z = pz;
        }
        view.camera->setPivot(pivot);
        view.camera->snap();
        _uiCanvas = uiCanvas;
        _pivot = pivot;
    }
}

// The lot's panels on the view's device, drawn over the scene in the
// renderer's overlay (the pie menu last, over everything).
- (void)attachLotPanels {
    OS2View* view = _view;
    gPie->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                 uint32_t(view.depthStencilPixelFormat));
    if (gLive)
        gLive->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                      uint32_t(view.depthStencilPixelFormat));
    if (gBuy)
        gBuy->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                     uint32_t(view.depthStencilPixelFormat));
    if (gBuild)
        gBuild->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                       uint32_t(view.depthStencilPixelFormat));
    if (gOptions)
        gOptions->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                         uint32_t(view.depthStencilPixelFormat));
    if (gTutorial)
        gTutorial->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                          uint32_t(view.depthStencilPixelFormat));
    _viewDelegate.renderer->setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gUi)
            gUi->encode(enc, w, h);
        const bool optionsOver = gLive && gLive->optionsOpen(); // the options replace Buy/Build's panel
        if (gBuy && !optionsOver) // under the puck
            gBuy->encode(enc, w, h, gUiScale);
        if (gBuild && !optionsOver)
            gBuild->encode(enc, w, h, gUiScale);
        if (gOptions) // under the puck, as Buy mode's panel (its tab rises behind the puck)
            gOptions->encode(enc, w, h, gUiScale);
        if (gLive)
            gLive->encode(enc, w, h, gUiScale);
        if (gTutorial) // the tutorial's window and arrow over the panels
            gTutorial->encode(enc, w, h, gUiScale);
        gPie->encode(enc, w, h, gUiScale);
    });
}

// --pie <object> [--pie-select N]: open an object's pie menu for the
// selected Sim (--sim-id <id|name>) and queue entry N; --click x,y
// (repeatable) clicks like the mouse. Before --run-vm's ticks.
- (void)openPieFromFlags:(float)cw height:(float)ch pivot:(simd_float3)pivot {
    OS2View* view = _view;
    if (auto q = argument(@"--pie")) {
        os2::vm::ObjectId id = os2::vm::findInteractionObject(gPie->vm(), *q);
        if (!id && !q->empty()) // a Sim by its character name (N001_User00005)
            id = os2::vm::findSim(gPie->vm(), *q);
        auto at = id ? gPie->anchor(id) : std::nullopt;
        if (id && !at) // a Sim: its pick box comes with the first frame; until then where it stands, chest high
            for (const auto& s : gLotSims)
                if (os2::vm::ObjectId(s.id) == id && s.actor) {
                    const os2::Vec3 sp = s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                                        s.actor->transform->t[2]}
                                                            : s.actor->position;
                    at = simd_make_float3(sp[0], sp[1], sp[2] + 1.2f);
                }
        if (!at)
            throw std::runtime_error("--pie: no drawn object \"" + *q + "\" with interactions");
        if (!argument(@"--pivot")) {
            view.camera->setPivot(simd_make_float3(at->x, at->y, pivot.z));
            view.camera->snap();
        }
        const simd_float4 c = simd_mul(simd_mul(view.camera->projection(cw / ch), view.camera->view()),
                                       simd_make_float4(at->x, at->y, at->z, 1));
        const simd_float2 p = simd_make_float2((c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
        if (!gPie->open(id, p))
            std::printf("opensims2: object %d: no interactions available\n", id);
        for (size_t i = 0; i < gPie->entries().size(); ++i)
            std::printf("opensims2: pie [%zu] %s\n", i, gPie->entries()[i].name.c_str());
        if (auto sel = argument(@"--pie-select"); sel && !gPie->select(std::stoul(*sel)))
            throw std::runtime_error("--pie-select: no entry " + *sel);
        // With --run-vm the menu closes after the pick, as a click does; else it stays, highlighted.
        if (argument(@"--pie-select") && argument(@"--run-vm"))
            gPie->close();
    }
}

// The --click / --right-click / --wait steps, in order (app/game_flow.hpp):
// a step that leaves the lot leaves the rest to the next scene.
- (void)runLotSteps:(float)cw height:(float)ch {
    OS2View* view = _view;
    const auto steps = os2::flowSteps(processArguments());
    followSelectedSim(); // the camera on the Sim's floor first, as the first frame does (a floor button then holds)
    if (!steps.empty()) // and there already, as the frames before a click leave it (the clicks pick what the shot shows)
        view.camera->snap();
    for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
        gStepsUsed = k + 1;
        const os2::FlowStep& st = steps[k];
        if (st.kind == os2::FlowStep::Kind::Wait) {
            runTicksHeadless(st.ticks, _viewDelegate.renderer);
            pollFlowDialog();
            continue;
        }
        if (st.kind == os2::FlowStep::Kind::Key) { // --key space
            if (st.text.rfind("hold-", 0) == 0) { // --key hold-<left|right|up|down|a|d|w|s|comma|period>:<seconds>
                static const std::map<std::string, unsigned short> kKeys{
                    {"left", kLeft}, {"right", kRight}, {"up", kUp}, {"down", kDown}, {"a", kA},
                    {"d", kD},       {"w", kW},         {"s", kS},   {"comma", kComma}, {"period", kPeriod},
                    {"q", kQ},       {"e", kE}};
                const size_t colon = st.text.find(':');
                const std::string name = st.text.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
                const double secs = colon == std::string::npos ? 0.5 : std::atof(st.text.c_str() + colon + 1);
                if (auto k = kKeys.find(name); k != kKeys.end()) {
                    const simd_float3 p0 = view.camera->pivot();
                    const float y0 = view.camera->yaw();
                    [view holdKey:k->second seconds:secs];
                    const simd_float3 p1 = view.camera->pivot();
                    if (freeCameraActive())
                        std::printf("opensims2: key %s held %.2f s: free camera at (%.2f, %.2f, %.2f), heading %.1f, pitch %.1f\n",
                                    name.c_str(), secs, gFreeCamera->position().x, gFreeCamera->position().y,
                                    gFreeCamera->position().z, gFreeCamera->heading() * 57.29578f, gFreeCamera->pitch() * 57.29578f);
                    else
                    std::printf("opensims2: key %s held %.2f s: pivot (%.2f, %.2f) -> (%.2f, %.2f), yaw %.1f -> %.1f\n",
                                name.c_str(), secs, p0.x, p0.y, p1.x, p1.y, y0, view.camera->yaw());
                }
                continue;
            }
            if (st.text == "space")
                selectNextFamilySim();
            if (st.text == "tab" || st.text == "y") { // "Cycle Camera Controller": the free camera, and back
                cycleCameraController();
                if (gCamera)
                    gCamera->snap();
                std::printf("opensims2: key %s: camera %s\n", st.text.c_str(),
                            gCameraSelector.freeCamera() ? "CameraMan" : gCamera ? gCamera->definition().name.c_str() : "-");
            }
            if (st.text.rfind("look:", 0) == 0 && freeCameraActive()) { // --key look:<dx>,<dy>: the mouse's points
                float dx = 0, dy = 0;
                std::sscanf(st.text.c_str() + 5, "%f,%f", &dx, &dy);
                gFreeCamera->look(dx, dy);
                [view applyHeldKeys:0.05];
            }
            if (st.text.rfind("fov:", 0) == 0 && freeCameraActive()) { // --key fov:<+n|-n>: steps of the fov table
                const int n = std::atoi(st.text.c_str() + 4);
                for (int i = 0; i < std::abs(n); ++i)
                    gFreeCamera->zoomStep(n > 0 ? 1 : -1);
                for (int i = 0; i < 60; ++i)
                    [view applyHeldKeys:0.05];
                std::printf("opensims2: key %s: fov %.1f\n", st.text.c_str(), gFreeCamera->fov());
            }
            if (st.text == "t") { // as keyDown: the top-down view (Buy and Build only)
                toggleTopDownView();
                if (gCamera)
                    gCamera->snap();
                std::printf("opensims2: key t: camera %s, pitch %.1f, distance %.1f\n",
                            gCamera ? gCamera->definition().name.c_str() : "-", gCamera ? gCamera->pitch() : 0.0f,
                            gCamera ? gCamera->distance() : 0.0f);
            }
            if (st.text.rfind("walls-", 0) == 0) // --key walls-<0..3>: the puck's wall fan (down, cutaway, up, roof)
                setWallMode(std::atoi(st.text.c_str() + 6));
            if (st.text == "pageup" || st.text == "pagedown") { // as keyDown: the floor shown
                changeFloor(st.text == "pageup" ? 1 : -1);
                if (gCamera) // the camera on that floor now, as the frames before a click would leave it
                    gCamera->snap();
                std::printf("opensims2: key %s: level shown %d\n", st.text.c_str(), viewLevel());
            }
            if (st.text == "escape") { // as keyDown: a pie menu or an option window first, else leave the lot
                const bool pieWasOpen = gPie && gPie->isOpen();
                if (pieWasOpen)
                    gPie->close();
                std::printf("opensims2: key escape (pie %s, option window %d)\n", pieWasOpen ? "closed" : "was not open",
                            gOptions ? gOptions->window() : 0);
                if (pieWasOpen)
                    continue;
                if (escapeOptions())
                    continue;
                if (gHoodLot && !(gPie && gPie->dialogOpen()))
                    leaveLot();
            }
            continue;
        }
        if (st.kind == os2::FlowStep::Kind::Drag || st.kind == os2::FlowStep::Kind::Hover)
            continue; // after the build steps (below)
        if (st.kind == os2::FlowStep::Kind::TutorialAuto) { // --tutorial-auto <turns>
            // Each turn: a click on what the arrow points at or Next;
            // else what a step waiting on the Sim waits for (a Go
            // Here, an object bought, a job: vm::tutorialAutoAct);
            // else time passes (the first step shows after some
            // ticks, a skill grows). Stops after ~3000 idle ticks.
            int idle = 0;
            // The trees' own dialogs (the job offers) get their first
            // button, as s2pkg tutorial --auto, unless --dialog-answer says.
            int& autoAnswer = gPie->vm().autoDialogAnswer;
            const int answerBefore = autoAnswer;
            if (autoAnswer < 0)
                autoAnswer = 0;
            for (long turn = 0; gTutorial && turn < st.ticks && !gPendingScene && idle < 50; ++turn) {
                if (st.entry > 0 && gPie->vm().tutorial.progress.visible && gPie->vm().tutorial.progress.step >= st.entry)
                    break; // --tutorial-auto <turns>:<step>
                gTutorial->setCanvas(int(cw), int(ch));
                gLive->setCanvas(int(cw), int(ch));
                if (gBuy)
                    gBuy->setCanvas(int(cw), int(ch));
                if (gBuild)
                    gBuild->setCanvas(int(cw), int(ch));
                pollTutorial(gHeadlessClock);
                if (gTutorial->autoClick()) {
                    idle = 0;
                    runTicksHeadless(40);
                    continue;
                }
                if (idle % 10 == 1)
                    if (const std::string done = os2::vm::tutorialAutoAct(gPie->vm(), gIndex.get()); !done.empty())
                        std::printf("opensims2: tutorial: %s\n", done.c_str());
                ++idle;
                runTicksHeadless(60);
            }
            autoAnswer = answerBefore;
            continue;
        }
        if (st.kind == os2::FlowStep::Kind::Pie) { // --pie-step <object>:<entry index or name>
            const os2::vm::ObjectId id = os2::vm::findInteractionObject(gPie->vm(), st.object);
            gPie->setCanvas(int(cw), int(ch));
            if (!id || !gPie->open(id, simd_make_float2(cw / 2, ch / 2))) {
                std::printf("opensims2: pie step: no pie menu on \"%s\"\n", st.object.c_str());
                continue;
            }
            if (st.text == "open") { // --pie-step <object>:open leaves the menu open for the shot
                auto at = gPie->anchor(id);
                if (!at) // a Sim: where it is drawn, chest high (as --pie)
                    for (const auto& s : gLotSims)
                        if (os2::vm::ObjectId(s.id) == id && s.actor) {
                            const os2::Vec3 sp = s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                                                s.actor->transform->t[2]}
                                                                    : s.actor->position;
                            at = simd_make_float3(sp[0], sp[1], sp[2] + 0.6f);
                        }
                if (at) { // over the object, the camera on it (as --pie)
                    view.camera->setPivot(simd_make_float3(at->x, at->y, view.camera->pivot().z));
                    view.camera->snap();
                    const simd_float4 c = simd_mul(simd_mul(view.camera->projection(cw / ch), view.camera->view()),
                                                   simd_make_float4(at->x, at->y, at->z, 1));
                    gPie->close();
                    gPie->open(id, simd_make_float2((c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch));
                }
                for (size_t e = 0; e < gPie->entries().size(); ++e)
                    std::printf("opensims2: pie step: [%zu] %s\n", e, gPie->entries()[e].name.c_str());
                continue;
            }
            bool picked = false;
            for (size_t e = 0; e < gPie->entries().size() && !picked; ++e)
                if (st.entry >= 0 ? e == size_t(st.entry) : gPie->entries()[e].name.find(st.text) != std::string::npos) {
                    std::printf("opensims2: pie step: %s on object %d\n", gPie->entries()[e].name.c_str(), id);
                    picked = gPie->select(e);
                }
            if (!picked)
                for (size_t e = 0; e < gPie->entries().size(); ++e)
                    std::printf("opensims2: pie step: [%zu] %s (not \"%s\")\n", e, gPie->entries()[e].name.c_str(), st.text.c_str());
            gPie->close();
            continue;
        }
        const float clickX = st.x, clickY = st.y;
        // --right-click x,y: the right button (the live panel's thumbnails follow a Sim).
        const bool right = st.kind == os2::FlowStep::Kind::RightClick;
        const simd_float2 p = simd_make_float2(clickX, clickY);
        gPie->setCanvas(int(cw), int(ch));
        if (gTutorial && !right) { // the tutorial's window (top right) first
            gTutorial->setCanvas(int(cw), int(ch));
            gLive->setCanvas(int(cw), int(ch)); // (its click trees find the controls laid out)
            if (gBuy)
                gBuy->setCanvas(int(cw), int(ch));
            if (gBuild)
                gBuild->setCanvas(int(cw), int(ch));
            if (gTutorial->press(p))
                continue;
        }
        if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p)) {
            pollFlowDialog();
            continue;
        }
        if (!right && pressOptions(p, cw, ch)) // the option windows float over everything
            continue;
        if (gLive && !right) {
            gLive->setCanvas(int(cw), int(ch));
            if (gLive->overModeButton(p) && gLive->press(p)) {
                gLive->screen(int(cw), int(ch)); // no frame lays it out again before the next step
                continue;
            }
        }
        if (gLive && !right && gLive->optionsOpen() && gLive->press(p)) { // over Buy/Build's panels
            gLive->screen(int(cw), int(ch));
            continue;
        }
        if (gBuy && gBuy->active() && !(gLive && gLive->optionsOpen())) {
            gBuy->setCanvas(int(cw), int(ch));
            if (gBuy->press(p))
                continue;
        }
        if (gBuild && gBuild->active() && !(gLive && gLive->optionsOpen())) {
            gBuild->setCanvas(int(cw), int(ch));
            if (gBuild->press(p))
                continue;
        }
        if (gLive) {
            gLive->setCanvas(int(cw), int(ch));
            const bool onPanel = gLive->press(p, right);
            if (right && !onPanel && gPie) { // a right click on a Sim of the family selects it
                const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
                const simd_float4 q =
                    simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / cw - 1, 1 - 2 * p.y / ch, 0.5f, 1));
                updateLotView(*_viewDelegate.renderer, true);
                selectFamilySim(gPie->pick(view.camera->eye(), simd_normalize(q.xyz / q.w - view.camera->eye())));
            }
            if (onPanel || right) {
                gLive->screen(int(cw), int(ch)); // no frame lays it out again before the next step
                if (gOptions && !gLive->optionsOpen())
                    gOptions->closeWindow();
                continue;
            }
        }
        if (pressOptions(p, cw, ch)) // under the puck
            continue;
        const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
        const simd_float4 q =
            simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / cw - 1, 1 - 2 * p.y / ch, 0.5f, 1));
        const simd_float3 eye = view.camera->eye();
        if (gBuy && gBuy->active()) { // the object tools, not the pie menu
            gBuy->click(eye, simd_normalize(q.xyz / q.w - eye), p);
            continue;
        }
        // The view as a frame would leave it (what the cut walls hide), the Sims posed where they stand.
        if (gSimulation)
            poseActors(*_viewDelegate.renderer, gHeadlessClock);
        updateLotView(*_viewDelegate.renderer, false);
        if (gObjectView)
            gObjectView->update();
        updateLotView(*_viewDelegate.renderer, true);
        keepSelectedSimHome();
        const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
        const os2::vm::Object* o = gPie->vm().object(id);
        std::printf("opensims2: click (%g, %g) picked object %d (%s)\n", clickX, clickY, id,
                    o && o->definition ? o->definition->fileName.c_str() : "nothing");
        if (std::getenv("OS2_DEBUG_PICK"))
            std::printf("opensims2: click eye (%.3f, %.3f, %.3f) canvas %gx%g\n", eye.x, eye.y, eye.z, cw, ch);
        if (id)
            openClicked(id, eye, simd_normalize(q.xyz / q.w - eye), p);
        if (!id) // the ground: Go Here
            openGoHere(eye, simd_normalize(q.xyz / q.w - eye), p);
        for (size_t e = 0; e < gPie->entries().size(); ++e)
            std::printf("opensims2: pie [%zu] %s\n", e, gPie->entries()[e].name.c_str());
    }
}

// OS2_PICK_MAP=<png>: what a click would pick at every 4th pixel (OS2_PICK_MAP_STEP), an
// object a colour (a look at the picking against the picture).
- (void)writePickMap:(float)cw height:(float)ch {
    OS2View* view = _view;
    if (const char* mapFile = std::getenv("OS2_PICK_MAP")) {
        if (gSimulation) // the Sims posed as drawn (their triangles are picked)
            poseActors(*_viewDelegate.renderer, gHeadlessClock);
        updateLotView(*_viewDelegate.renderer, false);
        if (gObjectView)
            gObjectView->update();
        updateLotView(*_viewDelegate.renderer, true);
        const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
        const simd_float4x4 inv = simd_inverse(m);
        const simd_float3 eye = view.camera->eye();
        os2::RgbaImage img;
        img.width = uint32_t(cw), img.height = uint32_t(ch);
        img.pixels.assign(size_t(img.width) * img.height * 4, 0);
        const CFTimeInterval t0 = CACurrentMediaTime();
        size_t rays = 0;
        std::map<int, int> findIds;
        if (const char* f = std::getenv("OS2_PICK_FIND"))
            for (std::string_view v{f}; !v.empty();) {
                const size_t c = v.find(',');
                findIds[std::atoi(std::string(v.substr(0, c)).c_str())] = 0;
                v = c == std::string_view::npos ? std::string_view{} : v.substr(c + 1);
            }
        const uint32_t step = std::getenv("OS2_PICK_MAP_STEP") ? uint32_t(std::max(1, std::atoi(std::getenv("OS2_PICK_MAP_STEP")))) : 4;
        for (uint32_t y = 0; y < img.height; y += step)
            for (uint32_t x = 0; x < img.width; x += step) {
                const simd_float4 q = simd_mul(inv, simd_make_float4(2 * float(x) / cw - 1, 1 - 2 * float(y) / ch, 0.5f, 1));
                const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
                ++rays;
                if (!id)
                    continue;
                if (findIds.count(int(id)) && findIds[int(id)]++ == 0) // OS2_PICK_FIND=id,id: where each is picked first
                    std::printf("opensims2: pick map: object %d at (%u, %u)\n", id, x, y);
                const uint32_t h = uint32_t(id) * 2654435761u;
                for (uint32_t dy = 0; dy < step && y + dy < img.height; ++dy)
                    for (uint32_t dx = 0; dx < step && x + dx < img.width; ++dx) {
                        uint8_t* px = &img.pixels[(size_t(y + dy) * img.width + x + dx) * 4];
                        px[0] = uint8_t(h >> 24), px[1] = uint8_t(h >> 16), px[2] = uint8_t(h >> 8), px[3] = 255;
                    }
            }
        std::printf("opensims2: pick map: eye (%.3f, %.3f, %.3f) canvas %gx%g\n", eye.x, eye.y, eye.z, cw, ch);
        for (const auto& [fid, n] : findIds) // where each one's origin projects
            if (gObjectView)
                if (const auto at = gObjectView->placement(int16_t(fid))) {
                    const simd_float4 c = simd_mul(m, simd_make_float4(at->t[0], at->t[1], at->t[2], 1));
                    std::printf("opensims2: pick map: object %d origin (%.2f, %.2f, %.2f) on screen (%.1f, %.1f)\n", fid, at->t[0], at->t[1], at->t[2],
                                (c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
                }
        std::printf("opensims2: pick map: %zu rays, %.3f ms each\n", rays, (CACurrentMediaTime() - t0) * 1000 / double(rays));
        os2::writePng(mapFile, img);
    }
}

// --ui-hover, then Buy and Build mode's steps (runBuySteps, runBuildSteps)
// and Build mode's --drag / --hover, after its tool is chosen.
- (void)runLotPointerSteps:(float)cw height:(float)ch {
    OS2View* view = _view;
    // --ui-hover x,y: the mouse over the live panel (a queue icon's name).
    float hx, hy;
    if (auto h = argument(@"--ui-hover"); gLive && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
        gLive->setCanvas(int(cw), int(ch));
        gLive->hover(simd_make_float2(hx, hy));
    }
    runBuySteps();
    runBuildSteps();
    // --drag / --hover: Build mode's mouse on the lot, after its tool is chosen.
    for (const os2::FlowStep& st : os2::flowSteps(processArguments()))
        [&] {
            if (st.kind != os2::FlowStep::Kind::Drag && st.kind != os2::FlowStep::Kind::Hover)
                return;
            // --drag x0,y0,x1,y1[,ctrl|hold] / --hover x,y: Build mode's mouse on the lot.
            if (!gBuild || !gBuild->active())
                return;
            const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
            const simd_float3 eye = view.camera->eye();
            auto ray = [&](float x, float y) {
                const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * x / cw - 1, 1 - 2 * y / ch, 0.5f, 1));
                return simd_normalize(q.xyz / q.w - eye);
            };
            auto where = [&](const char* what) {
                if (auto p = gBuild->pointer()) {
                    std::printf("opensims2: build mouse %s at lot (%.2f, %.2f)\n", what, p->x, p->y);
                    if (std::getenv("OS2_DEBUG_PICK")) {
                        const simd_float3 e = view.camera->eye();
                        std::printf("opensims2: eye (%.3f, %.3f, %.3f) pivot (%.3f, %.3f, %.3f)\n", e.x, e.y, e.z,
                                    view.camera->pivot().x, view.camera->pivot().y, view.camera->pivot().z);
                        for (simd_float3 w : {simd_make_float3(p->x, p->y, gBuild->pickHeight(*p))}) { // where the cursor is drawn
                            const simd_float4 c = simd_mul(m, simd_make_float4(w, 1));
                            std::printf("opensims2: world (%.1f, %.1f, %.1f) on screen (%.1f, %.1f)\n", w.x, w.y, w.z,
                                        (c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
                        }
                    }
                }
            };
            gBuild->setCanvas(int(cw), int(ch));
            if (st.kind == os2::FlowStep::Kind::Hover) {
                gBuild->pointAt(eye, ray(st.x, st.y));
                where("over");
            } else {
                gBuild->mouseDown(eye, ray(st.x, st.y), st.control, false);
                where("down");
                gBuild->mouseDrag(eye, ray(st.x1, st.y1));
                where("dragged");
                if (!st.hold) {
                    gBuild->mouseUp(eye, ray(st.x1, st.y1));
                    std::printf("opensims2: build drag: %s\n", gBuild->message().c_str());
                }
            }
            gBuild->update();
            return;
        }();
    if (auto h = argument(@"--ui-hover"); gBuy && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
        gBuy->setCanvas(int(cw), int(ch));
        gBuy->hover(simd_make_float2(hx, hy));
    }
    if (auto h = argument(@"--ui-hover"); gBuild && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
        gBuild->setCanvas(int(cw), int(ch));
        gBuild->hover(simd_make_float2(hx, hy));
    }
}

// --cinematic <scene>[@id[,id...]][:mirror][:house]: a cinematic with those actors (in the CINE's
// order; default: the lot's Sims for its Sim actors), started at once.
- (void)startCinematicFromFlags {
    if (auto cine = argument(@"--cinematic")) {
        std::string spec = *cine;
        bool mirror = false, house = false;
        if (spec.ends_with(":house")) // "Show Entire House" (flag 0x08)
            house = true, spec.resize(spec.size() - 6);
        if (spec.ends_with(":mirror"))
            mirror = true, spec.resize(spec.size() - 7);
        const auto at = spec.find('@');
        const std::string scene = spec.substr(0, at);
        std::vector<int16_t> actors;
        if (at != std::string::npos) {
            std::stringstream ids(spec.substr(at + 1));
            for (std::string id; std::getline(ids, id, ',');)
                actors.push_back(int16_t(std::stoi(id)));
        } else if (gIndex) {
            try {
                const os2::CinematicCamera probe(*gIndex, scene);
                size_t next = 0;
                for (const auto& a : probe.scene().actors) {
                    const bool sim = a.sgObjectName.find("skel") != std::string::npos ||
                                     a.sgObjectName.rfind("sim", 0) == 0;
                    int16_t id = sim && !a.autoCreate && next < gLotSims.size() ? gLotSims[next++].id : 0;
                    // An object actor ("mailbox::1"): a lot object of that model.
                    if (!sim && !a.autoCreate) {
                        const std::string model = a.sgObjectName.substr(0, a.sgObjectName.find(':'));
                        for (const auto& [oid, p] : gPlacedObjects)
                            if (!id && !p.contained && !model.empty() &&
                                strncasecmp(p.cres.c_str(), model.c_str(), model.size()) == 0)
                                id = oid;
                    }
                    actors.push_back(id);
                }
            } catch (const std::exception& err) {
                os2::logOnce("cinematic actors", scene, err);
            }
        }
        startCinematic(*_viewDelegate.renderer, scene, actors, gLotSims.empty() ? 0 : gLotSims.front().id, mirror, true,
                       std::numeric_limits<double>::infinity(), true, house); // starts at the first update
    }
}

// Test aids: OS2_LIST_OBJECTS=<name> lists those objects and where they
// are, OS2_LIST_QUEUES=1 each Sim's queue.
- (void)listLotTestAids {
    if (const char* want = std::getenv("OS2_LIST_OBJECTS"); want && gSimulation) // test aid: objects by name, where they are
        for (const auto& [id, o] : gSimulation->machine().objects())
            if (o.definition && o.definition->fileName.find(want) != std::string::npos)
                std::printf("opensims2: object %d (%s) at %.1f %.1f level %d, container %d\n", id,
                            o.definition->fileName.c_str(), o.position.x, o.position.y, o.position.level,
                            o.data[os2::vm::objdata::ContainerId]);
    if (std::getenv("OS2_LIST_QUEUES") && gSimulation) // test aid: each Sim's queue
        for (const auto& [id, o] : gSimulation->machine().objects())
            if (o.person) {
                std::printf("opensims2: Sim %d queue (active %d):", id, o.activeInteraction);
                for (const auto& q : o.queue)
                    std::printf(" [%s obj %d prio %d%s]", q.name.c_str(), q.object, q.priority, q.cancelled ? " cancelled" : "");
                std::printf("\n");
            }
}

// --screenshot <file.png>: render one frame offscreen, save it and quit.
- (void)takeLotScreenshots:(const std::optional<std::string>&)shot width:(uint32_t)shotW height:(uint32_t)shotH {
    OS2View* view = _view;
    const auto uiCanvas = _uiCanvas;
    const double base = gHeadlessClock; // after the --wait steps
    // --run-vm <ticks>: the moment after that many VM ticks; --frames
    // <n>: n shots spread up to it (out-1.png ... out-n.png).
    const auto runVm = argument(@"--run-vm");
    const double time = std::stod(argument(@"--time").value_or(
        runVm ? std::to_string(std::stod(*runVm) / os2::LotSimulation::kTicksPerSecond) : std::string("0")));
    const int frames = std::max(1, std::stoi(argument(@"--frames").value_or("1")));
    poseActors(*_viewDelegate.renderer, base);
    std::vector<simd_float2> dialogClicks;
    size_t dialogClick = 0;
    std::string dialogTyped;
    NSArray<NSString*>* shotArgs = sceneArgs();
    for (NSUInteger i = 1; i + 1 < shotArgs.count; ++i) {
        float x, y;
        if ([shotArgs[i] isEqualToString:@"--dialog-click"] && std::sscanf(shotArgs[i + 1].UTF8String, "%f,%f", &x, &y) == 2)
            dialogClicks.push_back(simd_make_float2(x, y));
        else if ([shotArgs[i] isEqualToString:@"--dialog-click"] && [shotArgs[i + 1] isEqualToString:@"return"])
            dialogClicks.push_back(simd_make_float2(-1, -1)); // the Return key
        else if ([shotArgs[i] isEqualToString:@"--dialog-click"] && [shotArgs[i + 1] isEqualToString:@"escape"])
            dialogClicks.push_back(simd_make_float2(-2, -2)); // the Esc key
        if ([shotArgs[i] isEqualToString:@"--dialog-type"])
            dialogTyped = shotArgs[i + 1].UTF8String;
    }
    uint32_t dialogTypedFor = 0;
    double perfSim = 0, perfCapture = 0, perfWorst = 0;
    // --frames-from <seconds>: the shots spread from that moment instead of the start
    // (the VM still runs from the start: a scene late in a long run).
    const double from = std::clamp(std::stod(argument(@"--frames-from").value_or("0")), 0.0, time);
    double previous = 0;
    for (int f = 1; f <= frames; ++f) {
        const CFTimeInterval frameStart = CACurrentMediaTime();
        const double at = frames > 1 && from > 0 ? from + (time - from) * (f - 1) / (frames - 1) : time * f / frames;
        // The VM in small steps, so that the Sims' turning is followed.
        for (double t = previous + 1.0 / 30; gSimulation && t < at; t += 1.0 / 30) {
            const double scene = gSimulation->update(base + t); // the scene clock (game speed)
            adoptVmSims(*_viewDelegate.renderer);            // a Sim the VM just made (its actor for the scene)
            updateCinematic(*_viewDelegate.renderer, scene); // a scene starts when the tree asks
            // The lights follow the VM clock as in a live run (the
            // day/night switch and its crossfade start when the clock gets there).
            if (gLighting && gSimulation->ticks() % 30 == 0)
                gLighting->update(scene);
            if (gLive)
                gLive->tick(base + t);
            // The Sims' bones at this step, so that balloons and other
            // effects on their bones follow the animations between shots.
            for (auto& sim : gLotSims)
                sim.actor->update(scene);
            for (os2::SimActor* a : simsReachingLaterSims())
                a->update(scene);
            // The objects too, as a live frame does (poseActors): a car
            // driving in, a chair pulled out, placed where the VM view
            // puts them, so that the seats on them are not left where
            // the last shot had them (the maid's seat stayed at the
            // road's end while the car came in).
            for (auto& [id, a] : gObjectActors)
                if (a)
                    a->update(scene);
            // OS2_DEBUG_LAYERS=1: each game hour, the object actor with
            // the most animation layers (the held ones merge: AnimPlayer).
            if (std::getenv("OS2_DEBUG_LAYERS") && gSimulation && gSimulation->ticks() % 1800 == 0) {
                uint64_t& lastHour = gScene.layersReportedAt;
                if (gSimulation->ticks() != lastHour) {
                    lastHour = gSimulation->ticks();
                    int most = -1;
                    size_t count = 0, total = 0;
                    for (const auto& [id, a] : gObjectActors)
                        if (a) {
                            total += a->player.layerCount();
                            if (a->player.layerCount() > count || most < 0) {
                                count = a->player.layerCount();
                                most = id;
                            }
                        }
                    std::printf("opensims2: layers: tick %llu most %zu (object %d), %zu in %zu object actors\n",
                                (unsigned long long)lastHour, count, most, total, gObjectActors.size());
                }
            }
            if (gObjectView)
                gObjectView->update();
            sampleDrawnPlaces(scene);
            // OS2_DEBUG_DOORS=1: the doors' leaves (r_door_rot, l_door_rot
            // about Z, degrees) as they turn by 10 degrees or come back closed.
            if (std::getenv("OS2_DEBUG_DOORS")) {
                auto& last = gScene.doorLeaves; // (by object ID: per scene)
                for (auto& [id, actor] : gObjectActors) {
                    if (!actor)
                        continue;
                    const int r = actor->skin.skeleton.find("r_door_rot"), l = actor->skin.skeleton.find("l_door_rot");
                    if (r < 0)
                        continue;
                    // OS2_DEBUG_DOORS=2: as drawn (the last update()'s bones, the
                    // leaf's turn from its parent), without starting the cues here.
                    static const bool drawn = std::string(std::getenv("OS2_DEBUG_DOORS")) == "2";
                    std::vector<os2::Xform> pose;
                    if (drawn) {
                        if (actor->bones.size() != actor->skin.skeleton.bones.size())
                            continue;
                        pose = actor->bones;
                        for (size_t b = 0; b < pose.size(); ++b)
                            if (const int parent = actor->skin.skeleton.bones[b].parent; parent >= 0)
                                pose[b] = actor->bones[size_t(parent)].inverse().then(actor->bones[b]);
                    } else {
                        actor->startCues(scene);
                        pose = actor->player.pose(scene);
                    }
                    auto ang = [&](int b) { return b < 0 ? 0.0f : 2 * std::atan2(pose[size_t(b)].q.z, pose[size_t(b)].q.w) * 57.2958f; };
                    const std::pair<float, float> a{ang(r), ang(l)};
                    auto& prev = last[id];
                    if (std::abs(prev.first - a.first) > 10 || std::abs(prev.second - a.second) > 10 || (std::abs(a.first) < 1 && std::abs(prev.first) >= 1)) {
                        std::fprintf(stderr, "door %d at %.2f: r %.0f l %.0f\n", id, scene, a.first, a.second);
                        prev = a;
                    }
                }
            }
            updateEffects(*_viewDelegate.renderer, scene); // effects the VM starts and stops between shots
            // --dialog-click x,y (repeatable): clicks, in turn, each dialog that opens.
            // --dialog-type <text>: typed, key by key, into the text box of a dialog that opens.
            if (gPie && gPie->dialogOpen() && gPie->dialogHasTextBox() && !dialogTyped.empty() &&
                dialogTypedFor != gPie->dialog()->id) {
                dialogTypedFor = gPie->dialog()->id;
                for (size_t k = 0; k < dialogTyped.size();) {
                    size_t n = 1;
                    while (k + n < dialogTyped.size() && (uint8_t(dialogTyped[k + n]) & 0xC0) == 0x80)
                        ++n;
                    gPie->dialogKey(0, dialogTyped.substr(k, n));
                    k += n;
                }
            }
            if (gPie && gPie->dialogOpen() && dialogClick < dialogClicks.size()) {
                gPie->setCanvas(int(shotW), int(shotH));
                const simd_float2 c = dialogClicks[dialogClick++];
                if (c.x == -1)
                    gPie->dialogKey(36, {});
                else if (c.x == -2)
                    gPie->dialogKey(53, {});
                else
                    gPie->click(c);
            }
            if (gAudio) {
                updateAudio(base + t);
                gAudio->renderOffline(base + t);
            }
        }
        const CFTimeInterval simmed = CACurrentMediaTime();
        // --track-sim-id <id|name>: each shot follows and centres on that Sim
        // instead of the selected one (a scene another Sim plays).
        const auto trackOther = argument(@"--track-sim-id");
        const os2::vm::ObjectId tracked =
            trackOther && gSimulation ? os2::vm::findSim(gSimulation->machine(), *trackOther) : 0;
        if (tracked && gCameraFollow)
            gCameraFollow->follow(gSimulation->machine(), tracked);
        else
            followSelectedSim();
        // --track-sim: each shot centred on the selected Sim (close-ups of poses).
        if (([shotArgs containsObject:@"--track-sim"] && gPie) || tracked)
            for (const auto& s : gLotSims)
                if (s.id == (tracked ? tracked : gPie->sim()))
                    view.camera->setPivot(simd_make_float3(s.actor->position[0], s.actor->position[1], s.actor->position[2]));
        view.camera->setViewport(float(shotW), float(shotH));
        view.camera->snap();
        poseActors(*_viewDelegate.renderer, base + at);
        previous = at;
        _viewDelegate.renderer->setTime(at); // texture animations
        const uint32_t w = uiCanvas ? uiCanvas->first : 1280, h = uiCanvas ? uiCanvas->second : 720;
        gLastFrameW = w, gLastFrameH = h;
        const os2::FrameView fv = cinematicView({view.camera->view(), view.camera->projection(float(w) / float(h))}, at,
                                                float(w) / float(h));
        MTLClearColor c = view.clearColor;
        _viewDelegate.renderer->setReflections(reflectionsOn());
        auto img = _viewDelegate.renderer->capture(
            w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), float(c.alpha)));
        std::string file = *shot;
        if (frames > 1) {
            const auto dot = file.rfind('.');
            file = file.substr(0, dot) + "-" + std::to_string(f) + (dot == std::string::npos ? "" : file.substr(dot));
        }
        const CFTimeInterval captured = CACurrentMediaTime();
        os2::writePng(file, img);
        if (const char* poseShot = std::getenv("OS2_POSE_SHOT"))
            writePoseSheets(*_viewDelegate.renderer, frames > 1 ? poseShot + ("-f" + std::to_string(f)) : poseShot);
        perfSim += simmed - frameStart, perfCapture += captured - simmed;
        perfWorst = std::max(perfWorst, captured - frameStart);
        std::printf("opensims2: wrote %s (%.2f s%s)\n", file.c_str(), at,
                    gSimulation ? (", " + std::to_string(gSimulation->ticks()) + " VM ticks").c_str() : "");
    }
    if (perfOn())
        std::printf("opensims2: perf: %d frames: VM+audio steps %.1f ms, capture %.1f ms per frame on average; "
                    "worst frame %.1f ms\n",
                    frames, perfSim / frames * 1000, perfCapture / frames * 1000, perfWorst * 1000);
    if (gAudio) {
        gAudio->renderOffline(base + time);
        std::printf("opensims2: sound: %s\n", gAudio->summary().c_str());
        if (auto mix = argument(@"--sound-mix"))
            gAudio->writeMix(*mix);
    }
    std::fflush(stdout);
    std::exit(0);
}

- (void)finishLot {
    OS2View* view = _view;
    const auto uiCanvas = _uiCanvas;
    const simd_float3 pivot = _pivot;
    const uint32_t shotW = uiCanvas ? uiCanvas->first : 1280, shotH = uiCanvas ? uiCanvas->second : 720;
    if (gPie) {
        [self attachLotPanels];
        const bool headless = argument(@"--screenshot").has_value();
        const float cw = headless ? float(shotW) : float(view.bounds.size.width);
        const float ch = headless ? float(shotH) : float(view.bounds.size.height);
        if (gTutorial && headless) { // the tutorial presses and points at controls laid out at the shot's size
            gLive->setCanvas(int(cw), int(ch));
            gTutorial->setCanvas(int(cw), int(ch));
            pollTutorial(gHeadlessClock);
        }
        [self openPieFromFlags:cw height:ch pivot:pivot];
        [self runLotSteps:cw height:ch];
        [self writePickMap:cw height:ch];
        [self runLotPointerSteps:cw height:ch];
        std::fflush(stdout);
    }
    attachLetterbox(*_viewDelegate.renderer, (__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                    uint32_t(view.depthStencilPixelFormat));
    [self startCinematicFromFlags];
    if (gPendingScene)
        return; // headless: the steps left the lot (runScenes loads the next scene)
    [self listLotTestAids];
    if (auto shot = argument(@"--screenshot"))
        [self takeLotScreenshots:shot width:shotW height:shotH];
}

@end
