#include "app/head_shot.hpp"
#include "formats/log_once.hpp"

#include "formats/model.hpp"
#include "formats/sim_actor.hpp"
#include "formats/dbpf.hpp"
#include "formats/lighting_config.hpp"
#include "formats/resindex.hpp"
#include "formats/scenegraph.hpp"
#include "formats/xform.hpp"
#include "formats/sim_character.hpp"
#ifdef OS2_SDL_GPU
#include "render/gpu_renderer.hpp"
#else
#include "render/renderer.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

namespace os2 {

namespace {

// The light rig "UIUCPHead" (Lights/LightRig.txt: resource and root
// "UIPieMenu"): the directional lights of UIPieMenu_cres (Sims3D/LightRigs.package),
// each turned by its node and the nodes above it, with Lights/LightRigs/PieMenu.txt's
// intensity and colour overrides. In the rig's own space.
struct RigLight {
    std::string name;
    Vec3 direction{0, 0, 1}; // the node's +Z (inferred: towards the light)
    simd_float3 color{1, 1, 1};
};
std::vector<RigLight> headLightRig(ResourceIndex& index, const LightOverrides& overrides) {
    std::vector<RigLight> out;
    auto loc = index.findByName("UIPieMenu_cres", 0xE519C933);
    if (!loc)
        return out;
    const auto bytes = index.read(*loc);
    const Rcol rc(bytes);
    std::map<uint32_t, size_t> parent;
    std::vector<Xform> local(rc.blocks.size());
    for (size_t i = 0; i < rc.blocks.size(); ++i) {
        const auto& b = rc.blocks[i];
        try {
            if (b.className == "cTransformNode") {
                const TransformNode t = TransformNode::parse(b.body);
                local[i] = {t.rotation, t.translation};
                for (const auto& c : t.children)
                    parent[c.index] = i;
            } else if (b.className == "cLightRefNode") {
                const LightRefNode l = LightRefNode::parse(b.body);
                local[i] = {l.renderable.transform.rotation, l.renderable.transform.translation};
                for (const auto& c : l.renderable.transform.children)
                    parent[c.index] = i;
            }
        } catch (const std::exception& err) {
            os2::logOnce("head shot light rig", b.className, err);
        }
    }
    for (size_t i = 0; i < rc.blocks.size(); ++i) {
        if (rc.blocks[i].className != "cLightRefNode")
            continue;
        LightRefNode node;
        try {
            node = LightRefNode::parse(rc.blocks[i].body);
        } catch (const std::exception& err) {
            os2::logOnce("head shot light rig", "light " + std::to_string(i), err);
            continue;
        }
        if (node.lightLink < 0 || size_t(node.lightLink) >= rc.links.size())
            continue;
        const auto lb = index.read(rc.links[size_t(node.lightLink)]);
        if (!lb)
            continue;
        const Rcol lr(*lb);
        for (const auto& blk : lr.blocks) {
            if (blk.className != "cDirectionalLight")
                continue;
            const StandardLight l = StandardLight::parse(blk.className, blk.body);
            if (l.values.size() < 5)
                continue;
            RigLight r;
            r.name = node.renderable.transform.name();
            for (auto& ch : r.name)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            float intensity = l.values[0];
            simd_float3 color = simd_make_float3(l.values[2], l.values[3], l.values[4]);
            if (auto ov = overrides.find(r.name); ov != overrides.end()) {
                if (auto f = ov->second.find("intensity"); f != ov->second.end() && !f->second.empty())
                    intensity = f->second[0];
                if (auto f = ov->second.find("color"); f != ov->second.end() && f->second.size() >= 3)
                    color = simd_make_float3(f->second[0], f->second[1], f->second[2]);
            }
            r.color = color * intensity;
            Xform w = local[i];
            for (size_t at = i; parent.count(uint32_t(at));) {
                at = parent[uint32_t(at)];
                w = local[at].then(w);
            }
            r.direction = w.rotate(Vec3{0, 0, 1});
            out.push_back(r);
        }
    }
    return out;
}

// cTSUIThumbnailFactorySimHead::SetupScene's pose (docs/neighborhood-mac.md
// 15.5): "<age>-UIpose-<M|F>NN" with a number drawn from 1 to N (here from
// the character file's name, so a Sim keeps its pose; the game draws it
// each time); the baby's are "b-UIpose-NN". The idle loop if none loads.
const Animation* poseOf(SimActor& actor, ResourceIndex& index, char age, bool female, const std::string& seed) {
    uint32_t h = 2166136261u;
    for (char ch : seed)
        h = (h ^ uint8_t(ch)) * 16777619u;
    std::vector<std::string> names;
    char buf[64];
    for (int k = 0; k < 9; ++k) {
        const int n = int((h + uint32_t(k)) % 8) + 1;
        if (age == 'b')
            std::snprintf(buf, sizeof buf, "b-UIpose-%02d", (n - 1) % 4 + 1);
        else
            std::snprintf(buf, sizeof buf, "%c-UIpose-%c%02d", age, female ? 'F' : (age == 'p' ? 'm' : 'M'), n);
        names.push_back(buf);
    }
    names.push_back(age == 'c' ? "c-idle-neutral-armsSide-wtC-breatheLoop50"
                    : age == 'p' ? "p-idle-neutral-breatheLoop-CAS"
                    : age == 'b' ? "b-floor-idle-loopBreathe"
                                 : "a-idle-neutral-armsSide-wtC-breatheLoop");
    for (const auto& n : names)
        try {
            return &actor.animation(index, n);
        } catch (const std::exception&) { // (the next one is tried)
        }
    os2::logOnce("head shot pose", std::string(1, age) + (female ? "F" : "M"), "none of the poses loads");
    return nullptr;
}

} // namespace

std::optional<RgbaImage> renderHeadShot(Renderer& renderer, ResourceIndex& index, const std::vector<SimPart>& parts,
                                        const std::filesystem::path& characterFile, uint32_t size,
                                        const std::filesystem::path& resDir) {
    try {
        static const std::vector<RigLight> rig = [&] {
            LightOverrides ov;
            if (!resDir.empty())
                ov = loadLightOverrides(resDir / "Lights/LightRigs/PieMenu.txt");
            auto r = headLightRig(index, ov);
            if (std::getenv("OS2_HEAD_DEBUG"))
                for (const auto& l : r)
                    std::fprintf(stderr, "head rig light %s dir (%.2f %.2f %.2f) colour (%.2f %.2f %.2f)\n", l.name.c_str(),
                                 l.direction[0], l.direction[1], l.direction[2], l.color.x, l.color.y, l.color.z);
            return r;
        }();
        SimActor actor(loadCharacterSim(index, parts, characterFile, 1));
        bool female = false;
        try {
            DbpfPackage pkg(characterFile);
            female = readCharacter(pkg, index).gender == 1;
        } catch (const std::exception& err) {
            os2::logOnce("head shot: gender", characterFile.filename().string(), err);
        }
        const char age = char(std::tolower(static_cast<unsigned char>(
            actor.skin.skeleton.name.empty() ? 'a' : actor.skin.skeleton.name[0])));
        if (const Animation* pose = poseOf(actor, index, age, female, characterFile.filename().string()))
            actor.cue(0, *pose);
        const simd_float3 at = simd_make_float3(-5000, -5000, 0); // outside the terrain
        actor.position = {at.x, at.y, at.z};
        actor.heading = 0;
        actor.update(0.4);
        const int model = renderer.addObject(actor.skin.model, index, true);
        const int inst = renderer.addInstance(model, matrix_identity_float4x4);
        renderer.updateObject(model, actor.posed);
        // The rig's lights in a light set of their own for the shot (the rig
        // space is the Sim's: +Y towards the camera, as the lights' names say).
        const std::vector<Renderer::LightSet> sceneSets = renderer.lightSets();
        int rigSet = 0;
        if (!rig.empty() && sceneSets.size() < Renderer::kMaxLightSets) {
            Renderer::LightSet set;
            set.ambient = simd_make_float3(0.08f, 0.08f, 0.08f); // inferred: the rig has no ambient light
            for (const auto& l : rig)
                set.lights.push_back({simd_normalize(simd_make_float3(l.direction[0], l.direction[1], l.direction[2])),
                                      l.color, 1});
            std::vector<Renderer::LightSet> sets = sceneSets;
            if (sets.empty())
                sets.push_back({});
            rigSet = int(sets.size());
            sets.push_back(set);
            renderer.setLightSets(std::move(sets));
        }
        renderer.setInstanceLightSet(inst, rigSet);
        // The box around the head bone: 0.30 wide and deep, from 0.10 under
        // the bone to 0.20 over it; the camera in front of the Sim (+Y, the
        // skeleton's forward), aimed at the box's centre, at the distance
        // that fits the box's larger side on screen in the field of view
        // (at least 0.01) plus twice the box's depth, near and far planes
        // 0.8 x the first and 1.2 x the whole (SetupScene, BV 0x392a88).
        // The field of view is the thumbnail renderer's camera's: 20 degrees
        // (cTSUIThumbnailFactorySceneBased::InitRenderer, BV 0xa819e0:
        // 20 x pi/180), x 1.0 for humans.
        simd_float3 head = at + simd_make_float3(0, 0, age == 'b' ? 0.2f : age == 'p' ? 0.35f : age == 'c' ? 1.05f : 1.55f);
        if (const int b = actor.skin.skeleton.find("head"); b >= 0 && size_t(b) < actor.bones.size()) {
            const auto& t = actor.bones[size_t(b)].t;
            simd_float3 p = simd_make_float3(t[0], t[1], t[2]);
            if (simd_length(p.xy - at.xy) > 50) // bones in the Sim's own space
                p += at;
            head = p;
        }
        const simd_float3 target = head + simd_make_float3(0, 0, 0.05f);
        const float fov = 20.0f * 3.14159265f / 180.0f;
        const float fit = std::max(0.01f, 0.30f / (2 * std::tan(fov * 0.5f)));
        const float dist = fit + 2 * 0.30f;
        const simd_float3 e = target + simd_make_float3(0, dist, 0);
        const simd_float3 f = simd_normalize(target - e);
        const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
        const simd_float3 u = simd_cross(sv, f);
        FrameView fv;
        fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                              simd_make_float4(sv.z, u.z, -f.z, 0),
                              simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
        const float n = fit * 0.8f, fa = dist * 1.2f, ys = 1.0f / std::tan(fov * 0.5f), zs = fa / (n - fa);
        fv.projection = simd_matrix(simd_make_float4(ys, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                    simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
        // The frame's UI overlay must not be in the shot.
        const Renderer::Overlay overlay = renderer.overlay();
        renderer.setOverlay({});
        RgbaImage out = renderer.capture(size, size, fv, simd_make_float4(0, 0, 0, 0));
        renderer.setOverlay(overlay);
        if (rigSet)
            renderer.setLightSets(sceneSets);
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
    } catch (const std::exception& err) {
        os2::logOnce("head shot", characterFile.filename().string(), err);
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

std::optional<RgbaImage> renderPartsThumbnail(Renderer& renderer, ResourceIndex& index,
                                              const std::vector<ThumbnailPart>& parts, uint32_t size) {
    ObjectModel all;
    std::array<float, 6> box{INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY};
    for (const ThumbnailPart& tp : parts) {
        ObjectModel m;
        try {
            m = loadObjectModel(index, tp.cres);
        } catch (const std::exception& err) {
            os2::logOnce("parts thumbnail", tp.cres, err);
            continue;
        }
        const int maskBase = int(all.masks.size());
        all.masks.insert(all.masks.end(), m.masks.begin(), m.masks.end());
        all.materials.insert(m.materials.begin(), m.materials.end());
        for (ObjectModel::Part part : m.parts) {
            if (!part.mesh.bones.empty())
                continue; // rigid models only
            for (auto& v : part.mesh.positions) {
                for (int k = 0; k < 3; ++k)
                    v[size_t(k)] += tp.offset[size_t(k)];
                box = {std::min(box[0], v[0]), std::min(box[1], v[1]), std::min(box[2], v[2]),
                       std::max(box[3], v[0]), std::max(box[4], v[1]), std::max(box[5], v[2])};
            }
            part.bone = -1;
            if (part.mask >= 0)
                part.mask += maskBase;
            all.parts.push_back(std::move(part));
        }
    }
    if (all.parts.empty() || !std::isfinite(box[0]))
        return std::nullopt;
    // Framed on the top of a tall model (the modular steps hang a story of
    // stringer under them): no more than 1.2 times its width high (inferred;
    // the factories use a fixed camera, not read).
    const float wide = std::max(box[3] - box[0], box[4] - box[1]);
    box[2] = std::max(box[2], box[5] - 1.2f * wide);
    all.name = parts.front().cres + "_thumbnail";
    const int model = renderer.addObject(all, index);
    return renderModelThumbnail(renderer, model, box, size);
}

} // namespace os2
