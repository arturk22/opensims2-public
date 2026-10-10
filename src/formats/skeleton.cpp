#include "skeleton.hpp"

#include <cctype>
#include <optional>

namespace os2 {

namespace {

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

} // namespace

Skeleton Skeleton::fromCres(const Rcol& cres) {
    const size_t n = cres.blocks.size();
    std::vector<std::optional<TransformNode>> nodes(n);
    std::vector<NodeRef> roots;
    Skeleton s;
    for (size_t i = 0; i < n; ++i) {
        const auto& b = cres.blocks[i];
        if (b.className == "cTransformNode") {
            nodes[i] = TransformNode::parse(b.body);
        } else if (b.className == "cResourceNode") {
            const auto r = ResourceNode::parse(b.body);
            if (s.name.empty())
                s.name = r.typeCode == 1 ? r.resource.fileName : r.graph.fileName;
            roots.insert(roots.end(), r.children.begin(), r.children.end());
        }
    }

    // Depth first from the resource node, so parents precede children; nodes
    // nothing points at become roots of their own.
    std::vector<bool> visited(n, false);
    auto visit = [&](auto&& self, size_t block, int parent) -> void {
        if (block >= n || visited[block] || !nodes[block])
            return;
        visited[block] = true;
        const TransformNode& t = *nodes[block];
        Bone bone;
        bone.name = t.name();
        bone.parent = parent;
        bone.rest = Xform{t.rotation, t.translation};
        bone.joint = t.jointReference == 0x7FFFFFFF ? -1 : t.jointReference;
        bone.block = block;
        const int self_index = int(s.bones.size());
        s.bones.push_back(std::move(bone));
        for (const auto& c : t.children)
            self(self, c.index, self_index);
    };
    for (const auto& r : roots)
        visit(visit, r.index, -1);
    std::vector<bool> isChild(n, false);
    for (size_t i = 0; i < n; ++i)
        if (nodes[i])
            for (const auto& c : nodes[i]->children)
                if (c.index < n)
                    isChild[c.index] = true;
    for (size_t i = 0; i < n; ++i)
        if (!isChild[i])
            visit(visit, i, -1);
    return s;
}

int Skeleton::find(std::string_view n) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (equalsNoCase(bones[i].name, n))
            return int(i);
    return -1;
}

int Skeleton::findJoint(int joint) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].joint == joint)
            return int(i);
    return -1;
}

std::vector<Xform> Skeleton::restPose() const {
    std::vector<Xform> out;
    out.reserve(bones.size());
    for (const auto& b : bones)
        out.push_back(b.rest);
    return out;
}

std::vector<Xform> Skeleton::world(const std::vector<Xform>& local) const {
    std::vector<Xform> out(bones.size());
    for (size_t i = 0; i < bones.size(); ++i) {
        const Xform& l = i < local.size() ? local[i] : bones[i].rest;
        const int p = bones[i].parent;
        out[i] = p >= 0 ? out[size_t(p)].then(l) : l;
    }
    return out;
}

std::vector<int> mapJoints(const Skeleton& own, const Skeleton& target) {
    std::vector<int> out;
    for (const auto& b : own.bones) {
        if (b.joint < 0 || b.joint > 0xFFFF)
            continue;
        if (out.size() <= size_t(b.joint))
            out.resize(size_t(b.joint) + 1, -1);
        out[size_t(b.joint)] = target.find(b.name);
    }
    return out;
}

} // namespace os2
