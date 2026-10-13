#pragma once

// The game's mouse cursors (plain C++; main.mm turns them into NSCursors).
//
// The files are Windows cursors in TSData/Res/UI/Cursors. The game names
// them with one table (the strings after "Cursor" / "_1 _8 .ani .cur" in
// both executables, cTSUIServiceManager::SetupCursors): arrow, hourglass,
// edit, finger, simsdelete, simslevel, simslower, simsmove, simsplace,
// simsraise, simsrotate, simsrotatenw/ne/sw/se, left, right, down, up,
// upright, upleft, downright, downleft, explore, deadexpl, pan, liveperson,
// livenothing, liveobjectavail, liveobjectunavail, addpts, movein, bulldoze,
// evictFamily, moveLotToCatalog, taxi, simsflip, removepts, eyedropperTool,
// designTool, railingtool; the file is <name>_8.cur (8-bit colour), else
// <name>_1.cur (black and white), else <name>.cur. The tool cursor ids in
// the docs fit "id = table index + 18" (designTool 0x39, railingtool 0x3A,
// the lot mover's simsmove 0x19 over a lot it can move and simsplace 0x1A
// when the button goes down, docs/neighborhood-mac.md §15.1; inferred).
// hand_8, closedhand*_8, worldgreen/red_8, rotateview, selectsim and
// walkhere are not in the table (unused by the base game).
//
// Which cursor each state shows is in docs/camera-ui-mac.md §2.7 (the object
// tool's by analogy with the lot mover: inferred).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os2 {

struct CursorImage {
    int width = 0, height = 0;
    int hotX = 0, hotY = 0;
    std::vector<uint8_t> rgba; // rows top to bottom, straight alpha
};

// A .cur file (ICONDIR type 2, BMP images of 1, 4, 8, 24 or 32 bits): the
// image with the most colours at the largest size. AND-mask pixels are
// transparent, except a 1-bit image's "invert the screen" pixels, drawn
// black (there is no inverting cursor on the Mac). nullopt if malformed.
std::optional<CursorImage> decodeCursor(std::span<const uint8_t> file);

enum class GameCursor {
    Arrow,      // arrow: the UI and Live mode
    Hourglass,  // hourglass: loading
    Move,       // simsmove: the open hand (holding an object, over one to pick up)
    Place,      // simsplace: the closed hand (the button down on what is held)
    Rotate,     // simsrotate: the drag turned what is held
    Eyedropper, // eyedropperTool
    Design,     // designTool
    Count
};

// The game's table name of a cursor (above).
const char* gameCursorName(GameCursor c);

// The file for a table name in the cursors folder: <name>_8.cur, <name>_1.cur,
// <name>.cur, ignoring case. Empty if none.
std::filesystem::path findCursorFile(const std::filesystem::path& dir, const std::string& name);

// What the mouse is over, for the choice below.
struct CursorContext {
    enum class Mode { Other, Live, Buy, Build } mode = Mode::Other;
    bool loading = false;
    bool overUi = false; // a panel, the puck, the tutorial window, a pie menu
    // Buy mode's object tool.
    enum class BuyTool { Hand, Eyedropper, Design } buyTool = BuyTool::Hand;
    bool holding = false;    // an object follows the mouse
    bool pressing = false;   // the button is down on it (a drag turns it or moves it)
    bool turned = false;     // that drag has turned it
    bool overObject = false; // nothing held: an object the tool acts on is under the mouse
};

GameCursor chooseCursor(const CursorContext& c);

} // namespace os2
