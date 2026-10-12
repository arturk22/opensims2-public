#include "app/cinematic_camera.hpp"

#include "render/camera.hpp"

#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <strings.h>

namespace os2 {

namespace {

// "key=value" lines (or '|'-separated) of an event's data.
std::string eventValue(const std::string& data, const std::string& key) {
    size_t at = 0;
    while (at < data.size()) {
        const size_t end = data.find_first_of("|\r\n", at);
        const std::string line = data.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
            return line.substr(key.size() + 1);
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return "";
}

} // namespace

CinematicCamera::CinematicCamera(ResourceIndex& index, const std::string& sceneName) {
    const std::string name = sceneName.ends_with("_cine") ? sceneName : sceneName + "_cine";
    const auto loc = index.findByName(name, kTypeCinematic);
    if (!loc)
        throw std::runtime_error("cinematic scene not found: " + name);
    scene_ = CinematicScene::parse(index.read(*loc));
    // One camera animation, or one per sequence ("%s-%1d", PrepCinematicScene).
    std::vector<std::string> names;
    std::string base = scene_.cameraAnimation;
    if (base.ends_with("_anim"))
        base.resize(base.size() - 5);
    if (scene_.sequencedAnimations > 1)
        for (uint32_t i = 1; i <= scene_.sequencedAnimations; ++i)
            names.push_back(base + "-" + std::to_string(i) + "_anim");
    else
        names.push_back(base + "_anim");
    double begin = 0;
    for (const auto& n : names) {
        Part p;
        p.anim = std::make_unique<Animation>(loadAnimation(index, n));
        p.begin = begin;
        for (const auto& e : p.anim->events)
            if (e.type == 8)
                effects_.push_back({begin + e.ticks / 1000.0, eventValue(e.data, "effectName"), eventValue(e.data, "op")});
        // Cuts: two keys at the same time in the camera's translation or rotation.
        if (const Animation::Target* t = p.anim->findTarget("cinecam"))
            for (const auto& c : t->channels)
                for (const auto& comp : c.components)
                    for (size_t k = 1; k < comp.keys.size(); ++k)
                        if (comp.curve != Animation::Curve::Baked && comp.keys[k].time == comp.keys[k - 1].time &&
                            comp.keys[k].value != comp.keys[k - 1].value)
                            cuts_.push_back(begin + comp.keys[k].time / 1000.0);
        begin += p.anim->duration / 1000.0; // 1 tick = 1 ms
        parts_.push_back(std::move(p));
    }
    std::sort(cuts_.begin(), cuts_.end());
    cuts_.erase(std::unique(cuts_.begin(), cuts_.end()), cuts_.end());
    Skeleton::Bone bone;
    bone.name = "cinecam";
    skeleton_.name = "cinecam";
    skeleton_.bones.push_back(bone);
}

double CinematicCamera::duration() const {
    return parts_.empty() ? 0 : parts_.back().begin + parts_.back().anim->duration / 1000.0;
}

void CinematicCamera::start(const Xform& origin, double now, bool mirror) {
    origin_ = origin;
    start_ = now;
    started_ = true;
    mirror_ = mirror;
}

bool CinematicCamera::cutBetween(double from, double to) const {
    for (double c : cuts_)
        if (c > from && c <= to)
            return true;
    return false;
}

const CinematicCamera::Part& CinematicCamera::partAt(double t, float& ticks) const {
    size_t i = 0;
    while (i + 1 < parts_.size() && t >= parts_[i + 1].begin)
        ++i;
    ticks = float(std::clamp(t - parts_[i].begin, 0.0, parts_[i].anim->duration / 1000.0) * 1000.0);
    return parts_[i];
}

float CinematicCamera::channel(const Animation& a, const char* target, const char* name, size_t component,
                               float ticks, float fallback) const {
    const Animation::Target* t = a.findTarget(target);
    if (!t)
        return fallback;
    for (const auto& c : t->channels)
        if (strcasecmp(c.name.c_str(), name) == 0 && c.components.size() > component &&
            !c.components[component].keys.empty())
            return c.components[component].sample(ticks, c.duration());
    return fallback;
}

CinematicCamera::Shot CinematicCamera::shot(double now) const {
    Shot s;
    float ticks = 0;
    const Part& part = partAt(std::clamp(now - start_, 0.0, duration()), ticks);
    const Animation& a = *part.anim;
    Xform local;
    if (const Animation::Target* t = a.findTarget("cinecam"))
        local = samplePose(*t, skeleton_, ticks)[0];
    if (mirror_) { // reflect across the origin's YZ plane: x -> -x
        local.t[0] = -local.t[0];
        local.q = {local.q.x, -local.q.y, -local.q.z, local.q.w};
    }
    const Xform world = origin_.then(local);
    const Vec3 f = world.rotate({0, 1, 0}), u = world.rotate({0, 0, 1});
    s.eye = simd_make_float3(world.t[0], world.t[1], world.t[2]);
    s.forward = simd_normalize(simd_make_float3(f[0], f[1], f[2]));
    s.up = simd_normalize(simd_make_float3(u[0], u[1], u[2]));
    s.fovY = channel(a, "viewersGroup", "fov", 0, ticks, 1.0f);
    s.letterbox = std::clamp(channel(a, "postprocessingmaterial", "cameraletterbox", 0, ticks, 0.0f), 0.0f, 0.5f);
    s.grain = std::max(0.0f, channel(a, "postprocessingmaterial", "camerafilmgrain", 0, ticks, 0.0f));
    for (size_t k = 0; k < 3; ++k)
        s.vignette[k] = channel(a, "postprocessingmaterial", "cameraVignette", k, ticks, s.vignette[k]);
    for (size_t k = 0; k < 4; ++k)
        s.bloom[k] = channel(a, "postprocessingmaterial", "cameraBloom", k, ticks, s.bloom[k]);
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
    // The scene graph viewer's convention (docs/camera-ui-mac.md 1.1): the
    // field of view is the horizontal angle (at 4:3; wider keeps its height).
    const simd_float2 sc = fovScales(shot(now).fovY, aspect);
    const float xs = sc.x, ys = sc.y;
    const float zs = fa / (n - fa);
    return simd_matrix(simd_make_float4(xs, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                       simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
}

} // namespace os2
