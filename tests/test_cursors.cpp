// The game's mouse cursors (app/game_cursors.hpp): the .cur decoder on a
// hand-built file, the state -> cursor choice, and, with the game's data
// (OS2_DATA), every cursor the app uses loads from TSData/Res/UI/Cursors with
// a sane size and hotspot. OS2_CURSOR_SHEET=<png> writes a contact sheet of
// them (3x, the hotspot as a red cross) in the order of GameCursor.
#include "app/game_cursors.hpp"
#include "formats/image.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)), put16(v, uint16_t(x >> 16)); }

// A 32x32 8-bit cursor, hotspot (5, 7): palette 0 black, 1 red, 2 blue;
// the top row red, the rest blue, the left half of every row masked.
static std::vector<uint8_t> makeCursor() {
    std::vector<uint8_t> v;
    put16(v, 0), put16(v, 2), put16(v, 1);
    v.push_back(32), v.push_back(32), v.push_back(0), v.push_back(0);
    put16(v, 5), put16(v, 7);
    put32(v, 40 + 1024 + 1024 + 128), put32(v, 22);
    put32(v, 40), put32(v, 32), put32(v, 64), put16(v, 1), put16(v, 8);
    for (int i = 0; i < 6; ++i)
        put32(v, 0);
    for (int i = 0; i < 256; ++i) {
        const uint32_t bgra = i == 1 ? 0x00FF0000u : i == 2 ? 0x000000FFu : 0;
        put32(v, bgra);
    }
    for (int row = 0; row < 32; ++row) // bottom-up: row 31 is the top
        for (int x = 0; x < 32; ++x)
            v.push_back(row == 31 ? 1 : 2);
    for (int row = 0; row < 32; ++row)
        v.push_back(0xFF), v.push_back(0xFF), v.push_back(0), v.push_back(0);
    return v;
}

static void testDecoder() {
    const auto file = makeCursor();
    const auto img = decodeCursor(file);
    check(img.has_value(), "hand-built cursor decodes");
    if (!img)
        return;
    check(img->width == 32 && img->height == 32, "size 32x32");
    check(img->hotX == 5 && img->hotY == 7, "hotspot (5, 7)");
    auto px = [&](int x, int y) { return &img->rgba[size_t(y * 32 + x) * 4]; };
    check(px(20, 0)[0] == 255 && px(20, 0)[2] == 0 && px(20, 0)[3] == 255, "top row red, opaque");
    check(px(20, 10)[2] == 255 && px(20, 10)[0] == 0 && px(20, 10)[3] == 255, "body blue, opaque");
    check(px(3, 10)[3] == 0, "masked pixel transparent");
    std::vector<uint8_t> bad(file.begin(), file.begin() + 30);
    check(!decodeCursor(bad).has_value(), "truncated file rejected");
}

static void testChoice() {
    using Ctx = CursorContext;
    Ctx c;
    check(chooseCursor(c) == GameCursor::Arrow, "default: arrow");
    c.mode = Ctx::Mode::Live;
    check(chooseCursor(c) == GameCursor::LiveNothing, "live, over nothing: livenothing");
    c.overUi = true;
    check(chooseCursor(c) == GameCursor::Arrow, "live, over the panel: arrow");
    c.overUi = false;
    c.liveHover = Ctx::LiveHover::Person;
    check(chooseCursor(c) == GameCursor::LivePerson, "live, over a Sim: liveperson");
    c.liveHover = Ctx::LiveHover::Object;
    check(chooseCursor(c) == GameCursor::LiveObjectAvail, "live, over an object: liveobjectavail");
    c.liveHover = Ctx::LiveHover::ObjectUnavailable;
    check(chooseCursor(c) == GameCursor::LiveObjectUnavail, "live, over an object with nothing to do: liveobjectunavail");
    c = {};
    c.mode = Ctx::Mode::Buy;
    check(chooseCursor(c) == GameCursor::Arrow, "buy, hand over nothing: arrow");
    c.overObject = true;
    check(chooseCursor(c) == GameCursor::Move, "buy, hand over an object: open hand");
    c.overObject = false, c.holding = true;
    check(chooseCursor(c) == GameCursor::Move, "buy, holding: open hand (tutorial step 20)");
    c.pressing = true;
    check(chooseCursor(c) == GameCursor::Place, "buy, button down on what is held: closed hand");
    c.turned = true;
    check(chooseCursor(c) == GameCursor::Rotate, "buy, the drag turned it: rotate");
    c.overUi = true;
    check(chooseCursor(c) == GameCursor::Arrow, "over the UI: arrow");
    c = {};
    c.mode = Ctx::Mode::Buy, c.buyTool = Ctx::BuyTool::Eyedropper;
    check(chooseCursor(c) == GameCursor::Eyedropper, "eyedropper tool");
    c.buyTool = Ctx::BuyTool::Design;
    check(chooseCursor(c) == GameCursor::Design, "design tool");
    c = {};
    c.loading = true;
    check(chooseCursor(c) == GameCursor::Hourglass, "loading: hourglass");

    // Build mode.
    c = {};
    c.mode = Ctx::Mode::Build;
    check(chooseCursor(c) == GameCursor::Arrow, "build: arrow (the wall, floor and roof tools draw their own marker)");
    c.buildTool = Ctx::BuildTool::Design;
    check(chooseCursor(c) == GameCursor::Design, "build, design tool");
    c.buildTool = Ctx::BuildTool::Railing;
    check(chooseCursor(c) == GameCursor::Railing, "build, railing tool (0x3A)");
    c.buildTool = Ctx::BuildTool::Raise;
    check(chooseCursor(c) == GameCursor::Raise, "build, terrain raise");
    c.buildTool = Ctx::BuildTool::Lower;
    check(chooseCursor(c) == GameCursor::Lower, "build, terrain lower");
    c.buildTool = Ctx::BuildTool::Level;
    check(chooseCursor(c) == GameCursor::Level, "build, terrain level");
    c.buildTool = Ctx::BuildTool::Roof;
    check(chooseCursor(c) == GameCursor::Arrow, "build, roof tool, no drag: arrow");
    c.buildDragging = true;
    check(chooseCursor(c) == GameCursor::Rotate, "build, roof tool dragging: simsrotate (0x1B)");
    c.buildTool = Ctx::BuildTool::Object;
    c.holding = true;
    check(chooseCursor(c) == GameCursor::Move, "build, door in hand: the open hand, as Buy's");
    c.overUi = true;
    check(chooseCursor(c) == GameCursor::Arrow, "build, over the panel: arrow");

    // The camera: the window's edge scrolls, the right button drags.
    c = {};
    c.mode = Ctx::Mode::Live;
    c.scrollX = -1;
    check(chooseCursor(c) == GameCursor::ScrollLeft, "edge scroll left");
    c.scrollX = 1, c.scrollY = 1;
    check(chooseCursor(c) == GameCursor::ScrollUpRight, "edge scroll up right");
    c.scrollX = 0, c.scrollY = -1;
    check(chooseCursor(c) == GameCursor::ScrollDown, "edge scroll down");
    c.scrollX = -1, c.scrollY = -1;
    check(chooseCursor(c) == GameCursor::ScrollDownLeft, "edge scroll down left");
    c.scrollX = 0, c.scrollY = 1;
    check(chooseCursor(c) == GameCursor::ScrollUp, "edge scroll up");
    c.scrollX = 1, c.scrollY = 0;
    check(chooseCursor(c) == GameCursor::ScrollRight, "edge scroll right");
    c.scrollX = -1, c.scrollY = 1;
    check(chooseCursor(c) == GameCursor::ScrollUpLeft, "edge scroll up left");
    c.scrollX = 1, c.scrollY = -1;
    check(chooseCursor(c) == GameCursor::ScrollDownRight, "edge scroll down right");
    c.overUi = true;
    check(chooseCursor(c) == GameCursor::Arrow, "edge scroll arrows not over the UI");
    c = {};
    c.panning = true;
    check(chooseCursor(c) == GameCursor::Pan, "dragging the camera: pan (0x2A)");

    // The neighborhood's lot mover (docs/neighborhood-mac.md 15.1).
    c = {};
    c.mode = Ctx::Mode::Hood;
    check(chooseCursor(c) == GameCursor::Arrow, "hood: arrow");
    c.hoodTool = Ctx::HoodTool::MoveLot;
    check(chooseCursor(c) == GameCursor::Arrow, "lot mover over nothing: cursor 0");
    c.overLot = true;
    check(chooseCursor(c) == GameCursor::Move, "lot mover over a lot: simsmove (0x19)");
    c.lotPressed = true;
    check(chooseCursor(c) == GameCursor::Place, "lot mover, button down on a lot: simsplace (0x1A)");
    c = {};
    c.mode = Ctx::Mode::Hood, c.hoodTool = Ctx::HoodTool::MoveLot, c.lotInHand = true;
    check(chooseCursor(c) == GameCursor::Arrow, "lot in hand, button up: cursor 0");
    c.lotPressed = true;
    check(chooseCursor(c) == GameCursor::Rotate, "lot in hand, button down, not turned yet: simsrotate");
    c.lotFront = 0;
    check(chooseCursor(c) == GameCursor::RotateNW && gameCursorId(GameCursor::RotateNW) == 0x1C, "front 0: 0x1C simsrotatenw");
    c.lotFront = 1;
    check(chooseCursor(c) == GameCursor::RotateSW && gameCursorId(GameCursor::RotateSW) == 0x1E, "front 1: 0x1E simsrotatesw");
    c.lotFront = 2;
    check(chooseCursor(c) == GameCursor::RotateSE && gameCursorId(GameCursor::RotateSE) == 0x1F, "front 2: 0x1F simsrotatese");
    c.lotFront = 3;
    check(chooseCursor(c) == GameCursor::RotateNE && gameCursorId(GameCursor::RotateNE) == 0x1D, "front 3: 0x1D simsrotatene");
    c = {};
    c.mode = Ctx::Mode::Hood, c.hoodTool = Ctx::HoodTool::Decorate;
    check(chooseCursor(c) == GameCursor::Move, "decoration tool: simsmove (0x19, cTSUserToolPlopNeighborhoodObject::OnMouseMove)");
    c.hoodTool = Ctx::HoodTool::Eyedropper;
    check(chooseCursor(c) == GameCursor::Eyedropper, "decoration eyedropper: 0x38");
    c.hoodTool = Ctx::HoodTool::Bulldoze;
    check(chooseCursor(c) == GameCursor::Arrow, "bulldoze over nothing: arrow");
    c.overLot = true;
    check(chooseCursor(c) == GameCursor::Bulldoze, "bulldoze over a lot: bulldoze");
    c.hoodTool = Ctx::HoodTool::MoveIn;
    check(chooseCursor(c) == GameCursor::MoveIn, "move in over a lot");
    c.hoodTool = Ctx::HoodTool::Evict;
    check(chooseCursor(c) == GameCursor::EvictFamily, "evict over a lot");
    c.hoodTool = Ctx::HoodTool::ToBin;
    check(chooseCursor(c) == GameCursor::MoveLotToCatalog, "send to the bin over a lot");

    // The ids are cTSUIServiceManager::SetupCursors's.
    check(gameCursorId(GameCursor::Arrow) == 1 && gameCursorId(GameCursor::Hourglass) == 0xF, "ids: arrow 1, hourglass 0xF");
    check(gameCursorId(GameCursor::Move) == 0x19 && gameCursorId(GameCursor::Place) == 0x1A, "ids: simsmove 0x19, simsplace 0x1A");
    check(gameCursorId(GameCursor::Design) == 0x39 && gameCursorId(GameCursor::Railing) == 0x3A, "ids: design 0x39, railing 0x3A");
    check(gameCursorId(GameCursor::Eyedropper) == 0x38 && gameCursorId(GameCursor::Pan) == 0x2A, "ids: eyedropper 0x38, pan 0x2A");
    check(gameCursorId(GameCursor::LiveNothing) == 0x2B && gameCursorId(GameCursor::LivePerson) == 0x2C &&
              gameCursorId(GameCursor::LiveObjectAvail) == 0x2D && gameCursorId(GameCursor::LiveObjectUnavail) == 0x2E,
          "ids: live cursors 0x2B-0x2E");
    check(gameCursorId(GameCursor::ScrollLeft) == 0x20 && gameCursorId(GameCursor::ScrollDownLeft) == 0x27, "ids: scroll arrows 0x20-0x27");
    check(gameCursorId(GameCursor::Delete) == 0x15 && gameCursorId(GameCursor::Level) == 0x16 && gameCursorId(GameCursor::Lower) == 0x17 &&
              gameCursorId(GameCursor::Raise) == 0x18,
          "ids: simsdelete 0x15, level 0x16, lower 0x17, raise 0x18");
}

static void testGameFiles(const char* data) {
    const std::filesystem::path dir = std::filesystem::path(data) / "TSData/Res/UI/Cursors";
    const int count = int(GameCursor::Count);
    RgbaImage sheet;
    const int cell = 32 * 3 + 8, perRow = 12, rows = (count + perRow - 1) / perRow;
    sheet.width = uint32_t(cell * perRow), sheet.height = uint32_t(cell * rows);
    sheet.pixels.assign(size_t(sheet.width) * sheet.height * 4, 0);
    for (size_t i = 0; i < sheet.pixels.size(); i += 4) { // a light checkerboard
        const size_t p = i / 4, x = p % sheet.width, y = p / sheet.width;
        const uint8_t g = ((x / 12 + y / 12) % 2) ? 200 : 235;
        sheet.pixels[i] = sheet.pixels[i + 1] = sheet.pixels[i + 2] = g, sheet.pixels[i + 3] = 255;
    }
    for (int n = 0; n < count; ++n) {
        const GameCursor cur = GameCursor(n);
        const auto f = findCursorFile(dir, gameCursorName(cur));
        std::printf("cursor 0x%02X %-18s -> %s", gameCursorId(cur), gameCursorName(cur), f.filename().string().c_str());
        check(!f.empty(), "cursor file found");
        if (f.empty()) {
            std::printf("\n");
            continue;
        }
        std::ifstream in(f, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto img = decodeCursor(bytes);
        check(img.has_value(), "game cursor decodes");
        if (!img) {
            std::printf("\n");
            continue;
        }
        std::printf("  %dx%d hotspot (%d, %d)\n", img->width, img->height, img->hotX, img->hotY);
        check(img->width == 32 && img->height == 32, "game cursor 32x32");
        check(img->hotX >= 0 && img->hotX < 32 && img->hotY >= 0 && img->hotY < 32, "game cursor hotspot inside");
        size_t opaque = 0;
        for (size_t i = 3; i < img->rgba.size(); i += 4)
            opaque += img->rgba[i] == 255;
        check(opaque > 20 && opaque < 32 * 32 * 3 / 4, "game cursor: a shape, not empty, not a filled square");
        // Into the sheet.
        const int ox = (n % perRow) * cell + 4, oy = (n / perRow) * cell + 4;
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x) {
                const uint8_t* s = &img->rgba[size_t((y / 3) * 32 + x / 3) * 4];
                if (s[3] == 0)
                    continue;
                uint8_t* d = &sheet.pixels[(size_t(oy + y) * sheet.width + size_t(ox + x)) * 4];
                d[0] = s[0], d[1] = s[1], d[2] = s[2];
            }
        for (int k = -5; k <= 5; ++k) // the hotspot
            for (auto [x, y] : {std::pair{img->hotX * 3 + 1 + k, img->hotY * 3 + 1}, std::pair{img->hotX * 3 + 1, img->hotY * 3 + 1 + k}})
                if (x >= 0 && y >= 0 && x < 96 && y < 96) {
                    uint8_t* d = &sheet.pixels[(size_t(oy + y) * sheet.width + size_t(ox + x)) * 4];
                    d[0] = 230, d[1] = 20, d[2] = 20;
                }
    }
    // The files the first state set picks (as round 21 found them).
    const struct {
        GameCursor c;
        const char* file;
    } expect[] = {{GameCursor::Arrow, "arrow_8.cur"}, {GameCursor::Hourglass, "HOURGLASS.cur"}, {GameCursor::Move, "simsmove_8.cur"},
                  {GameCursor::Place, "simsplace_8.cur"}, {GameCursor::Rotate, "simsrotate_8.cur"},
                  {GameCursor::Eyedropper, "eyedroppertool_8.cur"}, {GameCursor::Design, "designtool_8.cur"},
                  {GameCursor::Railing, "railingtool_8.cur"}, {GameCursor::LivePerson, "liveperson_8.cur"},
                  {GameCursor::MoveIn, "MOVEIN.cur"}, {GameCursor::RotateNW, "SimsRotateNW_8.cur"}};
    for (const auto& e : expect) {
        std::string name = findCursorFile(dir, gameCursorName(e.c)).filename().string(), want = e.file;
        for (auto& ch : name)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : want)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        check(name == want, "the expected file (8-bit first)");
    }
    if (const char* out = std::getenv("OS2_CURSOR_SHEET"))
        writePng(out, sheet), std::printf("contact sheet: %s\n", out);
}

int main() {
    testDecoder();
    testChoice();
    if (const char* data = std::getenv("OS2_DATA"))
        testGameFiles(data);
    else
        std::printf("game cursors skipped (no OS2_DATA)\n");
    if (failures)
        return 1;
    std::printf("cursors: ok\n");
    return 0;
}
