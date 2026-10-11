#pragma once

// The camera of a cinematic (formats/cinematic.hpp): the CINE's camera
// animation ("o-cineCam-<scene>_anim") played from the origin actor's
// placement. The joint "cinecam" carries the camera (Euler XYZ rotation in
// degrees and translation, Z up, as the Sims' skeletons); the camera looks
// along its +Y with +Z up (inferred from the data: the shots frame the
// actors' heads); "viewersGroup" gives the vertical field of view (inferred
// vertical) and "postprocessingmaterial" the letterbox (each bar's share of
// the height, 0.5 = black).

#include "formats/anim.hpp"
#include "formats/cinematic.hpp"
#include "formats/skeleton.hpp"
#include "formats/xform.hpp"

#include <simd/simd.h>

#include <memory>
#include <string>

namespace os2 {

class ResourceIndex;

class CinematicCamera {
public:
    // Loads "<scene>_cine" and its camera animation; throws if either is missing.
    CinematicCamera(ResourceIndex& index, const std::string& scene);

    // Starts at `now` (seconds) from `origin` (lot space: tiles, Z up, metres).
    void start(const Xform& origin, double now);
    bool active(double now) const { return started_ && now - start_ < duration(); }
    double duration() const;

    struct Shot {
        simd_float3 eye{0, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
        float fovY = 1.0f;     // radians
        float letterbox = 0;   // each bar's share of the height
    };
    Shot shot(double now) const;
    simd_float4x4 view(double now) const;
    simd_float4x4 projection(double now, float aspect) const;

    const CinematicScene& scene() const { return scene_; }
    const Animation& animation() const { return *anim_; }

private:
    CinematicScene scene_;
    std::unique_ptr<Animation> anim_;
    Skeleton skeleton_; // one bone, "cinecam"
    Xform origin_;
    double start_ = 0;
    bool started_ = false;
    float channel(const char* target, const char* name, float ticks, float fallback) const;
};

} // namespace os2
