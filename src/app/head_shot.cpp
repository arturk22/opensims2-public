#include "app/head_shot.hpp"

#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "render/renderer.hpp"

#include <cctype>
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
        const float h = age == 'b' ? 0.25f : age == 'p' ? 0.40f : age == 'c' ? 1.17f : 1.64f;
        const float closeness = 0.62f;
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
        RgbaImage out = renderer.capture(size, size, fv, simd_make_float4(0, 0, 0, 0));
        ObjectModel empty; // the instance stays, showing nothing
        renderer.setInstanceModel(inst, renderer.addObject(empty, index, true));
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace os2
