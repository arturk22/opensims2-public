#include "describe.hpp"

#include "formats/gmdc.hpp"
#include "formats/image.hpp"
#include "formats/rcol.hpp"
#include "formats/scenegraph.hpp"
#include "formats/txmt.hpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace os2 {

namespace {

namespace t {
constexpr uint32_t TXTR = 0x1C4A276C, TXMT = 0x49596978, GMND = 0x7BA3838C, GMDC = 0xAC4F8687, CRES = 0xE519C933,
                   LIFO = 0xED534136, SHPE = 0xFC6EB1F7, LGHT_AMB = 0xC9C81B9B,
                   LGHT_DIR = 0xC9C81BA3, LGHT_PNT = 0xC9C81BA9, LGHT_SPT = 0xC9C81BAD;
}

bool isRcolType(uint32_t type) {
    switch (type) {
    case t::TXTR: case t::TXMT: case t::GMND: case t::GMDC: case t::CRES: case t::LIFO: case t::SHPE:
    case t::LGHT_AMB: case t::LGHT_DIR: case t::LGHT_PNT: case t::LGHT_SPT:
        return true;
    default:
        return false;
    }
}

void appendf(std::string& out, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out += buf;
}

void describeImageData(std::string& out, const Rcol::Block& b) {
    const auto d = ImageData::parse(b.body);
    appendf(out, "  name=%s %ux%u %s mipLevels=%u textures=%zu\n", d.resource.fileName.c_str(), d.width,
            d.height, texFormatName(d.format), d.mipLevels, d.textures.size());
    for (const auto& tex : d.textures) {
        const auto& levels = tex.mips;
        appendf(out, "  texture creator=0x%08X\n", tex.creator);
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& m = levels[i];
            appendf(out, "  mip %zu %ux%u ", i, d.mipWidth(i, levels.size()), d.mipHeight(i, levels.size()));
            if (m.lifoName.empty())
                appendf(out, "inline %zu bytes\n", m.data.size());
            else
                appendf(out, "lifo %s\n", m.lifoName.c_str());
        }
    }
}

void describeLevelInfo(std::string& out, const Rcol::Block& b) {
    const auto l = LevelInfo::parse(b.body);
    appendf(out, "  name=%s %ux%u z=%u %zu bytes\n", l.resource.fileName.c_str(), l.width, l.height, l.zLevel,
            l.data.size());
}

void describeGeometry(std::string& out, const Rcol::Block& b) {
    const auto g = GeometryData::parse(b.body);
    appendf(out, "  name=%s\n", g.resource.fileName.c_str());
    for (size_t i = 0; i < g.elements.size(); ++i) {
        const auto& e = g.elements[i];
        const char* n = gmdc::elementName(e.identity);
        appendf(out, "  element %zu %s(0x%08X) number=%u fmt=%u set=%u values=%zu items=%zu\n", i, n, e.identity,
                e.number, e.blockFormat, e.setFormat, e.valueCount(), e.items.size());
    }
    for (size_t i = 0; i < g.links.size(); ++i) {
        const auto& l = g.links[i];
        appendf(out, "  link %zu size=%u active=%u elements=[", i, l.referencedSize, l.activeElements);
        for (auto e : l.elements)
            appendf(out, " %u", e);
        appendf(out, " ] aliases=[");
        for (const auto& a : l.aliases)
            appendf(out, " %zu", a.size());
        appendf(out, " ]\n");
    }
    for (const auto& gr : g.groups)
        appendf(out, "  group '%s' prim=%u link=%d faces=%zu opacity=0x%X joints=%zu\n", gr.name.c_str(),
                gr.primitiveType, gr.linkIndex, gr.faces.size(), gr.opacity, gr.usedJoints.size());
    appendf(out, "  transforms=%zu blendNames=%zu boundingMesh=%zu/%zu joints=%zu\n", g.transforms.size(),
            g.blendNames.size(), g.boundingMesh.vertices.size(), g.boundingMesh.faces.size(), g.joints.size());
    for (const auto& m : buildMeshes(g))
        appendf(out, "  mesh '%s' %zu vertices %zu triangles normals=%s uvs=%s\n", m.name.c_str(),
                m.positions.size(), m.triangles.size() / 3, m.normals.empty() ? "no" : "yes",
                m.uvs.empty() ? "no" : "yes");
}

void describeMaterial(std::string& out, const Rcol::Block& b) {
    const auto m = MaterialDefinition::parse(b.body);
    appendf(out, "  name=%s type=%s desc=%s\n", m.resource.fileName.c_str(), m.materialType.c_str(),
            m.description.c_str());
    for (const auto& [k, v] : m.properties)
        appendf(out, "  %s = %s\n", k.c_str(), v.c_str());
    for (const auto& t : m.textureNames)
        appendf(out, "  texture %s\n", t.c_str());
}

std::string refs(const std::vector<NodeRef>& v) {
    std::string out;
    for (const auto& n : v)
        out += " " + std::to_string(n.index) + (n.enabled ? "" : "(off)");
    return out;
}

void describeTransform(std::string& out, const TransformNode& t, const char* indent) {
    appendf(out, "%sname=%s joint=%d children=[%s ]\n", indent, t.name().c_str(), t.jointReference,
            refs(t.children).c_str());
    appendf(out, "%strans=(%g %g %g) rot=(%g %g %g %g)\n", indent, t.translation[0], t.translation[1],
            t.translation[2], t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w);
}

void describeExtensionValue(std::string& out, const ExtensionValue& v, int depth) {
    out.append(size_t(2 + 2 * depth), ' ');
    switch (v.type) {
    case ExtensionValue::Int: appendf(out, "%s = %d\n", v.name.c_str(), v.i); break;
    case ExtensionValue::Float: appendf(out, "%s = %g\n", v.name.c_str(), v.f[0]); break;
    case ExtensionValue::Vector: appendf(out, "%s = (%g %g %g)\n", v.name.c_str(), v.f[0], v.f[1], v.f[2]); break;
    case ExtensionValue::String: appendf(out, "%s = \"%s\"\n", v.name.c_str(), v.s.c_str()); break;
    case ExtensionValue::Quaternion:
        appendf(out, "%s = quat(%g %g %g %g)\n", v.name.c_str(), v.f[0], v.f[1], v.f[2], v.f[3]);
        break;
    case ExtensionValue::Binary: appendf(out, "%s = <%zu bytes>\n", v.name.c_str(), v.bytes.size()); break;
    case ExtensionValue::Array:
        appendf(out, "%s:\n", v.name.c_str());
        for (const auto& it : v.items)
            describeExtensionValue(out, it, depth + 1);
        break;
    }
}

void describeBlock(std::string& out, const Rcol::Block& b) {
    const auto& c = b.className;
    if (c == "cImageData") {
        describeImageData(out, b);
    } else if (c == "cLevelInfo") {
        describeLevelInfo(out, b);
    } else if (c == "cGeometryDataContainer") {
        describeGeometry(out, b);
    } else if (c == "cMaterialDefinition") {
        describeMaterial(out, b);
    } else if (c == "cTransformNode") {
        describeTransform(out, TransformNode::parse(b.body), "  ");
    } else if (c == "cShapeRefNode") {
        const auto s = ShapeRefNode::parse(b.body);
        describeTransform(out, s.renderable.transform, "  ");
        std::string links;
        for (const auto& l : s.shapes)
            links += " " + std::to_string(l.linkIndex);
        appendf(out, "  shape links=[%s ] blends=%zu named=%zu\n", links.c_str(), s.blendValues.size(),
                s.blendNames.size());
    } else if (c == "cResourceNode") {
        const auto n = ResourceNode::parse(b.body);
        appendf(out, "  name=%s typeCode=%u graph=[%s ] children=[%s ]\n",
                (n.typeCode == 1 ? n.resource.fileName : n.graph.fileName).c_str(), n.typeCode,
                refs(n.rootGraph().extensions).c_str(), refs(n.children).c_str());
    } else if (c == "cDataListExtension") {
        describeExtensionValue(out, DataListExtension::parse(b.body).root, 0);
    } else if (c == "cBoneDataExtension") {
        const auto e = BoneDataExtension::parse(b.body);
        appendf(out, "  %d %d %d %g rot=(%g %g %g %g)\n", e.unknown[0], e.unknown[1], e.unknown[2], e.unknownFloat,
                e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w);
    } else if (c == "cTagExtension") {
        appendf(out, "  tag=%s\n", TagExtension::parse(b.body).tag.c_str());
    } else if (c == "cLightRefNode") {
        const auto n = LightRefNode::parse(b.body);
        describeTransform(out, n.renderable.transform, "  ");
        appendf(out, "  light link=%d\n", n.lightLink);
    } else if (c == "cViewerRefNode") {
        const auto n = ViewerRefNode::parse(b.body);
        describeTransform(out, n.renderable.transform, "  ");
        appendf(out, "  camera data=%zu bytes\n", n.camera.size());
    } else if (c == "cViewerRefNodeRecursive") {
        const auto n = ViewerRefNodeRecursive::parse(b.body);
        describeTransform(out, n.renderable.transform, "  ");
        appendf(out, "  view=%s\n", n.name.c_str());
    } else if (c == "cAmbientLight" || c == "cDirectionalLight" || c == "cPointLight" || c == "cSpotLight") {
        const auto l = StandardLight::parse(c, b.body);
        appendf(out, "  name=%s node=%s values=", l.resource.fileName.c_str(), l.name.c_str());
        for (float v : l.values)
            appendf(out, " %g", v);
        appendf(out, "\n");
    } else if (c == "cShape") {
        const auto s = Shape::parse(b.body);
        appendf(out, "  name=%s\n", s.resource.fileName.c_str());
        for (const auto& it : s.items)
            appendf(out, "  lod %d -> %s\n", it.lod,
                    it.geometryName.empty() ? ("link " + std::to_string(it.linkIndex)).c_str()
                                            : it.geometryName.c_str());
        for (const auto& p : s.parts)
            appendf(out, "  part %s -> %s\n", p.subset.c_str(), p.material.c_str());
    } else if (c == "cGeometryNode") {
        const auto g = GeometryNode::parse(b.body);
        appendf(out, "  name=%s builders=%u\n", g.resource.fileName.c_str(), g.builderCount);
    } else {
        throw std::runtime_error("no decoder for block " + c);
    }
}

std::string describeRcol(std::span<const uint8_t> data) {
    std::string out;
    Rcol rc(data);
    for (const auto& l : rc.links)
        appendf(out, "link %08X-%08X-%08X-%08X\n", l.type, l.group, l.instanceHi, l.instance);
    for (const auto& b : rc.blocks) {
        appendf(out, "block %s 0x%08X v%u (%zu bytes)\n", b.className.c_str(), b.typeId,
                b.body.size() >= 4 ? le32(b.body.data()) : 0, b.body.size());
        try {
            describeBlock(out, b);
        } catch (const std::exception& ex) {
            throw std::runtime_error("block " + std::to_string(&b - rc.blocks.data()) + " (" + b.className +
                                     "): " + ex.what());
        }
    }
    return out;
}

} // namespace

bool hasDecoder(uint32_t type) { return isRcolType(type); }

std::string describeResource(uint32_t type, std::span<const uint8_t> data) {
    if (isRcolType(type))
        return describeRcol(data);
    throw std::runtime_error("no decoder for this type");
}

} // namespace os2
