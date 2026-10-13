#include "sim_censor.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "sim_actor.hpp"

namespace os2 {

namespace {

bool empty(const CensorBox& b) { return b.lo[0] > b.hi[0] || b.lo[1] > b.hi[1] || b.lo[2] > b.hi[2]; }

CensorBox none() { return {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}}; }

void unite(CensorBox& into, const CensorBox& b) {
    for (int k = 0; k < 3; ++k) {
        into.lo[k] = std::min(into.lo[k], b.lo[k]);
        into.hi[k] = std::max(into.hi[k], b.hi[k]);
    }
}

// The box about its centre (moved up by `up`) with half extents `scale` x its size.
CensorBox rescaled(const CensorBox& b, Vec3 scale, float up) {
    CensorBox r;
    for (int k = 0; k < 3; ++k) {
        const float c = (b.lo[k] + b.hi[k]) / 2 + (k == 2 ? up : 0), h = (b.hi[k] - b.lo[k]) * scale[k];
        r.lo[k] = c - h, r.hi[k] = c + h;
    }
    return r;
}

bool isHair(const ObjectModel::Part& p) {
    std::string s = p.shape;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s.find("hair") != std::string::npos;
}

} // namespace

CensorBox boneGeometryBox(const SimActor& actor, const char* bone) {
    CensorBox box = none();
    const int want = actor.skin.skeleton.find(bone);
    if (want < 0)
        return box;
    const size_t n = std::min(actor.skin.model.parts.size(), actor.posed.parts.size());
    for (size_t p = 0; p < n; ++p) {
        const auto& rest = actor.skin.model.parts[p];
        const auto& posed = actor.posed.parts[p].mesh;
        if (isHair(rest) || p >= actor.skin.jointMaps.size())
            continue;
        const auto& joints = actor.skin.jointMaps[p];
        const auto& mesh = rest.mesh;
        const size_t count = std::min({mesh.bones.size(), mesh.weights.size(), posed.positions.size()});
        for (size_t v = 0; v < count; ++v) {
            int best = 0;
            for (int k = 1; k < 4; ++k)
                if (mesh.bones[v][size_t(k)] != 0xFF && mesh.weights[v][size_t(k)] > mesh.weights[v][size_t(best)])
                    best = k;
            const uint8_t id = mesh.bones[v][size_t(best)];
            if (id == 0xFF || id >= joints.size() || joints[id] != want)
                continue;
            const auto& q = posed.positions[v];
            for (int k = 0; k < 3; ++k) {
                box.lo[size_t(k)] = std::min(box.lo[size_t(k)], q[size_t(k)]);
                box.hi[size_t(k)] = std::max(box.hi[size_t(k)], q[size_t(k)]);
            }
        }
    }
    return box;
}

std::vector<CensorBox> censorBoxes(const SimActor& actor, uint16_t flags, bool female) {
    using namespace censor;
    std::vector<CensorBox> out;
    if (!flags)
        return out;
    const bool full = flags & FullBody;
    CensorBox torso = none(), whole = none();
    // The pelvis (UpdateCensor: half extents 0.8, 0.8, 0.5 of its size on a man).
    if (flags & (Pelvis | FullBody)) {
        CensorBox b = boneGeometryBox(actor, "pelvis");
        if (!empty(b)) {
            if (!female)
                b = rescaled(b, {0.8f, 0.8f, 0.5f}, 0);
            unite(full ? whole : torso, b);
        }
    }
    // The chest of a woman: 0.7 of its height, 0.05 up (the breasts are
    // bound to spine1 in the af/tf/ef naked tops; the binary names the bone
    // by a hash we did not resolve: inferred).
    if ((flags & SpineIfFemale && female) || full) {
        CensorBox b = boneGeometryBox(actor, "spine1");
        if (!empty(b)) {
            if (female)
                b = rescaled(b, {0.5f, 0.5f, 0.35f}, 0.05f);
            unite(full ? whole : torso, b);
        }
    }
    if (!full && !empty(torso))
        out.push_back(torso);
    static constexpr struct {
        uint16_t flag;
        const char* bone;
    } kParts[] = {{Head, "head"}, {LeftHand, "l_hand"}, {RightHand, "r_hand"}, {LeftFoot, "l_foot"}, {RightFoot, "r_foot"}};
    for (const auto& part : kParts) {
        if (!(flags & part.flag) && !full)
            continue;
        const CensorBox b = boneGeometryBox(actor, part.bone);
        if (empty(b))
            continue;
        if (full)
            unite(whole, b);
        else
            out.push_back(b);
    }
    if (full && !empty(whole))
        out.push_back(whole);
    return out;
}

} // namespace os2
