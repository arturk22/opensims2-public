#include "app/data_location.hpp"

#import <AppKit/AppKit.h>

#include <vector>

namespace fs = std::filesystem;

namespace os2 {
namespace {

NSString* const kDefaultsKey = @"GameDataPath";

// `dir` itself, or dir/game, dir/data/game.
std::optional<fs::path> gameFolderIn(const fs::path& dir, const char* check) {
    for (const char* sub : {"", "game", "data/game"}) {
        const fs::path candidate = dir / sub;
        std::error_code ec;
        if (fs::exists(candidate / check, ec))
            return candidate;
    }
    return std::nullopt;
}

// A folder picked by hand may hold the data deeper down: a game install with
// TSData a few levels in, or a Mac release's .app bundle (Contents/...).
std::optional<fs::path> gameFolderUnder(const fs::path& dir, const char* check) {
    if (auto found = gameFolderIn(dir, check))
        return found;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 5 || !it->is_directory(ec) || it->is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::exists(it->path() / check, ec))
            return it->path();
    }
    return std::nullopt;
}

} // namespace

std::optional<fs::path> locateGameData(const fs::path& executable, const char* check) {
    if (NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:kDefaultsKey])
        if (auto found = gameFolderIn(saved.UTF8String, check))
            return found;
    std::vector<fs::path> places;
    // <name>.app/Contents/MacOS/<exe>: the folder holding the .app comes first.
    fs::path dir = executable.parent_path();
    for (int i = 0; i < 3 && dir.has_parent_path(); ++i)
        dir = dir.parent_path();
    places.push_back(dir);
    if (NSString* home = NSHomeDirectory())
        places.push_back(fs::path(home.UTF8String) / "Library/Application Support/opensims2");
    for (const fs::path& place : places)
        if (auto found = gameFolderIn(place, check))
            return found;
    return std::nullopt;
}

std::optional<fs::path> askForGameData(const char* check) {
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app activateIgnoringOtherApps:YES];
    for (;;) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"opensims2: locate the game data";
        panel.message = @"Choose your The Sims 2 folder (the one that contains TSData, or any "
                        @"folder above it), or the game's Mac app.";
        panel.prompt = @"Use this folder";
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.treatsFilePackagesAsDirectories = YES; // a Mac release's .app can be picked
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil)
            return std::nullopt;
        const fs::path chosen = panel.URL.path.UTF8String;
        if (auto found = gameFolderUnder(chosen, check)) {
            [NSUserDefaults.standardUserDefaults setObject:[NSString stringWithUTF8String:found->c_str()]
                                                    forKey:kDefaultsKey];
            return found;
        }
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"That folder has no game data";
        alert.informativeText = [NSString stringWithFormat:@"%s was not found in it.", check];
        [alert runModal];
    }
}

} // namespace os2
