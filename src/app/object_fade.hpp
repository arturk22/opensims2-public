#pragma once

// Objects that fade out near the camera (docs/camera-ui-mac.md §1.10, from
// the Mac binary): cTSSGSystem::ObjectFadeParams (BV 0x7b35c4) hands the
// scene graph the current camera's objectFade* params (LiveCamera.txt:
// distance 10, falloff 4, allObjects 0, cineCam 0) while a lot is shown, and
// each cObject::UpdateVisibilityAndRendering (BV 0x661e5c) takes from them
// its alpha. Plain C++; the app (lot_actors.cpp) feeds it the objects.
//
// For the lot cameras (no cinecam flag) an object fades when its "Flag Field
// 2" (object data 40, STR# 0xD6) has bit 3, "Obstructs View" (or when
// objectFadeAllObjects is above 0, no camera in the game's data sets it).
// The depth is how far the centre of the object's world bounding box is
// along the camera's view axis (the y of the viewer-space point); only
// between 0 and the distance does the alpha drop:
//
//   alpha = (depth / distance) ^ falloff     0 < depth < distance
//   alpha = 1                                 otherwise
//
// so the Live camera's objects 5 tiles in front of the eye are at 6%, 8 tiles
// at 41%, and fully drawn from 10. A faded object is drawn with alpha
// blending (render style 4) and its materials' alpha times the fade. A camera
// with no objectFadeDistance (Build's top-down view, the free camera) fades
// nothing. The cinematic camera's (objectFadeCineCam 1: a ray from the camera
// to its target against the objects' boxes, FUN_0065deb0) is not done.

#include "formats/camera_config.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

namespace os2 {

struct ObjectFadeParams {
    float distance = 0;      // objectFadeDistance
    float falloff = 1;       // objectFadeFalloff
    bool allObjects = false; // objectFadeAllObjects > 0
    bool cineCam = false;    // objectFadeCineCam > 0 (the cinematic camera's own rule)
};

// The camera's params (nothing when it has no objectFadeDistance).
inline std::optional<ObjectFadeParams> objectFadeParams(const CameraDef& camera) {
    if (!camera.params.count("objectfadedistance"))
        return std::nullopt;
    ObjectFadeParams p;
    p.distance = camera.param("objectfadedistance", 0);
    p.falloff = camera.param("objectfadefalloff", 1);
    p.allObjects = camera.param("objectfadeallobjects", 0) > 0;
    p.cineCam = camera.param("objectfadecinecam", 0) > 0;
    return p;
}

// Object data 40 bit 3 of STR# 0xD6: "Obstructs View".
constexpr int16_t kObstructsView = 1 << 3;

// Whether an object with this Flag Field 2 fades (the lot cameras' rule).
inline bool objectFades(const ObjectFadeParams& p, int16_t flagField2) {
    return p.allObjects || (flagField2 & kObstructsView) != 0;
}

// The alpha of an object whose bounding box centre is `depth` along the camera's view axis.
inline float objectFadeAlpha(const ObjectFadeParams& p, float depth) {
    if (!(depth > 0 && depth < p.distance))
        return 1;
    return std::pow(depth / p.distance, p.falloff);
}

} // namespace os2
