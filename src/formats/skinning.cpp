#include "skinning.hpp"

#include <cmath>

namespace os2 {

void SkinnedModel::add(const ObjectModel& part) {
    const std::vector<int> joints = mapJoints(part.skeleton, skeleton);
    for (const auto& p : part.parts) {
        model.parts.push_back(p);
        jointMaps.push_back(joints);
    }
    for (const auto& [name, m] : part.materials)
        model.materials.emplace(name, m);
    for (const auto& w : part.warnings)
        model.warnings.push_back(part.name + ": " + w);
    if (model.name.empty())
        model.name = part.name;
}

void SkinnedModel::pose(const std::vector<Xform>& world, ObjectModel& out,
                        const std::map<std::string, float>& morphs) const {
    if (out.parts.size() != model.parts.size()) {
        out = model;
        out.skeleton = {};
    }
    std::vector<Affine> palette;
    for (size_t pi = 0; pi < model.parts.size(); ++pi) {
        const auto& src = model.parts[pi];
        auto& dst = out.parts[pi].mesh;
        const auto& mesh = src.mesh;
        if (mesh.bones.empty())
            continue;
        // Morph weights by morph number; positions with the morphs applied.
        std::vector<float> weights;
        if (!morphs.empty() && !mesh.morphMap.empty())
            for (const auto& name : mesh.morphNames) {
                auto it = morphs.find(name);
                weights.push_back(it == morphs.end() ? 0.0f : it->second);
            }
        auto rest = [&](size_t v) {
            Vec3 p = mesh.positions[v];
            if (weights.empty() || v >= mesh.morphMap.size())
                return p;
            for (size_t k = 0; k < 4; ++k) {
                const auto& d = mesh.morphDeltas[k];
                const uint32_t morph = (mesh.morphMap[v] >> (8 * k)) & 0xFF;
                if (v < d.size() && morph < weights.size() && weights[morph] != 0)
                    for (int c = 0; c < 3; ++c)
                        p[size_t(c)] += weights[morph] * d[v][size_t(c)];
            }
            return p;
        };
        // Bone id -> model-space matrix: rest model space -> bone -> posed model space.
        const auto& map = jointMaps[pi];
        palette.assign(src.bindPoses.size(), Affine{});
        for (size_t j = 0; j < src.bindPoses.size(); ++j)
            if (j < map.size() && map[j] >= 0 && size_t(map[j]) < world.size())
                palette[j] = Affine::from(world[size_t(map[j])].then(src.bindPoses[j]));
        for (size_t v = 0; v < mesh.positions.size(); ++v) {
            const Vec3 base = rest(v);
            Vec3 p{0, 0, 0}, n{0, 0, 0};
            float total = 0;
            for (int s = 0; s < 4; ++s) {
                const uint8_t b = mesh.bones[v][size_t(s)];
                const float w = mesh.weights[v][size_t(s)];
                if (b == 0xFF || w <= 0 || b >= palette.size())
                    continue;
                const Vec3 pp = palette[b].point(base);
                for (int k = 0; k < 3; ++k)
                    p[size_t(k)] += w * pp[size_t(k)];
                if (v < mesh.normals.size()) {
                    const Vec3 nn = palette[b].vector(mesh.normals[v]);
                    for (int k = 0; k < 3; ++k)
                        n[size_t(k)] += w * nn[size_t(k)];
                }
                total += w;
            }
            if (total <= 0)
                continue; // unbound vertex: leave it in the rest pose
            for (int k = 0; k < 3; ++k)
                p[size_t(k)] /= total;
            dst.positions[v] = p;
            if (v < mesh.normals.size()) {
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (len > 0)
                    dst.normals[v] = {n[0] / len, n[1] / len, n[2] / len};
            }
        }
    }
}

} // namespace os2
