// The game's folder, the scene's arguments and the flags read from them
// (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// The arguments of the scene being shown (app/game_flow.hpp): the process's
// at start-up, replaced when the game flow switches scenes in place.
NSArray<NSString*>* gSceneArgs = nil;
NSArray<NSString*>* sceneArgs() {
    if (!gSceneArgs)
        gSceneArgs = NSProcessInfo.processInfo.arguments;
    return gSceneArgs;
}
void setSceneArguments(const std::vector<std::string>& args) {
    NSMutableArray<NSString*>* a = [NSMutableArray array];
    for (const auto& s : args)
        [a addObject:[NSString stringWithUTF8String:s.c_str()]];
    gSceneArgs = a;
}

// The extracted game (data/game): --data <dir>, else $OS2_DATA, else the first
// data/game found walking up from the executable (the repo when run from build/),
// else the packaged-app places (app/data_location.hpp).
std::optional<fs::path> findGameDir() {
    NSArray<NSString*>* args = sceneArgs();
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
    // Packaged app: remembered folder, next to the .app, Application Support.
    return os2::locateGameData(NSBundle.mainBundle.executablePath.UTF8String, kCameraConfig);
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

// The arguments, the executable's full path first (for a restart).
std::vector<std::string> processArguments() {
    std::vector<std::string> out{NSBundle.mainBundle.executablePath.UTF8String};
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i < args.count; ++i)
        out.emplace_back(args[i].UTF8String);
    return out;
}

// Value of `--name <value>`, if given.
std::optional<std::string> argument(NSString* name) {
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:name])
            return std::string(args[i + 1].UTF8String);
    return std::nullopt;
}

// Whether mirrors reflect: the Reflection graphics option (gSettings), or
// --reflections on|off for this run.
bool reflectionsOn() {
    static const std::optional<std::string> forced = argument(@"--reflections");
    if (forced)
        return *forced != "off" && *forced != "0";
    return gSettings.reflections;
}

// OS2_PERF=1: timings of the load phases and of the frames on stdout
// (docs/performance.md); nothing else changes.
bool perfOn() {
    static const bool on = std::getenv("OS2_PERF") != nullptr;
    return on;
}
// OS2_LIVE_SHOT=<file.png>: each SIGUSR1 saves the next frame the window
// draws (the scene and its UI, at the drawable's size) to that file, for
// live QA when a screen capture of the window is not possible.
volatile std::sig_atomic_t gLiveShotWanted = 0;
const char* liveShotFile() {
    static const char* file = [] {
        const char* f = std::getenv("OS2_LIVE_SHOT");
        if (f && *f)
            std::signal(SIGUSR1, [](int) { gLiveShotWanted = 1; });
        return f && *f ? f : nullptr;
    }();
    return file;
}
void perfLap(const char* what) {
    static CFTimeInterval last = CACurrentMediaTime();
    os2::loading::phase(what); // the loading screen's progress (app/scene_loader.hpp)
    if (!perfOn())
        return;
    const CFTimeInterval now = CACurrentMediaTime();
    std::printf("opensims2: perf: %-26s %8.1f ms\n", what, (now - last) * 1000);
    last = now;
}

// The lot to show: --lot, or the lot --hood-lot entered.
std::optional<std::string> lotQuery() {
    return gHoodLot ? gHoodLot : argument(@"--lot");
}

// Objects to show: every --object <CRES name | 0xGUID | OBJD name>, else the
// dining chair (unless a lot is shown).
std::vector<std::string> objectQueries() {
    std::vector<std::string> out;
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--object"])
            out.emplace_back(args[i + 1].UTF8String);
    if (out.empty() && !lotQuery() && !argument(@"--skinned") && !argument(@"--sim") && !argument(@"--character"))
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

// A character file given as a path, or by name ("N001_User00011") under the
// game's neighborhoods.
fs::path characterPath(const std::string& query) {
    if (fs::exists(query))
        return query;
    const fs::path hoods = gameDir() / "TSData/Res/UserData/Neighborhoods";
    const std::string file = query.ends_with(".package") ? query : query + ".package";
    if (fs::exists(hoods))
        for (const auto& hood : fs::directory_iterator(hoods))
            if (fs::exists(hood.path() / "Characters" / file))
                return hood.path() / "Characters" / file;
    throw std::runtime_error("character not found: " + query);
}

bool liveUiRequested() {
    return argument(@"--ui").value_or("") == "live";
}

// The mode flag the lot opens with: --mode, else Buy for a lot without a
// family (app/game_flow.hpp lotStartMode), else none (Live).
std::optional<std::string> startMode() {
    const std::string m = os2::lotStartMode(!gLotVacant, argument(@"--mode"));
    return m.empty() ? std::nullopt : std::optional<std::string>(m);
}

} // namespace os2app
