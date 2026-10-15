// The app's delegates (main.mm): the view's (frames) and the app's (the
// window, the scenes' loading and switching), with the app delegate's state
// shared by its scene parts in other files, categories of the same class:
// the main menu, neighborhood and Create-a-Sim scenes (scene_setup.mm) and
// the lot scene (scene_lot.mm).
#pragma once

#import "app/app_view.h"

@interface OS2ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view;
- (os2::Renderer*)renderer;
- (void)resetRenderer:(MTKView*)view;
- (void)restartClock;
@end

@interface OS2AppDelegate : NSObject <NSApplicationDelegate>
@end

// Its state (the ivars of its implementation in main.mm, seen by the categories).
@interface OS2AppDelegate () {
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
- (void)captureScene:(const std::string&)file;
@end

// scene_setup.mm
@interface OS2AppDelegate (Scenes)
- (void)prepareMainMenu:(const os2::Scene&)scene;
- (void)finishMainMenu:(const os2::Scene&)scene;
- (void)prepareHood:(OS2View*)view query:(const std::string&)query;
- (void)finishHood:(OS2View*)view;
- (void)prepareCas:(OS2View*)view;
- (void)finishCas:(OS2View*)view;
@end

// scene_lot.mm
@interface OS2AppDelegate (LotScene)
- (void)prepareLot;
- (void)finishLot;
@end
