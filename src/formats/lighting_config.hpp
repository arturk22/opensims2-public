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

} // namespace os2
