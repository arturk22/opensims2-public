// The Wants & Fears strip (wants_panel.hpp).

#include "app/wants_panel.hpp"
#include "formats/log_once.hpp"

#include "app/game_data_caches.hpp"
#include "app/ui_panel_util.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_script.hpp"
#include "vm/memories.hpp"
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

std::vector<MemoryEntry> simMemories(vm::VM& machine, int16_t id) {
    std::vector<MemoryEntry> out;
    ResourceIndex* index = machine.code().index();
    if (!index)
        return out;
    auto& icons = gameDataCaches().memoryIcons; // by GUID, for the life of the app (app/game_data_caches.hpp)
    const auto memories = vm::memoryInfo(machine, id);
    for (auto m = memories.rbegin(); m != memories.rend(); ++m) {
        const auto def = machine.code().definitionByGuid(m->guid);
        const ObjectDefinition* od = def ? machine.code().definition(def->first, def->second) : nullptr;
        if (!od || od->objectType() != 15) // only memory objects have a definition
            continue;
        MemoryEntry e;
        e.guid = m->guid;
        e.good = vm::memoryIsGood(m->kind);
        const uint32_t group = def->first;
        // The subject: a Sim (its neighbor, by the character's GUID) or an object.
        const vm::Neighbor* subjectSim = nullptr;
        if (m->subject)
            for (const auto& [nid, n] : machine.neighbors)
                if (n.guid == m->subject) {
                    subjectSim = &n;
                    break;
                }
        if (subjectSim) {
            e.subject = subjectSim->firstName;
        } else if (m->subject) {
            if (auto sd = machine.code().definitionByGuid(m->subject))
                if (const ObjectDefinition* so = machine.code().definition(sd->first, sd->second))
                    if (auto bytes = index->read(Tgi{0x43545353, sd->first, so->field(0x40 + 2 * 41), 0}))
                        try {
                            e.subject = StringTable::parse(*bytes).get(0);
                        } catch (const std::exception&) {
                        }
        }
        const uint32_t strings = od->field(0x40 + 2 * 41);
        const bool female = subjectSim && subjectSim->personData[65] == 1;
        auto load = [&](uint32_t inst, std::string& title, std::string& desc) {
            if (auto bytes = index->read(Tgi{0x43545353, group, inst, 0}))
                try {
                    const auto table = StringTable::parse(*bytes);
                    title = table.get(0);
                    desc = table.get(1);
                } catch (const std::exception& err) {
                    os2::logOnce("memory text", std::to_string(group), err);
                }
        };
        std::string t0, d0, t1, d1;
        load(strings, t0, d0);
        load(strings + 1, t1, d1);
        if (t0.empty()) t0 = t1;
        if (t1.empty()) t1 = t0;
        if (d0.empty()) d0 = d1;
        if (d1.empty()) d1 = d0;
        e.text = female ? t1 : t0;
        e.description = female ? d1 : d0;
        if (e.text.empty())
            e.text = od->fileName;
        for (std::string* s : {&e.text, &e.description})
            for (size_t k = 0; (k = s->find("$Subject", k)) != std::string::npos; k += e.subject.size())
                s->replace(k, 8, e.subject);
        auto& icon = icons[m->guid];
        if (!icon)
            if (auto bytes = index->read(Tgi{kTypeUiImage, group, 1, 0}))
                if (auto img = decodeUiImage(*bytes))
                    icon = std::make_unique<RgbaImage>(std::move(*img));
        e.icon = icon.get();
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<MemoryEntry> simMemories(PanelContext& c, int16_t id) { return simMemories(c.vm, id); }

} // namespace os2
