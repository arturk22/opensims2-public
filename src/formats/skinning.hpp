#pragma once

#include "model.hpp"
#include "skeleton.hpp"

#include <map>
#include <string>
#include <vector>

namespace os2 {

// Meshes from one or more CRES (body, head, hair, ...) driven by one skeleton.
// Each part's GMDC bone ids are resolved by name through the skeleton copy of
// the CRES it came from, so parts made against different joint numberings
// share the same bones.
struct SkinnedModel {
    Skeleton skeleton;
    ObjectModel model;                       // rest-pose geometry and materials of every part
    std::vector<std::vector<int>> jointMaps; // per part: GMDC bone id -> skeleton bone, -1 if unknown
    std::vector<int> rigidBones;             // per part: the skeleton bone carrying a rigid part, -1 if none
    std::vector<Xform> restWorld;            // the skeleton's rest pose in model space

    explicit SkinnedModel(Skeleton s = {}) : skeleton(std::move(s)) {}

    // Appends the parts and materials of `part`. Rigid parts (no bone
    // assignments) follow the bone that carries their node (an object's
    // door leaf), else stay where they are.
    void add(const ObjectModel& part);

    // Linear blend skinning: `out` gets the parts with their positions and
    // normals moved by the skeleton's model-space bone transforms `world`
    // (out keeps the same parts in the same order; pass the previous result to
    // reuse its storage).
    // `morphs` (morph name -> weight, e.g. from the "faceblends" target of an
    // animation) moves the vertices by their morph deltas first.
    void pose(const std::vector<Xform>& world, ObjectModel& out,
              const std::map<std::string, float>& morphs = {}) const;
};

} // namespace os2
