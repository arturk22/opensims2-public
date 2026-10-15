// macOS shell: an AppKit window whose MTKView drives os2::Renderer every frame
// and feeds input to the lot camera. The renderer itself is plain C++ on
// metal-cpp; this file only bridges the Objective-C objects across.
//
// Camera controls, the game's live-mode keys (ui.package accelerators
// 0x6A9362F2/F5): arrows or WASD move (Shift: faster), z/x or NumPad +/- zoom
// a level, = and - held zoom, , and . held rotate, Control/plain/Shift 4-9
// save/restore/jump to camera positions, Return centres on the selected Sim;
// with the mouse as in the game (app/camera_input.hpp): hold the right button
// and move away from where it went down to scroll (offset x 0.25), hold the
// middle button (or Option + right) to rotate and tilt, the wheel zooms, the
// mouse at the window's edge scrolls (--no-edge-scroll). --lighting <state> picks a lighting state from Lighting.txt
// (day, night, neighbourhood, ...); --lot <N001_Lot82 | file.package> shows a
// lot's terrain, floors, walls, roofs, fences, water and objects (--walls
// up | cutaway | down picks the wall mode, cutaway as seen from the starting view). --sim [body,hair,skin tone] shows
// an adult Sim built from catalog parts, --skinned <cres,cres,...> skinned
// meshes on the adult skeleton; either plays --anim <name> (looped; --time
// <seconds> picks the moment of a --screenshot), --overlay <name>[@bone] over
// the bones under <bone>, or --walk <seconds> (idle, a straight walk along
// --heading <degrees>, idle). --character <N001_User00011 | file.package>
// shows a neighbourhood Sim from its character file; with --lot the lot's
// Sims stand where they were saved, playing an idle (--sim-anim <name>). --play-sound <name | TGI>
// plays a sound (the --sound-index'th sample of a track; --sound-wav <out.wav>
// also saves it) and exits without opening a window. --ui <instance-hex> draws a
// UI script from ui.package over the scene (1024x768 canvas, letterboxed;
// --ui-show <id,id,...> shows windows the script hides, e.g. the main menu's
// background 0da36c7d); --ui live draws the game's live-mode panel instead
// (app/live_ui.hpp: puck, Sim panel, needs, family, action queue, wired to
// the VM; it replaces the pie menu's debug queue, which --debug-queue keeps;
// headless, --click presses its buttons and --ui-hover x,y puts the mouse
// over it). With the VM running, a left click on an object opens
// its pie menu for the selected Sim (--sim-id) and picking an entry queues it
// (Esc closes it); headless: --pie <object> [--pie-select N] and --click x,y
// (repeatable), which also start the VM (app/pie_menu.hpp). --vm runs the lot's objects in the SimAntics VM while it
// is shown, with the Sims following it (walking, animations); --vm-walk
// <sim-id>,<object-id>[,<anim>] makes a Sim walk to an object (then play entry
// <anim> of the global animation list); --run-vm <ticks> runs that many ticks
// (the game's 1000/42 a second at normal speed) before a --screenshot, and --frames <n> saves n screenshots
// spread over them (out-1.png ... out-n.png). A dialog the trees put up is
// shown over the scene and pauses the game until a button is clicked;
// headless: --dialog-answer <n> answers every one with button n, and
// --dialog-click x,y|return|escape (repeatable) clicks (or presses a key for) each dialog in turn,
// --dialog-type <text> types into the text box of a dialog, --dialog-demo <type>[:title[:message]] puts one up.
// --movie <name> plays a .movie fullscreen and quits (headless with --screenshot:
// frames and A/V timestamps, app/movie_host.hpp); --intro plays the start-up
// movies before the scene. With a window, scenes load on a worker thread
// behind the game's Reia loading screens (app/scene_loader.hpp,
// docs/loading-mac.md); headless, --loading-shot <png> [--loading-at <f>]
// loads the first scene that way and saves its loading screen, and
// --loading-hold <seconds> keeps a window's loading screen up that long.

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "app/cinematic_player.hpp"
#include "app/ui_panel_util.hpp"
#include "app/loading_screen.hpp"
#include "app/glide_detector.hpp"
#include "app/jump_detector.hpp"
#include "app/seat_check.hpp"
#include "formats/catalog.hpp"
#include "formats/config_text.hpp"
#include "formats/graphics_options.hpp"
#include "app/scene_loader.hpp"
#include "app/tv_broadcast.hpp"
#include "formats/neighborhood.hpp"
#include "formats/ui_script.hpp"
#include "vm/tutorial.hpp"
#include "vm/wants.hpp"
#include "app/movie_host.hpp"
#include "render/movie_layer.hpp"
#include "render/screen_fx_layer.hpp"
#include "vm/engine.hpp"
#include "vm/dialog.hpp"
#include "vm/go_here.hpp"
#include "audio/play_sound.hpp"
#include "formats/camera_config.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/cas_thumbnails.hpp"
#include "vm/hood_bridge.hpp"
#include "formats/character_write.hpp"
#include "formats/image.hpp"
#include "formats/dbpf.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/anim.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "formats/sim_censor.hpp"
#include "formats/lot_objects.hpp"
#include "formats/skinning.hpp"
#include "formats/font.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_screen.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#include "app/buy_mode.hpp"
#include "app/game_cursors.hpp"
#include "app/design_tool.hpp"
#include "app/build_mode.hpp"
#include "app/data_location.hpp"
#include "app/game_flow.hpp"
#include "app/game_data_caches.hpp"
#include "formats/log_once.hpp"
#include "app/wants_panel.hpp"
#include "app/head_shot.hpp"
#include "app/game_save.hpp"
#include "app/hood_mode.hpp"
#include "formats/lot_arrivals.hpp"
#include "app/main_menu.hpp"
#include "app/options_panel.hpp"
#include "app/user_data.hpp"
#include "app/lot_effects.hpp"
#include "app/cas_mode.hpp"
#include "app/live_ui.hpp"
#include "app/tutorial.hpp"
#include "app/pie_menu.hpp"
#include "lot_audio.hpp"
#include "anim_snaps.hpp"
#include "camera_follow.hpp"
#include "wall_view.hpp"
#include "camera_input.hpp"
#include "lot_vm.hpp"
#include "vm_objects.hpp"
#include "lot_lighting.hpp"
#include "door_portals.hpp"
#include "vm/lot.hpp"
#include "vm/placement.hpp"
#include "vm/baby_character.hpp"
#include "app/app_state.hpp"
#import "app/app_view.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <strings.h>
#include <sys/resource.h>
#include <sys/syslimits.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace os2app;

@interface OS2ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view;
- (os2::Renderer*)renderer;
- (void)resetRenderer:(MTKView*)view;
- (void)restartClock;
@end

@implementation OS2ViewDelegate {
    std::unique_ptr<os2::Renderer> _renderer;
    CFTimeInterval _last;
    CFTimeInterval _start;
}

- (instancetype)initWithView:(MTKView*)view {
    if ((self = [super init])) {
        _renderer = std::make_unique<os2::Renderer>((__bridge MTL::Device*)view.device,
                                                    uint32_t(view.colorPixelFormat),
                                                    uint32_t(view.depthStencilPixelFormat));
        _last = _start = CACurrentMediaTime();
    }
    return self;
}

- (os2::Renderer*)renderer {
    return _renderer.get();
}

// A new, empty renderer for the next scene (the old one waits for its frames
// in flight); the scene clock starts again.
- (void)resetRenderer:(MTKView*)view {
    _renderer.reset();
    _renderer = std::make_unique<os2::Renderer>((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                                uint32_t(view.depthStencilPixelFormat));
    _last = _start = CACurrentMediaTime();
}

// The scene clock starts when the scene is shown (after its load).
- (void)restartClock {
    _last = _start = CACurrentMediaTime();
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)mtkView {
    OS2View* view = (OS2View*)mtkView;
    CFTimeInterval now = CACurrentMediaTime();
    struct FramePerf { // OS2_PERF=1: CPU time of a frame (not the wait for the drawable), over 120 frames
        CFTimeInterval began, waited = 0, total = 0, worst = 0;
        int count = 0;
    };
    static FramePerf perf;
    perf.began = now;
    double dt = std::min(now - _last, 0.1); // no huge steps after a stall
    _last = now;

    // A scene loading on the worker (app/scene_loader.hpp): only the loading
    // screen (or the intro) is drawn; the scene's objects are the worker's.
    if (gSceneLoading) {
        if (gPollLoad)
            gPollLoad();
        if (gGameAudio) { // the music goes on behind the loading screen
            gGameAudio->setHeld(os2::moviePlaying());
            gGameAudio->update(now - _start);
        }
        MTLRenderPassDescriptor* pass = mtkView.currentRenderPassDescriptor;
        id<CAMetalDrawable> drawable = mtkView.currentDrawable;
        os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable);
        return;
    }

    if (!view.camera) { // no scene (--movie): only a movie
        MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
        id<CAMetalDrawable> drawable = view.currentDrawable;
        os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable);
        return;
    }
    [view applyHeldKeys:dt];
    pollFlowDialog();
    if (gOptions && gLive && !gLive->optionsOpen() && gOptions->active())
        gOptions->closeWindow(); // the options panel closed
    if (gLive)
        gLive->tick(now - _start);
    followSelectedSim();
    view.camera->update(dt);
    CGSize size = view.drawableSize;
    float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
    gLastFrameW = uint32_t(std::max(1.0, size.width)), gLastFrameH = uint32_t(std::max(1.0, size.height));
    poseActors(*_renderer, now - _start);
    if (gGameAudio) { // (on a lot, the lot's audio updates it)
        gGameAudio->setHeld(os2::moviePlaying());
        if (!gAudio)
            gGameAudio->update(now - _start);
    }
    const os2::FrameView frame = cinematicView({view.camera->view(), view.camera->projection(aspect)}, now - _start, aspect);
    if (gCas)
        gCas->update(now - _start);
    if (gBuy)
        gBuy->update();
    if (gBuild)
        gBuild->update();
    if (view.bounds.size.width > 0)
        gUiScale = float(size.width / view.bounds.size.width);

    const CFTimeInterval beforeDrawable = CACurrentMediaTime();
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    perf.waited = CACurrentMediaTime() - beforeDrawable;
    // A fullscreen movie (app/movie_host.hpp) in place of the scene.
    if (os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable))
        return;
    _renderer->setReflections(reflectionsOn());
    _renderer->draw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable, frame);
    if (const char* shot = liveShotFile(); shot && gLiveShotWanted) {
        gLiveShotWanted = 0;
        const MTLClearColor c = view.clearColor;
        os2::writePng(shot, _renderer->capture(uint32_t(gLastFrameW), uint32_t(gLastFrameH), frame,
                                               simd_make_float4(float(c.red), float(c.green), float(c.blue), 1)));
        std::printf("opensims2: live shot %s\n", shot);
        std::fflush(stdout);
    }
    if (perfOn()) {
        const CFTimeInterval spent = CACurrentMediaTime() - perf.began - perf.waited;
        perf.total += spent, perf.worst = std::max(perf.worst, spent);
        if (++perf.count == 120) {
            std::printf("opensims2: perf: 120 frames, CPU %.2f ms average, %.2f ms worst\n", perf.total / 120 * 1000,
                        perf.worst * 1000);
            std::fflush(stdout);
            perf.total = perf.worst = 0, perf.count = 0;
        }
    }
}
@end

@interface OS2AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation OS2AppDelegate {
    NSWindow* _window;
    OS2View* _view;
    OS2ViewDelegate* _viewDelegate; // MTKView holds its delegate weakly
    bool _introPlayed;
    int _flowShot;
    // The scene being loaded (prepareScene -> finishScene).
    os2::Scene _scene;
    bool _cas;
    std::optional<std::pair<uint32_t, uint32_t>> _uiCanvas; // the lot's --ui canvas
    simd_float3 _pivot;                                     // the lot's camera pivot
    std::vector<fs::path> _menuDirs;                        // the main menu's neighborhoods
    os2::LoadingScreen _loadingScreen;
    bool _loadCompletionQueued;
    CFTimeInterval _loadStart;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    perfLap("process start");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "opensims2: no Metal device\n");
        [NSApp terminate:nil];
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"opensims2";

    OS2View* view = [[OS2View alloc] initWithFrame:frame device:device];
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm; // gamma space, like the game
    view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    view.preferredFramesPerSecond = 60;
    _view = view;

    try {
        _viewDelegate = [[OS2ViewDelegate alloc] initWithView:view];
        applyLighting(*_viewDelegate.renderer);
        os2::movieHostInit((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                           uint32_t(view.depthStencilPixelFormat),
                           {gameDir() / "TSData/Res", ![sceneArgs() containsObject:@"--no-audio"]});
        // --movie <name>: play one movie in the window and quit when it ends.
        if (auto movie = argument(@"--movie")) {
            os2::playMovie(*movie, [] { [NSApp terminate:nil]; });
            view.delegate = _viewDelegate;
            _window.contentView = view;
            [_window center];
            [_window makeKeyAndOrderFront:nil];
            [_window makeFirstResponder:view];
            [NSApp activateIgnoringOtherApps:YES];
            return;
        }
        // --intro: the start-up movies (EA logo, intro) before the scene.
        if ([sceneArgs() containsObject:@"--intro"])
            os2::playMovies(os2::introMovies());
        // The start-up movies before the first main menu (EA logo, then the
        // intro; skippable), unless --intro already started them.
        gIntroMovies = [] {
            if (!os2::moviePlaying())
                os2::playMovies(os2::introMovies());
        };
        __weak OS2AppDelegate* weakSelf = self;
        gOnSceneRequest = [weakSelf] {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf switchScene];
            });
        };
        gPollLoad = [weakSelf] { [weakSelf pollSceneLoad]; };
        // Headless, the scenes load in order on this thread; with a window
        // the first one loads behind the loading screen (below).
        startGameAudio(); // (on this thread: the scenes' loads only use it)
        if (headless()) {
            [self ensureGameState];
            [self runScenes];
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        if (argument(@"--screenshot"))
            std::exit(1); // no one to show an alert to
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"opensims2 could not start";
        alert.informativeText = [NSString stringWithUTF8String:e.what()];
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    view.delegate = _viewDelegate;
    _window.contentView = view;
    [_window center];
    _window.acceptsMouseMovedEvents = YES;
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
    [self beginSceneLoad];
}

// Quitting while a scene loads: the worker cannot be stopped, so the process
// ends at once (nothing is being saved: saves run on the main thread).
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    if (gLoadJob.active()) {
        std::fflush(stdout);
        std::_Exit(0);
    }
    return NSTerminateNow;
}

// The game's resources and the user's folder, made once (with a window, on
// the loading thread of the first scene).
- (void)ensureGameState {
    if (!gIndex) {
        gIndex = std::make_unique<os2::ResourceIndex>(gameDir() / "TSData/Res");
        perfLap("ResourceIndex");
    }
    if (!gUser) {
        // The user's folder (app/user_data.hpp): --user-dir, $OS2_USER_DIR,
        // else ~/Library/Application Support/opensims2.
        fs::path user = os2::defaultUserDir();
        if (const char* env = std::getenv("OS2_USER_DIR"))
            user = env;
        if (auto u = argument(@"--user-dir"))
            user = *u;
        gUser = std::make_unique<os2::UserData>(gameDir(), user);
        gSettings = os2::GameSettings::load(*gUser);
        gSettings.applyGraphics();
    }
}

// --- Loading behind the loading screen (app/scene_loader.hpp) ---------------

// A lot opened by flags (--hood N --hood-lot id) has no click behind it: its
// loading screen's case comes from the neighborhood's lot info.
static void fillLotEntryFromFlags(const os2::Scene& scene) {
    if (scene.kind == os2::Scene::Kind::Neighborhood && !scene.hood.empty()) {
        // The neighborhood's own loading screen: its name, the Tutorial's, or
        // (back from Create-a-Sim) the new family (docs/loading-mac.md).
        try {
            const os2::hood::Neighborhood hood = os2::hood::Neighborhood::load(hoodFolder(scene.hood), false);
            gLotEntry = {};
            gLotEntry.hoodName = hood.name;
            gLotEntry.tutorial = hood.prefix == "Tutorial";
            std::optional<uint32_t> created = gCreatedFamily; // --loading-family N: test aid (case 6)
            if (auto a = argument(@"--loading-family"))
                created = uint32_t(std::stoul(*a));
            if (created)
                if (const auto* f = hood.family(*created)) {
                    gLotEntry.withFamily = true, gLotEntry.familyId = f->id, gLotEntry.familyName = f->name;
                    gLotEntry.familyThumbs =
                        hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package");
                }
        } catch (const std::exception& err) {
            os2::logOnce("loading screen", scene.hood, err);
        }
        return;
    }
    if (scene.kind != os2::Scene::Kind::Lot || gLotEntry.known || scene.hood.empty())
        return;
    try {
        const fs::path dir = hoodFolder(scene.hood);
        const os2::hood::Neighborhood hood = os2::hood::Neighborhood::load(dir, false);
        const os2::hood::Lot* lot = hood.lot(scene.lot);
        if (!lot)
            return;
        gLotEntry = {true, lot->info.type, !lot->families.empty(), lot->name, "", 0,
                     hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package")};
        if (!lot->families.empty()) {
            gLotEntry.familyId = lot->families.front();
            if (const auto* f = hood.family(gLotEntry.familyId))
                gLotEntry.familyName = f->name;
        }
    } catch (const std::exception& err) {
        os2::logOnce("loading screen", scene.hood, err);
    }
}

// With a window: starts loading the scene the arguments pick on the worker,
// and shows its loading screen (after the intro movies, when they play).
// --loading-hold <seconds>: test aid, the worker waits that long after the
// load, so the loading screen can be looked at.
- (void)beginSceneLoad {
    const os2::Scene scene = os2::sceneOf(processArguments());
    const bool cas = [sceneArgs() containsObject:@"--cas"];
    if (scene.kind == os2::Scene::Kind::MainMenu && !cas && !_introPlayed) {
        _introPlayed = true;
        if (gIntroMovies)
            gIntroMovies();
    }
    fillLotEntryFromFlags(scene);
    _loadingScreen = os2::loadingScreenFor(gameDir() / "TSData/Res", scene, cas, gLotEntry);
    gLotEntry = {};
    os2::loading::begin();
    gSceneLoading = true;
    _loadCompletionQueued = false;
    _loadStart = CACurrentMediaTime();
    [self showLoadingScreen];
    const double hold = std::stod(argument(@"--loading-hold").value_or("0"));
    gLoadJob.start([self, hold] {
        @autoreleasepool {
            [self prepareScene];
        }
        if (hold > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(hold));
    });
}

// The loading screen, unless another movie (the intro) is playing.
- (void)showLoadingScreen {
    if (os2::moviePlaying())
        return;
    os2::playLoadingScreen(_loadingScreen, [] { return os2::loading::shown(); });
}

// Each frame while loading (from the view's draw): the loading screen once
// the intro is over, and the end of the load handed to the run loop.
- (void)pollSceneLoad {
    if (!gSceneLoading)
        return;
    if (!os2::moviePlaying())
        [self showLoadingScreen];
    if (!gLoadJob.done() || _loadCompletionQueued)
        return;
    _loadCompletionQueued = true;
    __weak OS2AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf completeSceneLoad];
    });
}

// The worker is done: the main thread's part of the scene (AppKit, the view,
// the headless steps), then frames and input go to the scene again.
- (void)completeSceneLoad {
    std::exception_ptr error = gLoadJob.collect();
    const CFTimeInterval worked = CACurrentMediaTime() - _loadStart;
    gSceneLoading = false;
    os2::loading::end();
    os2::finishLoadingScreen(); // plays out quickly over the first frames
    try {
        if (error)
            std::rethrow_exception(error);
        const CFTimeInterval mainStart = CACurrentMediaTime();
        [self finishScene];
        if (perfOn())
            std::printf("opensims2: perf: scene loaded in %.1f ms (worker), then %.1f ms on the main thread\n",
                        worked * 1000, (CACurrentMediaTime() - mainStart) * 1000);
        std::fflush(stdout);
    } catch (const std::exception& e) {
        [self sceneFailed:e.what()];
        return;
    }
    [_viewDelegate restartClock];
    [_window makeFirstResponder:_view];
}

// A scene that could not load: an alert, then the main menu (or quit, when
// the main menu itself failed).
- (void)sceneFailed:(const char*)what {
    std::fprintf(stderr, "opensims2: %s\n", what);
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = gIndex ? @"opensims2 could not load the scene" : @"opensims2 could not start";
    alert.informativeText = [NSString stringWithUTF8String:what];
    [alert runModal];
    if (!gIndex || !gUser || (_scene.kind == os2::Scene::Kind::MainMenu && !_cas)) {
        [NSApp terminate:nil];
        return;
    }
    os2::Scene menu;
    menu.kind = os2::Scene::Kind::MainMenu;
    gPendingScene = menu;
    [self applyPendingScene];
    [self beginSceneLoad];
}

// Headless --loading-shot <file.png> [--loading-at <fraction>]: the first
// scene loads on the worker as with a window, and the loading screen is
// saved once the load has reported that much progress (default 0.5).
- (void)loadSceneWithLoadingShot {
    const os2::Scene scene = os2::sceneOf(processArguments());
    const bool cas = [sceneArgs() containsObject:@"--cas"];
    os2::loading::begin();
    fillLotEntryFromFlags(scene);
    os2::playLoadingScreen(os2::loadingScreenFor(gameDir() / "TSData/Res", scene, cas, gLotEntry),
                           [] { return os2::loading::shown(); });
    const float at = std::stof(argument(@"--loading-at").value_or("0.5"));
    const CFTimeInterval start = CACurrentMediaTime();
    gLoadJob.start([self] {
        @autoreleasepool {
            [self prepareScene];
        }
    });
    while (!gLoadJob.done() && os2::loading::reported() < at)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const double elapsed = CACurrentMediaTime() - start;
    const float shown = os2::loading::shown();
    const os2::RgbaImage img = os2::movieHostCapture(1280, 720, elapsed);
    const std::string file = *argument(@"--loading-shot");
    os2::writePng(file, img);
    std::printf("opensims2: wrote loading screen %s (%.2f s into the load, progress %.0f%%)\n", file.c_str(), elapsed,
                shown * 100);
    std::exception_ptr error = gLoadJob.collect();
    os2::loading::end();
    os2::finishLoadingScreen();
    if (error)
        std::rethrow_exception(error);
    std::printf("opensims2: scene loaded on the worker in %.2f s\n", CACurrentMediaTime() - start);
    [self finishScene];
}

// Loads the scene the arguments pick. Headless (--screenshot) each scene runs
// its steps; while one asks for another scene, that one is loaded in turn,
// and the last saves the screenshot and exits.
- (void)runScenes {
    for (bool first = true;; first = false) {
        if (first && argument(@"--loading-shot"))
            [self loadSceneWithLoadingShot];
        else
            [self loadScene];
        if (!gPendingScene || !headless())
            return;
        if (auto shot = argument(@"--screenshot"); shot && [sceneArgs() containsObject:@"--flow-shots"])
            [self captureScene:os2::numberedShot(*shot, ++_flowShot)];
        [self applyPendingScene];
    }
}

// A scene loads in two parts (docs/loading-mac.md): prepareScene builds its
// data, models, textures and VM, and may run on the worker (it touches no
// AppKit object: window titles and the view's colours wait for the other part);
// finishScene does the rest on the main thread (the view, the UI layers'
// attachment, the headless steps and screenshot). Headless both run here.
- (void)loadScene {
    [self prepareScene];
    [self finishScene];
}

- (void)prepareScene {
    [self ensureGameState];
    _scene = os2::sceneOf(processArguments());
    _cas = [sceneArgs() containsObject:@"--cas"]; // Create-a-Sim (app/cas_mode.hpp)
    gStepsUsed = 0;
    applyLighting(*_viewDelegate.renderer);
    if (_cas) {
        [self prepareCas:_view];
        return;
    }
    switch (_scene.kind) {
    case os2::Scene::Kind::MainMenu: [self prepareMainMenu:_scene]; break;
    case os2::Scene::Kind::Neighborhood: [self prepareHood:_view query:hoodFolder(_scene.hood).string()]; break;
    case os2::Scene::Kind::Lot:
        gHoodLot = os2::hoodLotPackage(gameDir() / "TSData/Res", hoodFolder(_scene.hood).string(), _scene.lot).string();
        gLotVacant = false;
        try {
            const auto hood = os2::hood::Neighborhood::load(hoodFolder(_scene.hood), false);
            if (const auto* l = hood.lot(_scene.lot))
                gLotVacant = l->families.empty();
        } catch (const std::exception& err) {
            os2::logOnce("lot vacancy", _scene.hood, err);
        }
        [self prepareLot];
        break;
    case os2::Scene::Kind::Other: [self prepareLot]; break;
    case os2::Scene::Kind::Cas: break; // handled above (_cas)
    }
}

- (void)finishScene {
    _view.clearColor = MTLClearColorMake(0.18, 0.22, 0.28, 1.0);
    applyAudioSettings(); // the options' volumes (the settings load with the first scene)
    // The scene's music state, as the game's music manager switches it
    // (app/lot_audio.hpp GameAudio): the opening music in the main menu,
    // NHood in the neighborhood (afresh, unless the opening music fades
    // into it), CAS in Create-a-Sim, the lot's (stereos only) on a lot.
    if (gGameAudio) {
        gGameAudio->setHeld(os2::moviePlaying()); // (the intro may still be playing)
        if (_cas) {
            gGameAudio->setMusic("CAS");
        } else if (_scene.kind == os2::Scene::Kind::MainMenu) {
            gGameAudio->setMusic("Splash");
        } else if (_scene.kind == os2::Scene::Kind::Neighborhood) {
            if (gGameAudio->music() != "Splash")
                gGameAudio->stopAllMusic();
            gGameAudio->setMusic("NHood");
        } else {
            gGameAudio->setMusic(argument(@"--music").value_or(""));
        }
        gGameAudio->setNeighborhoodAmbience(!_cas && _scene.kind == os2::Scene::Kind::Neighborhood);
    }
    if (_cas) {
        [self finishCas:_view];
        std::fflush(stdout);
        return;
    }
    switch (_scene.kind) {
    case os2::Scene::Kind::MainMenu: [self finishMainMenu:_scene]; break;
    case os2::Scene::Kind::Neighborhood: [self finishHood:_view]; break;
    case os2::Scene::Kind::Lot:
    case os2::Scene::Kind::Other: [self finishLot]; break;
    case os2::Scene::Kind::Cas: break;
    }
    std::fflush(stdout);
}

// Unloads the scene and sets the arguments of the one asked for (with the
// steps not used yet, headless).
- (void)applyPendingScene {
    const os2::Scene next = *gPendingScene;
    const size_t used = gStepsUsed;
    gPendingScene.reset();
    const std::vector<std::string> args = os2::sceneArguments(processArguments(), next, headless() ? used : 0);
    const CFTimeInterval start = CACurrentMediaTime();
    dwellHeadlessAudio(); // (headless: the scene's music heard for a while)
    unloadScene();
    if (gGameAudio)
        gGameAudio->beginScene();
    setSceneArguments(args);
    [_viewDelegate resetRenderer:_view];
    std::string line;
    for (size_t i = 1; i < args.size(); ++i)
        line += " " + args[i];
    std::printf("opensims2: scene switch (%.2f s to unload):%s\n", CACurrentMediaTime() - start, line.c_str());
    std::fflush(stdout);
}

// With a window: the scene asked for, in place of this one, loaded behind
// its loading screen.
- (void)switchScene {
    if (!gPendingScene || gSceneLoading)
        return;
    [self applyPendingScene];
    [self beginSceneLoad];
}

// Renders the current scene offscreen at 1280x720 and saves it.
- (void)captureScene:(const std::string&)file {
    const uint32_t w = 1280, h = 720;
    _view.camera->setViewport(float(w), float(h));
    _view.camera->snap();
    if (gSimulation)
        poseActors(*_viewDelegate.renderer, gHeadlessClock);
    else
        updateLotView(*_viewDelegate.renderer, false);
    const os2::FrameView fv{_view.camera->view(), _view.camera->projection(float(w) / float(h))};
    const MTLClearColor c = _view.clearColor;
    _viewDelegate.renderer->setReflections(reflectionsOn());
    const auto img = _viewDelegate.renderer->capture(w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), 1));
    os2::writePng(file, img);
    std::printf("opensims2: wrote %s\n", file.c_str());
    std::fflush(stdout);
}

// --menu (or no scene flag): the main menu, which chooses the neighborhood
// (app/main_menu.hpp). Headless: --menu-select <i> opens the preview of
// neighborhood i, and the steps click it.
- (void)prepareMainMenu:(const os2::Scene&)scene {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    renderer.hideTestScene();
    [_view setCamera:std::make_unique<os2::LotCamera>(loadLiveCamera())];
    gCamera = _view.camera;
    std::vector<fs::path> dirs;
    for (const auto& prefix : gUser->shipped())
        if (prefix != "Tutorial")
            dirs.push_back(gUser->neighborhoodForReading(prefix));
    gMenu = std::make_unique<os2::MainMenu>(*gIndex, gameDir() / "TSData/Res", os2::readMainMenuHoods(dirs),
                                            scene.fromNeighborhood);
    perfLap("menu: scripts");
    gMenu->setEpisodes(os2::loadTutorialEpisodes(
        gameDir() / "TSData/Res", gUser && gUser->hasCopy("Tutorial") ? gUser->neighborhoodsDir() / "Tutorial" : fs::path()));
    _menuDirs = dirs;
}

- (void)finishMainMenu:(const os2::Scene&)scene {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    if (gGameAudio) // the buttons' click sounds
        gMenu->setSoundPlayer([](const std::string& name) { gGameAudio->playUi(name); });
    const std::vector<fs::path>& dirs = _menuDirs;
    gMenu->attach((__bridge MTL::Device*)_view.device, uint32_t(_view.colorPixelFormat),
                  uint32_t(_view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gMenu)
            gMenu->encode(enc, w, h, gUiScale);
    });
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _window.title = @"opensims2";
    const float w = 1280, h = 720;
    gMenu->setCanvas(int(w), int(h));
    for (size_t i = 0; i < dirs.size(); ++i) {
        const auto r = gMenu->thumbnailRect(int(i));
        std::printf("opensims2: main menu: %s at (%.0f, %.0f)-(%.0f, %.0f)\n", dirs[i].filename().string().c_str(),
                    r[0], r[1], r[2], r[3]);
    }
    if (!gMenu->ok())
        std::fprintf(stderr, "opensims2: main menu: scripts missing\n");
    if (!headless())
        return;
    if (auto sel = argument(@"--menu-select"))
        gMenu->preview(std::stoi(*sel));
    // --menu-learn <i>: "Aprenda a Jogar" open with episode i selected.
    if (auto ep = argument(@"--menu-learn")) {
        gMenu->openLearn(std::stoi(*ep));
        for (int i = 0;; ++i) {
            const auto r = gMenu->episodeRect(i);
            if (r[2] <= r[0] && i > 8)
                break;
            if (r[2] > r[0])
                std::printf("opensims2: main menu: episode %d at (%.0f, %.0f)-(%.0f, %.0f)\n", i, r[0], r[1], r[2], r[3]);
        }
    }
    const auto steps = os2::flowSteps(processArguments());
    for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
        gStepsUsed = k + 1;
        if (steps[k].kind == os2::FlowStep::Kind::Click)
            pressMainMenu(simd_make_float2(steps[k].x, steps[k].y), w, h);
    }
    if (gPendingScene)
        return;
    [self captureScene:*argument(@"--screenshot")];
    std::exit(0);
}

// A lot (--lot, or --hood <N> --hood-lot <id>), or the test objects and Sims.
- (void)prepareLot {
    OS2View* view = _view;
    os2::CameraDef liveCamera = loadLiveCamera();
    // --camera-scale <k>: every zoom distance times k (close-ups of Sims below the closest level).
    if (auto k = argument(@"--camera-scale"))
        for (float& d : liveCamera.distances)
            d *= std::stof(*k);
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

- (void)finishLot {
    OS2View* view = _view;
    const auto uiCanvas = _uiCanvas;
    const simd_float3 pivot = _pivot;
    { // (indented as it was inside the start-up's try block)
        // --pie <object> [--pie-select N]: open an object's pie menu for the
        // selected Sim (--sim-id <id|name>) and queue entry N; --click x,y
        // (repeatable) clicks like the mouse. Before --run-vm's ticks.
        const uint32_t shotW = uiCanvas ? uiCanvas->first : 1280, shotH = uiCanvas ? uiCanvas->second : 720;
        if (gPie) {
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
            const bool headless = argument(@"--screenshot").has_value();
            const float cw = headless ? float(shotW) : float(view.bounds.size.width);
            const float ch = headless ? float(shotH) : float(view.bounds.size.height);
            if (gTutorial && headless) { // the tutorial presses and points at controls laid out at the shot's size
                gLive->setCanvas(int(cw), int(ch));
                gTutorial->setCanvas(int(cw), int(ch));
                pollTutorial(gHeadlessClock);
            }
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
            // The --click / --right-click / --wait steps, in order (app/game_flow.hpp):
            // a step that leaves the lot leaves the rest to the next scene.
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
                            {"d", kD},       {"w", kW},         {"s", kS},   {"comma", kComma}, {"period", kPeriod}};
                        const size_t colon = st.text.find(':');
                        const std::string name = st.text.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
                        const double secs = colon == std::string::npos ? 0.5 : std::atof(st.text.c_str() + colon + 1);
                        if (auto k = kKeys.find(name); k != kKeys.end()) {
                            const simd_float3 p0 = view.camera->pivot();
                            const float y0 = view.camera->yaw();
                            [view holdKey:k->second seconds:secs];
                            const simd_float3 p1 = view.camera->pivot();
                            std::printf("opensims2: key %s held %.2f s: pivot (%.2f, %.2f) -> (%.2f, %.2f), yaw %.1f -> %.1f\n",
                                        name.c_str(), secs, p0.x, p0.y, p1.x, p1.y, y0, view.camera->yaw());
                        }
                        continue;
                    }
                    if (st.text == "space")
                        selectNextFamilySim();
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
            // OS2_PICK_MAP=<png>: what a click would pick at every 4th pixel (OS2_PICK_MAP_STEP), an
            // object a colour (a look at the picking against the picture).
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
            std::fflush(stdout);
        }
        attachLetterbox(*_viewDelegate.renderer, (__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                        uint32_t(view.depthStencilPixelFormat));
        // --cinematic <scene>[@id[,id...]][:mirror][:house]: a cinematic with those actors (in the CINE's
        // order; default: the lot's Sims for its Sim actors), started at once.
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
        if (gPendingScene)
            return; // headless: the steps left the lot (runScenes loads the next scene)
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
        // --screenshot <file.png>: render one frame offscreen, save it and quit.
        if (auto shot = argument(@"--screenshot")) {
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
    }
}

// --hood <N>: the neighborhood view (app/hood_mode.hpp); with --screenshot
// runs the steps (clicks on lots and the dialog), then renders one frame and quits.
- (void)prepareHood:(OS2View*)view query:(const std::string&)query {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    gHood = std::make_unique<os2::HoodMode>(renderer, *gIndex, gameDir(), query, processArguments());
    for (const auto& line : gHood->view().report())
        std::printf("opensims2: %s\n", line.c_str());
    // Entering a lot and the main menu switch scenes in place (app/game_flow.hpp).
    gHood->onEnterLot = [](const os2::HoodLot& lot) {
        const auto& hood = gHood->view().neighborhood();
        gLotEntry = {true, lot.type, lot.family != 0, lot.name, lot.familyName, lot.family,
                     hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package")}; // its loading screen
        os2::Scene next;
        next.kind = os2::Scene::Kind::Lot;
        next.hood = os2::sceneOf(processArguments()).hood;
        next.lot = lot.id;
        requestScene(next);
    };
    // "Create New Family": Create-a-Sim for this neighborhood, then back here
    // with the new family selected in the bin.
    gHood->onCreateFamily = [](const fs::path& folder) {
        os2::Scene next;
        next.kind = os2::Scene::Kind::Cas;
        next.hood = folder.string();
        next.returnHood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    // A lot placed, moved or binned: the neighborhood view built again,
    // seen from where the camera was.
    gHood->onReload = [] {
        if (gCamera)
            gHoodReloadPose = gCamera->pose();
        os2::Scene next;
        next.kind = os2::Scene::Kind::Neighborhood;
        next.hood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    gHood->onQuit = [] { requestQuit(); };
    gHood->onMainMenu = [] {
        os2::Scene next;
        next.kind = os2::Scene::Kind::MainMenu;
        next.fromNeighborhood = true;
        next.returnHood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    [view setCamera:gHood->makeCamera()];
    gCamera = view.camera;
    if (auto z = argument(@"--zoom"))
        view.camera->zoomBy(std::stoi(*z));
    if (auto o = argument(@"--orientation"))
        view.camera->rotateBy(std::stoi(*o));
    view.camera->snap();
    if (gHoodReloadPose) {
        view.camera->setPose(*gHoodReloadPose);
        gHoodReloadPose.reset();
    }
}

- (void)finishHood:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    if (gGameAudio) // the buttons' click sounds
        gHood->ui().playSound = [](const std::string& name) { gGameAudio->playUi(name); };
    // Head shots for the characters whose files have none (app/head_shot.hpp).
    gHood->ui().renderHead = [rp = &renderer](const fs::path& file) {
        return os2::renderHeadShot(*rp, *gIndex, simParts(*gIndex), file, 128, gameDir() / "TSData/Res");
    };
    // The Decorations catalog's pictures: effects have UI images, the rest are rendered.
    gHood->ui().renderDecorThumb = [rp = &renderer](const os2::hood::DecorProduct& p) -> std::optional<os2::RgbaImage> {
        const int model = gHood->view().decorationModel(p);
        if (model < 0)
            return std::nullopt;
        return os2::renderModelThumbnail(*rp, model, gHood->view().decorationBounds(p), 94);
    };
    gHood->ui().setForceRenderHeads(argument(@"--hood-render-heads").has_value());
    // The Options panel's option windows (audio, game, graphics, camera, credits).
    gHood->ui().options().setSettings(gSettings);
    gHood->ui().options().onChanged = [](const os2::GameSettings& s) {
        gSettings = s;
        if (gUser)
            s.save(*gUser);
        gSettings.applyGraphics(); // for what loads next
        applyAudioSettings();
        if (gHood) // Shadows: the terrain's light map is rebuilt now (the object shadow planes follow the option when drawn)
            gHood->view().refreshLightMap();
    };
    if (gGameAudio)
        gHood->ui().options().playSound = [](const std::string& name) { gGameAudio->playUi(name); };
    view.clearColor = MTLClearColorMake(0.42, 0.55, 0.72, 1.0);
    gHood->ui().attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                       uint32_t(view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gHood)
            gHood->ui().encode(enc, w, h, gUiScale);
    });
    _window.title = [NSString stringWithUTF8String:("opensims2 - " + gHood->view().displayName()).c_str()];
    if (gCreatedFamily) {
        gHood->familyCreated(*gCreatedFamily);
        gCreatedFamily.reset();
    }
    std::fflush(stdout);
    if (auto shot = argument(@"--screenshot")) {
        const uint32_t w = 1280, h = 720;
        view.camera->setViewport(float(w), float(h));
        gHood->scriptedView(*view.camera, float(w), float(h));
        view.camera->snap();
        // Where the lots are on the 1280x720 canvas, for click steps.
        const simd_float4x4 m = simd_mul(view.camera->projection(float(w) / float(h)), view.camera->view());
        for (const auto& lot : gHood->view().lots()) {
            const simd_float3 c = gHood->view().lotCentre(lot);
            const simd_float4 q = simd_mul(m, simd_make_float4(c, 1));
            const float x = (q.x / q.w + 1) / 2 * float(w), y = (1 - q.y / q.w) / 2 * float(h);
            if (q.w > 0 && x >= 0 && x < float(w) && y >= 0 && y < float(h))
                std::printf("opensims2: lot %u \"%s\" at (%.0f, %.0f)\n", lot.id, lot.name.c_str(), x, y);
        }
        const auto steps = os2::flowSteps(processArguments());
        for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
            gStepsUsed = k + 1;
            if (steps[k].kind == os2::FlowStep::Kind::Click)
            {
                gHood->mouseDown(*view.camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
                gHood->mouseUp(*view.camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
            }
        }
        if (gPendingScene)
            return;
        [self captureScene:*shot];
        std::exit(0);
    }
}

// --cas: Create-a-Sim (app/cas_mode.hpp); with --screenshot renders one frame and quits.
- (void)prepareCas:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
    if (const auto* state = lighting.state(argument(@"--lighting").value_or("CAS"))) {
        renderer.setLighting(*state); // Lighting.txt's "CAS" state unless --lighting names another
        if (!argument(@"--lighting")) {
            // The CAS state leaves the Sim to the studio lamps of GlobalLots/CAS!.package
            // (app/cas_studio, which replaces these light sets once it loads); without the
            // studio (--cas-no-studio) its boom light (casStudio_lightingFloorStudioBoom_light,
            // intensity 1.25, colour (1, .95, .85)) stands in as a directional light from the
            // camera's side, above (inferred placement).
            auto set = os2::Renderer::outdoorLights(*state, true);
            set.lights.push_back({simd_normalize(simd_make_float3(0.35f, 1.0f, 0.9f)), simd_make_float3(1.0f, 0.95f, 0.85f) * 1.25f * 0.6f});
            renderer.setLightSets({set, set});
        }
    }
    // Families are saved into the user's neighborhoods (app/user_data.hpp) unless --cas-out says otherwise.
    std::vector<std::string> casArgs = processArguments();
    if (gUser && !argument(@"--cas-out")) {
        casArgs.push_back("--cas-out");
        casArgs.push_back(gUser->neighborhoodsDir().string());
    }
    gCas = std::make_unique<os2::CasMode>(renderer, *gIndex, gameDir(), casArgs);
    gCas->playSound = [](const std::string& name) { // the buttons' click sounds
        if (gGameAudio)
            gGameAudio->playUi(name);
    };
    // Opened from a neighborhood (Scene::Kind::Cas): Accept saves the family
    // there and goes back with it selected; Exit goes back without one.
    if (const os2::Scene sc = os2::sceneOf(processArguments()); sc.kind == os2::Scene::Kind::Cas && !sc.returnHood.empty()) {
        const std::string ret = sc.returnHood;
        auto back = [ret] {
            os2::Scene next;
            next.kind = os2::Scene::Kind::Neighborhood;
            next.hood = ret;
            requestScene(next);
        };
        gCas->onFamilySaved = [back](uint32_t family) {
            gCreatedFamily = family;
            back();
        };
        gCas->onExit = back;
    }
    [view setCamera:gCas->makeCamera()];
    gCamera = view.camera;
    gCas->setCamera(view.camera);
}

- (void)finishCas:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    view.clearColor = MTLClearColorMake(0.33, 0.36, 0.45, 1.0);
    gCas->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                 uint32_t(view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gCas)
            gCas->encode(enc, w, h, gUiScale);
    });
    if (auto shot = argument(@"--screenshot")) {
        uint32_t w = 1024, h = 768; // --cas-size WxH for another window shape
        if (auto sz = argument(@"--cas-size"))
            std::sscanf(sz->c_str(), "%ux%u", &w, &h);
        gCas->setCanvas(int(w), int(h));
        gCas->runScripted(processArguments());
        if (gPendingScene)
            return; // the steps went back to the neighborhood: the next scene takes the shot
        view.camera->setViewport(float(w), float(h));
        view.camera->snap();
        gCas->update(0.5);
        view.camera->snap();
        for (const auto& line : gCas->report())
            std::printf("opensims2: %s\n", line.c_str());
        const os2::FrameView fv{view.camera->view(), view.camera->projection(float(w) / float(h))};
        MTLClearColor c = view.clearColor;
        gUiScale = 1;
        renderer.setReflections(reflectionsOn());
        const auto img = renderer.capture(w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), 1));
        os2::writePng(*shot, img);
        std::printf("opensims2: wrote %s\n", shot->c_str());
        std::fflush(stdout);
        std::exit(0);
    }
    gCas->runScripted(processArguments());
    _window.title = [NSString stringWithUTF8String:gCas->title().c_str()];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

// Cmd-Q, the window closed or the game flow's Quit (not while a scene loads:
// applicationShouldTerminate ends the process then): the scene and the
// app's caches go here, before exit() runs any static's destructor.
- (void)applicationWillTerminate:(NSNotification*)note {
    releaseAppState();
}
@end

static void buildMenu() {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit opensims2" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
}

// --help / -h: the command line's options (the file's header comment has the
// details); prints and exits without opening a window.
static void printUsage() {
    std::puts(
        "usage: opensims2 [options]\n"
        "\n"
        "scenes (default: the main menu; the game data is found by --data <dir>, OS2_DATA or a folder asked for):\n"
        "  --hood <N001>            a neighborhood      --hood-lot <n>   enter one of its lots\n"
        "  --lot <N001_Lot82|file>  a lot's terrain, walls, objects        --cas    Create-a-Sim\n"
        "  --sim / --character / --skinned / --anim   a Sim, its animation  --movie <name> / --intro  movies\n"
        "  --play-sound <name>      play a sound and exit (--sound-index, --sound-wav <out.wav>)\n"
        "view:\n"
        "  --ui <instance-hex|live> a UI script (live: the game's panel)   --ui-show <id,...>  --ui-hover x,y\n"
        "  --vm                     run the lot's objects in the SimAntics VM   --run-vm <ticks>\n"
        "  --mode <live|buy|build>  --walls <up|cutaway|down>  --lighting <state>  --zoom --pivot --heading\n"
        "  --no-audio --mute --no-edge-scroll --reflections on|off --camera-scale <s> --user-dir <dir>\n"
        "headless (no window):\n"
        "  --screenshot <out.png>   render a frame (--frames <n>, --time <s>) and exit\n"
        "  --click x,y              click the canvas (repeatable; --right-click, --key <name>, --wait <s>)\n"
        "  --pie <object> [--pie-select N]   --dialog-answer <n> / --dialog-click x,y|return|escape\n"
        "  --loading-shot <png> [--loading-at <f>]   a scene's loading screen\n"
        "  -h, --help               this text\n");
}

// An app opened from the Finder or the Dock starts with 256 open files
// (launchd's soft limit; a terminal gives far more). The resource index keeps
// up to 128 packages open, the Sim parts' parallel read a descriptor per
// package it reads (the base game's and every neighborhood's character
// files), plus the scene's own files, Metal and audio: past 256 a package
// could not be reopened (DbpfError on the loading worker, std::terminate) on
// the first lot entered. Raised to the hard limit (at most OPEN_MAX).
void raiseOpenFileLimit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    const rlim_t want = std::min<rlim_t>(rl.rlim_max, OPEN_MAX);
    if (rl.rlim_cur >= want)
        return;
    rl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        std::fprintf(stderr, "opensims2: could not raise the open file limit (%s)\n", std::strerror(errno));
}

int main() {
    raiseOpenFileLimit();
    liveShotFile(); // (installs its SIGUSR1 handler before anything can send one)
    // Quitting tears the scene down as a scene switch does (in gScene's
    // order, while the app's globals it uses are still there), then frees
    // the app's caches of the game's data, before the namespace-scope
    // statics go: registered after them, it runs first (releaseAppState).
    std::atexit([] { releaseAppState(); });
    @autoreleasepool {
        for (NSString* a in NSProcessInfo.processInfo.arguments)
            if ([a isEqualToString:@"--help"] || [a isEqualToString:@"-h"]) {
                printUsage();
                return 0;
            }
        // The character folders of earlier sessions that did not exit
        // (crashed or killed) go (app/game_save.hpp); this one's is removed
        // at exit.
        {
            std::error_code ec;
            if (const size_t n = os2::removeStaleSessionCharacters(std::filesystem::temp_directory_path(ec)))
                std::printf("opensims2: removed %zu character folders of earlier sessions\n", n);
        }
        // --movie <name> --screenshot out.png [--frames n] [--time s] [--sound-wav mix.wav]: headless frames.
        if (auto movie = argument(@"--movie"); movie && argument(@"--screenshot")) {
            try {
                return os2::movieScreenshots(gameDir() / "TSData/Res", *movie, *argument(@"--screenshot"),
                                             std::stoi(argument(@"--frames").value_or("1")),
                                             std::stod(argument(@"--time").value_or("0")), 1280, 720,
                                             argument(@"--sound-wav").value_or(""));
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "error: %s\n", ex.what());
                return 1;
            }
        }
        if (auto sound = argument(@"--play-sound")) {
            try {
                const auto index = argument(@"--sound-index");
                return os2::playSoundCommand(gameDir() / "TSData/Res", *sound, index ? std::stoul(*index) : 0,
                                             argument(@"--sound-wav").value_or(""));
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "error: %s\n", ex.what());
                return 1;
            }
        }
        // No data found and a window is wanted: ask for the folder (remembered).
        if (!argument(@"--screenshot") && !findGameDir())
            os2::askForGameData(kCameraConfig);
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        buildMenu();
        OS2AppDelegate* delegate = [[OS2AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
