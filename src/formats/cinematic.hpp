#pragma once

// CINE (type 0x4D51F042, nGZSceneDBAnim::cCinematicScene; the 23 scenes of
// Sims3D/Sims01.package, group 0x1C0532FA): an RCOL with one block, read as
// the Mac binary's cCinematicScene::ReadFrom / WriteTo do (docs/cinematics-mac.md):
//
//   u32 version (4; 3 adds two unused strings after the camera name; < 3
//       has only name + required per actor; < 2 no sequence count)
//   cSGResource (the name: "<scene>_cine")
//   str7 camera animation ("o-cinecam-<scene>"; the ANIM is
//       "o-cineCam-<scene>_anim", skeleton "cinecam": joint "cinecam"
//       rotation/translation, "viewersGroup" fov / near-far clip,
//       "postprocessingmaterial" letterbox, film grain, vignette, bloom)
//   u32 actor count, per actor:
//       str7 name ("kisser", "sim", "baby", "tsCineLightRig.i", ...),
//       u8 required, str7 scenegraph object name ("auskel::1": skeleton
//       and instance), u32 cinematic render groups, u8 auto-create,
//       u8 auto-origin, str7 auto model ("cineLightRig-template"),
//       str7 auto animation list (comma-separated, one per sequence)
//   u32 origin actor index (the actor the camera and the light rig follow)
//   u32 number of sequenced animations (the "-1_anim,-2_anim" parts)

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

constexpr uint32_t kTypeCinematic = 0x4D51F042;

struct CinematicScene {
    struct Actor {
        std::string name;
        bool required = false;
        std::string sgObjectName;
        uint32_t renderGroups = 0;
        bool autoCreate = false, autoOrigin = false;
        std::string autoModel;
        std::vector<std::string> autoAnimations; // the list split at commas
    };
    uint32_t version = 0;
    std::string name;            // "firstKissTeen_cine"
    std::string cameraAnimation; // "o-cinecam-firstKissTeen"
    std::vector<Actor> actors;
    uint32_t originActor = 0;
    uint32_t sequencedAnimations = 0;

    // The whole CINE resource (RCOL).
    static CinematicScene parse(std::span<const uint8_t> resource);
    // The camera's ANIM name ("o-cinecam-x" -> "o-cinecam-x_anim").
    std::string cameraAnimationResource() const;
};

} // namespace os2
