// s2pkg commands for skeletons, animations and Sims.
#include "anim_cmds.hpp"

#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933;

std::string withSuffix(std::string s, const char* suffix) {
    if (!s.ends_with(suffix))
        s += suffix;
    return s;
}

// Largest deviation of world(rest) ∘ bind from the identity, over a few test points.
float bindError(const Xform& world, const Xform& bind) {
    const Xform m = world.then(bind);
    float err = 0;
    for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
        const Vec3 q = m.apply(p);
        err = std::max({err, std::fabs(q[0] - p[0]), std::fabs(q[1] - p[1]), std::fabs(q[2] - p[2])});
    }
    return err;
}

int cmdSkeleton(const char* res, const std::string& cresName) {
    ResourceIndex index(res);
    const ObjectModel model = loadObjectModel(index, cresName);
    const Skeleton& sk = model.skeleton;
    const auto world = sk.world(sk.restPose());
    std::printf("%s: %zu bones\n", sk.name.c_str(), sk.bones.size());
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        const auto& b = sk.bones[i];
        int depth = 0;
        for (int p = b.parent; p >= 0; p = sk.bones[size_t(p)].parent)
            ++depth;
        char joint[16] = "-";
        if (b.joint >= 0)
            std::snprintf(joint, sizeof joint, "%d", b.joint);
        std::printf("%3zu %*s%-*s joint=%-3s local t=(%.4f %.4f %.4f) q=(%.4f %.4f %.4f %.4f)  world t=(%.4f %.4f %.4f)\n",
                    i, depth * 2, "", 28 - depth * 2, b.name.c_str(), joint, b.rest.t[0], b.rest.t[1], b.rest.t[2],
                    b.rest.q.x, b.rest.q.y, b.rest.q.z, b.rest.q.w, world[i].t[0], world[i].t[1], world[i].t[2]);
    }
    // Skinned parts: bone ids used, their bones, and how well the GMDC bind
    // poses match the skeleton's rest pose.
    for (const auto& part : model.parts) {
        if (part.mesh.bones.empty())
            continue;
        std::vector<bool> used(part.bindPoses.size(), false);
        for (const auto& v : part.mesh.bones)
            for (auto b : v)
                if (b != 0xFF && b < used.size())
                    used[b] = true;
        size_t count = 0, missing = 0;
        float worst = 0;
        std::string worstBone;
        for (size_t j = 0; j < used.size(); ++j) {
            if (!used[j])
                continue;
            ++count;
            const int bone = sk.findJoint(int(j));
            if (bone < 0) {
                ++missing;
                continue;
            }
            const float e = bindError(world[size_t(bone)], part.bindPoses[j]);
            if (e > worst)
                worst = e, worstBone = sk.bones[size_t(bone)].name;
        }
        std::printf("part %s/%s: %zu vertices, %zu bones used of %zu bind poses, %zu without a joint, "
                    "max bind error %.2g (%s)\n",
                    part.shape.c_str(), part.mesh.name.c_str(), part.mesh.positions.size(), count,
                    part.bindPoses.size(), missing, worst, worstBone.c_str());
    }
    return 0;
}

} // namespace

int animCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "skeleton" && argc == 4)
        return cmdSkeleton(argv[2], withSuffix(argv[3], "_cres"));
    (void)kTypeCres;
    return -1;
}

const char* animUsage() {
    return "       s2pkg skeleton <Res-dir> <cres-name>\n";
}

} // namespace os2
