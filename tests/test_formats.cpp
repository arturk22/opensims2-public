#include "formats/bhav.hpp"
#include "formats/image.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <zlib.h>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool pixelIs(const RgbaImage& img, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = &img.pixels[(size_t(y) * img.width + x) * 4];
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static void testDxt1() {
    // c0 = red (0xF800), c1 = blue (0x001F); row 0 uses indices 0,1,2,3.
    const std::vector<uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};
    auto img = decodeTexture(TexFormat::Dxt1, 4, 4, block);
    check(pixelIs(img, 0, 0, 255, 0, 0, 255), "dxt1 c0");
    check(pixelIs(img, 1, 0, 0, 0, 255, 255), "dxt1 c1");
    check(pixelIs(img, 2, 0, 170, 0, 85, 255), "dxt1 2/3 c0 + 1/3 c1");
    check(pixelIs(img, 3, 0, 85, 0, 170, 255), "dxt1 1/3 c0 + 2/3 c1");
    check(pixelIs(img, 0, 1, 255, 0, 0, 255), "dxt1 row 1");

    // c0 <= c1 enables the transparent colour for index 3.
    const std::vector<uint8_t> punch = {0x1F, 0x00, 0x00, 0xF8, 0b11000000, 0, 0, 0};
    img = decodeTexture(TexFormat::Dxt1, 4, 4, punch);
    check(img.pixels[3 * 4 + 3] == 0, "dxt1 punch-through alpha");
}

static void testDxt3And5() {
    std::vector<uint8_t> dxt3(16, 0);
    dxt3[0] = 0xF0; // pixel 0 alpha 0, pixel 1 alpha 15
    dxt3[8] = 0xFF, dxt3[9] = 0xFF; // c0 = white
    auto img = decodeTexture(TexFormat::Dxt3, 4, 4, dxt3);
    check(pixelIs(img, 0, 0, 255, 255, 255, 0), "dxt3 alpha 0");
    check(pixelIs(img, 1, 0, 255, 255, 255, 255), "dxt3 alpha 15");

    std::vector<uint8_t> dxt5(16, 0);
    dxt5[0] = 255, dxt5[1] = 0;     // 8-step alpha ramp
    dxt5[2] = 0b010001;             // pixel 0 -> index 1 (a1 = 0), pixel 1 -> index 2
    dxt5[8] = 0xFF, dxt5[9] = 0xFF; // c0 = white
    img = decodeTexture(TexFormat::Dxt5, 4, 4, dxt5);
    check(img.pixels[3] == 0, "dxt5 index 1");
    check(img.pixels[4 + 3] == 218, "dxt5 index 2 = (6*255)/7");
    check(img.pixels[2 * 4 + 3] == 255, "dxt5 index 0");
}

static void testRaw() {
    const std::vector<uint8_t> bgra = {1, 2, 3, 4};
    check(pixelIs(decodeTexture(TexFormat::Raw32, 1, 1, bgra), 0, 0, 3, 2, 1, 4), "raw32 BGRA");
    const std::vector<uint8_t> gray = {7};
    check(pixelIs(decodeTexture(TexFormat::Raw8, 1, 1, gray), 0, 0, 7, 7, 7, 255), "raw8 gray");
    check(pixelIs(decodeTexture(TexFormat::Alpha8, 1, 1, gray), 0, 0, 255, 255, 255, 7), "alpha8");
    bool threw = false;
    try {
        decodeTexture(TexFormat::Dxt1, 8, 8, bgra);
    } catch (const FormatError&) {
        threw = true;
    }
    check(threw, "size mismatch throws");
}

static void testPng() {
    RgbaImage img;
    img.width = 3;
    img.height = 2;
    for (int i = 0; i < 24; ++i)
        img.pixels.push_back(uint8_t(i * 10));
    const auto path = std::filesystem::temp_directory_path() / "os2_test.png";
    writePng(path, img);
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)), {});
    std::filesystem::remove(path);
    check(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G', "png signature");

    // Inflate IDAT and compare with the filter-0 scanlines.
    const std::string bytes(png.begin(), png.end());
    const size_t idat = bytes.find("IDAT");
    check(idat != std::string::npos, "png has IDAT");
    if (idat == std::string::npos)
        return;
    const uint32_t len = uint32_t(png[idat - 4]) << 24 | png[idat - 3] << 16 | png[idat - 2] << 8 | png[idat - 1];
    std::vector<uint8_t> raw(2 * (1 + 3 * 4));
    uLongf rawLen = uLongf(raw.size());
    check(uncompress(raw.data(), &rawLen, &png[idat + 4], len) == Z_OK && rawLen == raw.size(), "png inflates");
    check(raw[0] == 0 && raw[1] == 0 && raw[13] == 0 && raw[14] == 120, "png scanlines");
}

static void testNameHash() {
    // Pairs taken from objects.package.
    check(nameInstanceHash("0xcd7eb376_age4_00_shpe") == 0xFF03600A, "instance hash (shpe)");
    check(nameInstanceHash("6c6b10bc-8e91-45d7-8911-b92defb69a3a_TXMT") == 0xFF07B665, "instance hash is case-insensitive");
}

static std::vector<uint8_t> strHeader(uint16_t format, uint16_t count) {
    std::vector<uint8_t> d(64, 0);
    const char name[] = "test";
    std::copy(name, name + 4, d.begin());
    d.push_back(uint8_t(format)), d.push_back(uint8_t(format >> 8));
    d.push_back(uint8_t(count)), d.push_back(uint8_t(count >> 8));
    return d;
}

static void put(std::vector<uint8_t>& d, const std::string& s) {
    d.insert(d.end(), s.begin(), s.end());
    d.push_back(0);
}

static void testStrings() {
    auto d = strHeader(0xFFFD, 5);
    for (auto [l, v] : std::initializer_list<std::pair<uint8_t, const char*>>{
             {1, "Chair"}, {1, "Table"}, {14, "Cadeira PT"}, {35, "Cadeira"}, {14, "Mesa"}}) {
        d.push_back(l);
        put(d, v);
        put(d, "");
    }
    d.insert(d.end(), 7, 0xA3); // fill padding seen in the game data
    const auto t = StringTable::parse(d);
    check(t.fileName == "test" && t.entries.size() == 5, "STR# entries");
    check(t.get(0) == "Cadeira", "STR# pt-BR");
    check(t.get(1) == "Mesa", "STR# pt-BR falls back to Portuguese");
    check(t.get(1, lang::EnglishUS) == "Table", "STR# English");
    check(t.count(14) == 2, "STR# count per language");

    std::vector<uint8_t> pascal(64, 0);
    pascal.insert(pascal.end(), {0x00, 0x02, 2, 'o', 'n', 3, 'o', 'f', 'f'});
    const auto p = StringTable::parse(pascal);
    check(p.entries.size() == 2 && p.get(1) == "off", "STR# legacy Pascal table");
}

static void testSimAntics() {
    // BCON: u16 count with a flag in bit 15.
    std::vector<uint8_t> bcon(64, 0);
    bcon.insert(bcon.end(), {0x02, 0x80, 0x05, 0x00, 0xFF, 0xFF});
    const auto c = Constants::parse(bcon);
    check(c.flags == 1 && c.values.size() == 2 && c.values[0] == 5 && c.values[1] == -1, "BCON");

    // OBJf: two u32, 'fJBO', count, (guard, action) pairs.
    std::vector<uint8_t> objf(64, 0);
    objf.insert(objf.end(), {0, 0, 0, 0, 0, 0, 0, 0, 'f', 'J', 'B', 'O', 2, 0, 0, 0, 0, 0, 0x00, 0x10, 0x01, 0x10, 0x02, 0x10});
    const auto f = ObjectFunctions::parse(objf);
    check(f.functions.size() == 2 && f.functions[0].action == 0x1000 && f.functions[1].guard == 0x1001, "OBJf");

    // GLOB: Pascal or C string, with optional fill padding.
    std::vector<uint8_t> glob(64, 0);
    glob.insert(glob.end(), {4, 'T', 'e', 's', 't', 0xA3, 0xA3});
    check(SemiGlobal::parse(glob).semiGlobalName == "Test", "GLOB");
}

static void testBhav() {
    std::vector<uint8_t> d(64, 0);
    d.insert(d.end(), {0x07, 0x80, 2, 0, 0, 1, 2, 0, 0, 0, 0, 0}); // 0x8007, 2 instructions, 1 arg, 2 locals
    auto instr = [&](uint16_t op, uint16_t t, uint16_t f) {
        d.insert(d.end(), {uint8_t(op), uint8_t(op >> 8), uint8_t(t), uint8_t(t >> 8), uint8_t(f), uint8_t(f >> 8), 0});
        d.insert(d.end(), 16, 0);
    };
    instr(0x0002, 1, 0xFFFE);      // Expression: true -> 1, false -> return false
    instr(0x2010, 0xFFFD, 0xFFFC); // semi-global call: true -> return true, false -> error
    const auto b = Behavior::parse(d);
    check(b.argCount == 1 && b.localCount == 2 && b.instructions.size() == 2, "BHAV header");
    check(b.instructions[0].onTrue.kind == Behavior::TargetKind::Instruction && b.instructions[0].onTrue.index == 1,
          "BHAV branch to instruction");
    check(b.instructions[0].onFalse.kind == Behavior::TargetKind::False, "BHAV return false");
    check(b.instructions[1].onFalse.kind == Behavior::TargetKind::Error, "BHAV error target");
    check(b.disassemble().find("call semiglobal 0x2010") != std::string::npos, "BHAV disassembly");

    d[64 + 12 + 2] = 5; // branch past the end
    bool threw = false;
    try {
        Behavior::parse(d);
    } catch (const FormatError&) {
        threw = true;
    }
    check(threw, "BHAV rejects out-of-range branches");
}

int main() {
    testDxt1();
    testDxt3And5();
    testRaw();
    testPng();
    testNameHash();
    testStrings();
    testSimAntics();
    testBhav();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all format tests passed\n");
    return failures ? 1 : 0;
}
