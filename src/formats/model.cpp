#include "model.hpp"

#include "resindex.hpp"
#include "scenegraph.hpp"
#include "simantics.hpp"
#include "strings.hpp"
#include "xform.hpp"

#include <cctype>
#include <cstdio>
#include <optional>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kTypeShpe = 0xFC6EB1F7, kTypeGmnd = 0x7BA3838C, kTypeGmdc = 0xAC4F8687,
                   kTypeTxmt = 0x49596978, kTypeObjd = 0x4F424A44;

// Design choice for an object's ground shadow subset: no shadow (there is no
// such TXMT; inferred from the name and from where it is used).
constexpr std::string_view kShadowOff = "guoboff";

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

StandardMaterial standardMaterial(const MaterialDefinition& m) {
    StandardMaterial out;
    auto flag = [&](const char* name, bool fallback) {
        auto v = m.property(name);
        return v.empty() ? fallback : v == "true" || v == "1";
    };
    auto number = [&](const char* name, float fallback) {
        auto v = m.property(name);
        try {
            return v.empty() ? fallback : std::stof(v);
        } catch (const std::logic_error&) {
            return fallback;
        }
    };
    if (flag("stdMatBaseTextureEnabled", true))
        out.baseTexture = baseTextureName(m);
    auto color = [&](const char* name, std::array<float, 3>& c) {
        if (auto v = m.property(name); !v.empty())
            std::sscanf(v.c_str(), "%f,%f,%f", &c[0], &c[1], &c[2]);
    };
    color("stdMatDiffCoef", out.diffuse);
    color("stdMatSpecCoef", out.specular);
    color("stdMatEmissiveCoef", out.emissive);
    out.specularPower = number("stdMatSpecPower", out.specularPower);
    out.alphaMultiplier = number("stdMatAlphaMultiplier", 1.0f);
    out.untexturedAlpha = number("stdMatUntexturedDiffAlpha", 1.0f);
    const auto blend = m.property("stdMatAlphaBlendMode");
    out.blend = blend == "blend" ? StandardMaterial::Blend::Alpha
              : blend == "additive" || blend == "addNoSrcAlphaScale" ? StandardMaterial::Blend::Additive
                                    : StandardMaterial::Blend::None;
    // Other material types rarely carry the StandardMaterial blend settings;
    // their names say how they blend ("TextureAlpha" ground shadows,
    // "AdditiveTextureBlend", ...).
    if (blend.empty() && m.materialType != "StandardMaterial") {
        if (m.materialType.find("Additive") != std::string::npos)
            out.blend = StandardMaterial::Blend::Additive;
        else if (m.materialType.find("Alpha") != std::string::npos)
            out.blend = StandardMaterial::Blend::Alpha;
    }
    out.alphaTest = flag("stdMatAlphaTestEnabled", false);
    out.alphaRef = number("stdMatAlphaRefValue", 127.0f) / 255.0f;
    const auto cull = m.property("stdMatCullMode");
    out.cull = cull == "none" ? StandardMaterial::Cull::None
             : cull == "cullCounterClockwise" ? StandardMaterial::Cull::CounterClockwise
                                              : StandardMaterial::Cull::Clockwise;
    out.lighting = flag("stdMatLightingEnabled", true);
    out.layer = int(number("stdMatLayer", 0));
    // WallMask parts are the shapes doors and windows cut out of walls.
    out.visible = m.materialType != "Null" && m.materialType != "WallMask";
    return out;
}

ObjectModel loadObjectModel(ResourceIndex& index, std::string_view cresName,
                            const std::map<std::string, std::string>& materials) {
    ObjectModel model;
    model.name = withSuffix(cresName, "_cres");
    auto cresLoc = index.findByName(model.name, kTypeCres);
    if (!cresLoc)
        throw FormatError("CRES not found: " + model.name);
    const auto cresBytes = index.read(*cresLoc);
    Rcol cres(cresBytes);

    const NodeTree tree(cres);
    model.skeleton = Skeleton::fromCres(cres);
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
                // A design choice naming a material the game no longer ships
                // keeps the shape's own.
                if (auto it = materials.find(mesh.name); it != materials.end())
                    if (it->second == kShadowOff || index.findByName(withSuffix(it->second, "_txmt"), kTypeTxmt))
                        part.material = it->second;
                if (!mesh.bones.empty())
                    for (const auto& t : geometry.transforms)
                        part.bindPoses.push_back(
                            Xform{{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]}, t.translation});
                part.mesh = std::move(mesh);
                model.parts.push_back(std::move(part));
            }
        }
    }

    for (const auto& part : model.parts) {
        if (part.material.empty() || model.materials.count(part.material))
            continue;
        // Hidden: shadows turned off, and the volume a mirror reflects.
        if (part.material == kShadowOff || part.material == "mirrorboundingboxmaterial") {
            MaterialDefinition hidden;
            hidden.materialType = "Null";
            model.materials.emplace(part.material, std::move(hidden));
            continue;
        }
        auto loc = index.findByName(withSuffix(part.material, "_txmt"), kTypeTxmt);
        // Counter tops cut out for a sink ("..._clean-cutout-0") are made by
        // the game at run time from the plain top.
        if (auto cut = part.material.find("-cutout"); !loc && cut != std::string::npos)
            loc = index.findByName(part.material.substr(0, cut) + "_txmt", kTypeTxmt);
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

std::string resolveObjectCres(ResourceIndex& index, std::string_view query, std::string* match) {
    if (index.findByName(withSuffix(query, "_cres"), kTypeCres)) {
        if (match)
            *match = "CRES " + withSuffix(query, "_cres");
        return std::string(query);
    }
    const bool isGuid = query.starts_with("0x") || query.starts_with("0X");
    const uint32_t guid = isGuid ? uint32_t(std::stoul(std::string(query), nullptr, 16)) : 0;
    auto lower = [](std::string_view in) {
        std::string s(in);
        for (auto& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const auto needle = lower(query);
    for (const auto& loc : index.ofType(kTypeObjd)) {
        const auto def = ObjectDefinition::parse(index.read(loc));
        if (isGuid ? def.guid() != guid : lower(def.fileName).find(needle) == std::string::npos)
            continue;
        auto cres = objectCresName(index, loc.entry->tgi);
        if (cres.empty())
            continue;
        if (match) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%08X", def.guid());
            *match = "object \"" + def.fileName + "\" (GUID " + buf + ") -> " + cres;
        }
        return cres;
    }
    throw FormatError("no object or CRES matches '" + std::string(query) + "'");
}

} // namespace os2
