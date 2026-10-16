#pragma once

// The lot's lamps for the light maps, without a renderer (round n42): what
// the app's LotLighting (app/lot_lighting.hpp) and the headless hosts
// (vm/lot_light.hpp: s2pkg, the tests) share, so both make the same light
// maps and read the same light under a Sim.
//
// - The lights of an object's model (the cPointLight / cSpotLight nodes of
//   its CRES, with LotLights.txt's per-light overrides and the
//   diffusionSource* attributes: docs/lighting-mac.md 10.3).
// - A lamp (one light of a drawn object at its Change Light level) and the
//   light-map lights made of the lamps (formats/lot_lightmap), with the key
//   the light maps are cached by.
// - The light under a point as the environment score reads it
//   (cLightingManager::LocationIlluminationStrength, docs/motives-mac.md 8.4).

#include "lighting_config.hpp"
#include "lot_lightmap.hpp"
#include "xform.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

// A light of an object's model (model space).
struct ModelLight {
    std::string name; // lowercased
    Vec3 position{};  // model space
    Vec3 color{1, 1, 1};
    float intensity = 1, falloff = 1;
    // A cSpotLight: its cone and penumbra (radians) and where it shines
    // (the light's -Z, model space: cSpotLight's light at a point, Mac BV
    // 0xc9f3ac, docs/cinematics-mac.md §9; so the models have it: ceiling
    // canisters down, garden uplights up, the portals' lights into their room).
    bool spot = false;
    float cone = 0, penumbra = 0;
    Vec3 direction{0, 0, -1};
    // Its cDiffusionLightExtension (the extension's defaults).
    Vec3 diffusionColor{1, 1, 1};
    float diffusionIntensity = 0, diffusionFalloff = 1, floorScale = 1, wallScale = 1, interFloor = 0.5f;
    bool directional = false;
    float directionalFalloff = 0;
};
// The lights of the model `cres` (without "_cres"): none when it has none
// or cannot be read.
std::vector<ModelLight> loadModelLights(ResourceIndex& index, const std::string& cres, const LightOverrides& overrides);

// One light of a drawn object, placed, at its level.
struct LotLamp {
    int16_t id = 0;
    std::string light;
    Vec3 position{};
    Vec3 color{};      // colour x intensity, before the level
    float falloff = 1; // falloffRate
    bool spot = false; // a spot: cone, penumbra, world direction
    float cone = 0, penumbra = 0;
    Vec3 direction{0, 0, -1};
    int level = 0;     // 0-100
    int16_t room = 0;
    int objectLevel = 0; // the object's level, as the lot numbers them (VM level - 1)
    // Its diffusion source (diffusionSource* attributes): colour x intensity, falloff, scales.
    Vec3 diffusionColor{};
    float diffusionFalloff = 1, floorScale = 1, wallScale = 1, interFloor = 0.5f;
    bool directional = false;
    float directionalFalloff = 0;
    bool portal = false; // a window's or door's light (outside light coming in)
    bool doorOrArch = false; // of a door or arch (OBJD type 8)
};
// A lamp of model light `l` of an object placed at `at` (`level` 0-100,
// `objectLevel` the lot's numbering); `room` is left 0.
LotLamp placedLamp(int16_t id, const ModelLight& l, const Xform& at, int level, int objectLevel, bool portal,
                   bool doorOrArch);

// The light-map lights of the lamps that shine (level > 0), and the key the
// light maps of `state` are cached by (the state and every lamp's object,
// light and level).
std::vector<LightMapLight> lightMapLights(const std::vector<LotLamp>& lamps, const std::string& state, std::string* key);

// The day/night phase of the hour (docs/lighting-mac.md 2): "day" from 7:00
// to 18:59, "night" otherwise.
const char* lightingStateOfHour(int hour);

// The light under a point as the environment score reads it
// (cLightingManager::LocationIlluminationStrength, docs/motives-mac.md
// §8.4): the floor light map's texel there (the maps `from` and `to`
// crossfading by `mix`; `from` null: `to` alone) x 2 (cLightMapManager
// +0x70, undoing the pages' 0.5) as an illuminance, 0.3 r + 0.59 g + 0.11 b.
// `vmLevel` as the VM numbers levels (1 = the ground). Off the pages 0;
// nothing without `to`.
std::optional<float> floorIlluminationAt(const LotLightMapLayout& layout, const LotLightMap* from,
                                         const LotLightMap* to, float mix, int vmLevel, float x, float y);

} // namespace os2
