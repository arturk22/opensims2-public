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
// nothing. ParentFadeAlpha (BV 0x662c30): an object the rule fades takes its
// container's alpha when that is lower (the app: lot_actors.cpp).
//
// The cinematic camera ("Animated", CineCamCamera.txt: distance 1, falloff 2,
// objectFadeCineCam 1) has its own rule only when the scene asks for it (the
// Cinematic primitive's flag 0x80, or the moveIn scene: the Prep's +0x165;
// docs/camera-ui-mac.md 1.10, round n50); otherwise the rule above with its
// numbers. Its own rule: each frame the scene's box from its actors and two
// numbers (cinematicFadeScene, FUN_007b8834), then for each object that is
// not an actor, not held by one and not a door or window, the alpha of
// cinematicFadeAlpha (FUN_0065deb0): objects near the line from the eye to
// the scene, in front of it, fade; the PlumbBob and the Overhead Meter hide;
// a Sim that is not an actor hides or shows whole (cinematicPersonHidden).

#include "formats/camera_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

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

// --- The cinematic camera's rule (docs/camera-ui-mac.md 1.10, round n50) ---

using FadeVec = std::array<float, 3>;
struct FadeBox {
    FadeVec lo{0, 0, 0}, hi{-1, -1, -1}; // empty while lo > hi
    bool empty() const { return lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]; }
};

namespace fadedetail {
inline FadeVec sub(const FadeVec& a, const FadeVec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline float dot(const FadeVec& a, const FadeVec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float length(const FadeVec& a) { return std::sqrt(dot(a, a)); }
inline FadeVec centre(const FadeBox& b) {
    return {(b.lo[0] + b.hi[0]) * 0.5f, (b.lo[1] + b.hi[1]) * 0.5f, (b.lo[2] + b.hi[2]) * 0.5f};
}
} // namespace fadedetail

// The cinematic camera's numbers of a frame (cTSSGSystem +0x390, +0x370,
// +0x374; FUN_007b8834): the box of the scene's actors in the world, each
// actor's model box cut to z0 + 0.3 h .. z1 - 0.05 h, an actor that is not a
// person with half a diagonal under 0.5 left out (zero when none); the depth
// of the box's centre along the view (at least 0) and 0.6 x half the box's
// diagonal over tan(fov / 2) x that depth (at most 1).
struct CinematicFadeActor {
    FadeBox box; // its model's world box
    bool person = false;
};
struct CinematicFadeScene {
    FadeBox box;
    float distance = 0; // ObjectFadeParams' +0x370
    float ratio = 1;    // +0x374
};
inline CinematicFadeScene cinematicFadeScene(const std::vector<CinematicFadeActor>& actors, const FadeVec& eye,
                                             const FadeVec& dir, float fov) {
    using namespace fadedetail;
    CinematicFadeScene out;
    for (const auto& a : actors) {
        if (a.box.empty())
            continue;
        FadeBox b = a.box;
        const float h = b.hi[2] - b.lo[2];
        b.lo[2] += 0.3f * h;
        b.hi[2] -= 0.05f * h;
        if (length(sub(b.hi, b.lo)) * 0.5f < 0.5f && !a.person)
            continue;
        if (out.box.empty()) {
            out.box = b;
        } else {
            for (int i = 0; i < 3; ++i)
                out.box.lo[size_t(i)] = std::min(out.box.lo[size_t(i)], b.lo[size_t(i)]),
                out.box.hi[size_t(i)] = std::max(out.box.hi[size_t(i)], b.hi[size_t(i)]);
        }
    }
    if (out.box.empty())
        out.box = {{0, 0, 0}, {0, 0, 0}};
    out.distance = std::max(0.0f, dot(sub(centre(out.box), eye), dir));
    const float half = length(sub(out.box.hi, out.box.lo)) * 0.5f;
    const float r = 0.6f * half / (std::tan(fov * 0.5f) * out.distance);
    out.ratio = std::isfinite(r) && r < 1 ? r : 1.0f;
    return out;
}

// An object's alpha under the cinematic camera (FUN_0065deb0): only within
// the scene's depth + 4 of the eye; t the depth of its box's centre (at most
// the scene's, a ratio < 1 nearer than 1, up to 4 past the scene), P the
// point at t on the view, r = max(t x ratio, 0.5) the clear radius there; the
// part of the segment from the centre to P outside the box, x the ratio x
// its length, gives a = (s - r/4) / (3r/4); an object whose box does not
// reach past the scene's box (by more than 0.1 on every axis, the far side
// along the view) is not faded, else max(a, 1 - min(g - 0.1, 0.4) (1 - a) / 0.4).
inline float cinematicFadeAlpha(const CinematicFadeScene& s, const FadeVec& eye, const FadeVec& dir,
                                const FadeVec& position, const FadeBox& box) {
    using namespace fadedetail;
    const float reach = s.distance + 4;
    if (!(dot(sub(position, eye), sub(position, eye)) < reach * reach) || box.empty())
        return 1;
    const FadeVec c = centre(box);
    const float halfDiag = length(sub(box.hi, box.lo)) * 0.5f;
    float t = dot(sub(c, eye), dir);
    float ratio = 1;
    if (t < 1) {
        float k;
        if (t >= 0.1f) {
            k = 1 - t;
        } else {
            k = 0.9f;
            t = 0.1f;
        }
        ratio = t / (k * 5 + 1);
    }
    if (s.distance < t) {
        ratio = std::min((t - s.distance) * 2 + 1, 4.0f);
        t = s.distance;
    }
    const FadeVec p{eye[0] + dir[0] * t, eye[1] + dir[1] * t, eye[2] + dir[2] * t};
    const float toP = length(sub(c, p));
    const float r = std::max(t * s.ratio, 0.5f);
    if (!(toP < r + halfDiag))
        return 1;
    // The part of the segment centre -> P inside the box (the faces' planes).
    float inside = 1;
    bool degenerate = false;
    for (int i = 0; i < 3 && !degenerate; ++i)
        for (int side = 0; side < 2; ++side) {
            const float a = side == 0 ? c[size_t(i)] - box.lo[size_t(i)] : box.hi[size_t(i)] - c[size_t(i)];
            const float b = side == 0 ? p[size_t(i)] - box.lo[size_t(i)] : box.hi[size_t(i)] - p[size_t(i)];
            if (a <= 0) {
                degenerate = true; // a flat box: all of it counts as outside
                break;
            }
            if (b < 0)
                inside = std::min(inside, a / (a - b));
        }
    const float outside = degenerate ? 1 : 1 - inside;
    const float sd = outside * ratio * toP;
    if (r <= sd)
        return 1;
    float a = (sd - r * 0.25f) / (r * 0.75f);
    if (a >= 1)
        return 1;
    a = std::max(a, 0.0f);
    float g = 3.4028235e38f;
    for (int i = 0; i < 3; ++i)
        g = std::min(g, dir[size_t(i)] >= 0 ? s.box.hi[size_t(i)] - box.lo[size_t(i)]
                                            : box.hi[size_t(i)] - s.box.lo[size_t(i)]);
    if (g > 0.1f) {
        const float gg = g <= 0.5f ? g - 0.1f : 0.4f;
        const float b = 1 - gg * (1 - a) / 0.4f;
        if (b < 1)
            return std::max(a, b);
    }
    return 1;
}

// A Sim that is not an actor (cObject +0x108 bit 3, set on persons by
// cObject::OnInit, BV 0x654f98): hidden whole once its alpha is at most 0.75,
// shown again at 1 (the bit 4 the binary keeps).
inline bool cinematicPersonHidden(bool hidden, float alpha) {
    if (!hidden)
        return alpha <= 0.75f;
    return !(alpha >= 1);
}

// Objects the cinematic camera's rule hides (+0x108 bit 1, by their GUID in
// cObject::OnInit): the PlumbBob and the Overhead Meter.
inline bool cinematicHiddenObject(uint32_t guid) { return guid == 0xF37E5AE0u || guid == 0x0CBBE10Eu; }

} // namespace os2
