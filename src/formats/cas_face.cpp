#include "cas_face.hpp"
#include "formats/log_once.hpp"

#include "config_text.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "scenegraph.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace os2::cas {

namespace {

constexpr uint32_t kTypeGmdc = 0xAC4F8687, kTypeGmnd = 0x7BA3838C, kTypeShpe = 0xFC6EB1F7;

using Vec3 = std::array<float, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dist2(const Vec3& a, const Vec3& b) {
    const Vec3 d = sub(a, b);
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalize(const Vec3& a) {
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 1e-12f ? scale(a, 1 / l) : Vec3{0, 0, 1};
}

int64_t uvKey(float u, float v) { return (int64_t(std::lround(u * 4096.0)) << 32) ^ int64_t(std::lround(v * 4096.0) & 0xFFFFFFFF); }

std::optional<ResourceIndex::Location> findAny(ResourceIndex& index, Tgi t) {
    if (auto loc = index.find(t))
        return loc;
    t.instanceHi = 0;
    return index.find(t);
}

} // namespace

// A face mesh: every vertex of its triangle groups (positions and texture
// coordinates), indexed by texture coordinate; and, for the neutral faces
// that get patched, the resource bytes and the link-level layout.
struct FaceBaker::Mesh {
    std::string name;
    std::vector<Vec3> positions;
    std::vector<std::array<float, 2>> uvs;
    std::vector<float> weights; // region masks: vertex colour weight (1 without colours)
    std::unordered_multimap<int64_t, size_t> byUv;
    std::vector<uint8_t> bytes;
    size_t bodyOffset = 0;
    GeometryData geometry;

    // The vertex with this texture coordinate nearest to `near`, or -1.
    long match(float u, float v, const Vec3& near) const {
        auto [lo, hi] = byUv.equal_range(uvKey(u, v));
        long best = -1;
        float bestD = 0;
        for (auto it = lo; it != hi; ++it) {
            const float d = dist2(positions[it->second], near);
            if (best < 0 || d < bestD)
                best = long(it->second), bestD = d;
        }
        return best;
    }
};

FaceBaker::FaceBaker(ResourceIndex& index, const CasCatalog& catalog) : index_(index), catalog_(catalog) {}
FaceBaker::~FaceBaker() = default;

const FaceBaker::Mesh& FaceBaker::meshByName(const std::string& name) {
    if (auto it = meshes_.find(name); it != meshes_.end())
        return *it->second;
    auto m = std::make_unique<Mesh>();
    m->name = name;
    auto loc = index_.findByName(name, kTypeGmdc);
    if (!loc)
        throw FormatError("face mesh " + name + " not found");
    m->bytes = index_.read(*loc);
    Rcol rc(m->bytes);
    const auto* block = rc.find("cGeometryDataContainer");
    if (!block)
        throw FormatError(name + ": no cGeometryDataContainer");
    m->bodyOffset = size_t(block->body.data() - m->bytes.data());
    m->geometry = GeometryData::parse(block->body);
    for (const auto& g : buildMeshes(m->geometry)) {
        for (size_t i = 0; i < g.positions.size(); ++i) {
            const std::array<float, 2> uv = i < g.uvs.size() ? g.uvs[i] : std::array<float, 2>{0, 0};
            m->byUv.emplace(uvKey(uv[0], uv[1]), m->positions.size());
            m->positions.push_back(g.positions[i]);
            m->uvs.push_back(uv);
            // A region mask's weight: one byte of the vertex colour / 255
            // (cTSCASBodyPartSystem::BuildRegionMask, Mac BV 0x804d0c ->
            // 0x7f9368, element 0xDB830795), the top byte as we read the
            // u32 (the masks' colours are 0x00000000, 0x3f000000,
            // 0x7f000000, 0xc0000000, 0xff000000); 0 without colours.
            float w = 0;
            if (i < g.colors.size())
                w = float((g.colors[i] >> 24) & 0xFF) / 255.0f;
            m->weights.push_back(w);
        }
    }
    return *meshes_.emplace(name, std::move(m)).first->second;
}

const FaceBaker::Mesh* FaceBaker::meshOfEntry(const SimPart& entry, bool lowDetail) {
    const auto shpe = entry.reference("shapekeyidx");
    if (!shpe)
        return nullptr;
    auto loc = findAny(index_, *shpe);
    if (!loc || loc->entry->tgi.type != kTypeShpe)
        return nullptr;
    const auto bytes = index_.read(*loc);
    Rcol rc(bytes);
    const auto* b = rc.find("cShape");
    if (!b)
        return nullptr;
    const Shape shape = Shape::parse(b->body);
    for (const auto& item : shape.items) {
        if (shape.items.size() > 1 && item.lod != 0)
            continue;
        std::optional<ResourceIndex::Location> gmnd;
        if (!item.geometryName.empty())
            gmnd = index_.findByName(item.geometryName, kTypeGmnd);
        else if (item.linkIndex >= 0 && size_t(item.linkIndex) < rc.links.size())
            gmnd = findAny(index_, rc.links[size_t(item.linkIndex)]);
        if (!gmnd)
            continue;
        const auto gb = index_.read(*gmnd);
        Rcol grc(gb);
        for (const auto& l : grc.links)
            if (l.type == kTypeGmdc)
                if (auto gl = findAny(index_, l)) {
                    // The GMDC's own name, to cache by.
                    const auto db = index_.read(*gl);
                    Rcol drc(db);
                    if (const auto* d = drc.find("cGeometryDataContainer")) {
                        BinReader r(d->body);
                        r.u32();
                        const std::string n = SgResource::read(r).fileName;
                        if (!lowDetail)
                            return &meshByName(n);
                        // The low level of detail: "<x>LOD15_tslocator_gmdc" beside
                        // "<x>_tslocator_gmdc" (CASFace.package, Sims03.package).
                        const std::string tail = "_tslocator_gmdc";
                        if (n.size() <= tail.size() || lowerAscii(n).compare(n.size() - tail.size(), tail.size(), tail) != 0)
                            return nullptr;
                        try {
                            return &meshByName(n.substr(0, n.size() - tail.size()) + "LOD15" + tail);
                        } catch (const std::exception& err) {
                            os2::logOnce("face mesh (low detail)", n, err);
                            return nullptr;
                        }
                    }
                }
    }
    return nullptr;
}

const SimPart* FaceBaker::archetypeFor(const std::string& fam, uint32_t ageBit, uint32_t genderBit) const {
    for (const auto& e : catalog_.archetypes)
        if (family(e) == fam && fits(e, ageBit, genderBit))
            return &e;
    return nullptr;
}

FaceBaker::Baked FaceBaker::bake(const FaceSpec& spec, uint32_t ageBit, uint32_t genderBit, bool lowDetail) {
    Baked out;
    const SimPart* neutralEntry = catalog_.neutralFor(ageBit, genderBit);
    if (!neutralEntry)
        throw FormatError(std::string("no neutral face for ") + ageName(ageBit));
    out.neutral = neutralEntry->name();
    const Mesh* tp = meshOfEntry(*neutralEntry, lowDetail);
    if (!tp)
        throw FormatError("neutral face " + out.neutral + ": mesh not found");
    const Mesh& t = *tp;
    const GeometryData& g = t.geometry;

    // The template's vertices, link by link.
    struct Slot {
        size_t link = 0, value = 0;
        Vec3 pos{}, normal{};
        std::array<float, 2> uv{};
    };
    std::vector<Slot> slots;
    struct LinkInfo {
        int vertices = -1, normals = -1, uvs = -1;
        size_t first = 0, count = 0;
    };
    std::vector<LinkInfo> links(g.links.size());
    for (size_t li = 0; li < g.links.size(); ++li) {
        const auto& link = g.links[li];
        LinkInfo& info = links[li];
        for (size_t k = 0; k < link.elements.size(); ++k) {
            const auto& e = g.elements[link.elements[k]];
            const bool aliased = k < link.aliases.size() && !link.aliases[k].empty();
            if (aliased)
                continue;
            if (e.identity == gmdc::Vertices && e.blockFormat == 2 && info.vertices < 0 && e.valueCount())
                info.vertices = int(link.elements[k]);
            else if (e.identity == gmdc::Normals && e.blockFormat == 2 && info.normals < 0 && e.valueCount())
                info.normals = int(link.elements[k]);
            else if (e.identity == gmdc::UVs && e.blockFormat == 1 && info.uvs < 0 && e.valueCount())
                info.uvs = int(link.elements[k]);
        }
        if (info.vertices < 0 || info.uvs < 0) {
            out.warnings.push_back("link " + std::to_string(li) + " has no plain vertices/uvs");
            continue;
        }
        const auto& ev = g.elements[size_t(info.vertices)];
        const auto& eu = g.elements[size_t(info.uvs)];
        const auto* en = info.normals >= 0 ? &g.elements[size_t(info.normals)] : nullptr;
        info.first = slots.size();
        info.count = std::min(ev.valueCount(), eu.valueCount());
        for (size_t i = 0; i < info.count; ++i) {
            Slot s;
            s.link = li;
            s.value = i;
            s.pos = {ev.f(i, 0), ev.f(i, 1), ev.f(i, 2)};
            s.uv = {eu.f(i, 0), eu.f(i, 1)};
            if (en && i < en->valueCount())
                s.normal = {en->f(i, 0), en->f(i, 1), en->f(i, 2)};
            slots.push_back(s);
        }
    }
    out.vertices = slots.size();
    std::vector<Vec3> disp(slots.size(), Vec3{0, 0, 0});
    std::vector<bool> covered(slots.size(), false);
    // cTSCASBodyPartSystem::BlendArchetype (Mac BV 0x80a380) leaves the
    // vertices of the "hairneck" region mask where they are (the seam with
    // the hair and the neck): no archetype or region blend moves them.
    std::vector<bool> hairneck(slots.size(), false);
    for (const auto& e : catalog_.regions)
        if (e.name() == "hairneck" && fits(e, ageBit, genderBit)) {
            const Mesh* mm = meshOfEntry(e, lowDetail);
            if (!mm && lowDetail)
                mm = meshOfEntry(e);
            if (mm)
                for (size_t i = 0; i < slots.size(); ++i)
                    if (long j = mm->match(slots[i].uv[0], slots[i].uv[1], slots[i].pos); j >= 0)
                        hairneck[i] = mm->weights[size_t(j)] > 0;
            break;
        }

    if (spec.structure) {
        const auto* sv = spec.structure->find(ageBit, genderBit, lowDetail);
        if (!sv && lowDetail)
            sv = spec.structure->find(ageBit, genderBit, false);
        if (!sv)
            out.warnings.push_back(std::string("the face structure has no ") + ageName(ageBit) + " face");
        else {
            std::unordered_multimap<int64_t, size_t> byUv;
            for (size_t k = 0; k < sv->size(); ++k)
                byUv.emplace(uvKey((*sv)[k].u, (*sv)[k].v), k);
            std::vector<bool> used(sv->size(), false);
            for (size_t i = 0; i < slots.size(); ++i) {
                auto [lo, hi] = byUv.equal_range(uvKey(slots[i].uv[0], slots[i].uv[1]));
                // Shared texture coordinates: the first entry not taken yet.
                long pick = -1;
                for (auto it = lo; it != hi; ++it)
                    if (pick < 0 || (!used[it->second] && used[size_t(pick)]))
                        pick = long(it->second);
                if (pick >= 0) {
                    used[size_t(pick)] = true;
                    disp[i] = (*sv)[size_t(pick)].delta;
                    covered[i] = true;
                }
            }
        }
    } else if (!spec.archetype.empty()) {
        const SimPart* a = archetypeFor(spec.archetype, ageBit, genderBit);
        const Mesh* am = a ? meshOfEntry(*a, lowDetail) : nullptr;
        if (!am && lowDetail)
            am = a ? meshOfEntry(*a) : nullptr;
        if (!am)
            out.warnings.push_back("archetype " + spec.archetype + " has no " + ageName(ageBit) + " head");
        else
            for (size_t i = 0; i < slots.size(); ++i)
                if (hairneck[i])
                    covered[i] = true;
                else if (long j = am->match(slots[i].uv[0], slots[i].uv[1], slots[i].pos); j >= 0) {
                    disp[i] = sub(am->positions[size_t(j)], slots[i].pos);
                    covered[i] = true;
                }
    }
    for (const auto& [maskList, fam] : spec.regions) {
        const SimPart* a = archetypeFor(fam, ageBit, genderBit);
        const Mesh* am = a ? meshOfEntry(*a) : nullptr;
        if (!am) {
            out.warnings.push_back("region archetype " + fam + " has no " + ageName(ageBit) + " head");
            continue;
        }
        std::vector<float> w(slots.size(), 0.0f);
        size_t start = 0;
        while (start <= maskList.size()) {
            const size_t end = std::min(maskList.find(';', start), maskList.size());
            const std::string region = maskList.substr(start, end - start);
            start = end + 1;
            if (region.empty())
                continue;
            const SimPart* maskEntry = nullptr;
            for (const auto& e : catalog_.regions)
                if (e.name() == region && fits(e, ageBit, genderBit))
                    maskEntry = &e;
            const Mesh* mm = maskEntry ? meshOfEntry(*maskEntry) : nullptr;
            if (!mm) {
                out.warnings.push_back("no region mask " + region);
                continue;
            }
            for (size_t i = 0; i < slots.size(); ++i)
                if (long j = mm->match(slots[i].uv[0], slots[i].uv[1], slots[i].pos); j >= 0)
                    w[i] = std::max(w[i], mm->weights[size_t(j)]);
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            if (w[i] <= 0 || hairneck[i])
                continue;
            if (long j = am->match(slots[i].uv[0], slots[i].uv[1], slots[i].pos); j >= 0) {
                const Vec3 target = sub(am->positions[size_t(j)], slots[i].pos);
                disp[i] = add(scale(disp[i], 1 - w[i]), scale(target, w[i]));
                covered[i] = true;
            }
        }
    }
    for (const auto& [fam, weight] : spec.modifiers) {
        if (weight == 0)
            continue;
        const SimPart* mod = nullptr;
        for (const auto& e : catalog_.modifiers)
            if (family(e) == fam && (fits(e, ageBit, genderBit) || !mod))
                mod = &e;
        if (!mod) {
            out.warnings.push_back("no modifier " + fam);
            continue;
        }
        const Mesh* mm = meshOfEntry(*mod);
        const SimPart* cmp = catalog_.neutralFor(mod->number("compareage") ? mod->number("compareage") : age::Adult,
                                                 mod->number("comparegender") ? mod->number("comparegender")
                                                                              : gender::Female);
        const Mesh* nm = cmp ? meshOfEntry(*cmp) : nullptr;
        if (!mm || !nm) {
            out.warnings.push_back(mod->name() + ": mesh not found");
            continue;
        }
        const float lo = floatProperty(*mod, "negativelimit", -1.5f), hi = floatProperty(*mod, "positivelimit", 1.5f);
        const float w = std::clamp(weight, lo, hi);
        const float s = w * (w > 0 ? floatProperty(*mod, "positivescale", 1) : floatProperty(*mod, "negativescale", 1));
        for (size_t i = 0; i < slots.size(); ++i) {
            const long j = mm->match(slots[i].uv[0], slots[i].uv[1], add(slots[i].pos, disp[i]));
            if (j < 0)
                continue;
            const long k = nm->match(slots[i].uv[0], slots[i].uv[1], mm->positions[size_t(j)]);
            if (k < 0)
                continue;
            disp[i] = add(disp[i], scale(sub(mm->positions[size_t(j)], nm->positions[size_t(k)]), s));
            covered[i] = true;
        }
    }
    // Vertices no head covers follow the nearest covered one.
    for (size_t i = 0; i < slots.size(); ++i) {
        if (covered[i]) {
            ++out.matched;
            continue;
        }
        long best = -1;
        float bestD = 0;
        for (size_t j = 0; j < slots.size(); ++j)
            if (covered[j]) {
                const float d = dist2(slots[i].pos, slots[j].pos);
                if (best < 0 || d < bestD)
                    best = long(j), bestD = d;
            }
        if (best >= 0)
            disp[i] = disp[size_t(best)];
    }

    // Normals: the old normal turned by the change of the surface normal.
    std::vector<Vec3> oldN(slots.size(), Vec3{0, 0, 0}), newN(slots.size(), Vec3{0, 0, 0});
    for (const auto& gr : g.groups) {
        if (gr.primitiveType != 2 || gr.linkIndex < 0 || size_t(gr.linkIndex) >= links.size())
            continue;
        const LinkInfo& info = links[size_t(gr.linkIndex)];
        if (info.vertices < 0 || info.uvs < 0)
            continue;
        for (size_t f = 0; f + 2 < gr.faces.size(); f += 3) {
            size_t v[3];
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                if (gr.faces[f + c] >= info.count)
                    ok = false;
                v[c] = info.first + gr.faces[f + c];
            }
            if (!ok)
                continue;
            const Vec3 fo = cross(sub(slots[v[1]].pos, slots[v[0]].pos), sub(slots[v[2]].pos, slots[v[0]].pos));
            const Vec3 p0 = add(slots[v[0]].pos, disp[v[0]]), p1 = add(slots[v[1]].pos, disp[v[1]]),
                       p2 = add(slots[v[2]].pos, disp[v[2]]);
            const Vec3 fn = cross(sub(p1, p0), sub(p2, p0));
            for (size_t c : v) {
                oldN[c] = add(oldN[c], fo);
                newN[c] = add(newN[c], fn);
            }
        }
    }

    // The LxNR's order: every triangle corner of the mesh's groups, in group
    // and face order (the game's cMeshEntry lists: 2211 = 3 x 737 corners for
    // the adult faces, 1335 for their low detail).
    for (const auto& gr : g.groups) {
        if (gr.primitiveType != 2 || gr.linkIndex < 0 || size_t(gr.linkIndex) >= links.size())
            continue;
        const LinkInfo& info = links[size_t(gr.linkIndex)];
        if (info.vertices < 0 || info.uvs < 0)
            continue;
        for (uint32_t f : gr.faces)
            if (f < info.count) {
                const size_t i = info.first + f;
                out.structure.push_back({disp[i], slots[i].uv[0], slots[i].uv[1]});
            }
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        out.positions.push_back(slots[i].pos);
        out.displacements.push_back(disp[i]);
    }
    out.gmdc = t.bytes;
    auto put = [&](size_t offset, const Vec3& v) {
        for (int c = 0; c < 3; ++c) {
            const uint32_t w = std::bit_cast<uint32_t>(v[c]);
            for (int b = 0; b < 4; ++b)
                out.gmdc[offset + 4 * c + b] = uint8_t(w >> (8 * b));
        }
    };
    for (size_t i = 0; i < slots.size(); ++i) {
        const Slot& s = slots[i];
        if (disp[i][0] != 0 || disp[i][1] != 0 || disp[i][2] != 0)
            ++out.moved;
        const LinkInfo& info = links[s.link];
        const auto& ev = g.elements[size_t(info.vertices)];
        put(t.bodyOffset + ev.rawOffset + s.value * 12, add(s.pos, disp[i]));
        if (info.normals >= 0) {
            const auto& en = g.elements[size_t(info.normals)];
            if (s.value < en.valueCount()) {
                const Vec3 n = normalize(add(s.normal, sub(normalize(newN[i]), normalize(oldN[i]))));
                put(t.bodyOffset + en.rawOffset + s.value * 12, n);
            }
        }
    }
    return out;
}

std::vector<uint8_t> FaceBaker::deformToFace(const std::vector<uint8_t>& bytes, const Baked& face) {
    std::vector<uint8_t> out = bytes;
    if (face.positions.empty())
        return out;
    Rcol rc(bytes);
    const auto* block = rc.find("cGeometryDataContainer");
    if (!block)
        return out;
    const size_t bodyOffset = size_t(block->body.data() - bytes.data());
    const GeometryData g = GeometryData::parse(block->body);
    std::vector<bool> done(g.elements.size(), false);
    for (const auto& link : g.links)
        for (size_t k = 0; k < link.elements.size(); ++k) {
            const size_t ei = link.elements[k];
            if (ei >= g.elements.size() || done[ei] || (k < link.aliases.size() && !link.aliases[k].empty()))
                continue;
            const auto& e = g.elements[ei];
            if (e.identity != gmdc::Vertices || e.blockFormat != 2)
                continue;
            done[ei] = true;
            for (size_t i = 0; i < e.valueCount(); ++i) {
                const Vec3 p{e.f(i, 0), e.f(i, 1), e.f(i, 2)};
                size_t best = 0;
                float bestD = -1;
                for (size_t j = 0; j < face.positions.size(); ++j) {
                    const float d = dist2(p, face.positions[j]);
                    if (bestD < 0 || d < bestD)
                        best = j, bestD = d;
                }
                const Vec3 q = add(p, face.displacements[best]);
                for (int c = 0; c < 3; ++c) {
                    const uint32_t w = std::bit_cast<uint32_t>(q[c]);
                    for (int b = 0; b < 4; ++b)
                        out[bodyOffset + e.rawOffset + i * 12 + 4 * c + b] = uint8_t(w >> (8 * b));
                }
            }
        }
    return out;
}

FaceStructure FaceBaker::descendant(const FaceStructure& a, const FaceStructure& b,
                                    const std::vector<std::pair<std::string, int>>& regions) {
    FaceStructure out;
    out.version = 3;
    for (uint32_t key : a.keys) {
        auto ia = a.data.find(key);
        auto ib = b.data.find(key);
        if (ia == a.data.end() || ib == b.data.end() || ia->second.empty())
            continue;
        const int ageIdx = int(key & 0xFF);
        static const uint32_t kAges[6] = {age::Toddler, age::Child, age::Teen, age::Adult, age::Elder, age::Baby};
        const uint32_t ageBit = ageIdx < 6 ? kAges[ageIdx] : age::Adult;
        const uint32_t genderBit = (key & 0x100) ? gender::Male : gender::Female;
        const bool low = (key & 0xF0000) != 0;
        // b's deltas by texture coordinate.
        std::unordered_multimap<int64_t, size_t> byUv;
        for (size_t k = 0; k < ib->second.size(); ++k)
            byUv.emplace(uvKey(ib->second[k].u, ib->second[k].v), k);
        std::vector<FaceStructure::Vertex> verts = ia->second;
        std::vector<Vec3> fromB(verts.size(), Vec3{0, 0, 0});
        std::vector<bool> hasB(verts.size(), false);
        for (size_t k = 0; k < verts.size(); ++k) {
            auto it = byUv.find(uvKey(verts[k].u, verts[k].v));
            if (it != byUv.end())
                fromB[k] = ib->second[it->second].delta, hasB[k] = true;
        }
        for (size_t k = 0; k < verts.size(); ++k)
            if (hasB[k])
                verts[k].delta = scale(add(verts[k].delta, fromB[k]), 0.5f);
        for (const auto& [region, parent] : regions) {
            std::vector<std::string> masks{region};
            if (region == "eyes")
                masks.push_back("cheekbones");
            if (region == "mouth")
                masks.push_back("cheek");
            for (const auto& m : masks) {
                const SimPart* maskEntry = nullptr;
                for (const auto& e : catalog_.regions)
                    if (e.name() == m && fits(e, ageBit, genderBit))
                        maskEntry = &e;
                const Mesh* mm = maskEntry ? meshOfEntry(*maskEntry, low) : nullptr;
                if (!mm && maskEntry)
                    mm = meshOfEntry(*maskEntry);
                if (!mm)
                    continue;
                for (size_t k = 0; k < verts.size(); ++k) {
                    const long j = mm->match(verts[k].u, verts[k].v, Vec3{0, 0, 0});
                    if (j < 0)
                        continue;
                    const float w = mm->weights[size_t(j)];
                    if (w <= 0)
                        continue;
                    const Vec3 target = parent == 0 ? ia->second[k].delta : (hasB[k] ? fromB[k] : ia->second[k].delta);
                    verts[k].delta = add(scale(verts[k].delta, 1 - w), scale(target, w));
                }
            }
        }
        out.set(key, std::move(verts));
    }
    return out;
}

FaceStructure FaceBaker::structureOf(const FaceSpec& spec, uint32_t genderBit) {
    FaceStructure fs;
    fs.version = 3;
    const uint32_t ages[6] = {age::Toddler, age::Child, age::Teen, age::Adult, age::Elder, age::Baby};
    // The game's characters hold both genders' keys (0x000 and 0x100); the
    // other gender's face is the same face over that gender's neutral (inferred).
    for (uint32_t g : {gender::Female, gender::Male})
        for (uint32_t a : ages) {
            const bool has = spec.structure ? spec.structure->find(a, genderBit) != nullptr
                                            : (spec.archetype.empty() || archetypeFor(spec.archetype, a, genderBit));
            if (!has || !catalog_.neutralFor(a, g))
                continue;
            for (bool low : {false, true}) {
                try {
                    FaceSpec s = spec;
                    Baked b;
                    if (g == genderBit)
                        b = bake(s, a, g, low);
                    else {
                        // The face of this gender at its own neutral: the deltas carry over by UV.
                        Baked own = bake(s, a, genderBit, low);
                        auto tmp = std::make_shared<FaceStructure>();
                        tmp->set(FaceStructure::key(a, g, low), own.structure);
                        FaceSpec t;
                        t.structure = tmp;
                        b = bake(t, a, g, low);
                    }
                    if (!b.structure.empty())
                        fs.set(FaceStructure::key(a, g, low), std::move(b.structure));
                } catch (const std::exception& err) {
                    os2::logOnce("face structure", std::to_string(a) + ":" + std::to_string(g), err);
                }
            }
        }
    return fs;
}

} // namespace os2::cas
