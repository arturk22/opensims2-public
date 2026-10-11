#include "app/cinematic_camera.hpp"

#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <strings.h>

namespace os2 {

CinematicCamera::CinematicCamera(ResourceIndex& index, const std::string& sceneName) {
    const std::string name = sceneName.ends_with("_cine") ? sceneName : sceneName + "_cine";
    const auto loc = index.findByName(name, kTypeCinematic);
    if (!loc)
        throw std::runtime_error("cinematic scene not found: " + name);
    scene_ = CinematicScene::parse(index.read(*loc));
    anim_ = std::make_unique<Animation>(loadAnimation(index, scene_.cameraAnimationResource()));
    Skeleton::Bone bone;
    bone.name = "cinecam";
    skeleton_.name = "cinecam";
    skeleton_.bones.push_back(bone);
}

double CinematicCamera::duration() const {
    return anim_->duration / 1000.0; // 1 tick = 1 ms
}

void CinematicCamera::start(const Xform& origin, double now) {
    origin_ = origin;
    start_ = now;
    started_ = true;
}

float CinematicCamera::channel(const char* target, const char* name, float ticks, float fallback) const {
    const Animation::Target* t = anim_->findTarget(target);
    if (!t)
        return fallback;
    for (const auto& c : t->channels)
        if (strcasecmp(c.name.c_str(), name) == 0 && !c.components.empty() && !c.components[0].keys.empty())
            return c.components[0].sample(ticks, c.duration());
    return fallback;
}

CinematicCamera::Shot CinematicCamera::shot(double now) const {
    Shot s;
    const float ticks = float(std::clamp(now - start_, 0.0, duration()) * 1000.0);
    Xform local;
    if (const Animation::Target* t = anim_->findTarget("cinecam"))
        local = samplePose(*t, skeleton_, ticks)[0];
    const Xform world = origin_.then(local);
    const Vec3 f = world.rotate({0, 1, 0}), u = world.rotate({0, 0, 1});
    s.eye = simd_make_float3(world.t[0], world.t[1], world.t[2]);
    s.forward = simd_normalize(simd_make_float3(f[0], f[1], f[2]));
    s.up = simd_normalize(simd_make_float3(u[0], u[1], u[2]));
    s.fovY = channel("viewersGroup", "fov", ticks, 1.0f);
    s.letterbox = std::clamp(channel("postprocessingmaterial", "cameraletterbox", ticks, 0.0f), 0.0f, 0.5f);
    return s;
}

simd_float4x4 CinematicCamera::view(double now) const {
    const Shot s = shot(now);
    const simd_float3 f = s.forward;
    const simd_float3 side = simd_normalize(simd_cross(f, s.up));
    const simd_float3 u = simd_cross(side, f);
    const simd_float3 e = s.eye;
    return simd_matrix(simd_make_float4(side.x, u.x, -f.x, 0), simd_make_float4(side.y, u.y, -f.y, 0),
                       simd_make_float4(side.z, u.z, -f.z, 0),
                       simd_make_float4(-simd_dot(side, e), -simd_dot(u, e), simd_dot(f, e), 1));
}

simd_float4x4 CinematicCamera::projection(double now, float aspect) const {
    const float n = 0.05f, fa = 1000.0f;
    const float ys = 1.0f / std::tan(shot(now).fovY * 0.5f);
    const float xs = ys / aspect;
    const float zs = fa / (n - fa);
    return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                       simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
}

} // namespace os2
