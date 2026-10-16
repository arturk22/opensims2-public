#pragma once

// The base game's lot lighting for the renderer, driven by the VM (plain
// C++, from main.mm), as docs/lighting-mac.md describes it:
//
// - The lighting state follows the VM's clock: "day" from 7:00 to 18:59,
//   "night" from 19:00 to 6:59 (Res/Lights/Lighting.txt), crossfading over
//   the state's "transitionTimes" in time (2 s; the game's cLerpLightSet).
// - Outside (room 0) objects get the state's outdoor lights (skylight as
//   ambient, each light's colour minus the skylight; simFill only for Sims),
//   light sets 0 and 1 (Renderer::outdoorLights).
// - Inside, each room gets an ambient and 4 "Delta" directional lights made
//   from the room's irradiance in 6 directions (roomLightingType 4), for
//   objects and for Sims with their own tuning (roomOverallStrength 0.5 /
//   0.8, roomSideStrength, roomBounceStrength, roomDirectionalBoost,
//   roomSpecular ... from Lighting.txt's globals, "object:"/"sim:"
//   prefixes). The irradiance comes from the light maps (GatherRoomLighting:
//   floor up and ceiling down, walls and portals by their normals), and the
//   direction table is where the light travels (docs/lighting-mac.md 4).
// - The lamps are the lights in their models (cPointLight nodes of the
//   CRES, with LotLights.txt's per-light overrides of intensity, falloff,
//   colour and the diffusionSource* attributes), at the level Change Light
//   gives them (vm::lightLevel); windows' and doors' lights are portals.
// - Walls, floors and terrain are lit by the lot's light maps
//   (formats/lot_lightmap: the game's diffusion as far as it is decoded),
//   made for the lamps at their levels and the state, crossfading with it.
//
// A light feeds the light maps only with diffusionSource* overrides (the
// game's cDiffusionLightExtension, docs/lighting-mac.md 10.3).
//
// With the light maps, objects and Sims are lit as the light room (3ARY
// 0x0B) under them, 0 outdoors: the room light sets are the lighting
// manager's per light room (docs/lighting-mac.md 11.1; inferred that an
// object's render group is that room). The Deltas are signed: a direction
// darker than the room's mean is a negative light (FUN_0070daa8).
// Inferred: without the light maps, the outdoors = room 0 or a room of the
// ground level's tiles along the lot's edge (the lots keep the outdoors as
// rooms of their own). The lamps' practical lights light the objects and
// Sims of their room directly (point lights, docs/lighting-mac.md 6).

#include "formats/lighting_config.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/xform.hpp"

#include <simd/simd.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {

class LotSimulation;
class Renderer;
class ResourceIndex;
class VmObjectView;
struct SimActor;

// The Sims' outdoor shadows for a lighting state (docs/lighting-mac.md 9):
// where the sun's light travels (straight down without a sun) and the
// outdoor shadow colour the lighting manager computes (Mac BV 0x705f70),
// with hmSunLightTransitionZone and shadowStrengthLimit.
struct SimShadowLight {
    simd_float3 sunTravel{0, 0, -1};
    simd_float3 outdoorColor{1, 1, 1};
};
SimShadowLight simShadowLight(const LightingState& state, float zone, float limit);

class LotLighting {
public:
    LotLighting(Renderer& renderer, ResourceIndex& index, LotSimulation& simulation, VmObjectView& objects,
                LightingConfig config, LightOverrides overrides);

    // A Sim's actor: lit with the Sims' light sets.
    void addSim(int16_t id, SimActor* actor) { sims_[id] = actor; }
    void removeSim(int16_t id) { sims_.erase(id); }
    // Forces a state ("day", "night", ...) instead of the clock's.
    void forceState(const std::string& name) { forced_ = name; }
    // Test aid: lamps that Change Light has not touched since the lot
    // loaded get this level (0-100) instead of their saved on/off.
    void setDefaultLampLevel(int level) { defaultLevel_ = level; }
    // The lot's light-map atlas (from buildLotModel): update() then keeps
    // the renderer's light maps in step with the lamps and the state.
    // A new layout (Build mode changed the lot): the light maps again.
    void setLightMapLayout(LotLightMapLayout layout) {
        layout_ = std::move(layout);
        lightMaps_.clear();
        outdoorRooms_.reset();
    }

    // Recomputes the lights for `seconds` (the caller's clock): call each
    // frame after the objects are placed.
    void update(double seconds);

    // What update() found (for the log and tests).
    struct Lamp {
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
    const std::vector<Lamp>& lamps() const { return lamps_; }

    // Extra point lights on some objects and Sims (a cinematic's light rig
    // on the actors of its render groups): asked for each drawn object and
    // Sim by ID; they go after the room's lights (16 in all).
    struct ExtraLight {
        Vec3 position{};
        Vec3 color{}; // times intensity
        float falloff = 1, range = 0;
        bool spot = false; // a spot: its cone, penumbra (radians) and direction
        float cone = 0, penumbra = 0;
        Vec3 direction{0, 0, -1};
    };
    std::function<std::vector<ExtraLight>(int16_t id)> extraLights;
    const std::string& stateName() const { return current_; }

private:
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
    const std::vector<ModelLight>& modelLights(const std::string& cres);
    LightingState blended() const;
    float global(const std::string& group, const std::string& name, float fallback) const;
    std::optional<Vec3> roomCentre(int16_t room, int level);
    // The room objects at (x, y) on a VM level are lit as: the light room
    // with the light maps (0 outdoors), else the VM's room.
    int16_t lightingRoom(int vmLevel, float x, float y);
    // Room 0, or without the light maps a room of the ground level's tiles
    // along the lot's edge (the lots keep the outdoors as rooms of their own;
    // inferred).
    bool outside(int16_t room);
    std::shared_ptr<const LotLightMap> lightMap(const std::string& state);
    void updateLightMaps(double seconds);

    Renderer& renderer_;
    ResourceIndex& index_;
    LotSimulation& sim_;
    VmObjectView& objects_;
    LightingConfig config_;
    LightOverrides overrides_;
    std::map<std::string, std::vector<ModelLight>> modelLights_;
    std::map<int16_t, SimActor*> sims_;
    std::map<std::pair<int, int16_t>, std::optional<Vec3>> centres_;
    std::string forced_;
    int defaultLevel_ = -1;
    std::optional<std::vector<int16_t>> outdoorRooms_;
    std::string current_, previous_; // states crossfading
    double changedAt_ = -1e9, last_ = 0;
    std::vector<Lamp> lamps_;
    std::optional<LotLightMapLayout> layout_;
    std::vector<std::pair<std::string, std::shared_ptr<const LotLightMap>>> lightMaps_; // by state and lamp levels
    std::shared_ptr<const LotLightMap> shownFrom_, shownTo_;
    float lightMapMix_ = 1;
};

} // namespace os2
