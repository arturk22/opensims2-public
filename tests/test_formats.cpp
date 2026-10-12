#include <cmath>
#include "formats/bhav.hpp"
#include "formats/camera_config.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/lot_model.hpp"
#include "app/wall_view.hpp"
#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/property_set.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "formats/strings.hpp"

#include <algorithm>
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

    // FCNS: 'SNCF', version 0x4B, 0, count, (Pascal name, float, Pascal description).
    std::vector<uint8_t> fcns(64, 0);
    fcns.insert(fcns.end(), {'S', 'N', 'C', 'F', 0x4B, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 4, 'h', 'o', 'u', 'r',
                             0x00, 0x00, 0x00, 0x41, 0});
    const auto fc = FloatConstants::parse(fcns);
    check(fc.entries.size() == 1 && fc.value("hour", 7.0f) == 8.0f && fc.value("other", 7.0f) == 7.0f, "FCNS");
}

static void testInteractionTable() {
    // Version 0x48, one interaction with 16 advertisements for the first age
    // group, then 96 zero bytes and the name, as in the game's "Reaction
    // Interaction Table" (group 0x7F9FC607).
    std::vector<uint8_t> d(64, 0);
    auto u16 = [&](uint32_t v) { d.insert(d.end(), {uint8_t(v), uint8_t(v >> 8)}); };
    auto u32 = [&](uint32_t v) { u16(v & 0xFFFF), u16(v >> 16); };
    u32(0xFFFFFFFF), u32(0x48), u32(0), u16(1);
    u16(0x2019), u16(0x0109);
    u32(16);
    for (int i = 0; i < 6; ++i)
        u32(0);
    d.insert(d.end(), 2 + 2 + 4 + 4 + 4 + 4 + 4 + 2 + 4, 0);
    d.insert(d.end(), 16 * 6 + 96, 0);
    const std::string name = "Reaction Interaction Table";
    d.push_back(uint8_t(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    const auto t = InteractionTable::parse(d);
    check(t.interactions.size() == 1 && t.interactions[0].action == 0x2019 && t.name == name,
          "TTAB with zero padding before the name");
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

static void testCameraConfig() {
    const char* text = R"(# comment
version 1
sinclude "missing.txt"
camera "Live" Live
zoom        1 (70   40   25)
fov           (45   45   45)
pitch         (30   22   14)   (40  45  50)
orientation 1 (10 55 100)   # trailing comment
smoothing   5 8
setparam KeyboardTranslationSpeed 50
if (maxVersion() >= 3)
   setparam keyboardtranslationspeed 99
endif
camera     "CAS" CreateASim
clip       0.2 2000
)";
    auto cams = parseCameraConfig(text);
    check(cams.size() == 2, "camera count");
    const CameraDef* live = findCamera(cams, "live");
    check(live && live->controller == "Live", "find camera by name");
    if (live) {
        check(live->defaultZoom == 1 && live->distances.size() == 3 && live->distances[1] == 40, "zoom table");
        check(live->pitchMin[2] == 14 && live->pitchMax[2] == 50, "pitch tables");
        check(live->defaultOrientation == 1 && live->yaws.size() == 3 && live->yaws[2] == 100, "orientation");
        check(live->zoomSmoothing == 5 && live->spinSmoothing == 8, "smoothing");
        check(live->param("keyboardtranslationspeed", 0) == 50, "setparam (if block skipped)");
    }
    const CameraDef* cas = findCamera(cams, "CAS");
    check(cas && cas->nearClip.size() == 1 && cas->nearClip[0] == 0.2f && cas->farClip[0] == 2000, "scalar clip");
}

static void testLightingConfig() {
    const char* text = R"(# Lighting Manager settings
set outsideFloorAmbient                0.25
setb morningEvening false
lightingState day
   control -edith true
   skylight                      (1,1,1)  0.5
   light sun      (1.5, 1.2, 2)    (1, .94, .875)     2
   light fill1    (1, 1, -1)     (1,0.8,.9)    .5
   light simFill1  (1, -1.2, 1) (1, .95, .89)  .5
   modifiers insidePortals       0.5  .7          1.3    1  #### trailing comment
end
lightingState dayNoPortals : day
   light sun      (0, 0, 1)    (1, 1, 1)     1
end
)";
    auto cfg = parseLightingConfig(text);
    check(cfg.globals["outsidefloorambient"] == "0.25" && cfg.globals["morningevening"] == "false", "lighting globals");
    check(cfg.states.size() == 2, "lighting state count");
    const LightingState* day = cfg.state("DAY");
    check(day && day->skyStrength == 0.5f && day->skyColor[2] == 1, "skylight");
    if (day) {
        check(day->lights.size() == 3 && day->lights[0].name == "sun" && day->lights[0].direction[1] == 1.2f &&
                  day->lights[0].color[2] == 0.875f && day->lights[0].intensity == 2,
              "light with comma lists");
        auto objects = day->objectLights();
        check(objects.size() == 2 && objects[1].name == "fill1" && objects[1].direction[2] == -1,
              "object lights skip simFill");
    }
    if (day) {
        auto it = day->modifiers.find("insideportals");
        check(it != day->modifiers.end() && it->second.practical == 0.5f && it->second.practicalFalloff == 1.3f,
              "lighting modifiers");
    }
    const auto overrides = parseLightOverrides(R"(define Four(lightName)
	lightAttribOverride &lightName intensity 1
	lightAttribOverride &lightName falloffRate 1 # comment
enddef
define Amber(lightName)
        lightAttribOverride &lightName color (1,0.9,0.76)
enddef
create Four(lightingWallValue_light)
create Amber(lightingWallValue_light)
lightAttribOverride gardenUplight coneAngle 35
)");
    {
        auto it = overrides.find("lightingwallvalue_light");
        check(it != overrides.end() && it->second.count("intensity") && it->second.at("intensity")[0] == 1 &&
                  it->second.count("color") && it->second.at("color").size() == 3 && it->second.at("color")[1] == 0.9f,
              "light overrides from macros");
        check(overrides.count("gardenuplight") && overrides.at("gardenuplight").at("coneangle")[0] == 35,
              "plain light override");
    }
    const LightingState* np = cfg.state("dayNoPortals");
    check(np && np->parent == "day" && np->skyStrength == 0.5f && np->lights.size() == 3 &&
              np->lights[0].intensity == 1 && np->lights[0].direction[2] == 1,
          "inherited state overrides a light by name");
}

static void testStandardMaterial() {
    MaterialDefinition m;
    m.materialType = "StandardMaterial";
    m.properties = {{"stdMatAlphaBlendMode", "blend"},       {"stdMatAlphaMultiplier", "0.300000"},
                    {"stdMatBaseTextureEnabled", "true"},    {"stdMatBaseTextureName", "chair-shadow-alpha"},
                    {"stdMatCullMode", "cullClockwise"},     {"stdMatDiffCoef", "0,0.5,1"},
                    {"stdMatLightingEnabled", "0"},          {"stdMatAlphaTestEnabled", "1"},
                    {"stdMatAlphaRefValue", "51"},          {"stdMatSpecCoef", "0.5,0.5,0.25"},
                    {"stdMatSpecPower", "1e+002"},          {"stdMatEmissiveCoef", "0,0.1,0"},
                    {"stdMatUntexturedDiffAlpha", "0.78"}};
    auto sm = standardMaterial(m);
    check(sm.specular[2] == 0.25f && sm.specularPower == 100 && sm.emissive[1] == 0.1f && sm.untexturedAlpha == 0.78f,
          "material specular, emissive, untextured alpha");
    check(sm.baseTexture == "chair-shadow-alpha_txtr", "material base texture");
    check(sm.blend == StandardMaterial::Blend::Alpha && sm.alphaMultiplier == 0.3f, "material blend");
    check(sm.diffuse[0] == 0 && sm.diffuse[1] == 0.5f && sm.diffuse[2] == 1, "material diffuse");
    check(!sm.lighting && sm.alphaTest && sm.alphaRef == 51.0f / 255.0f, "material flags");
    check(sm.cull == StandardMaterial::Cull::Clockwise && sm.visible, "material cull");

    MaterialDefinition none;
    none.materialType = "Null";
    auto ns = standardMaterial(none);
    check(!ns.visible && ns.baseTexture.empty() && ns.blend == StandardMaterial::Blend::None && ns.lighting,
          "null material defaults");
    none.properties = {{"stdMatAlphaBlendMode", "additive"}, {"stdMatCullMode", "none"},
                       {"stdMatBaseTextureEnabled", "false"}, {"stdMatBaseTextureName", "x"}};
    ns = standardMaterial(none);
    check(ns.blend == StandardMaterial::Blend::Additive && ns.cull == StandardMaterial::Cull::None &&
              ns.baseTexture.empty(),
          "additive, no cull, texture disabled");

    MaterialDefinition add;
    add.materialType = "StandardMaterial";
    add.properties = {{"stdMatAlphaBlendMode", "additive"}};
    check(!standardMaterial(add).lighting, "additive StandardMaterial is unlit (DetermineLightingMode)");
    add.properties = {{"stdMatAlphaBlendMode", "addNoSrcAlphaScale"}};
    const auto one = standardMaterial(add);
    check(one.blend == StandardMaterial::Blend::AddNoSrcAlpha && one.lighting, "addNoSrcAlphaScale adds one + one, lit");
    MaterialDefinition water;
    water.materialType = "WaterAnimatingTextures";
    water.properties = {{"stdMatBaseTextureName", "bathtubwater-water"}, {"stdMatBaseTextureEnabled", "true"},
                        {"stdMatSpecPower", "20"}, {"stdMatSpecCoef", "0.5,0.5,0.5"}, {"stdMatAlphaBlendMode", "none"}};
    const auto wm = standardMaterial(water);
    check(wm.shading == StandardMaterial::Shading::Water && wm.blend == StandardMaterial::Blend::Alpha &&
              wm.specularPower == 0 && wm.waterSpeed == 0.3f && !wm.depthWrite,
          "WaterAnimatingTextures as LowEndWater");
    MaterialDefinition wallpaper;
    wallpaper.materialType = "Wallpaper";
    wallpaper.properties = {{"stdMatDiffCoef", "0.5,0.5,0.5,1"}};
    const auto wp = standardMaterial(wallpaper);
    check(wp.lightMapped && wp.diffuse[0] == 1, "walls are light-mapped and ignore the diffuse");

    // standard.matShad defaults: no specular without a power; blended parts
    // go 7 layers up and stop writing depth.
    MaterialDefinition plain;
    plain.materialType = "StandardMaterial";
    plain.properties = {{"stdMatSpecCoef", "0.5,0.5,0.5"}, {"stdMatAlphaBlendMode", "blend"}, {"stdMatLayer", "1"}};
    auto ps = standardMaterial(plain);
    check(ps.specularPower == 0 && ps.layer == 15 && !ps.depthWrite && ps.envCube.empty() && ps.normalMap.empty(),
          "standard defaults and layer");
    plain.properties = {{"stdMatEnvCubeMode", "reflection"}, {"stdMatEnvCubeCoef", "0.1,0.1,0.1"},
                        {"stdMatEnvCubeTextureName", "reflectionkitchenhighcontrast-envcube"},
                        {"stdMatNormalMapTextureEnabled", "true"}, {"stdMatNormalMapTextureName", "wall-bump"},
                        {"stdMatTextureCoordAnimMode", "transform"},
                        {"stdMatTextureCoordTfAnimRotSpeed", "16.000000"},
                        {"stdMatTextureCoordTfAnimRotStartEnd", "0.000000,0.040000"},
                        {"stdMatTextureCoordTfAnimRotWaveform", "sawtooth"}};
    ps = standardMaterial(plain);
    check(ps.envCube == "reflectionkitchenhighcontrast-envcube_txtr" && ps.envCubeCoef[1] == 0.1f &&
              ps.normalMap == "wall-bump_txtr" && ps.layer == 0 && ps.depthWrite,
          "env cube and normal map");
    check(ps.texAnim == StandardMaterial::TexAnim::Transform && ps.rotation.speed == 16 &&
              ps.rotation.end[0] == 0.04f && ps.rotation.waveform == StandardMaterial::Waveform::Sawtooth &&
              ps.scale.speed == 0,
          "texture coordinate animation");
    plain.properties[1].second = "0.001,0.001,0"; // below CheckForCubeMapping's threshold
    check(standardMaterial(plain).envCube.empty(), "weak env cube is off");

    // Deprecated types (deprecated.matShad).
    MaterialDefinition phong;
    phong.materialType = "PhongTextureAlphaCubeMapReflection";
    phong.properties = {{"stdMatBaseTextureName", "glass"}, {"stdMatDiffCoef", "0.5,0.5,0.5,0.25"},
                        {"stdMatSpecCoef", "0.2,0.4,0.6"}, {"reflectivity", "0.5"},
                        {"deprecatedStdMatInvDiffuseCoeffMultiplier", "1.2"}};
    auto pm = standardMaterial(phong);
    check(pm.baseTexture == "glass_txtr" && pm.blend == StandardMaterial::Blend::Alpha && pm.alphaMultiplier == 0.25f &&
              std::abs(pm.diffuse[0] - 0.6f) < 1e-6f && std::abs(pm.envCubeCoef[2] - 0.3f) < 1e-6f &&
              pm.envCube == "reflectionCubeTemplate-envcube_txtr" && pm.layer == 7,
          "deprecated phong type");
    phong.materialType = "AdditiveTextureAlphaBlend";
    phong.properties.push_back({"blendStrength", "0.5"});
    pm = standardMaterial(phong);
    check(pm.blend == StandardMaterial::Blend::Additive && !pm.lighting && pm.alphaReplicate &&
              pm.alphaMultiplier == 0.5f && pm.envCube.empty(),
          "deprecated additive type");

    // Sim hair: blended, but alpha tested at 60 and writing depth, 3 layers up.
    MaterialDefinition hair;
    hair.materialType = "SimStandardMaterial";
    hair.properties = {{"stdMatBaseTextureEnabled", "true"}, {"stdMatBaseTextureName", "ufhairlong-red"},
                       {"stdMatAlphaBlendMode", "blend"}, {"stdMatSpecPower", "2"}};
    auto hm = standardMaterial(hair);
    check(hm.shading == StandardMaterial::Shading::Sim && hm.layer == 3 && hm.depthWrite && hm.alphaTest &&
              hm.alphaRef == 61.0f / 255.0f && hm.specularPower == 0,
          "sim hair");
}

static void testPropertySet() {
    const auto p = parsePropertySet(R"(<?xml version="1.0"?>
<cGZPropertySetString>
   <AnyUint32 key="guid" type="0xeb61e4f7">3435290776</AnyUint32>
   <AnyString key="name" type="0x0b8bea18">&quot;Soft Rock&quot; Faux &amp; Stone</AnyString>
   <AnyString key="material" type="0x0b8bea18">floor_loftcoralsandstone</AnyString>
</cGZPropertySetString>)");
    check(p.size() == 3 && p.at("guid") == "3435290776", "property set guid");
    check(p.at("name") == "\"Soft Rock\" Faux & Stone", "property set entities");
    check(p.at("material") == "floor_loftcoralsandstone", "property set material");
}

static void testLotModel() {
    // 2x1 lot, one level of elevations at 0 and a second at 3; one floor tile
    // with only its north and east quarters, and one wall along x = 1.
    Lot lot;
    lot.width = 2, lot.height = 1, lot.levelCount = 2;
    lot.elevations.assign(3 * 2 * 2, 0.0f);
    for (size_t i = 6; i < 12; ++i)
        lot.elevations[i] = 3.0f;
    lot.floors.assign(2 * 1 * 2, {});
    lot.floors[0] = {0, 1, 1, 0};
    lot.floorPatterns[1] = "tile";
    lot.wallPatterns[1] = "paper";
    Lot::Wall w;
    w.a = {1, 0, 0}, w.b = {1, 1, 0};
    w.type = Lot::Normal;
    w.patterns = {1, 0xFFFF};
    lot.walls.push_back(w);
    check(lot.elevation(1, 2, 1) == 3.0f && lot.elevation(0, 5, 5) == 0.0f, "lot elevation lookup and clamp");

    const auto empty = std::filesystem::temp_directory_path() / "os2_test_empty_res";
    std::filesystem::create_directories(empty);
    ResourceIndex index(empty);
    const PatternCatalog patterns(index, empty);
    const ObjectModel m = buildLotModel(lot, index, patterns);
    const ObjectModel::Part* floor = nullptr;
    const ObjectModel::Part* wall = nullptr;
    for (const auto& part : m.parts) {
        if (part.node == "floor:tile")
            floor = &part;
        if (part.node == "wall:paper")
            wall = &part;
    }
    check(floor && floor->mesh.triangles.size() == 6, "two floor quarters");
    if (floor) {
        bool upAndCcw = true, northEast = true;
        for (size_t t = 0; t < floor->mesh.triangles.size(); t += 3) {
            const auto& a = floor->mesh.positions[floor->mesh.triangles[t]];
            const auto& b = floor->mesh.positions[floor->mesh.triangles[t + 1]];
            const auto& c = floor->mesh.positions[floor->mesh.triangles[t + 2]];
            const float cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            upAndCcw = upAndCcw && cross > 0 && floor->mesh.normals[floor->mesh.triangles[t]][2] > 0.99f;
            northEast = northEast && a[0] + b[0] + c[0] >= 1.5f - 1e-4f && a[1] + b[1] + c[1] >= 1.5f - 1e-4f;
        }
        check(upAndCcw, "floor triangles face up, counter-clockwise");
        check(northEast, "floor quarters are the north and east ones");
    }
    // Pattern 0 is the right side of a -> b: +X for a wall running +Y.
    check(wall && wall->mesh.triangles.size() == 6 && wall->mesh.normals[0][0] == 1.0f, "wall right side faces +X");
    if (wall) {
        float top = 0;
        for (const auto& p : wall->mesh.positions)
            top = std::max(top, p[2]);
        check(top == 3.0f, "wall reaches the next level");
    }
    check(wall && wall->mesh.uvs2.size() == wall->mesh.positions.size(), "walls get light-map coordinates");
    check(floor && floor->mesh.uvs2.size() == floor->mesh.positions.size(), "floors get light-map coordinates");

    // Wall views: walls down cut the wall to its stub, a level below the
    // wall's hides it, and the light-map atlas is the same for every view.
    auto wallTop = [&](const ObjectModel& model) {
        float top = -1;
        for (const auto& part : model.parts)
            if (part.node == "wall:paper")
                for (const auto& p : part.mesh.positions)
                    top = std::max(top, p[2]);
        return top;
    };
    LotLightMapLayout upLayout, downLayout, hiddenLayout;
    buildLotModel(lot, index, patterns, {}, &upLayout);
    LotViewOptions down;
    down.walls = LotViewOptions::Walls::Down;
    LotModelCache cache;
    const ObjectModel dm = buildLotModel(lot, index, patterns, down, &downLayout, &cache);
    check(wallTop(dm) > 0 && wallTop(dm) < 0.5f, "walls down: a low stub");
    check(cache.holes.has_value(), "the cache keeps the wall holes");
    down.level = 0;
    check(wallTop(buildLotModel(lot, index, patterns, down, nullptr, &cache)) < 0.5f, "walls down on the wall's level");
    down.level = 1; // looking at the level above: this level's walls stay up
    check(wallTop(buildLotModel(lot, index, patterns, down, nullptr, &cache)) == 3.0f, "lower levels keep their walls up");
    down.level = -1;
    check(wallTop(buildLotModel(lot, index, patterns, down, &hiddenLayout, &cache)) < 0, "levels above the view are hidden");
    check(upLayout.walls.size() == downLayout.walls.size() && downLayout.walls.size() == hiddenLayout.walls.size() &&
              !upLayout.walls.empty() && upLayout.walls[0].h == downLayout.walls[0].h &&
              upLayout.walls[0].top == hiddenLayout.walls[0].top && upLayout.height == downLayout.height,
          "the light-map atlas does not depend on the view");
    check(lotTopLevel(lot) == 0, "top level: the highest with walls or floors");
    {
        Lot gabled = lot;
        Lot::Wall g = gabled.walls[0];
        g.a.level = g.b.level = 1;
        g.type = Lot::Attic;
        gabled.walls.push_back(g);
        check(lotTopLevel(gabled) == 0, "top level: not the gables (attic walls) over the roof's floor");
    }

    // WallView: cutaway rebuilds only when the camera passes a wall direction.
    WallView wv;
    check(wv.walls() == LotViewOptions::Walls::Cutaway, "lots start in cutaway");
    const LotViewOptions a = wv.options({1, 0.2f}, 0), b = wv.options({1, 0.4f}, 0), c = wv.options({1, -0.2f}, 0);
    check(WallView::sameCut(a, b) && !WallView::sameCut(a, c), "cutaway follows the camera's side of the walls");
    check(!WallView::sameCut(a, wv.options({1, 0.2f}, 1)), "another level rebuilds");
    check(wv.mode() == 1 && !wv.roof() && !wv.options({1, 0}, 0).roof, "cutaway shows no roof");
    wv.cycle(1);
    check(wv.mode() == 2 && wv.walls() == LotViewOptions::Walls::Up && !wv.roof(), "Home: cutaway to up");
    wv.cycle(1);
    check(wv.mode() == 3 && wv.walls() == LotViewOptions::Walls::Up && wv.roof() && wv.options({1, 0}, 0).roof,
          "Home: up to up with the roof (wall mode 3)");
    check(!WallView::sameCut(wv.options({1, 0}, 0), [&] {
              WallView up;
              up.setMode(2);
              return up.options({1, 0}, 0);
          }()),
          "the roof mode rebuilds");
    wv.cycle(1);
    check(wv.walls() == LotViewOptions::Walls::Down, "Home wraps to walls down");
    wv.cycle(-1);
    check(wv.mode() == 3 && WallView::sameCut(wv.options({1, 0}, 0), wv.options({0, 1}, 0)),
          "End: down to the roof; walls up ignore the camera");

    // Cutaway (cWallCutawayManager, docs/ui-live-mac.md 8c): inside walls
    // always cut, outside walls only with their outside face to the camera,
    // walls alone in the yard and fences never.
    {
        Lot c;
        c.width = 4, c.height = 1, c.minLevel = 0, c.levelCount = 1;
        c.elevations.assign(5 * 2, 0.0f);
        c.floors.assign(4, {});
        c.rooms.assign(4, {});
        c.rooms[0] = {1, 1, 1, 1}; // tile (0, 0): room 1
        c.rooms[1] = {2, 2, 2, 2}; // tile (1, 0): room 2; tiles 2 and 3 outside
        c.wallPatterns[1] = "paper";
        auto add = [&](float x, uint32_t type) {
            Lot::Wall w;
            w.a = {x, 0, 0}, w.b = {x, 1, 0};
            w.type = type;
            w.patterns = {1, 1};
            c.walls.push_back(w);
        };
        add(1, Lot::Normal);    // between rooms 1 and 2: inside
        add(2, Lot::Normal);    // room 2 | yard: outside, its outside face +X
        add(3, Lot::Normal);    // yard | yard: alone
        add(0, Lot::Foundation); // a style without cutaway
        auto cutAt = [&](std::array<float, 2> look) {
            LotViewOptions v;
            v.walls = LotViewOptions::Walls::Cutaway;
            v.level = 0;
            v.viewDirection = look;
            LotModelCache k;
            buildLotModel(c, index, patterns, v, nullptr, &k);
            std::vector<float> xs;
            for (const auto& w : k.cut)
                xs.push_back(w[0]);
            std::sort(xs.begin(), xs.end());
            return xs;
        };
        check(cutAt({-1, 0.2f}) == std::vector<float>{1, 2}, "cutaway from +X: the inside wall and the outside wall facing the camera");
        check(cutAt({1, 0.2f}) == std::vector<float>{1}, "cutaway from -X: the inside wall only (the outside wall's face looks away)");
        check(cutAt({0.0f, 1}) == std::vector<float>{1}, "an inside wall is cut even edge-on");
    }
    check(WallView::visible(lot, 0, 0.5f, 0.5f, 1.0f) && !WallView::visible(lot, 0, 0.5f, 0.5f, 3.0f) &&
              WallView::visible(lot, 1, 0.5f, 0.5f, 3.0f),
          "objects above the shown level are hidden");
}

static void testLotLightMap() {
    // 4x4 lot, one level: a 2x2 room with floor (room 2) in the corner,
    // the rest terrain (room 1, outdoors since it has no floor).
    Lot lot;
    lot.width = 4, lot.height = 4, lot.levelCount = 1;
    lot.elevations.assign(5 * 5, 0.0f);
    lot.floors.assign(16, {});
    lot.rooms.assign(16, {1, 1, 1, 1});
    for (uint32_t x = 0; x < 2; ++x)
        for (uint32_t y = 0; y < 2; ++y) {
            lot.floors[x * 4 + y] = {1, 1, 1, 1};
            lot.rooms[x * 4 + y] = {2, 2, 2, 2};
        }
    LotLightMapLayout layout;
    layout.init(lot);
    check(layout.outdoor(0, 1) && !layout.outdoor(0, 2) && layout.outdoor(0, 0), "rooms without floor are outdoors");
    check(layout.roomAt(0, 0.5f, 0.5f) == 2 && layout.roomAt(0, 3.5f, 3.5f) == 1, "light-map room lookup");
    const auto t = layout.floorTexel(0, 1.5f, 2.0f);
    check(t[0] == 12 && t[1] == 16, "floor page texels: 8 per tile");
    LotLightMapLayout::WallFace face;
    face.origin = {2, 0}, face.along = {0, 1}, face.normal = {-1, 0}, face.length = 2, face.base = 0, face.top = 3;
    face.room = 2;
    const size_t cell = layout.addWall(face);
    check(layout.walls[cell].w == 18 && layout.walls[cell].h == 26 && layout.walls[cell].y == 32, "wall cell below the floor page");
    const auto wt = layout.wallTexel(cell, 1, 3);
    check(wt[0] == 9 && wt[1] == 33, "wall texel: one-texel border, row 0 at the top");

    const auto cfg = parseLightingConfig(R"(
lightingState day
   skylight (1,1,1) 0.5
   light sun (0, 0, 1) (1, 1, 1) 1
   diffParam insideAmbientColor (1, 1, 1)
   diffParam insideFloorAmbient 0.1
   diffParam floorThreshold 0.5
   modifiers insideObjects 1 1 1 1
   modifiers outsideObjects 1 1 1 1
end
)");
    const LightingState& day = *cfg.state("day");
    auto texel = [](const RgbaImage& im, uint32_t x, uint32_t y) { return im.pixels[(size_t(y) * im.width + x) * 4] / 255.0f; };
    // Outdoors: half of skylight 0.5 + (sun - skylight) x N.L = 0.5.
    LotLightMap dark = computeLotLightMap(layout, day, {});
    check(std::fabs(texel(dark.image, 28, 28) - 0.5f) < 0.01f, "outdoor floor texel is half the outdoor light");
    check(std::fabs(texel(dark.image, 4, 4) - 0.1f) < 0.01f, "indoor floor texel is the inside ambient");
    // A lamp in the room: seeded at colour x floor scale, falling by 1 + k per half tile.
    LightMapLight lamp;
    lamp.position = {0.25f, 0.25f, 2};
    lamp.color = {1, 1, 1};
    lamp.floorScale = 0.8f;
    LotLightMap lit = computeLotLightMap(layout, day, {lamp});
    check(std::fabs(texel(lit.image, 1, 1) - 0.8f) < 0.02f, "lamp seeds its sample");
    const float far = texel(lit.image, 14, 14), farther = texel(lit.image, 30, 30);
    check(far < 0.8f && far > 0.1f, "lamp light spreads through the room");
    check(std::fabs(farther - 0.5f) < 0.01f, "lamp light stays in its room");
    check(lit.rooms.count({0, 2}) && lit.rooms.at({0, 2})[4][0] > dark.rooms.at({0, 2})[4][0], "room irradiance from the light map");
    // An outdoor lamp: a pool over the direct light (the outside ambient
    // 0.25 only shapes the falloff and is taken away again).
    LightMapLight garden;
    garden.position = {3.25f, 3.25f, 1};
    garden.color = {1, 1, 1};
    garden.floorScale = 1;
    LotLightMap pool = computeLotLightMap(layout, day, {garden});
    check(std::fabs(texel(pool.image, 26, 26) - 1.0f) < 0.02f, "outdoor lamp adds to the direct light");
    check(std::fabs(texel(pool.image, 26, 2) - 0.5f) < 0.03f, "outdoor lamp pool fades to the direct light");
    // The floor's colour tints the light coming up (roomSaturation 0.5).
    LotLightMapLayout red = layout;
    red.materialColors.push_back({1, 0, 0});
    for (auto& f : red.floorMaterials)
        f = {1, 1, 1, 1};
    LotLightMap tinted = computeLotLightMap(red, day, {lamp});
    check(tinted.rooms.at({0, 2})[4][1] < lit.rooms.at({0, 2})[4][1] * 0.6f &&
              std::fabs(tinted.rooms.at({0, 2})[4][0] - lit.rooms.at({0, 2})[4][0]) < 1e-4f,
          "floor colour in the room irradiance");
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
    testInteractionTable();
    testCameraConfig();
    testLightingConfig();
    testStandardMaterial();
    testPropertySet();
    testLotModel();
    testLotLightMap();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all format tests passed\n");
    return failures ? 1 : 0;
}
