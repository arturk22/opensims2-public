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
// Inferred: the outdoors = room 0 or, with the light maps, a room mostly
// without floor (LotLightMapLayout::outdoor), else a room of the ground
// level's tiles along the lot's edge (the lots keep the outdoors as rooms of
// their own); a
// light without diffusionSource* overrides diffuses with its own intensity
// and colour (the model's cDiffusionLightExtension is not read). The
// lamps' practical lights light the objects and Sims of their room
// directly (point lights, docs/lighting-mac.md 6).

#include "formats/lighting_config.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/xform.hpp"

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

class LotLighting {
public:
    LotLighting(Renderer& renderer, ResourceIndex& index, LotSimulation& simulation, VmObjectView& objects,
                LightingConfig config, LightOverrides overrides);

    // A Sim's actor: lit with the Sims' light sets.
    void addSim(int16_t id, SimActor* actor) { sims_[id] = actor; }
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
        int level = 0;     // 0-100
        int16_t room = 0;
        int objectLevel = 0; // the object's level, as the lot numbers them (VM level - 1)
        // Its diffusion source (diffusionSource* attributes): colour x intensity, falloff, scales.
        Vec3 diffusionColor{};
        float diffusionFalloff = 1, floorScale = 0.65f, wallScale = 1;
        bool portal = false; // a window's or door's light (outside light coming in)
    };
    const std::vector<Lamp>& lamps() const { return lamps_; }

    // Extra point lights on some objects and Sims (a cinematic's light rig
    // on the actors of its render groups): asked for each drawn object and
    // Sim by ID; they go after the room's lights (16 in all).
    struct ExtraLight {
        Vec3 position{};
        Vec3 color{}; // times intensity
        float falloff = 1, range = 0;
    };
    std::function<std::vector<ExtraLight>(int16_t id)> extraLights;
    const std::string& stateName() const { return current_; }

private:
    struct ModelLight {
        std::string name; // lowercased
        Vec3 position{};  // model space
        Vec3 color{1, 1, 1};
        float intensity = 1, falloff = 1;
        Vec3 diffusionColor{-1, -1, -1};
        float diffusionIntensity = -1, diffusionFalloff = 1, floorScale = 0.65f, wallScale = 1;
        bool hasDiffusionColor() const { return diffusionColor[0] >= 0; }
    };
    const std::vector<ModelLight>& modelLights(const std::string& cres);
    LightingState blended() const;
    float global(const std::string& group, const std::string& name, float fallback) const;
    std::optional<Vec3> roomCentre(int16_t room, int level);
    // Room 0, or a room of the ground level's tiles along the lot's edge
    // (the lots keep the outdoors as rooms of their own; inferred).
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
