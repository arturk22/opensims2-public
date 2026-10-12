#include "cas_studio.hpp"

#include "app/pie_menu.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/scenegraph.hpp"
#include "formats/skeleton.hpp"
#include "formats/xform.hpp"
#include "render/renderer.hpp"
#include "vm/lot.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

simd_float3 f3(const Vec3& v) { return simd_make_float3(v[0], v[1], v[2]); }

} // namespace

CasStudio::CasStudio(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir)
    : renderer_(renderer), index_(index), resDir_(resDir) {}

CasStudio::~CasStudio() = default;

void CasStudio::readMarks(const std::string& cres, const Xform& at) {
    // The model's skeleton as loadObjectModel builds it (the bare CRES's
    // misses the platform joints' offsets).
    const Skeleton sk = loadObjectModel(index_, cres).skeleton;
    const auto world = sk.world(sk.restPose());
    std::map<int, Mark> family, babies;
    for (size_t b = 0; b < sk.bones.size(); ++b) {
        const std::string n = lower(sk.bones[b].name);
        const Xform w = at.then(world[b]);
        const simd_float3 p = f3(w.t);
        const Vec3 f = w.rotate({0, 1, 0});
        Mark m{p, simd_normalize(simd_make_float2(f[0], f[1]))};
        if (n == "slot_casstudio_simmark_podium") {
            podium_ = m;
            podiumFound_ = true;
        } else if (n.starts_with("slot_casstudio_simmark_baby_")) {
            babies[std::atoi(n.c_str() + 28)] = m;
        } else if (n.starts_with("slot_casstudio_simmark_")) {
            family[std::atoi(n.c_str() + 23)] = m;
        } else if (n.starts_with("camera_")) {
            cameras_[n] = {p, simd_quaternion(float(w.q.x), float(w.q.y), float(w.q.z), float(w.q.w))};
        }
    }
    if (!family.empty()) {
        family_.clear();
        for (auto& [k, m] : family)
            family_.push_back(m);
    }
    if (!babies.empty()) {
        babies_.clear();
        for (auto& [k, m] : babies)
            babies_.push_back(m);
    }
}

bool CasStudio::load(simd_float2 viewDirection) {
    const auto file = resDir_ / "GlobalLots/CAS!.package";
    if (!std::filesystem::exists(file))
        return false;
    try {
        config_ = loadLightingConfig(resDir_ / "Lights/Lighting.txt");
        // The lamps' attributes: the lots' file, then the CAS files (later wins).
        for (const char* f : {"LotLights.txt", "CAS_lights.txt", "CAS_lighting.txt"}) {
            const auto path = resDir_ / "Lights" / f;
            if (!std::filesystem::exists(path))
                continue;
            for (auto& [light, attrs] : loadLightOverrides(path))
                for (auto& [k, v] : attrs)
                    overrides_[light][k] = v;
        }

        DbpfPackage pkg(file);
        Lot lot = Lot::load(pkg);
        vm::dropLeftoverCObjects(lot, pkg, index_);
        // The area models' marks first: the walls are cut away towards the
        // camera, which faces the podium's Sim.
        for (const auto& o : lot.objects) {
            const std::string def = lower(o.definition);
            if (!def.starts_with("cas - podium") && !def.starts_with("cas - backdrop"))
                continue;
            readMarks(o.model, Xform{Quat{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]},
                                     {o.position[0], o.position[1], o.position[2]}});
        }
        if (podiumFound_)
            viewDirection = -podium_.facing;
        const PatternCatalog patterns(index_, resDir_);
        LotViewOptions view;
        view.walls = LotViewOptions::Walls::Cutaway;
        view.viewDirection = {viewDirection.x, viewDirection.y};
        LotLightMapLayout layout;
        const ObjectModel model = buildLotModel(lot, index_, patterns, view, &layout);
        renderer_.addInstance(renderer_.addObject(model, index_), matrix_identity_float4x4);

        std::map<std::string, int> models;
        for (const auto& o : lot.objects) {
            const std::string def = lower(o.definition);
            const Xform at{Quat{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]},
                           {o.position[0], o.position[1], o.position[2]}};
            if (o.model.empty() || o.model == "nullModel" || o.model == "noModel" || o.person)
                continue;
            if (o.position[0] == 0 && o.position[1] == 0)
                continue; // in a slot
            if (std::getenv("OS2_CAS_DEBUG")) {
                const Xform a{Quat{o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]}, {0, 0, 0}};
                const Vec3 fx = a.rotate({1, 0, 0});
                std::fprintf(stderr, "cas studio: object %s (%s) at (%.2f %.2f %.2f) +X->(%.2f %.2f)\n", o.model.c_str(),
                             o.definition.c_str(), o.position[0], o.position[1], o.position[2], fx[0], fx[1]);
            }
            std::string key = o.model;
            for (const auto& [subset, material] : o.materials)
                key += "|" + subset + "=" + material;
            auto it = models.find(key);
            if (it == models.end()) {
                int id = -1;
                try {
                    const ObjectModel m = loadObjectModel(index_, o.model, o.materials);
                    id = renderer_.addObject(m, index_);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: cas studio: %s: %s\n", o.model.c_str(), e.what());
                }
                it = models.emplace(key, id).first;
            }
            if (it->second < 0)
                continue;
            simd_float4x4 m = simd_matrix4x4(simd_quaternion(o.rotation[0], o.rotation[1], o.rotation[2], o.rotation[3]));
            m.columns[3] = simd_make_float4(o.position[0], o.position[1], o.position[2], 1);
            renderer_.addInstance(it->second, m);

            // The model's lamps (cLightRefNode -> LGHT), in lot space.
            try {
                auto loc = index_.findByName(o.model + "_cres", kTypeCres);
                if (!loc)
                    continue;
                const auto bytes = index_.read(*loc);
                const Rcol rc(bytes);
                std::map<uint32_t, size_t> parent;
                for (size_t i = 0; i < rc.blocks.size(); ++i)
                    if (rc.blocks[i].className == "cTransformNode")
                        for (const auto& c : TransformNode::parse(rc.blocks[i].body).children)
                            parent[c.index] = i;
                const Skeleton sk = loadObjectModel(index_, o.model).skeleton; // as readMarks
                const auto world = sk.world(sk.restPose());
                for (size_t i = 0; i < rc.blocks.size(); ++i) {
                    if (rc.blocks[i].className != "cLightRefNode")
                        continue;
                    const LightRefNode node = LightRefNode::parse(rc.blocks[i].body);
                    if (node.lightLink < 0 || size_t(node.lightLink) >= rc.links.size())
                        continue;
                    const auto lb = index_.read(rc.links[size_t(node.lightLink)]);
                    if (!lb)
                        continue;
                    const Rcol lr(*lb);
                    for (const auto& blk : lr.blocks) {
                        if (blk.className != "cPointLight" && blk.className != "cSpotLight")
                            continue;
                        const StandardLight l = StandardLight::parse(blk.className, blk.body);
                        if (l.values.size() < 6)
                            continue;
                        Lamp lamp;
                        lamp.name = lower(l.name.empty() ? node.renderable.transform.name() : l.name);
                        float intensity = l.values[0];
                        Vec3 color{l.values[2], l.values[3], l.values[4]};
                        lamp.falloff = l.values[5];
                        float dIntensity = -1;
                        Vec3 dColor{-1, -1, -1};
                        if (auto ov = overrides_.find(lamp.name); ov != overrides_.end()) {
                            auto one = [&](const char* n, float& v) {
                                if (auto f = ov->second.find(n); f != ov->second.end() && !f->second.empty())
                                    v = f->second[0];
                            };
                            auto three = [&](const char* n, Vec3& v) {
                                if (auto f = ov->second.find(n); f != ov->second.end() && f->second.size() >= 3)
                                    v = {f->second[0], f->second[1], f->second[2]};
                            };
                            one("intensity", intensity);
                            one("falloffrate", lamp.falloff);
                            three("color", color);
                            one("diffusionsourceintensity", dIntensity);
                            three("diffusionsourcecolor", dColor);
                            one("diffusionsourcefalloff", lamp.diffusionFalloff);
                            one("diffusionsourcefloorintensityscale", lamp.floorScale);
                            one("diffusionsourcewallintensityscale", lamp.wallScale);
                        }
                        if (dIntensity < 0)
                            dIntensity = intensity;
                        if (dColor[0] < 0)
                            dColor = color;
                        const auto& t = node.renderable.transform;
                        Xform lx{t.rotation, t.translation};
                        if (auto p = parent.find(uint32_t(i)); p != parent.end())
                            for (size_t b = 0; b < sk.bones.size(); ++b)
                                if (sk.bones[b].block == p->second)
                                    lx = world[b].then(lx);
                        const Vec3 wp = at.then(lx).t;
                        lamp.position = f3(wp);
                        lamp.color = f3(color) * intensity;
                        lamp.diffusionColor = f3(dColor) * dIntensity;
                        lamp.level = int(std::floor(o.position[2] / 3.0f)); // inferred: 3 units a level
                        lamps_.push_back(lamp);
                    }
                }
            } catch (const std::exception&) {
            }
        }

        // The light maps for the CAS state and the lamps.
        if (const LightingState* state = config_.state("CAS")) {
            std::vector<LightMapLight> lights;
            for (const auto& l : lamps_) {
                if (simd_reduce_max(l.diffusionColor) <= 0)
                    continue;
                LightMapLight m;
                m.position = {l.position.x, l.position.y, l.position.z};
                m.color = {l.diffusionColor.x, l.diffusionColor.y, l.diffusionColor.z};
                m.falloff = l.diffusionFalloff;
                m.floorScale = l.floorScale;
                m.wallScale = l.wallScale;
                m.level = 0; // the studio is one storey (lot level 1, light-map level 0; inferred)
                lights.push_back(m);
            }
            const auto lm = computeLotLightMap(layout, *state, lights, LightMapGlobals::from(config_));
            renderer_.setLightMap(0, lm.image);
            renderer_.setLightMap(1, lm.image);
        }
        loaded_ = true;
        if (std::getenv("OS2_CAS_DEBUG")) {
            std::fprintf(stderr, "cas studio: podium (%.2f %.2f %.2f) facing (%.2f %.2f), %zu family marks, %zu cameras\n",
                         podium_.position.x, podium_.position.y, podium_.position.z, podium_.facing.x, podium_.facing.y,
                         family_.size(), cameras_.size());
            for (const auto& m : family_)
                std::fprintf(stderr, "  family mark (%.2f %.2f) facing (%.2f %.2f)\n", m.position.x, m.position.y,
                             m.facing.x, m.facing.y);
            for (const auto& [n, c] : cameras_) {
                const simd_float3 x = simd_act(c.orientation, simd_make_float3(1, 0, 0)),
                                  y = simd_act(c.orientation, simd_make_float3(0, 1, 0)),
                                  z = simd_act(c.orientation, simd_make_float3(0, 0, 1));
                std::fprintf(stderr, "  camera %s (%.2f %.2f %.2f) x (%.2f %.2f %.2f) y (%.2f %.2f %.2f) z (%.2f %.2f %.2f)\n",
                             n.c_str(), c.position.x, c.position.y, c.position.z, x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z);
            }
            for (const auto& l : lamps_)
                std::fprintf(stderr, "  lamp %s at (%.2f %.2f %.2f) colour (%.2f %.2f %.2f) falloff %.2f diffusion %.2f\n",
                             l.name.c_str(), l.position.x, l.position.y, l.position.z, l.color.x, l.color.y, l.color.z,
                             l.falloff, simd_reduce_max(l.diffusionColor));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: cas studio: %s\n", e.what());
        return false;
    }
    return loaded_;
}

void CasStudio::applyLights(simd_float3 spot, bool family) {
    const LightingState* state = config_.state("CAS");
    if (!state)
        return;
    Renderer::LightSet objects = Renderer::outdoorLights(*state, false);
    Renderer::LightSet sims = Renderer::outdoorLights(*state, true);
    // The lamps near the Sim light it from where they are.
    const simd_float3 centre = spot + simd_make_float3(0, 0, 1.0f);
    std::vector<std::pair<float, Renderer::LightSet::Directional>> dirs;
    // The area's lamps ("CASPodiumArea_*" for the podium, "CASFamilyArea_*"
    // and "CAS_FamilyArea*" for the family), not those for objects only.
    for (const auto& l : lamps_) {
        if (simd_reduce_max(l.color) <= 0.01f || l.name.find("objectsonly") != std::string::npos)
            continue;
        const bool podiumLamp = l.name.starts_with("caspodiumarea");
        const bool familyLamp = l.name.starts_with("casfamilyarea") || l.name.starts_with("cas_familyarea");
        if (family ? !familyLamp : !podiumLamp)
            continue;
        const simd_float3 d = l.position - centre;
        const float dist = simd_length(d);
        if (dist < 0.05f)
            continue;
        const float att = 1.0f / (1.0f + l.falloff * dist); // inferred: linear (the d^2 of the lamps leaves the podium dark)
        Renderer::LightSet::Directional dl;
        dl.direction = d / dist;
        dl.color = l.color * att;
        dirs.push_back({simd_reduce_max(dl.color), dl});
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (!dirs.empty()) {
        sims.lights.clear(); // the studio's lamps instead of the state's sun and fills
        for (size_t i = 0; i < dirs.size() && i < 6; ++i)
            sims.lights.push_back(dirs[i].second);
    }
    for (const auto& l : lamps_) {
        if (simd_reduce_max(l.color) <= 0.01f || objects.points.size() >= 8)
            continue;
        Renderer::LightSet::Point p;
        p.position = l.position;
        p.color = l.color;
        p.falloff = std::max(0.05f, l.falloff);
        p.range = 10;
        objects.points.push_back(p);
    }
    renderer_.setLightSets({objects, sims});
}

} // namespace os2
