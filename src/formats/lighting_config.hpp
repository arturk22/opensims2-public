#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// The game's lighting manager settings (TSData/Res/Lights/Lighting.txt):
// global `set*` values plus named lighting states ("day", "night",
// "neighbourhood", ...) that hold the outdoor direct lights.
//
//   lightingState dayNoPortals : day     # inherits everything from "day"
//      skylight (1,1,1) 0.5              # colour, strength
//      light sun (1.5, 1.2, 2) (1, .94, .875) 2   # name, direction, colour, intensity
//   end
//
// Light directions are read as pointing from the scene towards the light, in
// game space (Z up): the "sun" is the light with a positive Z and "fill1"
// (1, 1, -1) lights from below. Inferred from the values, not confirmed.
struct LightingState {
    struct Light {
        std::string name; // "sun", "sky", "fill1", "simFill1", ...
        std::array<float, 3> direction{0, 0, 1};
        std::array<float, 3> color{1, 1, 1};
        float intensity = 0;
    };

    std::string name;
    std::string parent; // empty if none
    std::array<float, 3> skyColor{0, 0, 0};
    float skyStrength = 0;
    std::vector<Light> lights; // in file order; redefining a name replaces it
    // `environment (colour) [strength]`: the "environmentCube" texture light
    // that scales env cube reflections (and particles): colour x strength.
    std::array<float, 3> environment{1, 1, 1};
    // `modifiers <type> <practical> <diffusion> [<practical falloff> <diffusion falloff>]`:
    // how strongly lights of a kind ("insideObjects", "outsideObjects",
    // "insidePortals", ...) light objects (practical) and the light maps
    // (diffusion), by lowercased type.
    struct Modifier {
        float practical = 1, diffusion = 1, practicalFalloff = 1, diffusionFalloff = 1;
    };
    std::map<std::string, Modifier> modifiers;
    // `unlitRoomColor (colour) [-threshold t]`: extra light for rooms
    // darker than the threshold, in file order. The game's state keeps one
    // colour and threshold (+0x144, +0x150): the last command wins.
    struct UnlitRoom {
        std::array<float, 3> color{0, 0, 0};
        float threshold = 0;
    };
    std::vector<UnlitRoom> unlitRooms;
    // `diffParam <name> <value>`: the light maps' diffusion
    // (insideAmbientColor, insideFloorAmbient, floorThreshold, ...), by
    // lowercased name: one number or a colour.
    std::map<std::string, std::vector<float>> diffParams;
    float diffParam(const std::string& name, float fallback) const {
        auto it = diffParams.find(name);
        return it == diffParams.end() || it->second.empty() ? fallback : it->second[0];
    }
    // `transitionTimes <max in> <max out>`, seconds.
    float transitionIn = 0, transitionOut = 0;
    // `projectiveShadowStrength f` (cTSDiffusionParams; -1 if not given):
    // how dark the Sims' shadows are outside (inferred, docs/lighting-mac.md 9).
    float projectiveShadowStrength = -1;

    // Lights for objects: everything except the Sim-only fills ("simFill*",
    // by name; inferred).
    std::vector<Light> objectLights() const;
};

struct LightingConfig {
    std::map<std::string, std::string> globals; // `set`, `setf`, `seti`, `setb`, lowercased names
    std::vector<LightingState> states;

    const LightingState* state(std::string_view name) const; // case-insensitive, or nullptr
};

LightingConfig parseLightingConfig(std::string_view text);
LightingConfig loadLightingConfig(const std::filesystem::path& path);

// A room light set's tuning (cTSRoomLightTuning, the "object:"/"sim:"/
// "wall:" room* globals of Lighting.txt; docs/lighting-mac.md 4).
struct RoomLightTuning {
    int type = 4; // roomLightingType: 2-8 = that many directional "Delta" lights
    float overall = 0.4f, bounce = 0.25f, bounceCrossover = 1, side = 1.2f, sideCrossover = 1, sideSlope = 0.75f;
    float specular = 0.8f, directionalBoost = 1;
};
// A room's lights from its irradiance (+X, -X, +Y, -Y travelling, +Z up from
// the floor, -Z down from the ceiling), as the lighting manager's
// FUN_0070daa8 (Mac BV) builds them: an ambient (the mean of the six
// colours) and `type` directionals, each the direction whose colour less the
// ambient has the largest max(|r|, |g|, |b|) (the four sides only when type
// < 3), signed: a direction darker than the mean is a negative light. A
// Delta's `towardLight` is the unit vector to the light (the table's
// travel direction, less sideSlope x Z on the sides, negated) and its colour
// is times directionalBoost.
struct RoomLights {
    std::array<float, 3> ambient{};
    struct Delta {
        std::array<float, 3> towardLight{0, 0, 1};
        std::array<float, 3> color{};
        float specular = 1;
    };
    std::vector<Delta> deltas;
};
RoomLights roomLights(const std::array<std::array<float, 3>, 6>& irradiance, const RoomLightTuning& tuning);

// Per-light overrides of the lights in the objects' models
// (Res/Lights/LotLights.txt and the files it `sinclude`s, mostly
// objects/lightObjects.txt and objects/colors.txt): macros
//
//   define Four(lightName)
//       lightAttribOverride &lightName intensity 1
//   enddef
//   create Four(lightingWallValue_light)
//
// and plain `lightAttribOverride <light> <attribute> <value>` lines. By
// lowercased light name, then lowercased attribute: the value's numbers
// (one, or three for a colour).
using LightOverrides = std::map<std::string, std::map<std::string, std::vector<float>>>;
LightOverrides parseLightOverrides(std::string_view text, const std::filesystem::path& includeDir = {});
LightOverrides loadLightOverrides(const std::filesystem::path& lotLightsFile);

} // namespace os2
