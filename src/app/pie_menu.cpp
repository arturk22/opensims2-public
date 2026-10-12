#include "pie_menu.hpp"

#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#include "app/ui_panel_util.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <chrono>
#include <functional>
#include <map>

namespace os2 {

namespace {

using Color = std::array<float, 4>;
constexpr Color kLabel{0.07f, 0.12f, 0.30f, 0.85f};
constexpr Color kLabelHot{0.98f, 0.80f, 0.25f, 0.95f};
constexpr Color kText{1, 1, 1, 1};
constexpr Color kTextHot{0.05f, 0.08f, 0.25f, 1};
constexpr Color kTitle{0.0f, 0.0f, 0.0f, 0.55f};
constexpr Color kRunning{0.10f, 0.45f, 0.20f, 0.85f};
constexpr Color kCancelled{0.35f, 0.35f, 0.35f, 0.85f};
constexpr Color kDim{0, 0, 0, 0.35f};
constexpr float kPad = 8, kLabelH = 24, kLineH = 20, kDialogPad = 16;
// Pie items: the window's corner offsets of position_item assume a window
// about 42 high whose text starts 14 past its edge (inferred); the head's
// size is a guess too.
constexpr float kItemH = 42, kItemInset = 14, kHeadSize = 72;
constexpr Color kItemBack{0, 0, 0, 0.55f};

std::string objectName(const vm::Object* o) {
    if (!o || !o->definition)
        return "?";
    const std::string& n = o->definition->fileName;
    if (o->person)
        if (auto dash = n.find(" - "); dash != std::string::npos)
            return n.substr(dash + 3);
    return o->definition->title.empty() ? n : o->definition->title;
}

std::string join(const std::vector<std::string>& parts, size_t from) {
    std::string s;
    for (size_t i = from; i < parts.size(); ++i)
        s += (i > from ? "/" : "") + parts[i];
    return s;
}

// Draws text into the screen: a texture just large enough, as a quad at (x, y).
struct Painter {
    UiScreen& out;
    const Font* font;
    float ppem;

    float width(const std::string& s) const { return font ? measureText(*font, decodeUtf8(s), ppem).width : 7.0f * float(s.size()); }

    void rect(float x0, float y0, float x1, float y1, Color c) {
        UiQuad q;
        q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
        q.color = c;
        out.quads.push_back(q);
    }

    void text(float x, float y, const std::string& s, Color c) {
        if (!font || s.empty())
            return;
        const auto u = decodeUtf8(s);
        const TextMetrics m = measureText(*font, u, ppem);
        auto img = std::make_unique<RgbaImage>();
        img->width = uint32_t(std::ceil(m.width)) + 2;
        img->height = uint32_t(std::ceil(m.ascent + m.descent)) + 2;
        img->pixels.assign(size_t(img->width) * img->height * 4, 0);
        const uint8_t rgba[4] = {uint8_t(std::lround(c[0] * 255)), uint8_t(std::lround(c[1] * 255)),
                                 uint8_t(std::lround(c[2] * 255)), 255};
        for (size_t i = 0; i < img->pixels.size(); i += 4)
            std::copy_n(rgba, 3, &img->pixels[i]); // transparent pixels carry the colour (no dark fringes)
        drawText(*img, *font, u, 1, 1 + std::round(m.ascent), ppem, rgba);
        UiQuad q;
        q.image = img.get();
        q.x0 = std::round(x), q.y0 = std::round(y);
        q.x1 = q.x0 + float(img->width), q.y1 = q.y0 + float(img->height);
        out.quads.push_back(q);
        out.textImages.push_back(std::move(img));
        ++out.texts;
    }

    // A label: background box with the text centred vertically.
    void label(float x0, float y0, float x1, float y1, const std::string& s, Color bg, Color fg) {
        rect(x0, y0, x1, y1, bg);
        const float th = font ? std::ceil(measureText(*font, U"", ppem).ascent + measureText(*font, U"", ppem).descent) + 2 : 14;
        text(x0 + kPad, y0 + std::round((y1 - y0 - th) / 2), s, fg);
    }
};

} // namespace

std::pair<simd_float3, simd_float3> modelBounds(const ObjectModel& model) {
    simd_float3 lo = simd_float3(std::numeric_limits<float>::max()), hi = -lo;
    for (const auto& part : model.parts)
        for (const auto& p : part.mesh.positions) {
            const simd_float3 v = simd_make_float3(p[0], p[1], p[2]);
            lo = simd_min(lo, v), hi = simd_max(hi, v);
        }
    if (lo.x > hi.x)
        lo = hi = simd_float3(0.0f);
    return {lo, hi};
}

namespace {
// The alpha of an alpha-tested material's base texture, at most 64 texels a
// side (nearest), shared by every model that uses it.
std::optional<PickMesh::Mask> alphaMask(const ObjectModel& model, const std::string& material, ResourceIndex* index) {
    if (!index)
        return std::nullopt;
    const auto it = model.materials.find(material);
    if (it == model.materials.end())
        return std::nullopt;
    const StandardMaterial sm = standardMaterial(it->second);
    if (!sm.alphaTest)
        return std::nullopt;
    const std::string texture = baseTextureName(it->second);
    if (texture.empty())
        return std::nullopt;
    static std::map<std::string, std::optional<PickMesh::Mask>> cache;
    const std::string key = texture + "@" + std::to_string(int(sm.alphaRef * 255));
    if (auto c = cache.find(key); c != cache.end())
        return c->second;
    std::optional<PickMesh::Mask> out;
    try {
        constexpr uint32_t kTypeTxtr = 0x1C4A276C;
        if (auto loc = index->findByName(texture, kTypeTxtr)) {
            const RgbaImage img = loadTexture(index->read(*loc), index);
            if (img.width && img.height) {
                PickMesh::Mask m;
                m.width = std::min<uint32_t>(img.width, 64), m.height = std::min<uint32_t>(img.height, 64);
                m.ref = uint8_t(std::clamp(sm.alphaRef * 255.0f, 0.0f, 255.0f));
                m.alpha.resize(size_t(m.width) * m.height);
                for (uint32_t y = 0; y < m.height; ++y)
                    for (uint32_t x = 0; x < m.width; ++x) {
                        // The texel's area at most (a hole is a hole only where all of it is).
                        uint8_t a = 0;
                        const uint32_t x0 = x * img.width / m.width, x1 = std::max(x0 + 1, (x + 1) * img.width / m.width);
                        const uint32_t y0 = y * img.height / m.height, y1 = std::max(y0 + 1, (y + 1) * img.height / m.height);
                        for (uint32_t yy = y0; yy < y1; ++yy)
                            for (uint32_t xx = x0; xx < x1; ++xx)
                                a = std::max(a, img.pixels[(size_t(yy) * img.width + xx) * 4 + 3]);
                        m.alpha[size_t(y) * m.width + x] = a;
                    }
                out = std::move(m);
            }
        }
    } catch (const std::exception&) {
    }
    return cache[key] = out;
}
} // namespace

std::shared_ptr<const PickMesh> makePickMesh(const ObjectModel& model, ResourceIndex* index) {
    auto lower = [](std::string x) {
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return x;
    };
    struct Part {
        const ObjectModel::Part* part;
        float zLo, zHi;
    };
    std::vector<Part> parts;
    for (const auto& part : model.parts) {
        if (part.mesh.positions.empty() || part.mesh.triangles.size() < 3)
            continue;
        if (lower(part.mesh.name).find("shadow") != std::string::npos ||
            lower(part.material).find("shadow") != std::string::npos)
            continue;
        float zLo = std::numeric_limits<float>::max(), zHi = -zLo;
        for (const auto& p : part.mesh.positions)
            zLo = std::min(zLo, p[2]), zHi = std::max(zHi, p[2]);
        parts.push_back({&part, zLo, zHi});
    }
    // Flat on the ground (within 2 cm, under 10 cm): left out when the model
    // stands up otherwise (a rug is all flat and stays).
    auto flat = [](const Part& p) { return p.zHi - p.zLo < 0.02f && p.zHi < 0.1f; };
    const bool standing = std::any_of(parts.begin(), parts.end(), [&](const Part& p) { return !flat(p); });
    auto out = std::make_shared<PickMesh>();
    simd_float3 lo = simd_float3(std::numeric_limits<float>::max()), hi = -lo;
    for (const Part& p : parts) {
        if (standing && flat(p))
            continue;
        const auto& m = p.part->mesh;
        int mask = -1;
        if (!m.uvs.empty())
            if (auto am = alphaMask(model, p.part->material, index)) {
                mask = int(out->masks.size());
                out->masks.push_back(std::move(*am));
            }
        for (size_t i = 0; i + 2 < m.triangles.size(); i += 3) {
            const uint32_t v[3] = {m.triangles[i], m.triangles[i + 1], m.triangles[i + 2]};
            if (v[0] >= m.positions.size() || v[1] >= m.positions.size() || v[2] >= m.positions.size())
                continue;
            for (uint32_t k : v) {
                const simd_float3 c = simd_make_float3(m.positions[k][0], m.positions[k][1], m.positions[k][2]);
                out->corners.push_back(c);
                lo = simd_min(lo, c), hi = simd_max(hi, c);
                out->uvs.push_back(mask >= 0 && k < m.uvs.size() ? simd_make_float2(m.uvs[k][0], m.uvs[k][1])
                                                                 : simd_float2(0));
            }
            out->triangleMask.push_back(mask);
        }
    }
    if (out->corners.empty())
        return nullptr;
    out->lo = lo, out->hi = hi;
    return out;
}

std::optional<float> rayHit(const PickMesh& mesh, simd_float3 origin, simd_float3 dir) {
    // Moller-Trumbore, both faces.
    std::optional<float> best;
    for (size_t i = 0; i + 2 < mesh.corners.size(); i += 3) {
        const simd_float3 a = mesh.corners[i], e1 = mesh.corners[i + 1] - a, e2 = mesh.corners[i + 2] - a;
        const simd_float3 p = simd_cross(dir, e2);
        const float det = simd_dot(e1, p);
        if (std::abs(det) < 1e-12f)
            continue;
        const float inv = 1 / det;
        const simd_float3 s = origin - a;
        const float u = simd_dot(s, p) * inv;
        if (u < 0 || u > 1)
            continue;
        const simd_float3 q = simd_cross(s, e1);
        const float v = simd_dot(dir, q) * inv;
        if (v < 0 || u + v > 1)
            continue;
        const float t = simd_dot(e2, q) * inv;
        if (t <= 0 || (best && t >= *best))
            continue;
        if (const int mi = i / 3 < mesh.triangleMask.size() ? mesh.triangleMask[i / 3] : -1; mi >= 0) {
            const PickMesh::Mask& m = mesh.masks[size_t(mi)];
            const simd_float2 uv = mesh.uvs[i] * (1 - u - v) + mesh.uvs[i + 1] * u + mesh.uvs[i + 2] * v;
            auto wrap = [](float f) { return f - std::floor(f); };
            const uint32_t x = std::min(m.width - 1, uint32_t(wrap(uv.x) * float(m.width)));
            const uint32_t y = std::min(m.height - 1, uint32_t(wrap(uv.y) * float(m.height)));
            if (m.alpha[size_t(y) * m.width + x] < m.ref)
                continue; // a hole: the ray goes on
        }
        best = t;
    }
    return best;
}

namespace {
bool shadowPart(const ObjectModel::Part& part) {
    return part.mesh.name.find("shadow") != std::string::npos || part.material.find("shadow") != std::string::npos;
}
simd_float3 vec(const Vec3& v) { return simd_make_float3(v[0], v[1], v[2]); }
} // namespace

namespace {
simd_float3 corner(const ObjectModel& model, const PosedPick::Tri& t, int k) {
    const auto& m = model.parts[t.part].mesh;
    const uint32_t v = m.triangles[t.index + uint32_t(k)];
    return v < m.positions.size() ? vec(m.positions[v]) : simd_float3(0);
}
} // namespace

uint64_t shapeOf(const ObjectModel& model) {
    uint64_t h = model.parts.size();
    for (const auto& part : model.parts)
        h = h * 1000003u + part.mesh.triangles.size() * 31u + part.mesh.positions.size();
    return h;
}

PosedPick makePosedPick(const ObjectModel& model) {
    PosedPick out;
    out.shape = shapeOf(model);
    for (uint32_t pi = 0; pi < model.parts.size(); ++pi) {
        const auto& part = model.parts[pi];
        if (shadowPart(part))
            continue;
        for (uint32_t k = 0; k + 2 < part.mesh.triangles.size(); k += 3)
            out.tris.push_back({pi, k});
    }
    std::vector<simd_float3> centre(out.tris.size());
    for (size_t i = 0; i < out.tris.size(); ++i)
        centre[i] = (corner(model, out.tris[i], 0) + corner(model, out.tris[i], 1) + corner(model, out.tris[i], 2)) / 3;
    std::vector<uint32_t> order(out.tris.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    // Median split along the widest axis of the centres.
    auto build = [&](auto&& self, uint32_t first, uint32_t count) -> void {
        const size_t me = out.nodes.size();
        out.nodes.push_back({});
        if (count <= 8) {
            out.nodes[me].first = first, out.nodes[me].count = count;
            return;
        }
        simd_float3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (uint32_t i = first; i < first + count; ++i)
            lo = simd_min(lo, centre[order[i]]), hi = simd_max(hi, centre[order[i]]);
        const simd_float3 d = hi - lo;
        const int axis = d.x >= d.y && d.x >= d.z ? 0 : d.y >= d.z ? 1 : 2;
        const uint32_t half = count / 2;
        std::nth_element(order.begin() + first, order.begin() + first + half, order.begin() + first + count,
                         [&](uint32_t a, uint32_t b) { return centre[a][axis] < centre[b][axis]; });
        self(self, first, half);
        out.nodes[me].first = uint32_t(out.nodes.size()), out.nodes[me].count = 0;
        self(self, first + half, count - half);
    };
    if (!out.tris.empty())
        build(build, 0, uint32_t(out.tris.size()));
    std::vector<PosedPick::Tri> sorted(out.tris.size());
    for (size_t i = 0; i < order.size(); ++i)
        sorted[i] = out.tris[order[i]];
    out.tris = std::move(sorted);
    out.refit(model);
    return out;
}

void PosedPick::refit(const ObjectModel& model) {
    for (size_t n = nodes.size(); n-- > 0;) {
        Node& node = nodes[n];
        if (node.count) {
            node.lo = simd_float3(std::numeric_limits<float>::max()), node.hi = -node.lo;
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
                for (int k = 0; k < 3; ++k) {
                    const simd_float3 p = corner(model, tris[i], k);
                    node.lo = simd_min(node.lo, p), node.hi = simd_max(node.hi, p);
                }
        } else { // children: n + 1 and first, both refitted already
            node.lo = simd_min(nodes[n + 1].lo, nodes[node.first].lo);
            node.hi = simd_max(nodes[n + 1].hi, nodes[node.first].hi);
        }
    }
}

std::optional<float> rayHit(const ObjectModel& model, const PosedPick& tree, simd_float3 origin, simd_float3 dir) {
    std::optional<float> best;
    if (tree.nodes.empty())
        return best;
    const simd_float3 inv = 1 / dir; // infinities where dir is 0 work in the slab test
    auto enters = [&](const PosedPick::Node& n) {
        const simd_float3 a = (n.lo - origin) * inv, b = (n.hi - origin) * inv;
        const simd_float3 tmin = simd_min(a, b), tmax = simd_max(a, b);
        const float t0 = std::max({tmin.x, tmin.y, tmin.z, 0.0f}), t1 = std::min({tmax.x, tmax.y, tmax.z});
        return t0 <= t1 && !(best && t0 >= *best);
    };
    uint32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top) {
        const PosedPick::Node& n = tree.nodes[stack[--top]];
        if (!enters(n))
            continue;
        if (!n.count) {
            if (top + 2 <= 64) {
                stack[top++] = n.first;
                stack[top++] = uint32_t(&n - tree.nodes.data()) + 1;
            }
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i) {
            const simd_float3 p0 = corner(model, tree.tris[i], 0);
            const simd_float3 e1 = corner(model, tree.tris[i], 1) - p0, e2 = corner(model, tree.tris[i], 2) - p0;
            const simd_float3 p = simd_cross(dir, e2);
            const float det = simd_dot(e1, p);
            if (std::abs(det) < 1e-12f)
                continue;
            const float id = 1 / det;
            const simd_float3 sv = origin - p0;
            const float u = simd_dot(sv, p) * id;
            if (u < 0 || u > 1)
                continue;
            const simd_float3 q = simd_cross(sv, e1);
            const float v = simd_dot(dir, q) * id;
            if (v < 0 || u + v > 1)
                continue;
            const float d = simd_dot(e2, q) * id;
            if (d > 0 && (!best || d < *best))
                best = d;
        }
    }
    return best;
}

PieMenuStyle PieMenuStyle::fromConstants(const std::map<std::string, std::string>& c) {
    PieMenuStyle st;
    auto number = [&](const char* key, float& out) {
        if (auto it = c.find(key); it != c.end())
            try {
                out = std::stof(it->second); // "1.6f" reads as 1.6
            } catch (const std::exception&) {
            }
    };
    auto color = [&](const char* key, std::array<float, 4>& out) {
        auto it = c.find(key);
        int r, g, b;
        if (it != c.end() && std::sscanf(it->second.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
            out = {float(r) / 255, float(g) / 255, float(b) / 255, 1};
    };
    auto text = [&](const char* key, std::string& out) {
        if (auto it = c.find(key); it != c.end() && !it->second.empty())
            out = it->second;
    };
    number("kItemRadius", st.itemRadius);
    number("kCancelBoundary", st.cancelBoundary);
    number("kGesturePickBoundary", st.gesturePickBoundary);
    number("kHeadAreaInflateForItemOverlap", st.headInflate);
    color("kPieMenuTextColorNormal", st.textNormal);
    color("kPieMenuTextColorHighlited", st.textHighlight);
    text("kPieMenuFont", st.font);
    text("kMainButtonSound", st.mainSound);
    text("kSubmenuButtonSound", st.submenuSound);
    if (auto it = c.find("kEnableItemAnimations"); it != c.end())
        st.itemAnimations = it->second == "true";
    return st;
}

void PieMenuHost::setPortrait(RgbaImage img) {
    // The face: a square from the top middle of the 256x256 shot, in a
    // circle (the game's callout box is not decoded; inferred).
    if (img.width == 0 || img.height == 0) {
        head_.reset();
        return;
    }
    const int S = int(kHeadSize) * 2; // drawn at half size: smooth edges on Retina
    const float side = float(std::min(img.width, img.height)) * 0.7f;
    const float ox = float(img.width) * 0.48f - side / 2, oy = float(img.height) * 0.42f - side / 2;
    auto out = std::make_unique<RgbaImage>();
    out->width = out->height = uint32_t(S);
    out->pixels.assign(size_t(S) * S * 4, 0);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const float dx = float(x) + 0.5f - S / 2.0f, dy = float(y) + 0.5f - S / 2.0f;
            const float r = std::sqrt(dx * dx + dy * dy), edge = S / 2.0f;
            if (r > edge)
                continue;
            const int sx = std::clamp(int(ox + (float(x) + 0.5f) * side / float(S)), 0, int(img.width) - 1);
            const int sy = std::clamp(int(oy + (float(y) + 0.5f) * side / float(S)), 0, int(img.height) - 1);
            const uint8_t* src = &img.pixels[(size_t(sy) * img.width + size_t(sx)) * 4];
            uint8_t* dst = &out->pixels[(size_t(y) * size_t(S) + size_t(x)) * 4];
            // A thin light ring around it.
            const bool ring = r > edge - 3;
            for (int k = 0; k < 3; ++k)
                dst[k] = ring ? uint8_t(std::lround(style_.textNormal[size_t(k)] * 255)) : src[k];
            dst[3] = uint8_t(std::lround(std::clamp(edge - r, 0.0f, 1.0f) * 255));
        }
    head_ = std::move(out);
    dirty_ = true;
}

PieMenuHost::PieMenuHost(vm::VM& vm, const std::filesystem::path& fontsDir, const std::string& simQuery)
    : vm_(&vm), fonts_(std::make_unique<FontLibrary>(fontsDir)) {
    sim_ = vm::findSim(vm, simQuery);
    dlg_ = std::make_unique<DialogWindow>(vm, nullptr, fonts_.get());
}

PieMenuHost::~PieMenuHost() = default;

namespace {
// The world box around a model-space box under a placement.
std::pair<simd_float3, simd_float3> transformBox(simd_float3 lo, simd_float3 hi, const simd_float4x4& at) {
    simd_float3 wlo = simd_float3(std::numeric_limits<float>::max()), whi = -wlo;
    for (int c = 0; c < 8; ++c) {
        const simd_float4 p = simd_mul(at, simd_make_float4(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z, 1));
        wlo = simd_min(wlo, p.xyz), whi = simd_max(whi, p.xyz);
    }
    return {wlo, whi};
}
} // namespace

void PieMenuHost::addPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi, const simd_float4x4& at,
                             std::shared_ptr<const PickMesh> mesh) {
    const auto id = vm::ObjectId(lotInstance & 0xFFFF);
    if (!vm_->object(id) || simd_equal(lo, hi))
        return;
    if (mesh) // the triangles' bounds (no shadow planes) are the box
        lo = mesh->lo, hi = mesh->hi;
    const auto [wlo, whi] = transformBox(lo, hi, at);
    PickBox b{id, wlo, whi, false, std::move(mesh), at};
    b.modelLo = lo, b.modelHi = hi;
    boxes_.push_back(std::move(b));
}

void PieMenuHost::addFollowingPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi,
                                      std::shared_ptr<const PickMesh> mesh) {
    const auto id = vm::ObjectId(lotInstance & 0xFFFF);
    if (!vm_->object(id) || simd_equal(lo, hi))
        return;
    if (mesh)
        lo = mesh->lo, hi = mesh->hi;
    PickBox b{id, lo, hi, true, std::move(mesh)};
    b.modelLo = lo, b.modelHi = hi;
    boxes_.push_back(std::move(b));
}

std::optional<PieMenuHost::PickBox> PieMenuHost::worldBox(const PickBox& b) const {
    // Where the object is drawn now (the VM moves objects; contained ones
    // follow their container); a static box where it was saved otherwise.
    const auto at = placement_ ? placement_(b.id) : std::nullopt;
    if (!b.follows && !at)
        return b;
    if (!at)
        return std::nullopt;
    const auto [wlo, whi] = transformBox(b.modelLo, b.modelHi, *at);
    PickBox w = b;
    w.lo = wlo, w.hi = whi, w.at = *at, w.follows = false;
    return w;
}

void PieMenuHost::setMovingBox(vm::ObjectId id, simd_float3 lo, simd_float3 hi, const ObjectModel* posed) {
    if (simd_equal(lo, hi)) {
        moving_.erase(id);
        return;
    }
    PickBox b{id, lo, hi};
    b.posed = posed;
    moving_[id] = std::move(b);
}

void PieMenuHost::setPosedObject(vm::ObjectId id, simd_float3 lo, simd_float3 hi, const ObjectModel* posed) {
    if (simd_equal(lo, hi) || !posed) {
        posedObjects_.erase(id);
        return;
    }
    PickBox b{id, lo, hi};
    b.posed = posed;
    posedObjects_[id] = std::move(b);
}

namespace {
constexpr float kSmallObject = 2.0f;      // an object whose box is at most this long (m) is picked with a tolerance
constexpr float kPickTolerance = 0.0045f; // rad: about 5 px at the default view (the ray's angle off the mouse's)
} // namespace

vm::ObjectId PieMenuHost::pick(simd_float3 origin0, simd_float3 dir0) const {
    // Pass 1 is the exact one: the ray as is. Pass 2, only if pass 1 found
    // nothing, picks small objects (a mailbox, a lamp, a phone) by rays a
    // few pixels around the mouse (`kPickTolerance`): their thin parts are a
    // couple of pixels wide and a click on the mailbox gave Go Here (live QA
    // 15). Inferred: the game's own tolerance is not known.
    simd_float3 origin = origin0, dir = dir0;
    bool tolerant = false; // pass 2: only small objects
    // The nearest hit: an object's triangles where it has them (its box
    // first, as a quick test), else its box; a Sim's skinned triangles. The
    // binary takes the scene graph's nearest node, Sim or object, and stops
    // at a floor (ObjectAtGameWindowPoint): nothing past the floor counts
    // (a little slack for what lies flat on it).
    std::optional<float> floorT = floorHit ? floorHit(origin, dir) : std::nullopt;
    vm::ObjectId best = 0;
    float bestT = floorT ? *floorT + 0.05f : std::numeric_limits<float>::max();
    auto nearest = [&](auto&& each) {
        each([&](const PickBox& b) {
            if (pickable && !pickable(b.lo, b.hi))
                return;
            if (tolerant && simd_reduce_max(b.hi - b.lo) > kSmallObject)
                return;
            // Slab test.
            float t0 = 0, t1 = std::numeric_limits<float>::max();
            bool hit = true;
            for (int k = 0; k < 3 && hit; ++k) {
                if (std::abs(dir[k]) < 1e-8f) {
                    hit = origin[k] >= b.lo[k] && origin[k] <= b.hi[k];
                    continue;
                }
                float a = (b.lo[k] - origin[k]) / dir[k], c = (b.hi[k] - origin[k]) / dir[k];
                if (a > c)
                    std::swap(a, c);
                t0 = std::max(t0, a), t1 = std::min(t1, c);
                hit = t0 <= t1;
            }
            if (!hit || t0 >= bestT)
                return;
            static const bool boxesOnly = std::getenv("OS2_PICK_BOXES") != nullptr; // the old picking, to compare
            if (b.posed && !boxesOnly) { // a Sim: its skinned triangles as drawn
                // The tree built once per model (same triangles every pose), refitted once a pose.
                auto tree = posedTrees_.find(b.posed);
                if (tree == posedTrees_.end() || tree->second.shape != shapeOf(*b.posed)) {
                    posedTrees_[b.posed] = makePosedPick(*b.posed);
                    tree = posedTrees_.find(b.posed);
                    b.fitted = true;
                }
                if (!b.fitted) {
                    tree->second.refit(*b.posed);
                    b.fitted = true;
                }
                const auto t = rayHit(*b.posed, tree->second, origin, dir);
                if (t && *t < bestT)
                    bestT = *t, best = b.id;
                return;
            }
            if (b.mesh && !boxesOnly) {
                // The ray in the model's space: t keeps its meaning (an
                // affine map takes the ray's points to the ray's points).
                const simd_float4x4 inv = simd_inverse(b.at);
                const simd_float3 o = simd_mul(inv, simd_make_float4(origin, 1)).xyz;
                const simd_float3 d = simd_mul(inv, simd_make_float4(dir, 0)).xyz;
                const auto t = rayHit(*b.mesh, o, d);
                if (std::getenv("OS2_DEBUG_PICK"))
                    std::fprintf(stderr, "pick: box of %d entered at %.2f, %zu triangles: %s %.2f (at %.2f %.2f %.2f; model ray %.2f %.2f %.2f; mesh %.2f %.2f %.2f - %.2f %.2f %.2f)\n", b.id, t0,
                                 b.mesh->corners.size() / 3, t ? "hit at" : "missed", t ? *t : 0.0f, b.at.columns[3].x,
                                 b.at.columns[3].y, b.at.columns[3].z, (o + d * t0).x, (o + d * t0).y, (o + d * t0).z, b.mesh->lo.x, b.mesh->lo.y, b.mesh->lo.z, b.mesh->hi.x, b.mesh->hi.y, b.mesh->hi.z);
                if (t && *t < bestT)
                    bestT = *t, best = b.id;
                return;
            }
            if (t0 < bestT - 1e-4f)
                bestT = t0, best = b.id;
        });
    };
    auto objects = [&](auto&& f) {
        for (const auto& box : boxes_)
            if (!moving_.count(box.id) && !posedObjects_.count(box.id) && (!drawn || drawn(box.id)))
                if (const auto placed = worldBox(box))
                    f(*placed);
        // Animated objects by their triangles as posed now.
        for (const auto& [id, b] : posedObjects_)
            if (!moving_.count(id) && (!drawn || drawn(id)))
                f(b);
    };
    nearest([&](auto&& f) {
        for (const auto& [id, b] : moving_)
            f(b);
    });
    nearest(objects);
    if (best || simd_length(dir0) == 0)
        return best;
    // Pass 2: the ring of rays (8 around the mouse's, a pick radius of
    // kPickTolerance pixels at the object's distance), the nearest hit wins.
    tolerant = true;
    simd_float3 side = simd_cross(dir0, simd_make_float3(0, 0, 1));
    if (simd_length(side) < 1e-4f)
        side = simd_make_float3(1, 0, 0);
    side = simd_normalize(side);
    const simd_float3 up = simd_normalize(simd_cross(side, dir0));
    float bestAll = std::numeric_limits<float>::max();
    vm::ObjectId found = 0;
    for (int ring = 0; ring < 8; ++ring) {
        const float a = float(ring) * 3.14159265f / 4;
        // `nearest` reads origin and dir by reference; the angle is the
        // ray's, in the plane across the ray at distance t from the eye.
        dir = simd_normalize(dir0 + (side * std::cos(a) + up * std::sin(a)) * kPickTolerance);
        best = 0;
        bestT = floorT ? *floorT + 0.05f : std::numeric_limits<float>::max();
        nearest(objects);
        if (best && bestT < bestAll)
            bestAll = bestT, found = best;
    }
    return found;
}

std::optional<simd_float3> PieMenuHost::anchor(vm::ObjectId id) const {
    if (auto it = moving_.find(id); it != moving_.end())
        return simd_make_float3((it->second.lo.x + it->second.hi.x) / 2, (it->second.lo.y + it->second.hi.y) / 2,
                                it->second.hi.z);
    for (const auto& box : boxes_)
        if (box.id == id)
            if (const auto b = worldBox(box))
                return simd_make_float3((b->lo.x + b->hi.x) / 2, (b->lo.y + b->hi.y) / 2, b->hi.z);
    return std::nullopt;
}

bool PieMenuHost::open(vm::ObjectId object, simd_float2 at, vm::ObjectId merged) {
    close();
    if (!sim_ || (simAway && simAway(sim_)))
        return false;
    for (const vm::ObjectId id : {object, merged})
        if (id)
            for (auto& e : vm::pieMenu(*vm_, sim_, id))
                if (e.available() && !e.debug)
                    entries_.push_back(std::move(e));
    if (entries_.empty())
        return false;
    object_ = object;
    merged_ = merged;
    title_ = objectName(vm_->object(entries_.front().object));
    at_ = at;
    open_ = true;
    gesture_ = true;
    dirty_ = true;
    return true;
}

void PieMenuHost::close() {
    const bool was = open_;
    open_ = false;
    if (was && onClose) {
        onClose(object_);
        if (merged_)
            onClose(merged_);
    }
    merged_ = 0;
    entries_.clear();
    submenu_.clear();
    gesture_ = false;
    hover_ = chosen_ = -1;
    dirty_ = true;
}

PieMenuHost::Layout PieMenuHost::layout() const {
    Layout lay;
    std::vector<Label>& out = lay.labels;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& path = entries_[i].path;
        std::string text;
        if (submenu_.empty())
            text = path[0];
        else if (path.size() > 1 && path[0] == submenu_)
            text = join(path, 1);
        else
            continue;
        // Entries of the same submenu share a label; leaves never do (a bed has two "Dormir").
        const bool sub = submenu_.empty() && path.size() > 1;
        auto it = std::find_if(out.begin(), out.end(), [&](const Label& l) {
            return sub && l.text == text && entries_[l.entries.front()].path.size() > 1;
        });
        if (it == out.end())
            out.push_back({text, {i}});
        else
            it->entries.push_back(i);
    }
    const FontStyle* st = fonts_->style(style_.font);
    if (!st)
        st = fonts_->style("piemenu");
    const Font* font = st ? fonts_->font(*st) : nullptr;
    const float ppem = st ? float(st->size) : 14.0f;
    const simd_float2 c = submenu_.empty() ? at_ : subAt_;
    const size_t n = out.size();
    const float R = style_.itemRadius;
    for (size_t i = 0; i < n; ++i) {
        const float tw = font ? measureText(*font, decodeUtf8(out[i].text), ppem).width : 7.0f * float(out[i].text.size());
        const float w = std::round(tw) + 2 * kItemInset;
        // cTSWinPieMenu::layout: from the top, clockwise.
        const float a = 2 * float(M_PI) * float(i) / float(std::max<size_t>(n, 1));
        const float px = std::round(c.x + R * std::sin(a)), py = std::round(c.y - R * std::cos(a));
        // determine_item / position_item: the window's corner by the angle.
        const float deg = a * 180 / float(M_PI);
        float x, y;
        if (deg < 1e-3f)
            x = px - w / 2, y = py - 48;
        else if (deg < 45)
            x = px - kItemInset, y = py - 26;
        else if (deg <= 135)
            x = px - kItemInset, y = py - 21;
        else if (deg <= 165)
            x = px - kItemInset, y = py - 16;
        else if (deg < 195)
            x = px - w / 2, y = py - 21;
        else if (deg < 225)
            x = px - (w - kItemInset), y = py - 16;
        else if (deg <= 315)
            x = px - (w - kItemInset), y = py - 21;
        else
            x = px - (w - kItemInset), y = py - 26;
        out[i].px = px, out[i].py = py;
        out[i].x0 = std::round(x), out[i].x1 = out[i].x0 + w;
        out[i].y0 = std::round(y + (kItemH - kLabelH) / 2), out[i].y1 = out[i].y0 + kLabelH;
    }
    // fit_on_screen: the whole menu moved inside the canvas.
    lay.cx = c.x, lay.cy = c.y;
    if (canvasW_ > 0 && canvasH_ > 0) {
        float x0 = c.x - kHeadSize / 2, y0 = c.y - kHeadSize / 2, x1 = c.x + kHeadSize / 2, y1 = c.y + kHeadSize / 2;
        for (const auto& l : out)
            x0 = std::min(x0, l.x0), y0 = std::min(y0, l.y0), x1 = std::max(x1, l.x1), y1 = std::max(y1, l.y1);
        float dx = 0, dy = 0;
        if (x1 > float(canvasW_))
            dx = float(canvasW_) - x1;
        if (x0 + dx < 0)
            dx = -x0;
        if (y1 > float(canvasH_))
            dy = float(canvasH_) - y1;
        if (y0 + dy < 0)
            dy = -y0;
        for (auto& l : out)
            l.x0 += dx, l.x1 += dx, l.px += dx, l.y0 += dy, l.y1 += dy, l.py += dy;
        lay.cx += dx, lay.cy += dy;
    }
    return lay;
}

int PieMenuHost::labelAt(const Layout& l, simd_float2 p) const {
    for (size_t i = 0; i < l.labels.size(); ++i) {
        const auto& b = l.labels[i];
        if (p.x >= b.x0 && p.x < b.x1 && p.y >= b.y0 && p.y < b.y1)
            return int(i);
    }
    return -1;
}

bool PieMenuHost::headAt(const Layout& l, simd_float2 p) const {
    return std::fabs(p.x - l.cx) <= kHeadSize / 2 && std::fabs(p.y - l.cy) <= kHeadSize / 2;
}

void PieMenuHost::hover(simd_float2 p) {
    if (const vm::Dialog* d = dialog()) {
        dlg_->hover(*d, p.x, p.y, canvasW_ ? canvasW_ : 1280, canvasH_ ? canvasH_ : 720);
        dirty_ = true;
        return;
    }
    if (!open_)
        return;
    const int h = labelAt(layout(), p);
    if (h != hover_)
        hover_ = h, dirty_ = true;
}

bool PieMenuHost::pickLabel(const Layout& lay, int index, bool keepOpen) {
    const Label& l = lay.labels[size_t(index)];
    const auto& first = entries_[l.entries.front()];
    if (submenu_.empty() && first.path.size() > 1) {
        // A child pie centred on the item.
        submenu_ = l.text;
        subAt_ = simd_make_float2(l.px, l.py);
        hover_ = -1;
        if (playSound_)
            playSound_(style_.submenuSound);
    } else {
        if (playSound_)
            playSound_(style_.mainSound);
        queueEntry(l.entries.front());
        if (!keepOpen)
            close();
    }
    dirty_ = true;
    return true;
}

bool PieMenuHost::press(simd_float2) { return open_; }

bool PieMenuHost::release(simd_float2 p, bool keepOpen) {
    if (!open_)
        return false;
    const Layout lay = layout();
    const int hit = labelAt(lay, p);
    if (gesture_) {
        // The release of the press that opened the menu.
        gesture_ = false;
        const float d = simd_distance(p, at_);
        if (d <= style_.cancelBoundary)
            return true; // a plain click: the menu stays
        if (d >= style_.gesturePickBoundary && hit >= 0)
            return pickLabel(lay, hit, keepOpen);
        close();
        return true;
    }
    if (hit >= 0)
        return pickLabel(lay, hit, keepOpen);
    if (!submenu_.empty() && headAt(lay, p)) {
        submenu_.clear(), hover_ = -1, dirty_ = true; // the child's "back"
        return true;
    }
    close();
    return true;
}

bool PieMenuHost::click(simd_float2 p, bool keepOpen) {
    if (dialogOpen()) {
        clickDialog(p);
        return true; // modal
    }
    if (!open_)
        return false;
    gesture_ = false;
    press(p);
    return release(p, keepOpen);
}

bool PieMenuHost::select(size_t index) {
    if (!open_ || index >= entries_.size())
        return false;
    const auto& e = entries_[index];
    submenu_.clear();
    if (e.path.size() > 1) {
        const Layout top = layout();
        for (const auto& l : top.labels)
            if (l.text == e.path[0])
                subAt_ = simd_make_float2(l.px, l.py);
        submenu_ = e.path[0];
    }
    chosen_ = int(index);
    gesture_ = false;
    queueEntry(index);
    dirty_ = true;
    return true;
}

void PieMenuHost::queueEntry(size_t index) {
    vm::Object* sim = vm_->object(sim_);
    auto q = vm::interactionFromPie(*vm_, entries_[index]);
    if (!sim || !q)
        return;
    q->pushedBy = sim_;
    try {
        vm::enqueue(*vm_, *sim, *q);
        std::printf("opensims2: queued \"%s\" (%s, TTA %u) for %s\n", q->name.c_str(), title_.c_str(), q->interaction,
                    objectName(sim).c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
    }
    std::fflush(stdout);
}

std::string PieMenuHost::queueText(const vm::QueuedInteraction& q) const {
    const std::string name = q.name.empty() ? "#" + std::to_string(q.interaction) : q.name;
    return objectName(vm_->object(q.object)) + ": " + name;
}


// --- The dialog window (app/dialog_window.hpp) -----------------------------------

void PieMenuHost::setResources(ResourceIndex& index, const std::filesystem::path& resDir) {
    dlgRes_ = std::make_unique<UiResources>(index, resDir);
    auto thumbs = dlg_ ? dlg_->thumbnail : nullptr;
    dlg_ = std::make_unique<DialogWindow>(*vm_, dlgRes_.get(), fonts_.get());
    dlg_->thumbnail = thumbs;
    dirty_ = true;
}

void PieMenuHost::setDialogThumbnails(std::function<const RgbaImage*(int16_t, int16_t)> f) {
    dlg_->thumbnail = std::move(f);
    dirty_ = true;
}

bool PieMenuHost::dialogHasTextBox() const {
    const vm::Dialog* d = dialog();
    return d && DialogWindow::hasTextBox(*d);
}

bool PieMenuHost::dialogKey(int keyCode, const std::string& typed) {
    const vm::Dialog* d = dialog();
    if (!d)
        return false;
    dirty_ = true;
    return dlg_->key(const_cast<vm::Dialog&>(*d), keyCode, typed);
}

void PieMenuHost::setDialogText(const std::string& text) {
    if (const vm::Dialog* d = dialog(); d && DialogWindow::hasTextBox(*d)) {
        DialogWindow::editText(const_cast<vm::Dialog&>(*d)) = text;
        dirty_ = true;
    }
}

const vm::Dialog* PieMenuHost::dialog() const {
    for (const auto& [who, d] : vm_->dialogs)
        if (d.answer < 0)
            return &d;
    return nullptr;
}

PieMenuHost::DialogLayout PieMenuHost::dialogLayout(const vm::Dialog& d, int width, int height) const {
    const FontStyle* st = fonts_->style("piemenu");
    const Font* font = st ? fonts_->font(*st) : nullptr;
    const float ppem = st ? float(st->size) : 14.0f;
    auto measure = [&](const std::string& s) {
        return font ? measureText(*font, decodeUtf8(s), ppem).width : 7.0f * float(s.size());
    };
    DialogLayout l;
    const float w = std::min(480.0f, float(width) - 40), inner = w - 2 * kDialogPad;
    // Words wrapped to the panel; the message's own line breaks kept.
    std::string para;
    auto flush = [&] {
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            const size_t sp = std::min(para.find(' ', i), para.size());
            const std::string word = para.substr(i, sp - i);
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure(next) > inner) {
                l.lines.push_back(line);
                line = word;
            } else {
                line = next;
            }
            i = sp + 1;
        }
        l.lines.push_back(line);
        para.clear();
    };
    for (char ch : d.message) {
        if (ch == '\r')
            continue;
        if (ch == '\n')
            flush();
        else
            para += ch;
    }
    flush();
    while (!l.lines.empty() && l.lines.back().empty())
        l.lines.pop_back();
    // The title bar only when the dialog has a title (most yes/no dialogs have none).
    const float h = (d.title.empty() ? kDialogPad - 12 : kLabelH) + 12 + float(l.lines.size()) * kLineH + 16 + kLabelH +
                    kDialogPad;
    l.x0 = std::round((float(width) - w) / 2), l.x1 = l.x0 + w;
    l.y0 = std::round((float(height) - h) / 2), l.y1 = l.y0 + h;
    // Buttons in a row, centred at the bottom.
    std::vector<float> bw;
    float total = 0;
    for (const auto& b : d.buttons) {
        bw.push_back(std::max(100.0f, measure(b) + 2 * kPad));
        total += bw.back();
    }
    total += 12 * float(bw.empty() ? 0 : bw.size() - 1);
    float x = std::round((l.x0 + l.x1 - total) / 2);
    const float by = l.y1 - kDialogPad - kLabelH;
    for (float bwi : bw) {
        l.buttons.push_back({x, by, x + bwi, by + kLabelH});
        x += bwi + 12;
    }
    return l;
}

bool PieMenuHost::clickDialog(simd_float2 p) {
    const vm::Dialog* d = dialog();
    if (!d)
        return false;
    const int cw = canvasW_ ? canvasW_ : 1280, ch = canvasH_ ? canvasH_ : 720;
    dirty_ = true;
    return dlg_->click(const_cast<vm::Dialog&>(*d), p.x, p.y, cw, ch);
}

UiScreen PieMenuHost::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    const FontStyle* st = fonts_->style("piemenu");
    Painter paint{out, st ? fonts_->font(*st) : nullptr, st ? float(st->size) : 14.0f};

    // The selected Sim's queue, top left.
    if (const vm::Object* sim = showQueue_ ? vm_->object(sim_) : nullptr) {
        float y = 12;
        const std::string head = "Fila de " + objectName(sim);
        paint.label(12, y, 12 + paint.width(head) + 2 * kPad, y + kLabelH, head, kTitle, kText);
        y += kLabelH + 4;
        if (sim->queue.empty()) {
            paint.label(12, y, 12 + paint.width("(vazia)") + 2 * kPad, y + kLabelH, "(vazia)", kLabel, kText);
        }
        for (size_t i = 0; i < sim->queue.size(); ++i) {
            const auto& q = sim->queue[i];
            const std::string s = queueText(q);
            const Color bg = q.cancelled ? kCancelled : int(i) == sim->activeInteraction ? kRunning : kLabel;
            paint.label(12, y, 12 + paint.width(s) + 2 * kPad, y + kLabelH, s, bg, kText);
            y += kLabelH + 4;
        }
    }

    if (open_) {
        const Layout lay = layout();
        // The head in the middle (the child pie's "back").
        const float h = kHeadSize / 2;
        if (head_) {
            UiQuad q;
            q.image = head_.get();
            q.x0 = std::round(lay.cx - h), q.y0 = std::round(lay.cy - h);
            q.x1 = q.x0 + kHeadSize, q.y1 = q.y0 + kHeadSize;
            out.quads.push_back(q);
        } else {
            paint.rect(lay.cx - h / 2, lay.cy - h / 2, lay.cx + h / 2, lay.cy + h / 2, kItemBack);
        }
        const float th = paint.font ? std::ceil(measureText(*paint.font, U"", paint.ppem).ascent +
                                                measureText(*paint.font, U"", paint.ppem).descent) + 2
                                    : 14;
        for (size_t i = 0; i < lay.labels.size(); ++i) {
            const auto& l = lay.labels[i];
            const bool hot = int(i) == hover_ ||
                             (chosen_ >= 0 && std::find(l.entries.begin(), l.entries.end(), size_t(chosen_)) != l.entries.end());
            paint.rect(l.x0, l.y0, l.x1, l.y1, kItemBack);
            paint.text(l.x0 + kItemInset, l.y0 + std::round((l.y1 - l.y0 - th) / 2), l.text,
                       hot ? style_.textHighlight : style_.textNormal);
        }
    }

    // The dialog waiting for the player, over everything.
    if (const vm::Dialog* d = dialog()) {
        paint.rect(0, 0, float(width), float(height), kDim);
        if (dlgRes_) {
            dlg_->draw(out, *d, width, height);
            return out;
        }
        const DialogLayout l = dialogLayout(*d, width, height);
        paint.rect(l.x0, l.y0, l.x1, l.y1, kLabel);
        // The dialog's own title string (byte 6, or temp 3), never the
        // editor's label of its type ("yes-no"): without one there is no
        // title bar, as 172 of the 176 yes/no dialogs of objects.package.
        float y = l.y0 + kDialogPad;
        if (!d->title.empty()) {
            paint.label(l.x0, l.y0, l.x1, l.y0 + kLabelH, d->title, kTitle, kText);
            y = l.y0 + kLabelH + 12;
        }
        for (const auto& line : l.lines) {
            paint.text(l.x0 + kDialogPad, y, line, kText);
            y += kLineH;
        }
        for (size_t i = 0; i < l.buttons.size(); ++i) {
            const auto& b = l.buttons[i];
            const float tw = paint.width(d->buttons[i]);
            paint.rect(b[0], b[1], b[2], b[3], kLabelHot);
            paint.text(b[0] + std::round((b[2] - b[0] - tw) / 2), b[1] + 2, d->buttons[i], kTextHot);
        }
    }
    return out;
}

void PieMenuHost::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void PieMenuHost::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != canvasW_ || ch != canvasH_)
        canvasW_ = cw, canvasH_ = ch, dirty_ = true;
    // The queue changes as the VM runs.
    uint64_t stamp = 1469598103934665603ull;
    if (const vm::Object* sim = vm_->object(sim_)) {
        auto mix = [&](uint64_t v) { stamp = (stamp ^ v) * 1099511628211ull; };
        mix(uint64_t(sim->activeInteraction + 1));
        for (const auto& q : sim->queue)
            mix(uint64_t(q.uid) << 1 | q.cancelled), mix(std::hash<std::string>{}(q.name));
    }
    if (const vm::Dialog* d = dialog()) {
        stamp = (stamp ^ (uint64_t(d->id) << 8)) * 1099511628211ull;
        stamp = (stamp ^ std::hash<std::string>{}(d->text)) * 1099511628211ull;
        if (DialogWindow::hasTextBox(*d)) // the caret blinks
            stamp = (stamp ^ uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 2)) *
                    1099511628211ull;
    }
    if (stamp != queueStamp_)
        queueStamp_ = stamp, dirty_ = true;
    if (dirty_) {
        layer_ = (layer_ + 1) % layers_.size();
        layers_[layer_]->setScreen(screen(cw, ch));
        dirty_ = false;
    }
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
