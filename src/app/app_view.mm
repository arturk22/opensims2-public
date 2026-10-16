// The game's view: OS2View, the MTKView that keeps the camera and the keys
// held down and hands the mouse and keys to the scene (its modes, the pie
// menu, the camera), with the game's mouse cursors (app_view.h).

#import "app/app_view.h"

namespace os2app {

// Camera position slot of the keys 4-9 (-1: another key).
int cameraSlot(unsigned short key) {
    switch (key) {
    case 21: return 0; case 23: return 1; case 22: return 2; case 26: return 3; case 28: return 4; case 25: return 5;
    default: return -1;
    }
}

// --- The game's mouse cursors (app/game_cursors.hpp) -------------------------

std::array<NSCursor*, size_t(os2::GameCursor::Count)> gCursors{};
bool gCursorsLoaded = false;

NSCursor* gameCursor(os2::GameCursor which) {
    if (!gCursorsLoaded) {
        gCursorsLoaded = true;
        const fs::path dir = gameDir() / "TSData/Res/UI/Cursors";
        for (size_t i = 0; i < gCursors.size(); ++i) {
            const fs::path f = os2::findCursorFile(dir, os2::gameCursorName(os2::GameCursor(i)));
            std::ifstream in(f, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto img = os2::decodeCursor(bytes);
            if (!img)
                continue;
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                            pixelsWide:img->width
                                                                            pixelsHigh:img->height
                                                                         bitsPerSample:8
                                                                       samplesPerPixel:4
                                                                              hasAlpha:YES
                                                                              isPlanar:NO
                                                                        colorSpaceName:NSDeviceRGBColorSpace
                                                                          bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                           bytesPerRow:img->width * 4
                                                                          bitsPerPixel:32];
            std::memcpy(rep.bitmapData, img->rgba.data(), img->rgba.size());
            NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(img->width, img->height)];
            [image addRepresentation:rep];
            gCursors[i] = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(img->hotX, img->hotY)];
        }
    }
    NSCursor* c = gCursors[size_t(which)];
    return c ? c : [NSCursor arrowCursor];
}

// What the view knows of the mouse for the cursor (the camera's state).
struct CursorExtras {
    bool inside = false;
    bool lotScene = false;       // a lot or neighborhood scene (the camera scrolls at the edges)
    int scrollX = 0, scrollY = 0; // the window's edge scrolling
    bool panning = false;        // the right button drags the camera
    os2::vm::ObjectId liveHover = 0; // the object under the mouse in Live mode (0: none)
    std::function<void(os2::CursorContext&)> hood; // fills the neighborhood's part
};

// Whether the selected Sim has something to do with an object (the pie
// menu's entries), for the Live cursor; kept for half a second per object so
// a mouse move does not run the check trees each time.
bool objectHasInteractions(os2::vm::ObjectId id) {
    os2::vm::ObjectId& lastId = gScene.interactionsAsked.id; // (an object ID: per scene)
    double& lastAt = gScene.interactionsAsked.at;
    bool& lastResult = gScene.interactionsAsked.result;
    const double now = CACurrentMediaTime();
    if (id == lastId && now - lastAt < 0.5)
        return lastResult;
    lastId = id, lastAt = now, lastResult = false;
    if (!gPie || !gPie->sim())
        return false;
    for (const auto& e : os2::vm::pieMenu(gPie->vm(), gPie->sim(), id))
        if (e.available() && !e.debug) {
            lastResult = true;
            break;
        }
    return lastResult;
}

// The cursor for what is under canvas point p (the game's arrow over the UI,
// Buy mode's hands over the lot, the Build tools', the neighborhood mover's,
// the camera's edge scrolling: docs/camera-ui-mac.md 2.7).
void refreshGameCursor(simd_float2 p, const CursorExtras& x) {
    if (!x.inside) {
        [[NSCursor arrowCursor] set];
        return;
    }
    using Ctx = os2::CursorContext;
    Ctx c;
    c.loading = gSceneLoading;
    c.mode = gBuy && gBuy->active() ? Ctx::Mode::Buy : gBuild && gBuild->active() ? Ctx::Mode::Build
             : gHood               ? Ctx::Mode::Hood : gLive ? Ctx::Mode::Live : Ctx::Mode::Other;
    if (!gSceneLoading) {
        if (gTutorial) {
            const auto r = gTutorial->panelRect();
            c.overUi |= p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
        }
        c.overUi |= gPie && (gPie->isOpen() || gPie->dialogOpen());
        c.overUi |= gLive && gLive->overPanel(p);
        if (gBuy && gBuy->active()) {
            c.overUi |= gBuy->overPanel(p);
            c.buyTool = Ctx::BuyTool(int(gBuy->tool()));
            c.holding = gBuy->held() != 0;
            c.pressing = gBuy->pressing();
            c.turned = gBuy->turned();
            c.overObject = gBuy->overObject();
        } else if (gBuild && gBuild->active()) {
            using Tool = os2::BuildMode::Tool;
            using Op = os2::vm::BuildEditor::TerrainOp;
            c.overUi |= gBuild->overPanel(p);
            switch (gBuild->tool()) {
            case Tool::Design: c.buildTool = Ctx::BuildTool::Design; break;
            case Tool::Railing: c.buildTool = Ctx::BuildTool::Railing; break;
            case Tool::Roof: c.buildTool = Ctx::BuildTool::Roof; break;
            case Tool::Object: c.buildTool = Ctx::BuildTool::Object; break;
            case Tool::Terrain:
                c.buildTool = gBuild->terrainOp() == Op::Raise   ? Ctx::BuildTool::Raise
                              : gBuild->terrainOp() == Op::Lower ? Ctx::BuildTool::Lower
                              : gBuild->terrainOp() == Op::Flatten ? Ctx::BuildTool::Level : Ctx::BuildTool::Other;
                break;
            default: break;
            }
            c.holding = gBuild->holdingObject();
            c.buildDragging = gBuild->dragging();
        } else if (gHood && x.hood) {
            x.hood(c);
        } else if (gLive && !c.overUi && x.liveHover) {
            using Hover = Ctx::LiveHover;
            const bool sim = std::any_of(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == x.liveHover; });
            c.liveHover = sim ? Hover::Person : objectHasInteractions(x.liveHover) ? Hover::Object : Hover::ObjectUnavailable;
        }
        if (x.lotScene && !c.overUi) {
            c.scrollX = x.scrollX, c.scrollY = x.scrollY;
            c.panning = x.panning;
        }
    }
    [gameCursor(os2::chooseCursor(c)) set];
}

} // namespace os2app

using namespace os2app;

@implementation OS2View {
    std::unique_ptr<os2::LotCamera> _camera;
    std::set<unsigned short> _held;
    std::set<unsigned short> _orbitOnly; // free camera: a d w s pressed with Shift ("Start orbit ...": not a move)
    os2::CameraMouseInput _mouse;
    bool _rotating; // right button with Control or Option: rotate/tilt instead of scroll
    bool _leftRotating; // Control + left on the lot (trackpads): rotate/tilt
    bool _leftRotated;  // ... and it moved (else the release is a plain click)
    float _leftMoved;   // points dragged since that press
    NSEvent* _leftPress; // that press, replayed as a click if it never moved
    bool _lookCaptured; // the free camera holds the mouse (cursor hidden, deltas only)
}

- (os2::LotCamera*)camera {
    return _camera.get();
}

- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera {
    _camera = std::move(camera);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (bool)held:(unsigned short)a orKey:(unsigned short)b {
    return (_held.count(a) && !_orbitOnly.count(a)) || (_held.count(b) && !_orbitOnly.count(b));
}

// The free camera's PickOrbitCenter (BV 0x7a6590): a ray from the eye along the
// view into the scene, here the floor or ground under it (inferred: the game's
// scene pick also stops at objects).
static std::optional<simd_float3> pickOrbitCentre() {
    const os2::LotCamera::FirstPerson pose = gFreeCamera->pose();
    const simd_float3 dir = simd_normalize(pose.forward);
    if (const auto t = floorUnderRay(pose.eye, dir))
        return pose.eye + dir * *t;
    return std::nullopt;
}

- (void)applyHeldKeys:(double)dt {
    _camera->setViewport(float(self.bounds.size.width), float(self.bounds.size.height));
    // The free camera looks with the mouse with no button, the cursor hidden and kept at the
    // window's middle while the app is active (FUN_00d92cd4 / FUN_00d90164).
    const bool capture = freeCameraActive() && self.window.isKeyWindow && NSApp.isActive && !gSceneLoading;
    if (capture != _lookCaptured) {
        _lookCaptured = capture;
        if (capture)
            [NSCursor hide];
        else
            [NSCursor unhide];
        CGAssociateMouseAndMouseCursorPosition(capture ? false : true);
    }
    if (gCas)
        return; // the CAS camera only follows the Sim and the zoom buttons
    // The free camera ("CameraMan", Tab): the movement keys steer it (Shift does not speed it up),
    // q and e lower and raise it; no panning, edge scrolling or orbit zoom.
    if (freeCameraActive()) {
        gFreeCamera->setMove(float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]),
                             float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]),
                             float(_held.count(kE)) - float(_held.count(kQ)));
        gFreeCamera->update(dt);
        _camera->setFirstPerson(gFreeCamera->pose());
        return;
    }
    // The accelerators' Start / Stop Move (arrows and a d w s; Shift + an arrow:
    // "Move ... Faster", the argument 2), Start / Stop Rotate (, .) and Zoom (= -):
    // velocities the camera turns into requests each update (docs/camera-ui-mac.md 1.11).
    const float fast = (NSEvent.modifierFlags & NSEventModifierFlagShift) ? 2.0f : 1.0f;
    const float right = float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]);
    const float forward = float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]);
    _camera->setKeyMove(right * fast, forward * fast);
    _camera->setKeyZoom(int(_held.count(kEquals)) - int(_held.count(kMinus)));
    _camera->setKeyRotate(int(_held.count(kPeriod)) - int(_held.count(kComma)));
    // The window's edge (DoWindowEdgeScrolling, once a frame): the step requests.
    if (self.window.isKeyWindow) {
        _mouse.config().edgeScroll = !argument(@"--no-edge-scroll") && gSettings.edgeScrolling;
        const os2::CameraMouseInput::EdgeSteps edge = _mouse.edgeSteps(float(self.bounds.size.width),
                                                                         float(self.bounds.size.height));
        if (edge.sendSideways)
            _camera->stepSideways(edge.sideways);
        if (edge.sendForward)
            _camera->stepForward(edge.forward);
    }
    // The right button held (DragCamera): the anchor's offset x 0.25 as the move request.
    if (_mouse.anchored()) {
        const auto [dragX, dragY] = _mouse.dragMove();
        _camera->moveRequest(dragX, dragY);
    }
}

// Headless (--key hold-<key>:<seconds>): the key held that long, frame by
// frame (50 ms) as the draw loop applies held keys, the camera smoothed.
- (void)holdKey:(unsigned short)key seconds:(double)seconds {
    _held.insert(key);
    for (double t = 0; t < seconds; t += 0.05) {
        [self applyHeldKeys:0.05];
        _camera->update(0.05);
    }
    _held.erase(key);
    for (int k = 0; k < 40; ++k) { // the smoothing settles (the key's velocity back to 0)
        [self applyHeldKeys:0.05];
        _camera->update(0.05);
    }
}

- (void)keyDown:(NSEvent*)e {
    if (gSceneLoading) { // the scene is the worker's; Esc, Space or Return skip the intro, not the loading screen
        if (os2::moviePlaying() && !os2::loadingScreenPlaying() && !e.isARepeat &&
            (e.keyCode == kEscape || e.keyCode == kReturn || e.keyCode == kEnter || e.keyCode == 49)) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (key %d)\n", int(e.keyCode));
        }
        return;
    }
    if (gCas) { // Create-a-Sim takes every key (names, undo, turning the Sim)
        std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        // The case typed is kept: a letter that arrives lower case with Shift (or Caps Lock) down
        // is made capital (synthesized key events carry the flag but not always the capital).
        const bool shifted = (e.modifierFlags & NSEventModifierFlagShift) != 0;
        const bool caps = (e.modifierFlags & NSEventModifierFlagCapsLock) != 0;
        if (typed.size() == 1 && typed[0] >= 'a' && typed[0] <= 'z' && (shifted != caps) &&
            !(e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
            typed[0] = char(typed[0] - 'a' + 'A');
        gCas->keyDown(e.keyCode, typed, e.modifierFlags & NSEventModifierFlagControl);
        return;
    }
    if (os2::moviePlaying()) { // Esc, Space or Return skips a movie
        if (!e.isARepeat && (e.keyCode == kEscape || e.keyCode == kReturn || e.keyCode == kEnter || e.keyCode == 49)) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (key %d)\n", int(e.keyCode));
        }
        return;
    }
    // A dialog the trees put up takes the keys (text, Return, Esc).
    if (gPie && gPie->dialogOpen()) {
        const std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        gPie->dialogKey(e.keyCode, typed);
        return;
    }
    // The neighborhood's place-lot dialog takes the keys (the lot's name).
    if (gHood && (gHood->ui().bins().textEntry() || gHood->ui().storyEditing())) {
        const std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        gHood->typeKey(e.keyCode, typed);
        return;
    }
    _held.insert(e.keyCode);
    // The free camera's orbit (Shift + a d w s, cCameraCameraManController::DoMessage):
    // every key-down picks the centre, repeats too; such a key is not a move.
    if (freeCameraActive() && (e.modifierFlags & NSEventModifierFlagShift) &&
        (e.keyCode == kA || e.keyCode == kD || e.keyCode == kW || e.keyCode == kS)) {
        if (!e.isARepeat)
            _orbitOnly.insert(e.keyCode);
        gFreeCamera->setOrbit(pickOrbitCentre());
    }
    if (e.isARepeat)
        return;
    // Any key (Shift sends no keyDown) cancels the pie menu; Esc that did so does nothing more.
    const bool pieWasOpen = gPie && gPie->isOpen();
    if (pieWasOpen)
        gPie->close();
    if (e.keyCode == kEscape && pieWasOpen)
        return;
    // Esc closes the options panel (and its window) before leaving the lot.
    if (e.keyCode == kEscape && !gHood && escapeOptions())
        return;
    // ... and in the free camera it is the camera's (the way back to the controller it came from).
    if (e.keyCode == kEscape && leaveFreeCamera())
        return;
    if (freeCameraActive()) { // cCameraCameraManController's keys (the map 0x6A9362F7 over the lot's)
        const bool control = e.modifierFlags & NSEventModifierFlagControl;
        if (e.keyCode == kTab || e.keyCode == kEscape || (e.keyCode == kY && !control && !gHood)) {
            cycleCameraController();
            return;
        }
        if (e.keyCode == kZ || e.keyCode == kEquals || e.keyCode == kPadPlus) { // "Zoom in": the next narrower fov
            gFreeCamera->zoomStep(1);
            return;
        }
        if (e.keyCode == kX || e.keyCode == kMinus || e.keyCode == kPadMinus) {
            gFreeCamera->zoomStep(-1);
            return;
        }
        if (const int slot = cameraSlot(e.keyCode); slot >= 0) {
            if (control)
                gFreeCamera->savePosition(slot);
            else
                gFreeCamera->restorePosition(slot);
            return;
        }
    }
    if (const int slot = cameraSlot(e.keyCode); slot >= 0) {
        if (e.modifierFlags & NSEventModifierFlagControl)
            _camera->savePosition(slot);
        else
            _camera->restorePosition(slot, !(e.modifierFlags & NSEventModifierFlagShift));
        return;
    }
    // Build mode: Ctrl-Z/Ctrl-Y undo and redo, , and . turn a door or window.
    if (gBuild && gBuild->active()) {
        const bool control = e.modifierFlags & NSEventModifierFlagControl;
        if (control && e.keyCode == kZ) { gBuild->undo(); return; }
        if (control && e.keyCode == kY) { gBuild->redo(); return; }
        if (gBuild->tool() == os2::BuildMode::Tool::Object && (e.keyCode == kComma || e.keyCode == kPeriod)) {
            gBuild->rotate(e.keyCode == kPeriod ? 2 : -2);
            _held.erase(e.keyCode);
            return;
        }
    }
    // Buy mode: , and . turn what is held, Delete sells it, Ctrl-Z/Ctrl-Y undo and redo.
    if (gBuy && gBuy->active()) {
        const bool control = e.modifierFlags & NSEventModifierFlagControl;
        if (control && e.keyCode == kZ) { gBuy->undo(); return; }
        if (control && e.keyCode == kY) { gBuy->redo(); return; }
        if (gBuy->held() && (e.keyCode == kComma || e.keyCode == kPeriod)) {
            gBuy->rotate(e.keyCode == kPeriod ? 2 : -2);
            _held.erase(e.keyCode); // not the camera's
            return;
        }
        if (e.keyCode == kBackspace || e.keyCode == kForwardDelete) { gBuy->deleteHeld(); return; }
    }
    // The neighborhood puck's accelerators: F1 families, F2 lots, F5 options.
    if (gHood && (e.keyCode == kF1 || e.keyCode == kF2 || e.keyCode == kF3 || e.keyCode == kF5)) {
        gHood->ui().togglePanel(e.keyCode == kF1   ? os2::HoodBins::Panel::Families
                                : e.keyCode == kF2 ? os2::HoodBins::Panel::Lots
                                : e.keyCode == kF3 ? os2::HoodBins::Panel::Decorations
                                                   : os2::HoodBins::Panel::Options);
        return;
    }
    if (gHood && e.keyCode == kF4) { // "História da Vizinhança - F4"
        gHood->ui().toggleStory();
        return;
    }
    // Ctrl-Z / Ctrl-Y (or Command): the Decorations panel's Undo and Redo.
    if (gHood && gHood->ui().bins().panel() == os2::HoodBins::Panel::Decorations && (e.keyCode == kZ || e.keyCode == kY) &&
        (e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand))) {
        gHood->undoDecoration(e.keyCode == kY);
        return;
    }
    // The neighborhood's lot tools: , and . turn the lot being placed.
    if (gHood && gHood->placingLot() && (e.keyCode == kComma || e.keyCode == kPeriod)) {
        gHood->turnLot(e.keyCode == kPeriod ? 1 : -1);
        _held.erase(e.keyCode);
        return;
    }
    switch (e.keyCode) {
    case kTab: cycleCameraController(); break; // "Cycle Camera Controller": the free camera (lot and neighborhood)
    case kY: // the same in a lot, as its keys file has it ("Y"; not the neighborhood's); Control-Y is redo
        if (!gHood && !(e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
            cycleCameraController();
        break;
    case kSpace: selectNextFamilySim(); break;
    case kPageUp: changeFloor(1); break;
    // "Paredes ... - Origem/Fim": Home and End step through the wall modes.
    case kHome: gWallView.cycle(1); break;
    case kEnd: gWallView.cycle(-1); break;
    case kPageDown: changeFloor(-1); break;
    case kZ: case kPadPlus: _camera->zoomBy(1); break;
    case kX: case kPadMinus: _camera->zoomBy(-1); break;
    case kReturn: case kEnter: // "Center on active Sim": CenterOnSelectedSim
        if (gCameraFollow && gSimulation && gPie)
            gCameraFollow->centre(gSimulation->machine(), gPie->sim());
        break;
    case kEscape:
        if (gBuild && gBuild->active() && gBuild->tool() != os2::BuildMode::Tool::None) {
            gBuild->cancel();
            break;
        }
        if (gBuy && gBuy->active() && gBuy->held()) { // drops what Buy mode holds
            gBuy->cancel();
            break;
        }
        if (gHood) {
            gHood->escape();
            break;
        }
        // A lot entered from the neighborhood: back to it, asking whether to
        // save (unless the pie menu is open).
        if (gHoodLot && !(gPie && (gPie->isOpen() || gPie->dialogOpen()))) {
            leaveLot();
            break;
        }
        if (gPie) gPie->close();
        break;
    // The speed buttons' accelerators (their tips: "Pausa - P", "Velocidade Normal - 1", ...).
    case kP: if (gLive) gLive->togglePause(); break;
    // "Top down view" (T): the Buy and Build panels' toggle, nothing in Live mode.
    case kT:
        if (!(e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
            toggleTopDownView();
        break;
    case k1: case k2: case k3: if (gLive) gLive->setSpeed(e.keyCode - k1 + 2); break;
    default: break;
    }
}

- (void)keyUp:(NSEvent*)e {
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->keyUp(e.keyCode);
    _held.erase(e.keyCode);
    _orbitOnly.erase(e.keyCode);
    // Stop Move ... / Stop lower / raise (any modifiers) end the free camera's orbit.
    if (gFreeCamera && (e.keyCode == kA || e.keyCode == kD || e.keyCode == kW || e.keyCode == kS ||
                        e.keyCode == kQ || e.keyCode == kE))
        gFreeCamera->setOrbit(std::nullopt);
}

- (void)flagsChanged:(NSEvent*)e {
    // A key released while Cmd was down never sends keyUp.
    if (e.modifierFlags & NSEventModifierFlagCommand)
        _held.clear();
}

- (void)scrollWheel:(NSEvent*)e {
    if (gSceneLoading || freeCameraActive())
        return;
    _camera->wheel(_mouse.wheelNotches(e.scrollingDeltaY, e.hasPreciseScrollingDeltas));
}

// The game's rotate request: pixels dragged (only the dominant axis counts).
- (void)dragRotate:(NSEvent*)e {
    if (gSceneLoading)
        return;
    if (freeCameraActive()) { // the free camera looks round with the mouse (buttons or not)
        gFreeCamera->look(float(e.deltaX), float(e.deltaY));
        return;
    }
    _camera->mouseRotate(float(e.deltaX), float(e.deltaY)); // dx negated unless rotateCameraSims1Mode
}

// The mouse position for edge and anchor scrolling.
- (void)trackMouse:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    _mouse.mouseAt(float(p.x), float(self.bounds.size.height - p.y), NSPointInRect(p, self.bounds));
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* a in self.trackingAreas)
        [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                           NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
}

- (void)mouseExited:(NSEvent*)e {
    [self trackMouse:e];
    _mouse.mouseAt(-1, -1, false);
    refreshGameCursor({}, CursorExtras{});
}

- (void)cursorUpdate:(NSEvent*)e {
    [self refreshCursor:e];
}

// The game's cursor for the state after this event.
- (void)refreshCursor:(NSEvent*)e {
    const NSPoint q = [self convertPoint:e.locationInWindow fromView:nil];
    const simd_float2 p = [self canvasPoint:e];
    CursorExtras x;
    x.inside = NSPointInRect(q, self.bounds);
    x.lotScene = _camera && !gCas && (gLive || gBuy || gBuild || gHood);
    if (x.inside && x.lotScene) {
        const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
        if (self.window.isKeyWindow && !gSceneLoading) {
            _mouse.config().edgeScroll = !argument(@"--no-edge-scroll") && gSettings.edgeScrolling;
            const auto [mr, mf] = _mouse.panAxes(w, h);
            x.scrollX = int(mr), x.scrollY = int(mf);
        }
        x.panning = _mouse.anchored() && !_rotating;
        if (gPie && gLive && !(gBuy && gBuy->active()) && !(gBuild && gBuild->active()) && !gHood && !gSceneLoading)
            x.liveHover = gPie->pick(_camera->eye(), [self rayAt:p]);
        if (gHood)
            x.hood = [&](os2::CursorContext& c) { gHood->fillCursor(c, *_camera, p, w, h); };
    }
    refreshGameCursor(p, x);
}

- (void)rightMouseDown:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading)
        return;
    if (gPie && gPie->isOpen()) { // the right button cancels the pie menu
        gPie->close();
        return;
    }
    if (gLive && gLive->press([self canvasPoint:e], true))
        return;
    // Control (the game's "CTRL + botão DIREITO") or Option: rotate and tilt. The free
    // camera has no panning: the right button looks round.
    _rotating = freeCameraActive() ||
                os2::CameraMouseInput::rotatePress(os2::CameraMouseInput::Button::Right,
                                                   e.modifierFlags & NSEventModifierFlagControl,
                                                   e.modifierFlags & NSEventModifierFlagOption);
    if (!_rotating && gPie && !(gBuy && gBuy->active()) && !(gBuild && gBuild->active())) // a right click on a Sim of the family selects it
        selectFamilySim(gPie->pick(_camera->eye(), [self rayAt:[self canvasPoint:e]]));
    if (_rotating)
        [self holdCursor:YES];
    else
        _mouse.beginAnchor();
}

- (void)rightMouseUp:(NSEvent*)e {
    [self trackMouse:e];
    if (_mouse.anchored() && _camera)
        _camera->moveRequest(0, 0); // DragCamera on release: velocity 0
    _mouse.endAnchor();
    if (_rotating)
        [self holdCursor:NO];
    _rotating = false;
}

// While rotating the cursor stays where it was clicked (the game warps it
// back after each move: an endless drag); the deltas keep coming.
- (void)holdCursor:(BOOL)hold {
    CGAssociateMouseAndMouseCursorPosition(hold || _lookCaptured ? false : true);
}

- (void)otherMouseDown:(NSEvent*)e {
    [self holdCursor:YES];
}

- (void)otherMouseUp:(NSEvent*)e {
    [self holdCursor:NO];
}

// Mouse position in view points, origin top left (the pie menu's canvas).
- (simd_float2)canvasPoint:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return simd_make_float2(float(p.x), float(self.bounds.size.height - p.y));
}

// Left click (with --vm): on an open pie menu picks an entry (or closes it);
// else casts a ray from the camera through the mouse and opens the menu of
// the object hit.
- (void)mouseDown:(NSEvent*)e {
    struct Refresh { // the cursor after the press, whichever way it returns
        OS2View* view; NSEvent* event;
        ~Refresh() { [view refreshCursor:event]; }
    } refresh{self, e};
    if (gSceneLoading) { // a click skips the intro, not the loading screen
        if (os2::moviePlaying() && !os2::loadingScreenPlaying()) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (click)\n");
        }
        return;
    }
    if (gCas) {
        gCas->setCanvas(int(self.bounds.size.width), int(self.bounds.size.height));
        gCas->mouseDown([self canvasPoint:e]);
        return;
    }
    if (os2::moviePlaying()) { // a click skips a movie
        os2::skipMovie();
        std::printf("opensims2: movie skipped (click)\n");
        return;
    }
    if (gMenu) {
        pressMainMenu([self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
        return;
    }
    if (gHood) {
        gHood->mouseDown(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
        return;
    }
    if (!gPie)
        return;
    const simd_float2 p = [self canvasPoint:e];
    if (gPie->dialogOpen() && gPie->click(p)) {
        pollFlowDialog();
        return;
    }
    if (gTutorial) { // the tutorial's window (top right)
        gTutorial->setCanvas(int(self.bounds.size.width), int(self.bounds.size.height));
        if (gTutorial->press(p))
            return;
    }
    if (gPie->isOpen() && gPie->press(p))
        return;
    if (pressOptions(p, float(self.bounds.size.width), float(self.bounds.size.height))) // its body is over all
        return;
    if (gLive && gLive->overModeButton(p) && gLive->press(p)) // the puck's modes win over the panels
        return;
    // The options panel floats over Buy/Build's panels: it must take its clicks first
    // (their backdrops would swallow Salvar/Sair and the like).
    if (gLive && gLive->optionsOpen() && gLive->press(p))
        return;
    const bool optionsOver = gLive && gLive->optionsOpen(); // Buy/Build's panels are hidden under it
    if (!optionsOver && gBuy && gBuy->active() && gBuy->press(p))
        return;
    if (!optionsOver && gBuild && gBuild->active() && gBuild->press(p))
        return;
    if (gLive && gLive->press(p))
        return;
    if (pressOptions(p, float(self.bounds.size.width), float(self.bounds.size.height))) // under the puck
        return;
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = _camera->eye();
    if (gBuy && gBuy->active()) { // the object tool, not the pie menu (a drag turns what is held, the release drops it)
        gBuy->mouseDown(eye, simd_normalize(q.xyz / q.w - eye), p);
        return;
    }
    if (gBuild && gBuild->active()) { // Build mode's tools
        gBuild->mouseDown(eye, simd_normalize(q.xyz / q.w - eye), e.modifierFlags & NSEventModifierFlagControl,
                          e.modifierFlags & NSEventModifierFlagShift);
        return;
    }
    // Control + left on the lot: the rotate/tilt tool (a trackpad's or a
    // one-button mouse's Ctrl-click is the right button); a press that
    // never moves is still a click, replayed on release.
    if (!_leftPress && os2::CameraMouseInput::rotatePress(os2::CameraMouseInput::Button::Left,
                                                          e.modifierFlags & NSEventModifierFlagControl, false)) {
        _leftRotating = true;
        _leftRotated = false;
        _leftMoved = 0;
        _leftPress = e;
        [self holdCursor:YES];
        return;
    }
    const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
    if (id)
        openClicked(id, eye, simd_normalize(q.xyz / q.w - eye), p);
    if (!id) // the ground: Go Here
        openGoHere(eye, simd_normalize(q.xyz / q.w - eye), p);
    std::fflush(stdout);
}

// The pie menu picks on release (a drag from the opening press is a gesture).
- (void)mouseUp:(NSEvent*)e {
    if (_leftRotating) {
        _leftRotating = false;
        [self holdCursor:NO];
        NSEvent* press = _leftPress;
        if (!_leftRotated && !gSceneLoading)
            [self mouseDown:press]; // _leftPress set: a plain click now
        _leftPress = nil;
        return;
    }
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->mouseUp([self canvasPoint:e]);
    if (gPie && gPie->isOpen())
        gPie->release([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseUp(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    if (gBuy && gBuy->active())
        gBuy->mouseUp();
    [self refreshCursor:e];
    if (gHood)
        gHood->mouseUp(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
}

// The mouse ray (world space) through a canvas point.
- (simd_float3)rayAt:(simd_float2)p {
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    return simd_normalize(q.xyz / q.w - _camera->eye());
}

- (void)mouseMoved:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading) {
        [self refreshCursor:e];
        return;
    }
    if (_lookCaptured && freeCameraActive()) { // the free camera's mouse look
        gFreeCamera->look(float(e.deltaX), float(e.deltaY));
        return;
    }
    if (gHood)
        gHood->mouseMoved(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
    if (gPie)
        gPie->hover([self canvasPoint:e]);
    if (gLive)
        gLive->hover([self canvasPoint:e]);
    if (gBuy && gBuy->active()) {
        const simd_float2 p = [self canvasPoint:e];
        gBuy->hover(p);
        const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
        const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
        const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
        gBuy->pointAt(_camera->eye(), simd_normalize(q.xyz / q.w - _camera->eye()));
    }
    if (gBuild && gBuild->active()) {
        gBuild->hover([self canvasPoint:e]);
        gBuild->pointAt(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    }
    [self refreshCursor:e];
}

- (void)mouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading)
        return;
    if (_leftRotating) {
        _leftMoved += std::fabs(float(e.deltaX)) + std::fabs(float(e.deltaY));
        _leftRotated = _leftMoved > 3; // a click's jitter is still a click
        [self dragRotate:e];
        return;
    }
    if (gCas)
        gCas->mouseDragged([self canvasPoint:e]);
    if (gPie)
        gPie->hover([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseDrag(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    if (gBuy && gBuy->active())
        gBuy->mouseDrag(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    [self refreshCursor:e];
    if (gHood)
        gHood->mouseDragged(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
}

- (void)rightMouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (_rotating)
        [self dragRotate:e];
}

- (void)otherMouseDragged:(NSEvent*)e {
    [self dragRotate:e];
}
@end
