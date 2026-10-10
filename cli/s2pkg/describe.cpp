#include "describe.hpp"

#include "formats/gmdc.hpp"
#include "formats/image.hpp"
#include "formats/rcol.hpp"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>

namespace os2 {

namespace {

namespace t {
constexpr uint32_t TXTR = 0x1C4A276C, TXMT = 0x49596978, GMND = 0x7BA3838C, GMDC = 0xAC4F8687, CRES = 0xE519C933,
                   LIFO = 0xED534136, SHPE = 0xFC6EB1F7, CINE = 0x4D51F042, LGHT_AMB = 0xC9C81B9B,
                   LGHT_DIR = 0xC9C81BA3, LGHT_PNT = 0xC9C81BA9, LGHT_SPT = 0xC9C81BAD;
}

bool isRcolType(uint32_t type) {
    switch (type) {
    case t::TXTR: case t::TXMT: case t::GMND: case t::GMDC: case t::CRES: case t::LIFO: case t::SHPE:
    case t::CINE: case t::LGHT_AMB: case t::LGHT_DIR: case t::LGHT_PNT: case t::LGHT_SPT:
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

std::string describeRcol(std::span<const uint8_t> data) {
    std::string out;
    Rcol rc(data);
    for (const auto& l : rc.links)
        appendf(out, "link %08X-%08X-%08X-%08X\n", l.type, l.group, l.instanceHi, l.instance);
    for (const auto& b : rc.blocks) {
        appendf(out, "block %s 0x%08X v%u (%zu bytes)\n", b.className.c_str(), b.typeId,
                b.body.size() >= 4 ? le32(b.body.data()) : 0, b.body.size());
        if (b.className == "cImageData")
            describeImageData(out, b);
        else if (b.className == "cLevelInfo")
            describeLevelInfo(out, b);
        else if (b.className == "cGeometryDataContainer")
            describeGeometry(out, b);
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
