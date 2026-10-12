#pragma once

// The Create-a-Sim studio: the global lot TSData/Res/GlobalLots/CAS!.package
// (a brick loft on a 60x50 lot: walls, floors, the studio props, the podium
// "CAS - Podium" where the Sim being made stands and the backdrop "CAS -
// Backdrop" the family poses in front of) drawn around CAS, lit by its own
// lights: the lamps in the props' and the podium's models (cLightRefNode ->
// cPointLight/cSpotLight, as LotLighting reads them) with the attributes of
// Res/Lights/LotLights.txt, CAS_lights.txt and CAS_lighting.txt (later files
// win: "CASPodiumArea_podiumKey7" intensity 1.5, the family area's key,
// fill and rims, the studio lamps' diffusion), over the "CAS" lighting state.
//
// - Light maps: computeLotLightMap for the CAS state with the lamps'
//   diffusion sources (walls and floors).
// - Objects: the CAS state's outdoor lights (Renderer::outdoorLights) as
//   ambient, plus the lamps as practical point lights.
// - Sims: the lamps that light the area they stand in, as directional
//   lights from where each lamp is (colour x intensity, attenuated by the
//   lamp's falloff over the distance), and the state's sky as ambient
//   (inferred: the game lights the podium with its spot lights; cones and
//   penumbras are left out).

#include "formats/lighting_config.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/xform.hpp"

#include <simd/simd.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

class Renderer;
class ResourceIndex;

class CasStudio {
public:
    CasStudio(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir);
    ~CasStudio();

    // Loads the lot into the renderer, its walls cut away along
    // `viewDirection` (from the camera towards the Sim). False if the lot is
    // missing or cannot be read (then nothing is drawn).
    bool load(simd_float2 viewDirection);
    bool loaded() const { return loaded_; }

    // The places the area models mark (their CRES locators): where a Sim
    // stands, the way it faces (unit, the skeleton's +Y turned by the mark),
    // in lot space. CASPodiumArea: "slot_casstudio_simmark_podium";
    // CASFamilyArea: "slot_casstudio_simmark_0".."7" (back row 0-3, front
    // row 4-7) and "..._baby_0", "_1".
    struct Mark {
        simd_float3 position{0, 0, 0};
        simd_float2 facing{0, 1};
    };
    const Mark& podium() const { return podium_; }
    const std::vector<Mark>& familyMarks() const { return family_; }
    const std::vector<Mark>& babyMarks() const { return babies_; }
    // The camera locators by lowercased name ("camera_face_adult",
    // "camera_clothes_teen", "camera_family_0", ...), lot space: where the
    // CreateASim camera stands and how it is turned (the locator's world
    // transform; docs/camera-ui-mac.md 1.6).
    struct CameraMark {
        simd_float3 position{0, 0, 0};
        simd_quatf orientation{simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f)};
    };
    const std::map<std::string, CameraMark>& cameras() const { return cameras_; }

    // Light sets for the scene: set 0 objects, set 1 the Sims at `spot`
    // (the podium's lamps, or the family area's).
    void applyLights(simd_float3 spot, bool family);

    struct Lamp {
        std::string name;
        simd_float3 position{0, 0, 0};
        simd_float3 color{1, 1, 1}; // x intensity
        float falloff = 0;
        // diffusion source for the light maps
        simd_float3 diffusionColor{0, 0, 0};
        float diffusionFalloff = 1, floorScale = 0.65f, wallScale = 1;
        int level = 0;
    };
    const std::vector<Lamp>& lamps() const { return lamps_; }

private:
    void readMarks(const std::string& cres, const Xform& at);
    bool podiumFound_ = false;
    Renderer& renderer_;
    ResourceIndex& index_;
    std::filesystem::path resDir_;
    bool loaded_ = false;
    Mark podium_;
    std::vector<Mark> family_, babies_;
    std::map<std::string, CameraMark> cameras_;
    std::vector<Lamp> lamps_;
    LightingConfig config_;
    LightOverrides overrides_;
};

} // namespace os2
