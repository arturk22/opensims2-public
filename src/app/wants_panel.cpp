// The Wants & Fears strip (wants_panel.hpp).

#include "app/wants_panel.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_script.hpp"
#include "vm/wants.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

namespace os2 {

namespace {

using Color = std::array<int, 3>;

std::string rgb(Color c) {
    return "(" + std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + ")";
}

void rect(PanelContext& c, float x0, float y0, float x1, float y1, Color col) {
    UiElement e;
    e.tag = "LEGACY";
    e.attrs = {{"clsid", "GZWinFlatRect"}, {"fillcolor", rgb(col)}, {"winflag_visible", "yes"},
               {"area", uiArea(0, 0, int(x1 - x0), int(y1 - y0))}};
    UiDrawHooks hk;
    layoutUiElement(e, x0, y0, c.res, c.fonts, c.out, hk);
}

void text(PanelContext& c, float x, float y, float w, float h, const std::string& s, Color col, const char* align) {
    UiElement e;
    e.tag = "LEGACY";
    e.attrs = {{"clsid", "GZWinText"}, {"caption", s},          {"font", "LiveModePanelPuck"}, {"forecolor", rgb(col)},
               {"align", align},       {"winflag_visible", "yes"}, {"area", uiArea(0, 0, int(w), int(h))}};
    UiDrawHooks hk;
    layoutUiElement(e, x, y, c.res, c.fonts, c.out, hk);
}

void image(PanelContext& c, const RgbaImage* img, float x0, float y0, float x1, float y1) {
    if (!img || !img->width)
        return;
    UiQuad q;
    q.image = img;
    q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
    c.out.quads.push_back(q);
}

const RgbaImage* uiImage(PanelContext& c, uint32_t instance) {
    return instance ? c.res.image({kTypeUiImage, kUiImageGroup, instance}) : nullptr;
}

} // namespace

float layoutWantsPanel(PanelContext& c, vm::ObjectId simId, float x, float y, float width) {
    const vm::Object* sim = c.vm.object(simId);
    vm::WantSystem* ws = vm::wantSystem(c.vm);
    if (!sim || !ws)
        return 0;
    const int16_t id = vm::neighborIdOf(*sim);
    const vm::SimWants* s = vm::simWants(c.vm, id);
    if (!s)
        return 0;
    const auto& lib = *ws->library;
    constexpr float kIcon = 34, kGap = 6, kHeight = 70;
    const Color panel{132, 146, 214}, edge{58, 68, 158}, well{36, 44, 116}, ink{5, 6, 113};
    const Color wantEdge{96, 210, 96}, fearEdge{222, 84, 72};
    // The strip: an outline, the panel's blue, a darker band for the labels.
    rect(c, x, y, x + width, y + kHeight, edge);
    rect(c, x + 2, y + 2, x + width - 2, y + kHeight - 2, panel);
    c.solid({x, y, x + width, y + kHeight});
    text(c, x + 8, y + 3, 120, 14, "Desejos", ink, "lefttop");
    const float wantsW = 4 * kIcon + 3 * kGap, fearsW = 3 * kIcon + 2 * kGap;
    const float fearsX = x + width - 10 - fearsW;
    text(c, fearsX, y + 3, 120, 14, "Medos", ink, "lefttop");
    // A thin separator between the two groups.
    const float mid = (x + 10 + wantsW + fearsX) / 2;
    rect(c, mid, y + 8, mid + 1, y + kHeight - 8, edge);

    auto slot = [&](const vm::WantPtr& w, float sx, float sy, bool fear) {
        const Color frame = fear ? fearEdge : wantEdge;
        rect(c, sx - 2, sy - 2, sx + kIcon + 2, sy + kIcon + 2, w ? frame : edge);
        rect(c, sx, sy, sx + kIcon, sy + kIcon, well);
        if (!w)
            return;
        const auto* d = lib.definition(w->data.guid);
        const RgbaImage* main = nullptr;
        const RgbaImage* badge = nullptr;
        if (w->data.subjectType == wants::SubjectType::Sim && w->data.subjectValue) {
            main = c.portrait(int16_t(w->data.subjectValue));
            if (main && d)
                badge = uiImage(c, d->secondaryIcon);
        }
        if (!main && w->data.subjectType == wants::SubjectType::Category)
            if (const auto* cat = lib.category(int32_t(w->data.subjectValue)))
                main = uiImage(c, cat->primaryIcon);
        if (!main && d)
            main = uiImage(c, d->primaryIcon ? d->primaryIcon : d->secondaryIcon);
        image(c, main, sx + 1, sy + 1, sx + kIcon - 1, sy + kIcon - 1);
        if (badge)
            image(c, badge, sx + kIcon - 16, sy + kIcon - 16, sx + kIcon + 1, sy + kIcon + 1);
        char pts[16];
        std::snprintf(pts, sizeof pts, "%+d", w->data.score);
        text(c, sx - 6, sy + kIcon + 2, kIcon + 12, 12, pts, fear ? Color{150, 20, 20} : Color{10, 90, 10}, "center");
        // The tooltip: the want, its points, the help text (entry 3).
        std::string tip = vm::wantText(c.vm, w->data) + "|";
        tip += (fear ? "Medo: " : "Desejo: ") + std::string(pts) + " pontos de aspiração";
        if (d)
            if (std::string help = lib.text(*d, 3); !help.empty())
                tip += "\n" + help;
        c.tip({sx - 2, sy - 2, sx + kIcon + 2, sy + kIcon + 2}, tip);
    };
    const float iy = y + 17;
    for (size_t i = 0; i < 4; ++i)
        slot(i < s->wants.size() ? s->wants[i] : nullptr, x + 10 + float(i) * (kIcon + kGap), iy, false);
    for (size_t i = 0; i < 3; ++i)
        slot(i < s->fears.size() ? s->fears[i] : nullptr, fearsX + float(i) * (kIcon + kGap), iy, true);
    return kHeight;
}

std::vector<MemoryEntry> simMemories(PanelContext& c, int16_t id) {
    std::vector<MemoryEntry> out;
    const auto it = c.vm.inventories.find({vm::InventoryKind::Neighbor, id});
    ResourceIndex* index = c.vm.code().index();
    if (it == c.vm.inventories.end() || !index)
        return out;
    const vm::Neighbor* me = c.vm.neighbor(id);
    const bool female = me && me->personData[65] == 1;
    static std::map<uint32_t, std::unique_ptr<RgbaImage>> icons; // by GUID, for the life of the app
    for (auto t = it->second.singular.rbegin(); t != it->second.singular.rend(); ++t) {
        if (t->flags != 3)
            continue;
        MemoryEntry m;
        const auto def = c.vm.code().definitionByGuid(t->guid);
        if (!def)
            continue;
        const uint32_t group = def->first;
        for (uint32_t inst : {female ? 0x7D1u : 0x7D0u, 0x7D0u})
            if (auto bytes = index->read(Tgi{0x43545353, group, inst, 0})) {
                try {
                    const auto table = StringTable::parse(*bytes);
                    m.text = table.get(0);
                    m.description = table.get(1);
                } catch (const std::exception&) {
                }
                if (!m.text.empty())
                    break;
            }
        if (m.text.empty())
            if (const ObjectDefinition* od = c.vm.code().definition(def->first, def->second))
                m.text = od->fileName;
        const auto& p = t->properties;
        std::string subject = "?";
        if (p.size() > 6) {
            const uint32_t guid = uint16_t(p[5]) | uint32_t(uint16_t(p[6])) << 16;
            for (const auto& [nid, n] : c.vm.neighbors)
                if (n.guid == guid && guid) {
                    subject = n.firstName;
                    break;
                }
        }
        for (size_t k; (k = m.text.find("$Subject")) != std::string::npos;)
            m.text.replace(k, 8, subject);
        m.bad = !p.empty() && (p[0] & 4);
        auto& icon = icons[t->guid];
        if (!icon)
            if (auto bytes = index->read(Tgi{kTypeUiImage, group, 1, 0}))
                if (auto img = decodeUiImage(*bytes))
                    icon = std::make_unique<RgbaImage>(std::move(*img));
        m.icon = icon.get();
        out.push_back(std::move(m));
    }
    return out;
}

void layoutMemories(PanelContext& c, vm::ObjectId simId, std::array<float, 4> a) {
    const vm::Object* sim = c.vm.object(simId);
    if (!sim)
        return;
    const auto memories = simMemories(c, vm::neighborIdOf(*sim));
    constexpr float kIcon = 30, kGap = 5;
    // Right of the tab buttons, left of the script's page arrows.
    const float x0 = a[0] + 72, y0 = a[1] + 10;
    const int cols = std::max(1, int((a[2] - a[0] - 72 - 56 + kGap) / (kIcon + kGap)));
    const int rows = std::max(1, int((a[3] - a[1] - 20 + kGap) / (kIcon + kGap)));
    if (memories.empty()) {
        text(c, x0, y0, a[2] - a[0] - 20, 16, "Nenhuma lembrança ainda", {5, 6, 113}, "lefttop");
        return;
    }
    for (size_t i = 0; i < memories.size() && int(i) < cols * rows; ++i) {
        const float x = x0 + float(int(i) % cols) * (kIcon + kGap), y = y0 + float(int(i) / cols) * (kIcon + kGap);
        const auto& m = memories[i];
        rect(c, x - 1, y - 1, x + kIcon + 1, y + kIcon + 1, m.bad ? Color{200, 80, 70} : Color{70, 90, 190});
        rect(c, x, y, x + kIcon, y + kIcon, {36, 44, 116});
        image(c, m.icon, x, y, x + kIcon, y + kIcon);
        c.tip({x, y, x + kIcon, y + kIcon}, m.text + (m.description.empty() ? "" : "|" + m.description));
    }
}

} // namespace os2
