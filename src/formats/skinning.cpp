#include "skinning.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace os2 {

void SkinnedModel::add(const ObjectModel& part) {
    const std::vector<int> joints = mapJoints(part.skeleton, skeleton);
    if (restWorld.size() != skeleton.bones.size())
        restWorld = skeleton.world(skeleton.restPose());
    for (const auto& p : part.parts) {
        model.parts.push_back(p);
        jointMaps.push_back(joints);
        int bone = -1;
        if (p.mesh.bones.empty() && p.bone >= 0 && size_t(p.bone) < part.skeleton.bones.size())
            bone = skeleton.find(part.skeleton.bones[size_t(p.bone)].name);
        rigidBones.push_back(bone);
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
        if (mesh.bones.empty()) {
            // A rigid part follows the bone that carries it: rest model
            // space -> that bone at rest -> the bone posed.
            if (pi < rigidBones.size() && rigidBones[pi] >= 0 && size_t(rigidBones[pi]) < world.size() &&
                size_t(rigidBones[pi]) < restWorld.size()) {
                const Xform m = world[size_t(rigidBones[pi])].then(restWorld[size_t(rigidBones[pi])].inverse());
                for (size_t v = 0; v < mesh.positions.size() && v < dst.positions.size(); ++v)
                    dst.positions[v] = m.apply(mesh.positions[v]);
                for (size_t v = 0; v < mesh.normals.size() && v < dst.normals.size(); ++v)
                    dst.normals[v] = m.rotate(mesh.normals[v]);
            }
            continue;
        }
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

std::optional<std::vector<Xform>> savedPose(const Skeleton& skeleton,
                                            const std::vector<std::pair<std::string, Xform>>& nodes) {
    std::vector<Xform> pose = skeleton.restPose();
    bool moved = false;
    for (const auto& [name, x] : nodes) {
        const int b = skeleton.find(name);
        if (b < 0 || size_t(b) >= pose.size())
            continue;
        const Xform& r = pose[size_t(b)];
        const float dq = std::abs(std::abs(r.q.x * x.q.x + r.q.y * x.q.y + r.q.z * x.q.z + r.q.w * x.q.w) - 1);
        const float dt = std::abs(r.t[0] - x.t[0]) + std::abs(r.t[1] - x.t[1]) + std::abs(r.t[2] - x.t[2]);
        if (dt > 1e-3f || dq > 1e-3f)
            moved = true;
        pose[size_t(b)] = x;
    }
    if (!moved)
        return std::nullopt;
    return pose;
}

ObjectModel posedModel(const ObjectModel& model, const std::vector<Xform>& local) {
    SkinnedModel skin(model.skeleton);
    skin.add(model);
    ObjectModel out;
    skin.pose(skin.skeleton.world(local), out);
    ObjectModel result = model;
    for (size_t i = 0; i < result.parts.size() && i < out.parts.size(); ++i) {
        result.parts[i].mesh.positions = std::move(out.parts[i].mesh.positions);
        result.parts[i].mesh.normals = std::move(out.parts[i].mesh.normals);
    }
    return result;
}

std::string poseKey(const std::vector<Xform>& local) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over the values to a tenth of a millimetre
    auto mix = [&](float v) {
        const int64_t q = std::llround(double(v) * 10000);
        for (int k = 0; k < 8; ++k)
            h = (h ^ uint64_t((q >> (8 * k)) & 0xFF)) * 1099511628211ull;
    };
    for (const auto& x : local) {
        mix(x.q.x), mix(x.q.y), mix(x.q.z), mix(x.q.w);
        mix(x.t[0]), mix(x.t[1]), mix(x.t[2]);
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return buf;
}

} // namespace os2
