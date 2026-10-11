#include "pie_menu.hpp"

#include "formats/font.hpp"
#include "formats/model.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

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
constexpr float kPad = 8, kLabelH = 24;

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

PieMenuHost::PieMenuHost(vm::VM& vm, const std::filesystem::path& fontsDir, const std::string& simQuery)
    : vm_(&vm), fonts_(std::make_unique<FontLibrary>(fontsDir)) {
    sim_ = vm::findSim(vm, simQuery);
}

PieMenuHost::~PieMenuHost() = default;

void PieMenuHost::addPickBox(uint32_t lotInstance, simd_float3 lo, simd_float3 hi, const simd_float4x4& at) {
    const auto id = vm::ObjectId(lotInstance & 0xFFFF);
    if (!vm_->object(id) || simd_equal(lo, hi))
        return;
    simd_float3 wlo = simd_float3(std::numeric_limits<float>::max()), whi = -wlo;
    for (int c = 0; c < 8; ++c) {
        const simd_float4 p = simd_mul(at, simd_make_float4(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z, 1));
        wlo = simd_min(wlo, p.xyz), whi = simd_max(whi, p.xyz);
    }
    boxes_.push_back({id, wlo, whi});
}

vm::ObjectId PieMenuHost::pick(simd_float3 origin, simd_float3 dir) const {
    vm::ObjectId best = 0;
    float bestT = std::numeric_limits<float>::max();
    for (const auto& b : boxes_) {
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
        // The ray enters it nearest; ties (a box inside another) go to the smaller box.
        if (hit && (t0 < bestT - 1e-4f)) {
            bestT = t0;
            best = b.id;
        }
    }
    return best;
}

std::optional<simd_float3> PieMenuHost::anchor(vm::ObjectId id) const {
    for (const auto& b : boxes_)
        if (b.id == id)
            return simd_make_float3((b.lo.x + b.hi.x) / 2, (b.lo.y + b.hi.y) / 2, b.hi.z);
    return std::nullopt;
}

bool PieMenuHost::open(vm::ObjectId object, simd_float2 at) {
    close();
    if (!sim_)
        return false;
    for (auto& e : vm::pieMenu(*vm_, sim_, object))
        if (e.available() && !e.debug)
            entries_.push_back(std::move(e));
    if (entries_.empty())
        return false;
    object_ = object;
    title_ = objectName(vm_->object(entries_.front().object));
    at_ = at;
    open_ = true;
    dirty_ = true;
    return true;
}

void PieMenuHost::close() {
    open_ = false;
    entries_.clear();
    submenu_.clear();
    hover_ = chosen_ = -1;
    dirty_ = true;
}

std::vector<PieMenuHost::Label> PieMenuHost::labels() const {
    std::vector<Label> out;
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
    const float ppem = 14;
    const FontStyle* st = fonts_->style("piemenu");
    const Font* font = st ? fonts_->font(*st) : nullptr;
    const size_t n = out.size();
    const float r = n <= 1 ? 0 : std::max(70.0f, float(n) * 13.0f);
    for (size_t i = 0; i < n; ++i) {
        const float w = (font ? measureText(*font, decodeUtf8(out[i].text), font ? float(st->size) : ppem).width : 80) +
                        2 * kPad;
        const float a = -float(M_PI) / 2 + 2 * float(M_PI) * float(i) / float(n);
        const float px = at_.x + r * std::cos(a), py = at_.y + r * std::sin(a);
        // Labels grow away from the centre: right of it on the right, left on the left.
        const float cx = px + std::cos(a) * w / 2;
        out[i].x0 = std::round(cx - w / 2), out[i].x1 = std::round(cx + w / 2);
        out[i].y0 = std::round(py - kLabelH / 2), out[i].y1 = out[i].y0 + kLabelH;
    }
    return out;
}

void PieMenuHost::hover(simd_float2 p) {
    if (!open_)
        return;
    int h = -1;
    const auto ls = labels();
    for (size_t i = 0; i < ls.size(); ++i)
        if (p.x >= ls[i].x0 && p.x < ls[i].x1 && p.y >= ls[i].y0 && p.y < ls[i].y1)
            h = int(i);
    if (h != hover_)
        hover_ = h, dirty_ = true;
}

bool PieMenuHost::click(simd_float2 p, bool keepOpen) {
    if (!open_)
        return false;
    const auto ls = labels();
    for (const auto& l : ls) {
        if (!(p.x >= l.x0 && p.x < l.x1 && p.y >= l.y0 && p.y < l.y1))
            continue;
        const auto& first = entries_[l.entries.front()];
        if (submenu_.empty() && first.path.size() > 1) {
            submenu_ = l.text;
            hover_ = -1;
        } else {
            queueEntry(l.entries.front());
            if (!keepOpen)
                close();
        }
        dirty_ = true;
        return true;
    }
    if (!submenu_.empty())
        submenu_.clear(), hover_ = -1, dirty_ = true;
    else
        close();
    return true;
}

bool PieMenuHost::select(size_t index) {
    if (!open_ || index >= entries_.size())
        return false;
    const auto& e = entries_[index];
    submenu_ = e.path.size() > 1 ? e.path[0] : "";
    chosen_ = int(index);
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

UiScreen PieMenuHost::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    const FontStyle* st = fonts_->style("piemenu");
    Painter paint{out, st ? fonts_->font(*st) : nullptr, st ? float(st->size) : 14.0f};

    // The selected Sim's queue, top left.
    if (const vm::Object* sim = vm_->object(sim_)) {
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
        const auto ls = labels();
        // Centre marker and the object's name above the ring.
        paint.rect(at_.x - 4, at_.y - 4, at_.x + 4, at_.y + 4, kLabelHot);
        float top = at_.y;
        for (const auto& l : ls)
            top = std::min(top, l.y0);
        const std::string title = submenu_.empty() ? title_ : title_ + " / " + submenu_;
        const float tw = paint.width(title) + 2 * kPad;
        paint.label(std::round(at_.x - tw / 2), top - kLabelH - 10, std::round(at_.x + tw / 2), top - 10, title, kTitle,
                    kText);
        for (size_t i = 0; i < ls.size(); ++i) {
            const auto& l = ls[i];
            const bool hot = int(i) == hover_ ||
                             (chosen_ >= 0 && std::find(l.entries.begin(), l.entries.end(), size_t(chosen_)) != l.entries.end());
            paint.label(l.x0, l.y0, l.x1, l.y1, l.text, hot ? kLabelHot : kLabel, hot ? kTextHot : kText);
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
