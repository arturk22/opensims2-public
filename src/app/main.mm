// macOS shell: an AppKit window whose MTKView drives os2::Renderer every frame
// and feeds input to the lot camera. The renderer itself is plain C++ on
// metal-cpp; this file only bridges the Objective-C objects across.
//
// Camera controls: WASD/arrows pan, Q/E or ,/. rotate, Z/X, +/- or the wheel
// zoom, R/F or Page Up/Down tilt, right/middle drag rotates and tilts freely,
// Home resets. --lighting <state> picks a lighting state from Lighting.txt
// (day, night, neighbourhood, ...); --lot <N001_Lot82 | file.package> shows a
// lot's terrain, floors and walls.

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "formats/camera_config.hpp"
#include "formats/image.hpp"
#include "formats/dbpf.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kCameraConfig = "TSData/Res/UserData/Cameras/cameras.txt";
constexpr const char* kLightingConfig = "TSData/Res/Lights/Lighting.txt";

// The extracted game (data/game): --data <dir>, else $OS2_DATA, else the first
// data/game found walking up from the executable (the repo when run from build/).
std::optional<fs::path> findGameDir() {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--data"])
            return fs::path(args[i + 1].UTF8String);
    if (const char* env = std::getenv("OS2_DATA"))
        return fs::path(env);
    fs::path dir = fs::path(NSBundle.mainBundle.executablePath.UTF8String).parent_path();
    for (; !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "data/game" / kCameraConfig))
            return dir / "data/game";
        if (dir == dir.parent_path())
            break;
    }
    return std::nullopt;
}

fs::path gameDir() {
    std::optional<fs::path> game = findGameDir();
    if (!game)
        throw std::runtime_error("data/game not found (pass --data <dir> or set OS2_DATA)");
    return *game;
}

os2::CameraDef loadLiveCamera() {
    const fs::path game = gameDir();
    auto cameras = os2::loadCameraConfig(game / kCameraConfig);
    const os2::CameraDef* live = os2::findCamera(cameras, "Live");
    if (!live)
        throw std::runtime_error("no \"Live\" camera in " + (game / kCameraConfig).string());
    return *live;
}

// Value of `--name <value>`, if given.
std::optional<std::string> argument(NSString* name) {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:name])
            return std::string(args[i + 1].UTF8String);
    return std::nullopt;
}

// Objects to show: every --object <CRES name | 0xGUID | OBJD name>, else the
// dining chair (unless a lot is shown).
std::vector<std::string> objectQueries() {
    std::vector<std::string> out;
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--object"])
            out.emplace_back(args[i + 1].UTF8String);
    if (out.empty() && !argument(@"--lot"))
        out.emplace_back("0xEC7618F6"); // Chair - Dining - Value1
    return out;
}

// A lot package given as a path, or by name ("N001_Lot82") under the game's neighborhoods.
fs::path lotPath(const std::string& query) {
    if (fs::exists(query))
        return query;
    const fs::path hoods = gameDir() / "TSData/Res/UserData/Neighborhoods";
    const std::string file = query.ends_with(".package") ? query : query + ".package";
    if (fs::exists(hoods))
        for (const auto& hood : fs::directory_iterator(hoods))
            if (fs::exists(hood.path() / "Lots" / file))
                return hood.path() / "Lots" / file;
    throw std::runtime_error("lot not found: " + query);
}

// Loads --lot into the renderer in place of the test floor. Returns the middle
// of the lot at ground level, for the camera.
std::optional<simd_float3> loadLot(os2::Renderer& renderer, os2::ResourceIndex& index) {
    const auto query = argument(@"--lot");
    if (!query)
        return std::nullopt;
    CFTimeInterval start = CACurrentMediaTime();
    os2::DbpfPackage pkg(lotPath(*query));
    const os2::Lot lot = os2::Lot::load(pkg);
    const os2::PatternCatalog patterns(index, gameDir() / "TSData/Res");
    const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns);
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    renderer.hideTestScene();
    renderer.addInstance(renderer.addObject(model, index), matrix_identity_float4x4);
    std::printf("opensims2: lot %s: %ux%u tiles, levels %d..%d, %zu walls, %zu paints (%zu parts) in %.2f s\n",
                query->c_str(), lot.width, lot.height, lot.minLevel, lot.minLevel + lot.levelCount - 1,
                lot.walls.size(), lot.paints.size(), model.parts.size(), CACurrentMediaTime() - start);
    const int cx = int(lot.width / 2), cy = int(lot.height / 2);
    return simd_make_float3(float(lot.width) / 2, float(lot.height) / 2, lot.elevation(0, cx, cy));
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

// Virtual key codes (layout independent).
enum Key : unsigned short {
    kA = 0, kS = 1, kD = 2, kF = 3, kZ = 6, kX = 7, kQ = 12, kW = 13, kE = 14, kR = 15,
    kEquals = 24, kMinus = 27, kComma = 43, kPeriod = 47, kPadPlus = 69, kPadMinus = 78,
    kHome = 115, kPageUp = 116, kPageDown = 121, kLeft = 123, kRight = 124, kDown = 125, kUp = 126,
};

} // namespace

// MTKView that keeps the camera and the keys held down.
@interface OS2View : MTKView
- (os2::LotCamera*)camera;
- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera;
- (void)applyHeldKeys:(double)dt;
@end

@implementation OS2View {
    std::unique_ptr<os2::LotCamera> _camera;
    std::set<unsigned short> _held;
    double _scroll; // accumulated precise (trackpad) scroll
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
    return _held.count(a) || _held.count(b);
}

- (void)applyHeldKeys:(double)dt {
    float right = float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]);
    float forward = float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]);
    _camera->pan(right, forward, dt);
    float tilt = float([self held:kR orKey:kPageUp]) - float([self held:kF orKey:kPageDown]);
    if (tilt != 0)
        _camera->tilt(tilt * 40.0f * float(dt));
}

- (void)keyDown:(NSEvent*)e {
    _held.insert(e.keyCode);
    if (e.isARepeat)
        return;
    switch (e.keyCode) {
    case kQ: case kComma: _camera->rotateBy(-1); break;
    case kE: case kPeriod: _camera->rotateBy(1); break;
    case kZ: case kEquals: case kPadPlus: _camera->zoomBy(1); break;
    case kX: case kMinus: case kPadMinus: _camera->zoomBy(-1); break;
    case kHome: _camera->reset(); break;
    default: break;
    }
}

- (void)keyUp:(NSEvent*)e {
    _held.erase(e.keyCode);
}

- (void)flagsChanged:(NSEvent*)e {
    // A key released while Cmd was down never sends keyUp.
    if (e.modifierFlags & NSEventModifierFlagCommand)
        _held.clear();
}

- (void)scrollWheel:(NSEvent*)e {
    if (!e.hasPreciseScrollingDeltas) {
        if (e.scrollingDeltaY != 0)
            _camera->zoomBy(e.scrollingDeltaY > 0 ? 1 : -1);
        return;
    }
    _scroll += e.scrollingDeltaY;
    const double notch = 40.0; // points of trackpad scroll per zoom level
    while (_scroll >= notch) {
        _camera->zoomBy(1);
        _scroll -= notch;
    }
    while (_scroll <= -notch) {
        _camera->zoomBy(-1);
        _scroll += notch;
    }
}

- (void)dragRotate:(NSEvent*)e {
    _camera->freeRotate(float(e.deltaX) * 0.3f);
    _camera->tilt(float(e.deltaY) * 0.2f);
}

- (void)rightMouseDragged:(NSEvent*)e {
    [self dragRotate:e];
}

- (void)otherMouseDragged:(NSEvent*)e {
    [self dragRotate:e];
}
@end

@interface OS2ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view;
- (os2::Renderer*)renderer;
@end

@implementation OS2ViewDelegate {
    std::unique_ptr<os2::Renderer> _renderer;
    CFTimeInterval _last;
}

- (instancetype)initWithView:(MTKView*)view {
    if ((self = [super init])) {
        _renderer = std::make_unique<os2::Renderer>((__bridge MTL::Device*)view.device,
                                                    uint32_t(view.colorPixelFormat),
                                                    uint32_t(view.depthStencilPixelFormat));
        _last = CACurrentMediaTime();
    }
    return self;
}

- (os2::Renderer*)renderer {
    return _renderer.get();
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)mtkView {
    OS2View* view = (OS2View*)mtkView;
    CFTimeInterval now = CACurrentMediaTime();
    double dt = std::min(now - _last, 0.1); // no huge steps after a stall
    _last = now;

    [view applyHeldKeys:dt];
    view.camera->update(dt);
    CGSize size = view.drawableSize;
    float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
    os2::FrameView frame{view.camera->view(), view.camera->projection(aspect)};

    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    _renderer->draw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable, frame);
}
@end

@interface OS2AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation OS2AppDelegate {
    NSWindow* _window;
    OS2ViewDelegate* _viewDelegate; // MTKView holds its delegate weakly
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
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
    view.clearColor = MTLClearColorMake(0.18, 0.22, 0.28, 1.0);
    view.preferredFramesPerSecond = 60;

    try {
        [view setCamera:std::make_unique<os2::LotCamera>(loadLiveCamera())];
        _viewDelegate = [[OS2ViewDelegate alloc] initWithView:view];
        applyLighting(*_viewDelegate.renderer);
        os2::ResourceIndex index(gameDir() / "TSData/Res");
        const simd_float3 objects = loadObjects(*_viewDelegate.renderer, index);
        view.camera->setPivot(loadLot(*_viewDelegate.renderer, index).value_or(objects));
        // --zoom <levels> / --orientation <steps>: start the camera that many steps from its default.
        if (auto z = argument(@"--zoom"))
            view.camera->zoomBy(std::stoi(*z));
        if (auto o = argument(@"--orientation"))
            view.camera->rotateBy(std::stoi(*o));
        view.camera->snap();
        // --screenshot <file.png>: render one frame offscreen, save it and quit.
        if (auto shot = argument(@"--screenshot")) {
            const uint32_t w = 1280, h = 720;
            os2::FrameView fv{view.camera->view(), view.camera->projection(float(w) / float(h))};
            MTLClearColor c = view.clearColor;
            auto img = _viewDelegate.renderer->capture(
                w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), float(c.alpha)));
            os2::writePng(*shot, img);
            std::printf("opensims2: wrote %s\n", shot->c_str());
            std::fflush(stdout);
            std::exit(0);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
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
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
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

int main() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        buildMenu();
        OS2AppDelegate* delegate = [[OS2AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
