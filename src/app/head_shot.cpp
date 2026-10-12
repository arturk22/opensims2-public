#include "app/head_shot.hpp"

#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

namespace os2 {

namespace {

// The loop of the age's skeleton the Sim stands in (CAS's pose first).
const Animation* idleOf(SimActor& actor, ResourceIndex& index, char age) {
    std::vector<const char*> names;
    switch (age) {
    case 'c': names = {"c-UIpose-M0", "c-idle-neutral-armsSide-wtC-breatheLoop50"}; break;
    case 'p': names = {"p-UIpose-m0", "p-idle-neutral-breatheLoop-CAS", "p-idle-neutral-lookAround"}; break;
    case 'b': names = {"b-floor-idle-loopBreathe"}; break;
    default: names = {"a-UIpose-M0", "a-idle-neutral-armsSide-wtC-breatheLoop"}; break;
    }
    for (const char* n : names)
        try {
            return &actor.animation(index, n);
        } catch (const std::exception&) {
        }
    return nullptr;
}

} // namespace

std::optional<RgbaImage> renderHeadShot(Renderer& renderer, ResourceIndex& index, const std::vector<SimPart>& parts,
                                        const std::filesystem::path& characterFile, uint32_t size) {
    try {
        SimActor actor(loadCharacterSim(index, parts, characterFile, 1));
        const char age = char(std::tolower(static_cast<unsigned char>(
            actor.skin.skeleton.name.empty() ? 'a' : actor.skin.skeleton.name[0])));
        if (const Animation* idle = idleOf(actor, index, age))
            actor.cue(0, *idle);
        const simd_float3 at = simd_make_float3(-5000, -5000, 0); // outside the terrain
        actor.position = {at.x, at.y, at.z};
        actor.heading = 0;
        actor.update(0.4);
        const int model = renderer.addObject(actor.skin.model, index, true);
        const int inst = renderer.addInstance(model, matrix_identity_float4x4);
        renderer.updateObject(model, actor.posed);
        renderer.setInstanceLightSet(inst, 0);
        const float h = age == 'b' ? 0.25f : age == 'p' ? 0.40f : age == 'c' ? 1.17f : 1.64f;
        const float closeness = 0.8f; // a little of the shoulders, as the game's own head shots
        const float dist = (age == 'b' || age == 'p' ? 0.8f : 1.0f) * closeness;
        const float aim = h + (1 - closeness) * 0.3f;
        const simd_float3 e = at + simd_make_float3(0, dist, aim + 0.03f), target = at + simd_make_float3(0, 0, aim);
        const simd_float3 f = simd_normalize(target - e);
        const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
        const simd_float3 u = simd_cross(sv, f);
        FrameView fv;
        fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                              simd_make_float4(sv.z, u.z, -f.z, 0),
                              simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
        const float n = 0.05f, fa = 50, ys = 1.0f / std::tan(15.0f * 3.14159265f / 180.0f), zs = fa / (n - fa);
        fv.projection = simd_matrix(simd_make_float4(ys, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                    simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
        // The frame's UI overlay must not be in the shot.
        const Renderer::Overlay overlay = renderer.overlay();
        renderer.setOverlay({});
        RgbaImage out = renderer.capture(size, size, fv, simd_make_float4(0, 0, 0, 0));
        renderer.setOverlay(overlay);
        // The neighborhood's sun leaves a face looking along the view in shade: lift it
        // (inferred; the game's thumbnail factory has its own lights).
        for (size_t i = 0; i + 3 < out.pixels.size(); i += 4)
            for (size_t c = 0; c < 3; ++c)
                out.pixels[i + c] = uint8_t(std::min(255.0f, 255.0f * std::pow(out.pixels[i + c] / 255.0f, 0.62f) * 1.05f));
        if (const char* dbg = std::getenv("OS2_HEAD_DEBUG")) { // test aid: where the shot is saved
            const auto png = encodePng(out);
            if (FILE* f = std::fopen(dbg, "wb")) {
                std::fwrite(png.data(), 1, png.size(), f);
                std::fclose(f);
            }
        }
        ObjectModel empty; // the instance stays, showing nothing
        renderer.setInstanceModel(inst, renderer.addObject(empty, index, true));
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

RgbaImage renderModelThumbnail(Renderer& renderer, int model, const std::array<float, 6>& b, uint32_t size) {
    const simd_float3 at = simd_make_float3(-5000, -5000, 0);
    const int inst = renderer.addInstance(model, matrix_identity_float4x4);
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(at.x, at.y, at.z, 1);
    renderer.setInstanceTransform(inst, m);
    const simd_float3 c = at + simd_make_float3((b[0] + b[3]) / 2, (b[1] + b[4]) / 2, (b[2] + b[5]) / 2);
    const float radius = std::max(0.5f, 0.5f * std::sqrt((b[3] - b[0]) * (b[3] - b[0]) + (b[4] - b[1]) * (b[4] - b[1]) +
                                                         (b[5] - b[2]) * (b[5] - b[2])));
    const simd_float3 f = simd_normalize(simd_make_float3(-0.6f, -0.6f, -0.5f)); // looking down from the south-east... (+x, +y side)
    const float fov = 30.0f * 3.14159265f / 180.0f;
    const float dist = radius / std::sin(fov / 2) * 1.05f;
    const simd_float3 e = c - f * dist;
    const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
    const simd_float3 u = simd_cross(sv, f);
    FrameView fv;
    fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                          simd_make_float4(sv.z, u.z, -f.z, 0),
                          simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
    const float n = 0.5f, fa = dist * 4 + 100, ys = 1.0f / std::tan(fov / 2), zs = fa / (n - fa);
    fv.projection = simd_matrix(simd_make_float4(ys, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
    const Renderer::Overlay overlay = renderer.overlay();
    renderer.setOverlay({});
    RgbaImage out = renderer.capture(size, size, fv, simd_make_float4(0, 0, 0, 0));
    renderer.setOverlay(overlay);
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[0].x = away.columns[1].y = away.columns[2].z = 0;
    renderer.setInstanceTransform(inst, away); // hidden again (instances cannot be removed)
    return out;
}

} // namespace os2
