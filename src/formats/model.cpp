#include "model.hpp"

#include "resindex.hpp"
#include "scenegraph.hpp"
#include "simantics.hpp"
#include "strings.hpp"

#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kTypeShpe = 0xFC6EB1F7, kTypeGmnd = 0x7BA3838C, kTypeGmdc = 0xAC4F8687,
                   kTypeTxmt = 0x49596978;

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string withSuffix(std::string_view name, std::string_view suffix) {
    std::string s(name);
    if (!endsWith(s, suffix))
        s += suffix;
    return s;
}

// RCOL links of the CRES/SHPE carry the full TGI; older ones leave instanceHi out.
std::optional<ResourceIndex::Location> findLink(ResourceIndex& index, Tgi tgi) {
    if (auto loc = index.find(tgi))
        return loc;
    tgi.instanceHi = 0;
    return index.find(tgi);
}

const Rcol::Block& firstBlock(const Rcol& rc, std::string_view cls, const std::string& what) {
    const auto* b = rc.find(cls);
    if (!b)
        throw FormatError(what + " has no " + std::string(cls));
    return *b;
}

// Rigid transform: rotation (unit quaternion) then translation.
struct Xform {
    Quat q;
    Vec3 t{0, 0, 0};

    Vec3 rotate(const Vec3& v) const {
        // v + 2w(q x v) + 2 q x (q x v)
        const Vec3 u{q.x, q.y, q.z};
        auto cross = [](const Vec3& a, const Vec3& b) {
            return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        const Vec3 c1 = cross(u, v);
        const Vec3 c2 = cross(u, c1);
        return {v[0] + 2 * (q.w * c1[0] + c2[0]), v[1] + 2 * (q.w * c1[1] + c2[1]), v[2] + 2 * (q.w * c1[2] + c2[2])};
    }
    Vec3 apply(const Vec3& v) const {
        const Vec3 r = rotate(v);
        return {r[0] + t[0], r[1] + t[1], r[2] + t[2]};
    }
    // this ∘ local
    Xform then(const Xform& local) const {
        Xform out;
        const Quat& a = q;
        const Quat& b = local.q;
        out.q = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        out.t = apply(local.t);
        return out;
    }
};

// Local transforms and parent links of the CRES blocks that form the node tree.
struct NodeTree {
    std::vector<std::optional<Xform>> local;
    std::vector<int> parent;

    explicit NodeTree(const Rcol& cres) : local(cres.blocks.size()), parent(cres.blocks.size(), -1) {
        auto link = [&](size_t from, const std::vector<NodeRef>& children) {
            for (const auto& c : children)
                if (c.index < parent.size())
                    parent[c.index] = int(from);
        };
        auto setLocal = [&](size_t i, const TransformNode& t) {
            local[i] = Xform{t.rotation, t.translation};
            link(i, t.children);
        };
        for (size_t i = 0; i < cres.blocks.size(); ++i) {
            const auto& b = cres.blocks[i];
            if (b.className == "cTransformNode")
                setLocal(i, TransformNode::parse(b.body));
            else if (b.className == "cShapeRefNode")
                setLocal(i, ShapeRefNode::parse(b.body).renderable.transform);
            else if (b.className == "cLightRefNode")
                setLocal(i, LightRefNode::parse(b.body).renderable.transform);
            else if (b.className == "cResourceNode")
                link(i, ResourceNode::parse(b.body).children);
        }
    }

    Xform world(size_t i) const {
        Xform w;
        for (int n = int(i), guard = 0; n >= 0 && guard < 256; n = parent[size_t(n)], ++guard)
            if (local[size_t(n)])
                w = local[size_t(n)]->then(w);
        return w;
    }
};

} // namespace

std::string objectCresName(ResourceIndex& index, const Tgi& objd) {
    const auto objdBytes = index.read(objd);
    if (!objdBytes)
        throw FormatError("OBJD not found");
    const auto def = ObjectDefinition::parse(*objdBytes);
    const auto names = index.read(Tgi{0x53545223, objd.group, 0x85, 0});
    if (!names)
        return {};
    return StringTable::parse(*names).get(def.modelIndex(), lang::EnglishUS);
}

std::string baseTextureName(const MaterialDefinition& m) {
    auto tex = m.property("stdMatBaseTextureName");
    if (tex.empty())
        return {};
    return withSuffix(tex, "_txtr");
}

ObjectModel loadObjectModel(ResourceIndex& index, std::string_view cresName) {
    ObjectModel model;
    model.name = withSuffix(cresName, "_cres");
    auto cresLoc = index.findByName(model.name, kTypeCres);
    if (!cresLoc)
        throw FormatError("CRES not found: " + model.name);
    const auto cresBytes = index.read(*cresLoc);
    Rcol cres(cresBytes);

    const NodeTree tree(cres);
    for (size_t bi = 0; bi < cres.blocks.size(); ++bi) {
        const auto& block = cres.blocks[bi];
        if (block.className != "cShapeRefNode")
            continue;
        const auto ref = ShapeRefNode::parse(block.body);
        const Xform toObject = tree.world(bi);
        for (const auto& link : ref.shapes) {
            if (link.linkIndex < 0 || size_t(link.linkIndex) >= cres.links.size()) {
                model.warnings.push_back("shape link " + std::to_string(link.linkIndex) + " out of range");
                continue;
            }
            const Tgi shpeTgi = cres.links[size_t(link.linkIndex)];
            if (shpeTgi.type != kTypeShpe)
                continue;
            auto shpeLoc = findLink(index, shpeTgi);
            if (!shpeLoc) {
                model.warnings.push_back("SHPE not found for link " + std::to_string(link.linkIndex));
                continue;
            }
            const auto shpeBytes = index.read(*shpeLoc);
            Rcol shpeRc(shpeBytes);
            const auto shape = Shape::parse(firstBlock(shpeRc, "cShape", "SHPE").body);
            if (shape.items.empty())
                continue;

            // Lowest level of detail is the most detailed.
            const auto* item = &shape.items[0];
            for (const auto& it : shape.items)
                if (it.lod < item->lod)
                    item = &it;

            std::optional<ResourceIndex::Location> gmndLoc;
            if (!item->geometryName.empty())
                gmndLoc = index.findByName(item->geometryName, kTypeGmnd);
            else if (item->linkIndex >= 0 && size_t(item->linkIndex) < shpeRc.links.size())
                gmndLoc = findLink(index, shpeRc.links[size_t(item->linkIndex)]);
            if (!gmndLoc) {
                model.warnings.push_back("GMND not found for " + shape.resource.fileName);
                continue;
            }
            const auto gmndBytes = index.read(*gmndLoc);
            Rcol gmndRc(gmndBytes);
            std::optional<ResourceIndex::Location> gmdcLoc;
            for (const auto& l : gmndRc.links)
                if (l.type == kTypeGmdc && (gmdcLoc = findLink(index, l)))
                    break;
            if (!gmdcLoc) {
                model.warnings.push_back("GMDC not found for " + shape.resource.fileName);
                continue;
            }
            const auto gmdcBytes = index.read(*gmdcLoc);
            Rcol gmdcRc(gmdcBytes);
            const auto geometry = GeometryData::parse(firstBlock(gmdcRc, "cGeometryDataContainer", "GMDC").body);

            for (auto& mesh : buildMeshes(geometry)) {
                for (auto& p : mesh.positions)
                    p = toObject.apply(p);
                for (auto& n : mesh.normals)
                    n = toObject.rotate(n);
                ObjectModel::Part part;
                part.node = ref.renderable.transform.name();
                part.shape = shape.resource.fileName;
                for (const auto& p : shape.parts)
                    if (p.subset == mesh.name)
                        part.material = p.material;
                part.mesh = std::move(mesh);
                model.parts.push_back(std::move(part));
            }
        }
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        auto loc = index.findByName(withSuffix(part.material, "_txmt"), kTypeTxmt);
        if (!loc) {
            model.warnings.push_back("TXMT not found: " + part.material);
            continue;
        }
        const auto bytes = index.read(*loc);
        Rcol rc(bytes);
        model.materials.emplace(part.material,
                                MaterialDefinition::parse(firstBlock(rc, "cMaterialDefinition", "TXMT").body));
    }
    return model;
}

} // namespace os2
