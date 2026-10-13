#pragma once

// The game's mouse cursors (plain C++; main.mm turns them into NSCursors).
//
// The files are Windows cursors in TSData/Res/UI/Cursors. The game names
// them with one table, cTSUIServiceManager::SetupCursors (BV PPC 0x253260),
// which gives each name an explicit id (read in round 22; the id "=
// table index + 18" the round before had guessed is wrong): 1 arrow, 0xF
// hourglass, 3 edit, 0x12 finger, 0x15 simsdelete, 0x16 simslevel, 0x17
// simslower, 0x18 simsraise, 0x19 simsmove, 0x1A simsplace, 0x1B simsrotate,
// 0x1C..0x1F simsrotate nw/ne/sw/se, 0x20..0x27 left right up down upright
// upleft downright downleft, 0x28 explore, 0x29 deadexpl, 0x2A pan, 0x2B
// livenothing, 0x2C liveperson, 0x2D liveobjectavail, 0x2E liveobjectunavail,
// 0x2F addpts, 0x30 movein, 0x31 bulldoze, 0x32 evictFamily, 0x33
// Cursor_EvictStudents, 0x34 moveLotToCatalog, 0x35 taxi, 0x36 simsflip,
// 0x37 removepts, 0x38 eyedropperTool, 0x39 designTool, 0x3A railingtool,
// and in Bon Voyage 0x3B forsale and 0x3C Sledgehammer (no such file in the
// base game's folder). The file is <name>_8.cur (8-bit colour), else
// <name>_1.cur (black and white), else <name>.cur (hourglass: Hourglass_8.ani
// is animated and not read, HOURGLASS.cur is used).
// hand_8, closedhand*_8, worldgreen/red_8, rotateview, selectsim and
// walkhere are not in the table (unused by the base game).
//
// Which cursor each state shows is in docs/camera-ui-mac.md section 2.7: what
// the tools' code sets (lot mover and decoration tool 0x19/0x1A, rotating
// a lot 0x1C-0x1F, hand drag 0x2A, the roof tool's drag 0x1B, eyedropper
// 0x38, Design 0x39, railing 0x3A) is read from the binary; the rest, by
// the names, is marked inferred there.

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
    Railing,    // railingtool
    Delete,     // simsdelete: a "no entry" sign (not selected: the demolish tool's cursor is BV's Sledgehammer)
    Level,      // simslevel: terrain flatten
    Lower,      // simslower: terrain lower
    Raise,      // simsraise: terrain raise
    RotateNW,   // simsrotatenw: turning a lot to face front 0
    RotateNE,   // simsrotatene: front 3
    RotateSW,   // simsrotatesw: front 1
    RotateSE,   // simsrotatese: front 2
    ScrollLeft, ScrollRight, ScrollUp, ScrollDown, // the window's edge scrolls the camera
    ScrollUpRight, ScrollUpLeft, ScrollDownRight, ScrollDownLeft,
    Pan,                // pan: dragging the camera
    LiveNothing,        // livenothing
    LivePerson,         // liveperson
    LiveObjectAvail,    // liveobjectavail
    LiveObjectUnavail,  // liveobjectunavail
    MoveIn,             // movein
    Bulldoze,           // bulldoze
    EvictFamily,        // evictFamily
    MoveLotToCatalog,   // moveLotToCatalog
    Count
};

// The game's table name of a cursor (above), and its id there.
const char* gameCursorName(GameCursor c);
int gameCursorId(GameCursor c);

// The file for a table name in the cursors folder: <name>_8.cur, <name>_1.cur,
// <name>.cur, ignoring case. Empty if none.
std::filesystem::path findCursorFile(const std::filesystem::path& dir, const std::string& name);

// What the mouse is over, for the choice below.
struct CursorContext {
    enum class Mode { Other, Live, Buy, Build, Hood } mode = Mode::Other;
    bool loading = false;
    bool overUi = false; // a panel, the puck, the tutorial window, a pie menu
    // Buy mode's object tool.
    enum class BuyTool { Hand, Eyedropper, Design } buyTool = BuyTool::Hand;
    bool holding = false;    // an object follows the mouse
    bool pressing = false;   // the button is down on it (a drag turns it or moves it)
    bool turned = false;     // that drag has turned it
    bool overObject = false; // nothing held: an object the tool acts on is under the mouse

    // Camera (every mode, not over the UI). The window's edge scrolls it
    // (cTSWinSceneGraph::DoWindowEdgeScrolling): scrollX -1 left, 1 right,
    // scrollY 1 up (forward), -1 down; the arrow cursors by name (inferred).
    int scrollX = 0, scrollY = 0;
    bool panning = false; // the right button drags the camera (the hand drag's pan cursor 0x2A, inferred)

    // Live mode: what the mouse is over in the world (the arrows with a badge
    // form one set; which state picks which is by their names: inferred).
    enum class LiveHover { None, Person, Object, ObjectUnavailable } liveHover = LiveHover::None;

    // Build mode. The tool's own cursor (Design 0x39 and the Railing Tool
    // 0x3A are what the tools set; the terrain brushes by their names:
    // inferred), the object tool's hand while it holds a door or window
    // (as Buy mode's), the roof tool's drag (simsrotate 0x1B while
    // dragging, cTSUserToolRoof::OnMove). Ctrl, which deletes with most
    // tools, has no cursor: the demolish cursor is Bon Voyage's Sledgehammer
    // (0x3C, no such file in the base game) and simsdelete is a "no entry" sign.
    enum class BuildTool { Other, Design, Railing, Raise, Lower, Level, Object, Roof } buildTool = BuildTool::Other;
    bool buildDragging = false;
    // buildTool == Object: holding = `holding` above.

    // The neighborhood (docs/neighborhood-mac.md 15.1, cTSUserToolMoveLot):
    // the lot mover shows simsmove (0x19) over a lot it can move and
    // simsplace (0x1A) when the button goes down; with a lot in hand and the
    // button down, the drag turns it and the cursor follows its front
    // (0x1C/0x1E/0x1F/0x1D for 0/1/2/3); otherwise cursor 0 (arrow). The
    // decoration tool's mouse move sets 0x19 (cTSUserToolPlopNeighborhoodObject),
    // the decoration eyedropper 0x38; the other tools' cursors by name (inferred).
    enum class HoodTool { None, MoveIn, Evict, Bulldoze, MoveLot, ToBin, Decorate, Eyedropper } hoodTool = HoodTool::None;
    bool overLot = false;        // a lot is under the mouse (the mover: one it can move)
    bool lotInHand = false;      // a lot follows the mouse (moving, or a new one placed)
    bool lotPressed = false;     // the button is down with the lot in hand, or on a lot to pick up
    int lotFront = -1;           // the front the drag turned it to (-1: not yet)
};

GameCursor chooseCursor(const CursorContext& c);

} // namespace os2
