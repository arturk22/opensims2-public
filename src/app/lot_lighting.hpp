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
//   0.8, roomSideStrength, roomBounceStrength, roomDirectionalBoost, ...
//   from Lighting.txt's globals, "object:"/"sim:" prefixes).
// - The lamps are the lights in their models (cPointLight nodes of the
//   CRES, with LotLights.txt's per-light overrides of intensity, falloff and
//   colour), at the level Change Light gives them (vm::lightLevel), times
//   the state's "modifiers insideObjects/outsideObjects" practical strength.
//
// Inferred (not decoded in the game): how a room's irradiance is gathered
// (here: each lamp in the room, at its falloff 1 / (1 + C r^2) from the
// room's centre, split over the 6 directions by where it is; windows are not
// counted as lamps: their lights in the model, "..._light" spots at each
// pane, bring the outside in with the state's "modifiers insidePortals"
// practical strength, 0.5 by day and 0 at night), the 6 directions (+-X, +-Y, +-Z; the side lights tilted down by
// roomSideSlope), unlitRoomColor as the minimum ambient of a room whose
// brightness is under its threshold, C = falloffRate x the modifier's
// practical falloff, and the range where the falloff reaches
// lightRangeCutoff (0.275). Walls, floors and terrain should use the light
// maps (not computed): they get the lamps as point lights instead.

#include "formats/lighting_config.hpp"
#include "formats/xform.hpp"

#include <map>
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
        bool portal = false; // a window's or door's light (outside light coming in)
    };
    const std::vector<Lamp>& lamps() const { return lamps_; }
    const std::string& stateName() const { return current_; }

private:
    struct ModelLight {
        std::string name; // lowercased
        Vec3 position{};  // model space
        Vec3 color{1, 1, 1};
        float intensity = 1, falloff = 1;
    };
    const std::vector<ModelLight>& modelLights(const std::string& cres);
    LightingState blended() const;
    float global(const std::string& group, const std::string& name, float fallback) const;
    std::optional<Vec3> roomCentre(int16_t room, int level);
    // Room 0, or a room of the ground level's tiles along the lot's edge
    // (the lots keep the outdoors as rooms of their own; inferred).
    bool outside(int16_t room);

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
};

} // namespace os2
