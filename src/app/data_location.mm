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
        panel.message = @"Choose the extracted game folder (the one that contains TSData), "
                        @"or the folder that contains data/game.";
        panel.prompt = @"Use this folder";
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil)
            return std::nullopt;
        const fs::path chosen = panel.URL.path.UTF8String;
        if (auto found = gameFolderIn(chosen, check)) {
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
