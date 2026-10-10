#include "gmdc.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>

namespace os2 {

namespace {

constexpr uint32_t kMaxCount = 1u << 22;

uint32_t count(BinReader& r, const char* what) {
    const uint32_t n = r.u32();
    if (n > kMaxCount)
        throw FormatError(std::string("cGeometryDataContainer: bad ") + what + " count");
    return n;
}

// Index list: u16 entries in version 4, u32 before.
std::vector<uint32_t> readList(BinReader& r, uint32_t version, const char* what) {
    std::vector<uint32_t> v(count(r, what));
    for (auto& x : v)
        x = version == 4 ? r.u16() : r.u32();
    return v;
}

GeometryData::SimpleMesh readSimpleMesh(BinReader& r, uint32_t version) {
    GeometryData::SimpleMesh m;
    const uint32_t nv = count(r, "mesh vertex");
    if (nv == 0)
        return m;
    const uint32_t nf = count(r, "mesh face");
    m.vertices.resize(nv);
    for (auto& v : m.vertices)
        v = {r.f32(), r.f32(), r.f32()};
    m.faces.resize(nf);
    for (auto& f : m.faces)
        f = version == 4 ? r.u16() : r.u32();
    return m;
}

} // namespace

const char* gmdc::elementName(uint32_t id) {
    switch (id) {
    case Vertices: return "vertices";
    case Normals: return "normals";
    case UVs: return "uvs";
    case BlendIndices: return "blendIndices";
    case BlendWeights: return "blendWeights";
    case BoneAssignments: return "boneAssignments";
    case BoneWeights: return "boneWeights";
    case TangentNormals: return "tangentNormals";
    case Binormals: return "binormals";
    case VertexMorphDeltas: return "vertexMorphDeltas";
    case NormalMorphDeltas: return "normalMorphDeltas";
    case Colors: return "colors";
    default: return "";
    }
}

uint32_t GeometryData::Element::components() const {
    switch (blockFormat) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 4: return 1;
    default: throw FormatError("cGeometryDataContainer: unknown block format " + std::to_string(blockFormat));
    }
}

float GeometryData::Element::f(size_t value, uint32_t component) const {
    return std::bit_cast<float>(raw.at(value * components() + component));
}

GeometryData GeometryData::parse(std::span<const uint8_t> body) {
    BinReader r(body);
    auto g = read(r);
    r.expectEnd("cGeometryDataContainer");
    return g;
}

GeometryData GeometryData::read(BinReader& r) {
    GeometryData g;
    g.version = r.u32();
    if (g.version != 1 && g.version != 2 && g.version != 4)
        throw FormatError("cGeometryDataContainer: unsupported version " + std::to_string(g.version));
    g.resource = SgResource::read(r);

    g.elements.resize(count(r, "element"));
    for (auto& e : g.elements) {
        e.number = r.u32();
        e.identity = r.u32();
        e.repeat = r.u32();
        e.blockFormat = r.u32();
        e.setFormat = r.u32();
        const uint32_t bytes = r.u32();
        if (bytes % (4 * e.components()))
            throw FormatError("cGeometryDataContainer: element data size not a multiple of its value size");
        e.raw.resize(bytes / 4);
        for (auto& w : e.raw)
            w = r.u32();
        e.items = readList(r, g.version, "element item");
    }

    g.links.resize(count(r, "link"));
    for (auto& l : g.links) {
        l.elements = readList(r, g.version, "link element");
        for (auto idx : l.elements)
            if (idx >= g.elements.size())
                throw FormatError("cGeometryDataContainer: link references a missing element");
        l.referencedSize = r.u32();
        l.activeElements = r.u32();
        l.aliases.resize(3); // always three lists, for the link's first three elements
        for (auto& a : l.aliases)
            a = readList(r, g.version, "alias");
    }

    g.groups.resize(count(r, "group"));
    for (auto& gr : g.groups) {
        gr.primitiveType = r.u32();
        gr.linkIndex = r.i32();
        gr.name = r.str7();
        gr.faces = readList(r, g.version, "face");
        gr.opacity = r.u32();
        if (g.version != 1)
            gr.usedJoints = readList(r, g.version, "joint");
    }

    g.transforms.resize(count(r, "transform"));
    for (auto& t : g.transforms) {
        t.rotation = {r.f32(), r.f32(), r.f32(), r.f32()};
        t.translation = {r.f32(), r.f32(), r.f32()};
    }
    g.blendNames.resize(count(r, "blend name"));
    for (auto& b : g.blendNames) {
        b.blendGroup = r.str7();
        b.element = r.str7();
    }
    g.boundingMesh = readSimpleMesh(r, g.version);
    g.joints.resize(count(r, "joint"));
    for (auto& j : g.joints)
        j = readSimpleMesh(r, g.version);
    return g;
}

std::vector<MeshGroup> buildMeshes(const GeometryData& g) {
    std::vector<MeshGroup> out;
    for (const auto& gr : g.groups) {
        if (gr.primitiveType != 2)
            continue;
        if (gr.linkIndex < 0 || size_t(gr.linkIndex) >= g.links.size())
            throw FormatError("GMDC group '" + gr.name + "' references a missing link");
        const auto& link = g.links[size_t(gr.linkIndex)];
        const uint32_t n = link.referencedSize;
        if (gr.faces.size() % 3)
            throw FormatError("GMDC group '" + gr.name + "' face list is not a multiple of 3");

        // Groups often share one link; keep only the vertices this group uses.
        std::vector<uint32_t> remap(n, UINT32_MAX), used;
        MeshGroup m;
        m.name = gr.name;
        for (auto f : gr.faces) {
            if (f >= n)
                throw FormatError("GMDC group '" + gr.name + "' face index out of range");
            if (remap[f] == UINT32_MAX) {
                remap[f] = uint32_t(used.size());
                used.push_back(f);
            }
            m.triangles.push_back(remap[f]);
        }

        // Value index of vertex `v` in the link's k-th element.
        int weightElement = -1; // position in link.elements of the bone weights
        auto valueIndex = [&](size_t k, uint32_t v) -> size_t {
            if (k < link.aliases.size() && !link.aliases[k].empty())
                return link.aliases[k].at(v);
            return v;
        };
        for (size_t k = 0; k < link.elements.size(); ++k) {
            const auto& e = g.elements[link.elements[k]];
            if (e.identity == gmdc::Vertices || e.identity == gmdc::Normals) {
                auto& dst = e.identity == gmdc::Vertices ? m.positions : m.normals;
                if (!dst.empty() || e.blockFormat != 2)
                    continue; // first float3 stream wins
                for (auto v : used) {
                    const size_t i = valueIndex(k, v);
                    dst.push_back({e.f(i, 0), e.f(i, 1), e.f(i, 2)});
                }
            } else if (e.identity == gmdc::UVs && m.uvs.empty() && e.blockFormat == 1) {
                for (auto v : used) {
                    const size_t i = valueIndex(k, v);
                    m.uvs.push_back({e.f(i, 0), e.f(i, 1)});
                }
            } else if (e.identity == gmdc::BoneAssignments && m.bones.empty() && e.blockFormat == 4 &&
                       e.valueCount() > 0) {
                // One byte per slot, low byte first: an index into the group's
                // used joints, 0xFF for an unused slot.
                for (auto v : used) {
                    const uint32_t packed = e.raw.at(valueIndex(k, v));
                    std::array<uint8_t, 4> b{0xFF, 0xFF, 0xFF, 0xFF};
                    for (int s = 0; s < 4; ++s) {
                        const uint32_t local = (packed >> (8 * s)) & 0xFF;
                        if (local != 0xFF && local < gr.usedJoints.size() && gr.usedJoints[local] < 0xFF)
                            b[s] = uint8_t(gr.usedJoints[local]);
                    }
                    m.bones.push_back(b);
                }
            } else if (e.identity == gmdc::BoneWeights && weightElement < 0 && e.blockFormat <= 2 &&
                       e.valueCount() > 0) {
                weightElement = int(k);
            }
        }
        // Weights per slot; a slot past the stored components gets what is left
        // of 1 (the stored weights sum to 1 in the base game meshes).
        if (!m.bones.empty()) {
            m.weights.assign(m.bones.size(), {0, 0, 0, 0});
            const GeometryData::Element* we = weightElement >= 0 ? &g.elements[link.elements[size_t(weightElement)]] : nullptr;
            for (size_t i = 0; i < used.size(); ++i) {
                float sum = 0;
                for (uint32_t s = 0; s < 4; ++s) {
                    if (m.bones[i][s] == 0xFF)
                        continue;
                    float w;
                    if (we && s < we->components())
                        w = we->f(valueIndex(size_t(weightElement), used[i]), s);
                    else
                        w = std::max(0.0f, 1.0f - sum);
                    m.weights[i][s] = w;
                    sum += w;
                }
            }
        }
        if (m.positions.empty() && !used.empty())
            throw FormatError("GMDC group '" + gr.name + "' has no vertex positions");
        out.push_back(std::move(m));
    }
    return out;
}

void ObjWriter::write(const MeshGroup& m, const std::string& material) {
    char buf[160];
    out << "g " << m.name << "\n";
    if (!material.empty())
        out << "usemtl " << material << "\n";
    // The game is Z-up; OBJ is Y-up.
    for (const auto& p : m.positions) {
        std::snprintf(buf, sizeof buf, "v %.6f %.6f %.6f\n", p[0], p[2], -p[1]);
        out << buf;
    }
    for (const auto& t : m.uvs) {
        // OBJ puts v=0 at the bottom; the game's textures start at the top.
        std::snprintf(buf, sizeof buf, "vt %.6f %.6f\n", t[0], 1.0f - t[1]);
        out << buf;
    }
    for (const auto& nrm : m.normals) {
        std::snprintf(buf, sizeof buf, "vn %.6f %.6f %.6f\n", nrm[0], nrm[2], -nrm[1]);
        out << buf;
    }
    const bool hasUv = !m.uvs.empty(), hasN = !m.normals.empty();
    for (size_t i = 0; i + 2 < m.triangles.size(); i += 3) {
        out << "f";
        for (size_t k = 0; k < 3; ++k) {
            const size_t v = m.triangles[i + k] + 1;
            out << ' ' << vBase + v;
            if (hasUv || hasN)
                out << '/' << (hasUv ? std::to_string(vtBase + v) : "");
            if (hasN)
                out << '/' << vnBase + v;
        }
        out << "\n";
    }
    vBase += m.positions.size();
    vtBase += m.uvs.size();
    vnBase += m.normals.size();
}

} // namespace os2
