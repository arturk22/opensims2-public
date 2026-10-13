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
    check(chooseCursor(c) == GameCursor::Arrow, "live: arrow");
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
    c.mode = Ctx::Mode::Build;
    check(chooseCursor(c) == GameCursor::Arrow, "build: arrow");
    c.loading = true;
    check(chooseCursor(c) == GameCursor::Hourglass, "loading: hourglass");
}

static void testGameFiles(const char* data) {
    const std::filesystem::path dir = std::filesystem::path(data) / "TSData/Res/UI/Cursors";
    struct Want {
        GameCursor c;
        const char* file;
        int hx, hy;
    };
    const Want wants[] = {
        {GameCursor::Arrow, "arrow_8.cur", 0, 0},          {GameCursor::Hourglass, "HOURGLASS.cur", 16, 15},
        {GameCursor::Move, "simsmove_8.cur", 15, 16},      {GameCursor::Place, "simsplace_8.cur", 15, 16},
        {GameCursor::Rotate, "simsrotate_8.cur", 16, 14},  {GameCursor::Eyedropper, "eyedroppertool_8.cur", 0, 31},
        {GameCursor::Design, "designtool_8.cur", 0, 0},
    };
    RgbaImage sheet;
    const int cell = 32 * 3 + 8;
    sheet.width = uint32_t(cell * int(std::size(wants))), sheet.height = uint32_t(cell);
    sheet.pixels.assign(size_t(sheet.width) * sheet.height * 4, 0);
    for (size_t i = 0; i < sheet.pixels.size(); i += 4) { // a light checkerboard
        const size_t p = i / 4, x = p % sheet.width, y = p / sheet.width;
        const uint8_t g = ((x / 12 + y / 12) % 2) ? 200 : 235;
        sheet.pixels[i] = sheet.pixels[i + 1] = sheet.pixels[i + 2] = g, sheet.pixels[i + 3] = 255;
    }
    int n = 0;
    for (const Want& w : wants) {
        const auto f = findCursorFile(dir, gameCursorName(w.c));
        std::printf("cursor %-15s -> %s\n", gameCursorName(w.c), f.filename().string().c_str());
        check(!f.empty(), "cursor file found");
        if (f.empty())
            continue;
        std::string name = f.filename().string();
        for (auto& ch : name)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        std::string want = w.file;
        for (auto& ch : want)
            ch = char(std::tolower(static_cast<unsigned char>(ch)));
        check(name == want, "the expected file (8-bit first)");
        std::ifstream in(f, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto img = decodeCursor(bytes);
        check(img.has_value(), "game cursor decodes");
        if (!img)
            continue;
        check(img->width == 32 && img->height == 32, "game cursor 32x32");
        check(img->hotX == w.hx && img->hotY == w.hy, "game cursor hotspot");
        size_t opaque = 0;
        for (size_t i = 3; i < img->rgba.size(); i += 4)
            opaque += img->rgba[i] == 255;
        check(opaque > 40 && opaque < 32 * 32 * 3 / 4, "game cursor: a shape, not empty, not a filled square");
        // Into the sheet.
        const int ox = n * cell + 4, oy = 4;
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
        ++n;
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
