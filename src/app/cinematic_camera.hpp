#pragma once

// The camera of a cinematic (formats/cinematic.hpp): the CINE's camera
// animation ("o-cineCam-<scene>_anim", or "<camera>-1_anim", "-2_anim", ...
// one after the other when the scene has sequenced animations, as the Mac
// binary's PrepCinematicScene names them with "%s-%1d") played from the
// origin actor's placement. The joint "cinecam" carries the camera (Euler
// XYZ rotation in degrees and translation, Z up, as the Sims' skeletons);
// the camera looks along its +Y with +Z up (inferred from the data: the
// shots frame the actors' heads); "viewersGroup" gives the vertical field of
// view (inferred vertical) and "postprocessingmaterial" the letterbox (each
// bar's share of the height, 0.5 = black), film grain, vignette and bloom.
// Cuts are keys at the same time in the camera's channels; the "effect"
// events (type 8, effectName=crossFadeSnap*) ask for a snapshot crossfade.
// Mirrored scenes (the Cinematic primitive's flag 0x01, or 0x02 with temp 0)
// reflect the camera's path across the origin actor's YZ plane.

#include "formats/anim.hpp"
#include "formats/cinematic.hpp"
#include "formats/skeleton.hpp"
#include "formats/xform.hpp"

#include <simd/simd.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

class ResourceIndex;

class CinematicCamera {
public:
    // Loads "<scene>_cine" and its camera animation(s); throws if missing.
    CinematicCamera(ResourceIndex& index, const std::string& scene);

    // Starts at `now` (seconds) from `origin` (lot space: tiles, Z up, metres).
    void start(const Xform& origin, double now, bool mirror = false);
    bool active(double now) const { return started_ && now - start_ < duration(); }
    double duration() const;
    double startTime() const { return start_; }
    bool mirrored() const { return mirror_; }
    const Xform& origin() const { return origin_; }

    struct Shot {
        simd_float3 eye{0, 0, 0}, forward{0, 1, 0}, up{0, 0, 1};
        float fovY = 1.0f;   // radians
        float letterbox = 0; // each bar's share of the height
        float grain = 0;
        std::array<float, 3> vignette{0.5f, 0.5f, 0};
        std::array<float, 4> bloom{0, 0, 0, 1};
    };
    Shot shot(double now) const;
    simd_float4x4 view(double now) const;
    simd_float4x4 projection(double now, float aspect) const;

    // The camera animations' effect events (type 8), in scene seconds.
    struct Effect {
        double at = 0;
        std::string name; // effectName
        std::string op;   // softStart, hardStart, ...
    };
    const std::vector<Effect>& effects() const { return effects_; }
    // True if the camera jumps (a cut: keys at the same time) in (from, to].
    bool cutBetween(double from, double to) const;

    const CinematicScene& scene() const { return scene_; }

private:
    struct Part {
        std::unique_ptr<Animation> anim;
        double begin = 0; // seconds into the scene
    };
    const Part& partAt(double t, float& ticks) const;
    float channel(const Animation& a, const char* target, const char* name, size_t component, float ticks,
                  float fallback) const;

    CinematicScene scene_;
    std::vector<Part> parts_;
    std::vector<Effect> effects_;
    std::vector<double> cuts_;
    Skeleton skeleton_; // one bone, "cinecam"
    Xform origin_;
    double start_ = 0;
    bool started_ = false, mirror_ = false;
};

} // namespace os2
