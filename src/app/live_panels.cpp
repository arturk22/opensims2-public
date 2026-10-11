#include "app/live_panels.hpp"

#include "formats/font.hpp"
#include "formats/simantics.hpp"
#include "formats/ui_resources.hpp"
#include "vm/animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace os2 {

namespace {

constexpr std::array<uint32_t, LivePanels::Count> kScripts = {0x49064905, 0x49065002, 0x49065003, 0x49064820,
                                                              0x49064800};
constexpr uint32_t kRelThumbScript = 0x49065005;
constexpr uint32_t kLive = 0x7F96C284; // Live.package's string group

// Person data (objects.package STR# 0x7FE59FD0/0xC8 "Person Data Labels").
namespace pd {
constexpr int Nice = 2, Active = 3, Playful = 5, Outgoing = 6, Neat = 7;
constexpr int Cleaning = 9, Cooking = 10, Charisma = 11, Mechanical = 12, Creativity = 15, Body = 17, Logic = 18;
constexpr int OriginalNeat = 47, OriginalNice = 48, OriginalActive = 49, OriginalOutgoing = 50, OriginalPlayful = 51;
constexpr int SchoolGrade = 56, Zodiac = 70;
constexpr int JobLevel = 57, NeighborId = 31, FamilyNumber = 61, JobPerformance = 63, Pto = 69;
constexpr int JobGuid1 = 89, JobGuid2 = 90;
} // namespace pd

// Relationship flags (SREL variable 1, docs/data-findings.md §8).
namespace relflag {
constexpr int Crush = 1 << 0, Love = 1 << 1, Engaged = 1 << 2, Married = 1 << 3, Friend = 1 << 4,
              BestFriend = 1 << 5, Steady = 1 << 6, Enemy = 1 << 7;
}

std::string str(PanelContext& c, uint32_t instance, uint32_t index) { // 1-based, as the scripts' refs
    if (index == 0)
        return {};
    return c.res.string({kLive, instance, index - 1}).value_or("");
}

std::string printf1(const std::string& fmt, const std::string& a) {
    std::string out = fmt;
    if (auto p = out.find("%s"); p != std::string::npos)
        out.replace(p, 2, a);
    else if (auto q = out.find("%d"); q != std::string::npos)
        out.replace(q, 2, a);
    return out;
}

std::string hourText(int h) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d:00", ((h % 24) + 24) % 24);
    return b;
}

// The school grade (person data 56) as a letter, the STR# 0x8B entries 6-18
// (A+ ... F). Inferred: the value is 0..100 and the letters split it at the
// usual points (97, 93, 90, 87, ..., 60).
std::string gradeLetter(PanelContext& c, int value) {
    static const int kLimits[13] = {97, 93, 90, 87, 83, 80, 77, 73, 70, 67, 63, 60, 0};
    static const char* const kLetters[13] = {"A+", "A", "A-", "B+", "B", "B-", "C+", "C", "C-", "D+", "D", "D-", "F"};
    int i = 0;
    while (i < 12 && value < kLimits[i])
        ++i;
    // The Brazilian Portuguese entries are empty: the letters are the same.
    std::string s = str(c, 0x8B, uint32_t(6 + i));
    return s.empty() ? kLetters[i] : s;
}

// The relationship description popup (script 0x4906502B): the description
// (STR# 0x84 entries 2-12 of Live.package: married, love, crush, going
// steady, friend, best friend, enemy, acquaintance, just met, engaged,
// family) and the type (entries 23-34). Which one a state shows is inferred
// from the texts: the romantic states first, then family, then the rest.
struct RelationshipText {
    uint32_t description, type; // 1-based entries of STR# 0x84
};
RelationshipText describeRelationship(const LivePanels::RelationshipRow& r) {
    using namespace relflag;
    if (r.flags & Married)
        return {3, 25};
    if (r.flags & Engaged)
        return {12, 24};
    if (r.flags & Steady)
        return {6, 28};
    if (r.flags & Love)
        return {4, 26};
    if (r.flags & Crush)
        return {5, 27};
    if (r.household || r.familial)
        return {13, 35};
    if (r.flags & BestFriend)
        return {8, 29};
    if (r.flags & Enemy)
        return {9, 31};
    if (r.flags & Friend)
        return {7, 30};
    return r.lifetime || r.daily ? RelationshipText{10, 32} : RelationshipText{11, 33};
}

// The zodiac sign in person data 70: 1..12 (Aries ... Pisces, as the STR#
// 0x90 of Live.package orders them); inferred (0 or out of range: none).
int zodiacSign(int value) { return value >= 1 && value <= 12 ? value - 1 : -1; }
// The pictures of the 12 signs in ui.package (group 0x499DB772), by sign
// (Aries ... Pisces); the instances were matched to the signs by looking at them.
constexpr std::array<uint32_t, 12> kZodiacImages = {0xAB202525, 0xAB202505, 0xAB202515, 0xAB202540,
                                                   0xAB202530, 0xAB202535, 0xAB202520, 0xAB202545,
                                                   0xAB202510, 0xAB202550, 0xAB202555, 0xAB202500};

const RgbaImage* imageOf(PanelContext& c, const UiElement* e) {
    if (!e)
        return nullptr;
    auto ref = uiResourceRef(e->get("image"));
    return ref ? c.res.image(*ref) : nullptr;
}

// Frame i of a horizontal strip of n frames.
RgbaImage stripFrame(const RgbaImage& strip, int i, int n) {
    RgbaImage out;
    const uint32_t w = strip.width / uint32_t(std::max(n, 1));
    out.width = w, out.height = strip.height;
    out.pixels.resize(size_t(w) * strip.height * 4);
    for (uint32_t y = 0; y < strip.height; ++y)
        std::copy_n(&strip.pixels[(size_t(y) * strip.width + size_t(i) * w) * 4], w * 4,
                    &out.pixels[size_t(y) * w * 4]);
    return out;
}

void rectSolid(PanelContext& c, const UiElement* e) {
    if (e)
        if (auto it = c.rects.find(e); it != c.rects.end())
            c.solid(it->second);
}

std::optional<std::array<float, 4>> rectOf(PanelContext& c, const UiElement* e) {
    if (!e)
        return std::nullopt;
    auto it = c.rects.find(e);
    return it == c.rects.end() ? std::nullopt : std::optional(it->second);
}

} // namespace

void placeWindow(PanelContext& c, const UiElement& e, float x, float y, UiDrawHooks& hk) {
    const auto a = e.area();
    hk.rects = &c.rects;
    layoutUiElement(e, x - float(a[0]), y - float(a[1]), c.res, c.fonts, c.out, hk);
}

void fillBar(PanelContext& c, const UiElement& bar, float value, float min, float max, UiDrawHooks& hk) {
    const UiElement* maskWin = bar.child(3);
    const UiElement* gradWin = bar.child(4);
    const RgbaImage* mask = imageOf(c, maskWin);
    const RgbaImage* grad = imageOf(c, gradWin);
    if (!mask || !grad)
        return;
    const bool columns = grad->width > mask->width || mask->height > mask->width;
    hk.images[maskWin] = c.keep(gradientFill(*grad, *mask, value, min, max, columns));
    hk.hide.insert(gradWin);
}

void stepBar(PanelContext& c, const UiElement& bar, float value, UiDrawHooks& hk) {
    const UiElement* full = bar.child(3);
    if (const RgbaImage* img = imageOf(c, full))
        hk.images[full] = c.keep(segmentFill(*img, value, 1000, 10));
}

LivePanels::LivePanels(UiResources& res) : res_(&res) {
    for (size_t i = 1; i < kScripts.size(); ++i)
        scripts_[i] = res.script(kScripts[i]);
    relThumb_ = res.script(kRelThumbScript);
}

bool LivePanels::ok(Panel p) const { return p > Needs && p < Count && scripts_[p] && !scripts_[p]->roots.empty(); }

float LivePanels::layout(Panel p, PanelContext& c, vm::ObjectId sim, float x, float bottom) {
    if (!ok(p))
        return x;
    switch (p) {
    case Relationships: return relationships(c, sim, x, bottom);
    case Job: return job(c, sim, x, bottom);
    case Simology: return simology(c, sim, x, bottom);
    case Rewards: return rewards(c, sim, x, bottom);
    default: return x;
    }
}

// --- Relationships ---------------------------------------------------------

std::vector<LivePanels::RelationshipRow> LivePanels::relationshipsOf(vm::VM& vm, vm::ObjectId simId, Filter f) {
    std::vector<RelationshipRow> rows;
    const vm::Object* sim = vm.object(simId);
    if (!sim || !sim->person)
        return rows;
    const int16_t me = sim->person->data[pd::NeighborId];
    const int16_t family = sim->person->data[pd::FamilyNumber];
    if (!me)
        return rows;
    for (const auto& [key, values] : vm.relationships) {
        if (!key.first.neighbor || key.first.id != me || !key.second.neighbor || key.second.id == me)
            continue;
        RelationshipRow r;
        r.neighbor = key.second.id;
        auto at = [&](size_t i) { return i < values.size() ? int(values[i]) : 0; };
        r.daily = at(0), r.flags = at(1) & 0xFFFF, r.lifetime = at(2), r.familial = at(3);
        if (auto n = vm.neighbors.find(r.neighbor); n != vm.neighbors.end())
            r.household = family && n->second.personData[pd::FamilyNumber] == family;
        else
            continue; // not a neighbor the neighborhood knows
        // LoadRelationships (docs/ui-live-mac.md §7.1): household = the same
        // family; 2 = they know each other (variable 3; here also the
        // household); friends = friend or best friend (or a friendship float
        // over 0.001 that is not kept here).
        bool keep = true;
        switch (f) {
        case Filter::Household: keep = r.household; break;
        case Filter::Relatives: keep = r.household || r.familial != 0; break;
        case Filter::Friends:
            keep = (r.flags & (relflag::Friend | relflag::BestFriend)) != 0;
            break;
        case Filter::All: break;
        }
        if (keep)
            rows.push_back(r);
    }
    // cFriendshipSortPredicate (inferred: lifetime, then daily, highest first).
    std::stable_sort(rows.begin(), rows.end(), [](const RelationshipRow& a, const RelationshipRow& b) {
        return a.lifetime != b.lifetime ? a.lifetime > b.lifetime : a.daily > b.daily;
    });
    return rows;
}

float LivePanels::relationships(PanelContext& c, vm::ObjectId sim, float x, float bottom) {
    const UiElement& root = scripts_[Relationships]->roots.front();
    const UiElement* collapsed = root.child(0x1000);
    if (!collapsed)
        return x;
    UiDrawHooks hk;
    if (const UiElement* e = root.child(0x2000))
        hk.hide.insert(e);
    // The background's left (window 0x10A1 at x 12) at x; the collapsed
    // group's bottom at `bottom`.
    const auto ca = collapsed->area();
    const float rootX = x - 12 - float(ca[0]), rootY = bottom - float(ca[3]);
    // The filter buttons: the current one pressed.
    const std::array<std::pair<uint32_t, Filter>, 4> filters = {{{0x0BA16779, Filter::Relatives},
                                                                 {0x0BA16778, Filter::Friends},
                                                                 {0x0BA16776, Filter::Household},
                                                                 {0x0BA16777, Filter::All}}};
    for (auto [id, f] : filters)
        if (const UiElement* b = collapsed->child(id))
            hk.buttonState[b] = f == filter_ ? 2 : 1;
    const std::vector<RelationshipRow> rows = relationshipsOf(c.vm, sim, filter_);
    const int perPage = 6; // kCollapsedColumns x kCollapsedRows
    const int pages = std::max(1, int((rows.size() + perPage - 1) / perPage));
    relPage_ = std::clamp(relPage_, 0, pages - 1);
    if (const UiElement* b = collapsed->child(0x1004))
        hk.buttonState[b] = relPage_ > 0 ? 1 : 0;
    if (const UiElement* b = collapsed->child(0x1005))
        hk.buttonState[b] = relPage_ + 1 < pages ? 1 : 0;
    placeWindow(c, root, rootX, rootY, hk);
    for (const UiElement& e : collapsed->children)
        if (e.clsid() == "GZWinBMP")
            rectSolid(c, &e);
    for (auto [id, f] : filters)
        if (auto r = rectOf(c, collapsed->child(id)))
            c.click(*r, [this, f = f, snd = c.sound] {
                filter_ = f, relPage_ = 0;
                snd("ui_secondary_button");
            });
    if (auto r = rectOf(c, collapsed->child(0x1004)); r && relPage_ > 0)
        c.click(*r, [this, snd = c.sound] { --relPage_, snd("ui_secondary_button"); });
    if (auto r = rectOf(c, collapsed->child(0x1005)); r && relPage_ + 1 < pages)
        c.click(*r, [this, snd = c.sound] { ++relPage_, snd("ui_secondary_button"); });

    // The thumbnails in the grid window 0x1006, six across.
    const auto grid = rectOf(c, collapsed->child(0x1006));
    if (grid && relThumb_ && !relThumb_->roots.empty()) {
        const UiElement& t = relThumb_->roots.front();
        const float tw = float(t.area()[2] - t.area()[0]);
        const RgbaImage* strip = nullptr;
        if (const UiElement* icon = t.child(0xA5))
            strip = imageOf(c, icon);
        for (int k = 0; k < perPage && relPage_ * perPage + k < int(rows.size()); ++k) {
            const RelationshipRow& r = rows[size_t(relPage_ * perPage + k)];
            UiDrawHooks th;
            if (const UiElement* p = t.child(0xA3)) {
                if (const RgbaImage* img = c.portrait(r.neighbor))
                    th.images[p] = img;
                else
                    th.hide.insert(p);
            }
            if (const UiElement* bars = t.children.empty() ? nullptr : &t.children.front()) {
                if (const UiElement* d = bars->child(0xA1))
                    fillBar(c, *d, float(r.daily), -100, 100, th);
                if (const UiElement* l = bars->child(0xA2))
                    fillBar(c, *l, float(r.lifetime), -100, 100, th);
            }
            if (const UiElement* e = t.child(0xA9))
                th.captions[e] = std::to_string(r.daily);
            if (const UiElement* e = t.child(0xAA))
                th.captions[e] = std::to_string(r.lifetime);
            // The icons (UpdateSTRUI/UpdateLTRUI, frames of the 11-frame
            // strip: 0 love, 1 crush, 5 married, 6 engaged, 7 friend, 8
            // enemy, 9 best friend, 10 going steady).
            auto icon = [&](uint32_t id, int frame) {
                const UiElement* e = t.child(id);
                if (!e)
                    return;
                if (frame < 0 || !strip)
                    th.hide.insert(e);
                else
                    th.images[e] = c.keep(stripFrame(*strip, frame, 11));
            };
            icon(0xA5, r.flags & relflag::Crush ? 1 : -1);
            icon(0xA6, r.flags & relflag::Friend ? 7 : -1);
            icon(0xA7, r.flags & relflag::Married   ? 5
                       : r.flags & relflag::Engaged ? 6
                       : r.flags & relflag::Steady  ? 10
                       : r.flags & relflag::Love    ? 0
                                                    : -1);
            icon(0xA8, r.flags & relflag::BestFriend ? 9 : r.flags & relflag::Enemy ? 8 : -1);
            if (const UiElement* e = t.child(0xAB)) // UpdateFamilyIcon
                (r.household ? th.show : th.hide).insert(e);
            if (const UiElement* b = t.child(0xA4))
                th.buttonState[b] = 1;
            placeWindow(c, t, (*grid)[0] + float(k) * tw, (*grid)[1], th);
            // UpdateTooltip: the other Sim's name; a click opens the
            // relationship description popup over the thumbnail.
            if (auto n = c.vm.neighbors.find(r.neighbor); n != c.vm.neighbors.end())
                if (auto rr = rectOf(c, t.child(0xA4))) {
                    const std::string name = n->second.firstName + " " + n->second.lastName;
                    c.tip(*rr, name);
                    const RelationshipText rt = describeRelationship(r);
                    std::string desc = str(c, 0x84, rt.description);
                    if (auto bar = desc.find('|'); bar != std::string::npos)
                        desc.resize(bar);
                    c.click(*rr, [popup = c.popup, anchor = *rr, name, desc, type = str(c, 0x84, rt.type)] {
                        popup(0x4906502B, anchor, {{0xA1, name}, {0xA2, desc}, {0xA3, type}});
                    });
                }
        }
    }
    return x + 487;
}

// --- Skills and career -------------------------------------------------------

float LivePanels::job(PanelContext& c, vm::ObjectId simId, float x, float bottom) {
    const UiElement& root = scripts_[Job]->roots.front();
    const vm::Object* sim = c.vm.object(simId);
    if (!sim || !sim->person)
        return x;
    const auto& data = sim->person->data;
    UiDrawHooks hk;
    if (const UiElement* e = root.child(0x8CB609AB))
        hk.hide.insert(e);
    // The age's group (onSelectedPersonJobInfoChanged): toddler A1, child
    // A2, teen A3, adult A4 (A5 without a job), elder A9.
    const uint32_t guid = uint32_t(uint16_t(data[pd::JobGuid1])) | uint32_t(uint16_t(data[pd::JobGuid2])) << 16;
    const std::string age = vm::agePrefix(*sim);
    uint32_t group = age == "p" || age == "b" ? 0xA1 : age == "c" ? 0xA2 : age == "t" ? 0xA3 : age == "e" ? 0xA9 : 0xA4;
    if (group == 0xA4 && !guid)
        group = 0xA5;
    for (uint32_t id : {0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA9u})
        if (const UiElement* e = root.child(id))
            (id == group ? hk.show : hk.hide).insert(e);
    const UiElement* g = root.child(group);
    // The job object's tables (JobData objects).
    const StringTable* titles = nullptr;
    const Constants *wages = nullptr, *start = nullptr, *hours = nullptr, *friends = nullptr, *days = nullptr;
    if (guid)
        if (auto def = c.vm.code().definitionByGuid(guid)) {
            titles = c.vm.code().strings(def->first, 0x93);
            wages = c.vm.code().constants(def->first, 0x1000);
            start = c.vm.code().constants(def->first, 0x1001);
            hours = c.vm.code().constants(def->first, 0x1002);
            friends = c.vm.code().constants(def->first, 0x1003);
            days = c.vm.code().constants(def->first, 0x101A);
        }
    const int level = std::max<int>(0, data[pd::JobLevel]);
    auto bcon = [&](const Constants* k, int i) { return k && i >= 0 && size_t(i) < k->values.size() ? int(k->values[size_t(i)]) : 0; };
    const bool working = guid && (group == 0xA4 || group == 0xA9 || group == 0xA3);
    if (g) {
        const UiElement* jobWin = g;
        if (group == 0xA3 || group == 0xA9) {
            // Teen: B1 school without a job, B3 the job; elder: C2 retired, C3 the job.
            const uint32_t with = group == 0xA3 ? 0xB3 : 0xC3, without = group == 0xA3 ? 0xB1 : 0xC2;
            for (uint32_t id : {0xB1u, 0xB2u, 0xB3u, 0xC1u, 0xC2u, 0xC3u})
                if (const UiElement* e = g->child(id))
                    (id == (guid ? with : without) ? hk.show : hk.hide).insert(e);
            jobWin = g->child(guid ? with : without);
        }
        if (working && jobWin) {
            if (const UiElement* e = findWindow(*jobWin, 0x77E74B47); e && titles)
                hk.captions[e] = titles->get(size_t(std::max(0, 2 * level - 1)));
            if (const UiElement* e = findWindow(*jobWin, 0x47E74B6D))
                hk.captions[e] = formatMoney(bcon(wages, level));
            const int h0 = bcon(start, level), h1 = h0 + bcon(hours, level);
            if (const UiElement* e = findWindow(*jobWin, 0x27E74B64))
                hk.captions[e] = printf1(printf1(str(c, 0x89, 0xC), hourText(h0)), hourText(h1));
            if (const UiElement* e = findWindow(*jobWin, 0xEC2CFCFD)) {
                char b[16];
                std::snprintf(b, sizeof b, "%d", data[pd::Pto] / 100);
                hk.captions[e] = printf1(str(c, 0x88, 0xC), b);
            }
            // Work days: the light-blue (pressed) ones are the days of the
            // job's work day flags, bit 0 = Sunday (inferred).
            if (const UiElement* daysWin = findWindow(*jobWin, 0x47E74B6E)) {
                const int flags = bcon(days, level);
                for (int d = 0; d < 7; ++d)
                    if (const UiElement* b = daysWin->child(0x0C1FC411 + uint32_t(d))) {
                        hk.buttonState[b] = flags & (1 << d) ? 2 : 1;
                        if (!(flags & (1 << d))) // days off: dark blue
                            hk.colors[b] = {0.25f, 0.3f, 0.75f, 1};
                    }
            }
        } else if (jobWin) {
            for (uint32_t id : {0x77E74B47u, 0x47E74B6Du, 0x27E74B64u, 0xEC2CFCFDu})
                if (const UiElement* e = findWindow(*jobWin, id); e && !e->attr("captionres"))
                    hk.captions[e] = "";
        }
        for (uint32_t id : {0x4C2D86DFu, 0x0DB4B9D6u}) // the career/school icons (not decoded)
            if (const UiElement* e = findWindow(*g, id); e && !e->attr("image"))
                hk.hide.insert(e);
        if (const UiElement* e = findWindow(*g, 0xEC2D3BBA)) // the school grade
            hk.captions[e] = (group == 0xA2 || group == 0xA3) && !guid ? gradeLetter(c, data[pd::SchoolGrade]) : "";
    }
    // Performance (-100..100) and the family friends, with a job.
    if (const UiElement* perf = root.child(0xA7)) {
        if (working) {
            hk.show.insert(perf);
            fillBar(c, *perf, float(data[pd::JobPerformance]), -100, 100, hk);
        } else {
            hk.hide.insert(perf);
        }
    }
    // Family friends, shown only while short of the next level's (§7.2):
    // "( + %s )" with the ones missing.
    if (const UiElement* fr = root.child(0xA8)) {
        const int16_t fam = data[pd::FamilyNumber];
        const auto f = c.vm.families.find(fam);
        const int have = f != c.vm.families.end() ? f->second.friends : 0;
        const int need = bcon(friends, level + 1);
        (working && have < need ? hk.show : hk.hide).insert(fr);
        if (const UiElement* e = fr->child(0x4C3CC65C))
            hk.captions[e] = std::to_string(have);
        if (const UiElement* e = fr->child(0x4C3CC65D))
            hk.captions[e] = printf1(e->get("caption"), std::to_string(need - have));
    }
    // Skills (cTSUISkillsCtrl: 0..1000 in 10 divisions; person data by the
    // captions: the windows 0x4BDEFF01-07 are cooking, mechanical, charisma,
    // body, logic, creativity, cleaning).
    if (const UiElement* skills = root.child(0xA6)) {
        const std::array<int, 7> which = {pd::Cooking, pd::Mechanical, pd::Charisma, pd::Body,
                                          pd::Logic,   pd::Creativity, pd::Cleaning};
        for (size_t i = 0; i < which.size(); ++i)
            if (const UiElement* bar = skills->child(0x4BDEFF01 + uint32_t(i))) {
                stepBar(c, *bar, float(data[size_t(which[i])]), hk);
                if (const UiElement* m = bar->child(5)) // the partial-progress marker
                    hk.hide.insert(m);
            }
    }
    const UiElement* bg = root.child(0x8CB609AC);
    const float bx = bg ? float(bg->area()[0]) : 12;
    const auto ra = root.area();
    placeWindow(c, root, x - bx, bottom - float(ra[3] - ra[1]), hk);
    rectSolid(c, bg);
    return x + (bg ? float(bg->area()[2] - bg->area()[0]) : 487);
}

// --- Simology ------------------------------------------------------------------

float LivePanels::simology(PanelContext& c, vm::ObjectId simId, float x, float bottom) {
    const UiElement& root = scripts_[Simology]->roots.front();
    const vm::Object* sim = c.vm.object(simId);
    if (!sim || !sim->person)
        return x;
    const auto& data = sim->person->data;
    UiDrawHooks hk;
    const std::array<uint32_t, 3> tabs = {0x10000000, 0x20000000, 0x30000000};
    const std::array<uint32_t, 3> buttons = {0x40000000, 0x50000000, 0x60000000};
    for (int i = 0; i < 3; ++i) {
        if (const UiElement* e = root.child(tabs[size_t(i)]))
            (i == simologyTab_ ? hk.show : hk.hide).insert(e);
        if (const UiElement* b = root.child(buttons[size_t(i)]))
            hk.buttonState[b] = i == simologyTab_ ? 2 : 1;
    }
    const UiElement* stat = root.child(0x10000000);
    if (stat) {
        if (const UiElement* e = stat->child(0xA002)) // the collapsed panel shows the "minimized" art
            hk.hide.insert(e);
        // Personality (UpdatePersonalityBar): the bar is the current value,
        // the outline (window 0xAA) the original one.
        struct Trait {
            uint32_t window;
            int now, original;
        };
        const std::array<Trait, 5> traits = {{{0x1001, pd::Neat, pd::OriginalNeat},
                                              {0x1003, pd::Outgoing, pd::OriginalOutgoing},
                                              {0x1005, pd::Active, pd::OriginalActive},
                                              {0x1004, pd::Playful, pd::OriginalPlayful},
                                              {0x1002, pd::Nice, pd::OriginalNice}}};
        for (const Trait& t : traits)
            if (const UiElement* bar = stat->child(t.window)) {
                stepBar(c, *bar, float(data[size_t(t.now)]), hk);
                if (const UiElement* orig = bar->child(0xAA))
                    stepBar(c, *orig, float(data[size_t(t.original)]), hk);
            }
        if (const UiElement* z = stat->child(0x1006)) { // the zodiac sign's picture
            const int sign = zodiacSign(data[pd::Zodiac]);
            if (sign >= 0) {
                if (const RgbaImage* img = c.res.image({kTypeUiImage, 0x499DB772, kZodiacImages[size_t(sign)]}))
                    hk.images[z] = img;
            } else {
                hk.hide.insert(z);
            }
        }
        for (uint32_t id : {0x1009u, 0x100Au, 0x100Bu}) // fitness trend arrows
            if (const UiElement* e = findWindow(*stat, id))
                hk.hide.insert(e);
    }
    // Interests and memories: their thumbnails are not drawn yet; the
    // collapsed variants only.
    for (uint32_t tab : {0x20000000u, 0x30000000u})
        if (const UiElement* t = root.child(tab)) {
            if (const UiElement* e = t->child(0x2000))
                hk.hide.insert(e);
            if (const UiElement* e = t->child(0x1000))
                hk.show.insert(e);
        }
    // The background's left (window 0xA001 at 8 + 2 in the stats tab) at x,
    // the panel's bottom at `bottom`.
    const auto ra = root.area();
    placeWindow(c, root, x - 10, bottom - float(ra[3] - ra[1]), hk);
    if (stat) {
        rectSolid(c, stat->child(0xA001));
        const int sign = zodiacSign(data[pd::Zodiac]);
        if (auto r = rectOf(c, stat->child(0x1006)); r && sign >= 0)
            c.tip(*r, str(c, 0x90, uint32_t(sign + 1)));
    }
    for (int i = 0; i < 3; ++i)
        if (auto r = rectOf(c, root.child(buttons[size_t(i)])))
            c.click(*r, [this, i, snd = c.sound] { simologyTab_ = i, snd("ui_secondary_button"); });
    return x + 489;
}

// --- Rewards -------------------------------------------------------------------

float LivePanels::rewards(PanelContext& c, vm::ObjectId, float x, float bottom) {
    // The career rewards catalog (cTSWinProcPanelRewards); the aspiration
    // rewards are an expansion's. No reward objects are listed yet.
    const UiElement& root = scripts_[Rewards]->roots.front();
    UiDrawHooks hk;
    for (uint32_t id : {0x10000000u, 0x30000000u, 0x40000000u, 0x60000000u})
        if (const UiElement* e = root.child(id))
            hk.hide.insert(e);
    for (uint32_t id : {0x20000000u, 0x50000000u})
        if (const UiElement* e = root.child(id))
            hk.show.insert(e);
    const auto ra = root.area();
    placeWindow(c, root, x - 3, bottom - float(ra[3] - ra[1]), hk);
    rectSolid(c, root.child(0x20000000));
    return x + 496;
}

} // namespace os2
