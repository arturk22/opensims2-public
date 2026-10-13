#pragma once

#include "rcol.hpp"
#include "xform.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// Bone hierarchy of a CRES: its cTransformNode tree (e.g. auskel_cres, or the
// copy of the skeleton every body/hair/face CRES carries). Bones are named by
// the node's name ("root_trans", "spine0", "l_hand_ikctr", ...); the rest pose
// is each node's local transform. Joint ids (cTransformNode::jointReference)
// are the bone numbers GMDC meshes of the same CRES use.
struct Skeleton {
    struct Bone {
        std::string name;
        int parent = -1;  // bone index, parents come before their children
        Xform rest;       // local transform in the rest (bind) pose
        int joint = -1;   // jointReference, -1 if none
        size_t block = 0; // CRES block index
        // The joint orientation of the node's cBoneDataExtension (identity
        // if none): an animation's rotation key is turned by it, local =
        // orient * key. Only the clavicles (a 14 degree twist on auskel,
        // 10 on cuskel), the thumbs' base (45) and the baby's neck carry one.
        Quat orient;
    };

    std::string name; // CRES resource name
    std::vector<Bone> bones;

    // `withShapes`: the cShapeRefNodes (the nodes that carry meshes) become
    // bones too (no joint), so that object animations that turn them (a
    // door's "r_door_rot") move their rigid meshes.
    static Skeleton fromCres(const Rcol& cres, bool withShapes = false);

    int find(std::string_view name) const; // case-insensitive, -1 if absent
    int findJoint(int joint) const;        // bone with that joint id, -1 if absent

    // Local transforms of the rest pose.
    std::vector<Xform> restPose() const;
    // Model-space transforms from local ones (one per bone).
    std::vector<Xform> world(const std::vector<Xform>& local) const;
};

// For each joint id of `own` (index), the bone of `target` with the same name,
// or -1. Used to drive a mesh bound to one CRES's skeleton copy with another
// skeleton (e.g. a hair mesh with the body's).
std::vector<int> mapJoints(const Skeleton& own, const Skeleton& target);

} // namespace os2
