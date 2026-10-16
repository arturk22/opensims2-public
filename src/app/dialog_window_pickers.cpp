// The Dialog windows of the picking dialogs (dialog_window.hpp): the phone
// books (cTSWinProcPhoneBookServices/Neighbors/Party), the aspiration
// (cTSWinProcSetAspirationDialog with its wants catalog), buying food
// (cTSWinProcFillDialog) and the racks of items (cTSWinProcSelectItemsDialog).
// Numbers marked "binary" are from the Mac BV executable
// (docs/primitives-mac.md "Dialog"); what is marked inferred is not read.

#include "app/dialog_window_internal.hpp"

#include "formats/font.hpp"
#include "formats/ui_resources.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace os2 {

// --- cTSWinProcPhoneBook* -----------------------------------------------------

namespace {
// Phone book window ids (scripts 0x8C159242-44).
constexpr uint32_t kPhoneOk = 0xAAAA, kPhoneCancel = 0xBBBB;
// FUN_0003d7dc, the case-blind compare of the phone books' sorts: each byte
// sign-extended and lowered (tolower leaves the negative ones, the UTF-8
// bytes of accented letters, as they are), so those come before every ASCII
// letter and a name sorts after a longer one whose next byte is one of them.
int compareText(const std::string& a, const std::string& b) {
    for (size_t i = 0;; ++i) {
        const int x = i < a.size() ? int(static_cast<signed char>(a[i])) : 0;
        const int y = i < b.size() ? int(static_cast<signed char>(b[i])) : 0;
        const int lx = x >= 0 ? std::tolower(x) : x, ly = y >= 0 ? std::tolower(y) : y;
        if (lx != ly)
            return lx < ly ? -1 : 1;
        if (lx == 0)
            return 0;
    }
}
// cTSUIPhoneBookEntryNeighbor::CompareByName (BV 0xa7a118), the party's
// lists' name sort (ListBoxSortPredicate_Name): the last names, then the
// first names.
int compareByName(const vm::DialogEntry* a, const vm::DialogEntry* b) {
    if (int c = compareText(a->last, b->last))
        return c;
    return compareText(a->first.empty() ? a->name : a->first, b->first.empty() ? b->name : b->first);
}
bool byName(const vm::DialogEntry* a, const vm::DialogEntry* b) { return compareByName(a, b) < 0; }
// The neighbors' book by name (SortNeighborCallListByName, BV 0xa7c664):
// CompareStrings of the full names (GetFullName, MakeFullName's "first
// last"), case-blind (_CompareStringA -> FUN_0003d7dc).
bool byFullName(const vm::DialogEntry* a, const vm::DialogEntry* b) { return compareText(a->name, b->name) < 0; }
// CompareByRelationship (BV 0xa7a204): the ones with love or a crush
// first, then the higher daily + lifetime score, then CompareByName.
bool byRelationship(const vm::DialogEntry* a, const vm::DialogEntry* b) {
    constexpr int Crush = 1, Love = 2;
    const bool fa = a->flags & (Crush | Love), fb = b->flags & (Crush | Love);
    if (fa != fb)
        return fa;
    if (a->relationship != b->relationship)
        return a->relationship > b->relationship;
    return byName(a, b);
}
} // namespace

std::vector<const vm::DialogEntry*> DialogWindow::phoneRows(const vm::Dialog& d, int list) {
    std::vector<const vm::DialogEntry*> rows;
    const Phone& p = phones_[d.id];
    int sort = 0;
    if (d.cls.cls == vm::DialogClass::PhoneParty) {
        if (list == kRowsList) {
            for (const auto& e : d.entries)
                if (!contains(d.selection, e.id))
                    rows.push_back(&e);
        } else {
            for (int16_t id : d.selection)
                for (const auto& e : d.entries)
                    if (e.id == id)
                        rows.push_back(&e);
        }
        sort = list == kRowsList ? p.leftSort : p.rightSort;
    } else {
        for (const auto& e : d.entries)
            rows.push_back(&e);
        if (d.cls.cls == vm::DialogClass::PhoneNeighbors)
            sort = p.mode == 2 || p.mode == 5 ? 1 : 2;
    }
    if (sort == 1)
        std::stable_sort(rows.begin(), rows.end(), d.cls.cls == vm::DialogClass::PhoneParty ? byName : byFullName);
    else if (sort == 2)
        std::stable_sort(rows.begin(), rows.end(), byRelationship);
    return rows;
}


DialogWindow::Layout DialogWindow::phoneDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(d.cls.script);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    using C = vm::DialogClass;
    // The window's panel: a GZWinBMP without an image under the frame
    // overlay, with transparentbkg=no: cGZWinBMP::GZPaint (BV 0x8c7320) fills
    // its area with its fillcolor, opaque (GetFillColorARGB), when flag 2
    // (transparentbkg, InterpretPropertiesGZWinBMP BV 0x95f068) is off.
    // (The pictures with transparentbkg=no get no fill under them here.)
    for (auto& c : root.children)
        if (c.clsid() == "GZWinBMP" && !c.attr("image")) {
            c.set("clsid", "GZWinFlatRect");
            c.set("iid", "IGZWinFlatRect");
        }
    // The texts, as each class's PreShow / SetSelectedSim fills them: the
    // dialog's strings, else the script's own (UIText STR# 0xAB).
    const vm::DialogEntry* chosen = nullptr;
    const int16_t chosenId = d.selection.empty() ? int16_t(0) : d.selection[0];
    for (const auto& e : d.entries)
        if (e.id == chosenId)
            chosen = &e;
    auto callFormat = [&](const std::string& own) {
        std::string format = own;
        if (format.empty() && res_)
            if (auto t = res_->string(UiStringRef{0x7FA7DC7C, 0xAB, 15}))
                format = *t;
        return format.empty() ? std::string("Ligar para %s") : format;
    };
    auto sprintfName = [&](std::string format, const std::string& name) {
        if (auto p = format.find("%s"); p != std::string::npos)
            format.replace(p, 2, name);
        return format;
    };
    if (d.cls.cls == C::PhoneServices) {
        if (!d.title.empty())
            L.captions[0x1000] = d.title;
        L.captions[0x2000] = chosen ? sprintfName(callFormat(d.message), chosen->name) : "";
    } else if (d.cls.cls == C::PhoneNeighbors) {
        if (!d.message.empty())
            L.captions[0xA000] = d.message;
        L.captions[0x6000] = chosen ? sprintfName(callFormat(d.title), chosen->name) : "";
        L.captions[0x7000] = chosen ? chosen->detail : "";
    } else {
        if (!d.title.empty())
            L.captions[0xA000] = d.title;
        if (!d.message.empty())
            L.captions[0xB000] = d.message;
        if (!d.caption2.empty())
            L.captions[0xD000] = d.caption2;
        if (UiElement* limit = findWindow(root, 0xC000)) {
            if (d.caption.empty())
                limit->set("winflag_visible", "no");
            else
                L.captions[0xC000] = d.caption;
        }
    }
    // The list boxes' look: the script's constants table 0xCCCC.
    std::map<std::string, std::string> k = uiConstants(*s);
    auto color = [&](const char* name, std::array<float, 4> fallback) {
        int r, g, b;
        if (auto it = k.find(name); it != k.end() && std::sscanf(it->second.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
            return std::array<float, 4>{float(r) / 255, float(g) / 255, float(b) / 255, 1};
        return fallback;
    };
    auto number = [&](const char* name, uint32_t fallback) {
        auto it = k.find(name);
        return it == k.end() ? fallback : uint32_t(std::stoul(it->second, nullptr, 0));
    };
    const float rowH = float(number("kListBoxRowHeight", 20));
    const auto textNormal = color("kListBoxTextColorNormal", {0.02f, 0.02f, 0.44f, 1});
    const auto textSelected = color("kListBoxTextColorSelected", {0, 1, 0, 1});
    const auto rowSelected = color("kListBoxSelectedRowColor", {0.43f, 0.65f, 0.71f, 1});
    const std::string listFont = hex(number("kListBoxFont", 0x8DE3D64F));
    const uint32_t iconFriend = number("kListBoxIconFriend", 0xA9590161), iconEnemy = number("kListBoxIconEnemy", 0xA9590157);
    const uint32_t iconCrush = number("kListBoxIconCrush", 0xA9590166), iconLove = number("kListBoxIconLove", 0xA9590171);

    struct ListRows {
        uint32_t window = 0;
        int list = 0;
        std::vector<const vm::DialogEntry*> rows;
        int16_t focus = 0;
    };
    std::vector<ListRows> lists;
    // A party starts on the first neighbor (PreShow selects row 0 of 0x5000).
    if (d.cls.cls == C::PhoneParty && !partyFocus_.count(d.id))
        partyFocus_[d.id] = kRowsList << 16;
    const int focus = partyFocus_[d.id];
    const Phone& view = phones_[d.id];
    const bool slides = d.cls.cls == C::PhoneNeighbors && view.mode > 3;
    if (d.cls.cls == C::PhoneParty) {
        ListRows left{0x5000, kRowsList, phoneRows(d, kRowsList), 0}, right{0x6000, kRowsInvite, phoneRows(d, kRowsInvite), 0};
        if (focus >= 0) {
            ListRows& l = (focus >> 16) == kRowsInvite ? right : left;
            const size_t row = size_t(focus & 0xFFFF);
            if (row < l.rows.size())
                l.focus = l.rows[row]->id, chosen = l.rows[row];
        }
        lists = {left, right};
    } else {
        ListRows l{d.cls.cls == C::PhoneServices ? 0x4000u : 0x8001u, kRowsList, phoneRows(d, kRowsList), chosenId};
        if (!slides)
            lists = {l};
    }
    // The neighbors' view (SetMode): the text list 0x8000 or the slides 0x9000
    // (the heads of script 0x8C159245 in 0x1006: 7 x 4 of 52 x 52, the
    // overlay pressed for the chosen one, the friend / enemy and love / crush
    // marks), and the sort and view buttons pressed as the mode is.
    if (d.cls.cls == C::PhoneNeighbors) {
        const bool byNameNow = view.mode == 2 || view.mode == 5;
        L.states[0x1000] = byNameNow ? 2 : 1;
        L.states[0x2000] = byNameNow ? 1 : 2;
        L.states[0x3000] = slides ? 2 : 1;
        L.states[0x4000] = slides ? 1 : 2;
        if (UiElement* e = findWindow(root, 0x8000))
            e->set("winflag_visible", slides ? "no" : "yes");
        if (UiElement* e = findWindow(root, 0x9000))
            e->set("winflag_visible", slides ? "yes" : "no");
        std::array<float, 4> gr{};
        UiElement* panel = findWin(root, 0x9000, 0, 0, &gr);
        std::array<float, 4> cr{};
        UiElement* grid = panel ? findWin(*panel, 0x1006, gr[0], gr[1], &cr) : nullptr;
        if (slides && grid) {
            const auto rows = phoneRows(d, kRowsList);
            constexpr int cols = 7, rowsN = 4, per = cols * rowsN;
            Phone& pv = phones_[d.id];
            pv.first = std::clamp(pv.first, 0, std::max(0, (int(rows.size()) - 1) / per * per));
            grid->set("winflag_visible", "yes");
            grid->set("style", "nofill"); // a container (hidden in the script, black if filled)
            uint32_t serial = 0;
            for (int k = 0; k < per && pv.first + k < int(rows.size()); ++k) {
                const vm::DialogEntry& e = *rows[size_t(pv.first + k)];
                const RgbaImage* head = thumbnail ? thumbnail(0, e.id) : nullptr;
                const float x = float(k % cols) * 52, y = float(k / cols) * 52;
                const UiScript* cs = script(0x8C159245);
                if (!cs || cs->roots.empty())
                    break;
                UiElement cell = cs->roots.front();
                cell.set("area", uiArea(int(x), int(y), int(x) + 52, int(y) + 52));
                const uint32_t base = 0x00C00000u | (serial++ << 8);
                constexpr int Crush = 1, Love = 2, Friend = 16, BestFriend = 32, Enemy = 128;
                for (auto& c : cell.children) {
                    const uint32_t id = c.id();
                    if (id == 1 && head) {
                        c.set("image", "");
                        L.images[base | 1] = head;
                    }
                    if (id == 0 && head)
                        c.set("winflag_visible", "no");
                    if (id == 2)
                        L.states[base | 2] = e.id == chosenId ? 2 : 1;
                    if (id == 3)
                        c.set("winflag_visible", (e.flags & (Friend | BestFriend)) ? "yes" : "no");
                    if (id == 4)
                        c.set("winflag_visible", (e.flags & Enemy) && !(e.flags & (Friend | BestFriend)) ? "yes" : "no");
                    if (id == 5)
                        c.set("winflag_visible", (e.flags & Love) ? "yes" : "no");
                    if (id == 6)
                        c.set("winflag_visible", (e.flags & Crush) && !(e.flags & Love) ? "yes" : "no");
                    c.set("id", hex(base | id));
                }
                grid->children.push_back(std::move(cell));
                L.regions.push_back({{L.x0 + cr[0] + x, L.y0 + cr[1] + y, L.x0 + cr[0] + x + 52, L.y0 + cr[1] + y + 52},
                                     Hit::Row, pv.first + k});
            }
            std::array<float, 4> q{};
            if (findWin(*panel, 0x1004, gr[0], gr[1], &q))
                L.regions.push_back({{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}, Hit::Prev, 0});
            if (findWin(*panel, 0x1005, gr[0], gr[1], &q))
                L.regions.push_back({{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}, Hit::Next, 0});
        }
    }
    if (d.cls.cls == C::PhoneParty) {
        L.states[0x3000] = view.leftSort == 1 ? 2 : 1;
        L.states[0x4000] = view.leftSort == 2 ? 2 : 1;
        L.states[0x7000] = view.rightSort == 1 ? 2 : 1;
        L.states[0x8000] = view.rightSort == 2 ? 2 : 1;
    }
    // The chosen row's thumbnail (services 0x3001, neighbors 0x5002, party 0x9002).
    const uint32_t thumbId = d.cls.cls == C::PhoneServices ? 0x3001 : d.cls.cls == C::PhoneNeighbors ? 0x5002 : 0x9002;
    if (chosen && thumbnail) {
        const bool object = d.cls.cls == C::PhoneServices;
        if (const RgbaImage* t = thumbnail(object ? chosen->id : int16_t(0), object ? int16_t(0) : chosen->id))
            L.images[thumbId] = t;
    }
    // Hidden by the class until it fills them: the grid view of the
    // neighbors (0x9000) stays hidden in the list view.
    for (const auto& list : lists) {
        const UiElement* box = findWindow(root, list.window);
        if (!box)
            continue;
        // The rows, drawn after the list box window (its canvas rectangle is
        // found by walking the parents' areas).
        float ox = 0, oy = 0;
        std::function<bool(const UiElement&, float, float)> find = [&](const UiElement& e, float px, float py) {
            const auto r = e.area();
            if (&e == box) {
                ox = px + float(r[0]), oy = py + float(r[1]);
                return true;
            }
            for (const auto& c : e.children)
                if (find(c, px + float(r[0]), py + float(r[1])))
                    return true;
            return false;
        };
        for (const auto& c : root.children)
            if (find(c, 0, 0))
                break;
        const auto r = box->area();
        const float bw = float(r[2] - r[0]), bhh = float(r[3] - r[1]);
        for (size_t i = 0; i < list.rows.size() && float(i + 1) * rowH <= bhh + 0.5f; ++i) {
            const vm::DialogEntry& e = *list.rows[i];
            const float ry = oy + float(i) * rowH;
            const bool on = e.id == list.focus && (d.cls.cls != C::PhoneParty || focus >= 0);
            if (on) {
                UiElement sel = element("GZWinFlatRect", "IGZWinFlatRect", ox, ry, ox + bw, ry + rowH);
                char c[48];
                std::snprintf(c, sizeof c, "(%d,%d,%d)", int(rowSelected[0] * 255), int(rowSelected[1] * 255),
                              int(rowSelected[2] * 255));
                sel.set("fillcolor", c);
                root.children.push_back(sel);
            }
            // cTSWinProcPhoneBookParty::onAttach (BV 0x34264c): the columns
            // 3, width - 33, 15 and 15 (AddColumn, left aligned, no gutter);
            // AddEntry (BV 0x340b48): the full name in column 1, cut to the
            // column with "..." (FUN_0019450c), the love or else crush icon
            // in column 2 and the friend or else (no friendship) enemy icon
            // in column 3 (inferred: the neighbors' book has the party's
            // columns; the services' list, without icons, its whole width).
            // cWinListBoxRow::GZPaint (BV 0x194830): each element at its
            // column's left, centred in the row ((row - height) >> 1), the
            // icon strip's frame 1 on the chosen row, 0 on the others.
            constexpr int Crush = 1, Love = 2, Friend = 16, BestFriend = 32, Enemy = 128;
            const bool marks = d.cls.cls != C::PhoneServices;
            constexpr float kGutterColumn = 3, kIconColumn = 15;
            const float nameX = marks ? kGutterColumn : 4, nameW = marks ? bw - kGutterColumn - 2 * kIconColumn : bw - 8;
            UiElement t = element("GZWinText", "IGZWinText", ox + nameX, ry, ox + nameX + nameW, ry + rowH);
            const uint32_t id = 0x00F00000u | uint32_t(list.list << 12) | uint32_t(i);
            t.set("id", hex(id));
            t.set("font", listFont);
            t.set("align", "leftcenter");
            const auto& tc = on ? textSelected : textNormal;
            char c[48];
            std::snprintf(c, sizeof c, "(%d,%d,%d)", int(tc[0] * 255), int(tc[1] * 255), int(tc[2] * 255));
            t.set("forecolor", c);
            root.children.push_back(t);
            L.captions[id] = fitText(listFont, e.name, nameW);
            auto iconAt = [&](uint32_t image, float x, uint32_t column) {
                if (!res_ || !image)
                    return;
                const RgbaImage* im = frame(image, on ? 1 : 0);
                if (!im || !im->width)
                    return;
                const float y = ry + std::floor((rowH - float(im->height)) / 2);
                UiElement p = element("GZWinBMP", "IGZWinBMP", x, y, x + float(im->width), y + float(im->height));
                const uint32_t pid = 0x00E00000u | uint32_t(list.list << 12) | uint32_t(i << 1) | column;
                p.set("id", hex(pid));
                root.children.push_back(p);
                L.images[pid] = im;
            };
            if (marks) {
                iconAt(e.flags & Love ? iconLove : e.flags & Crush ? iconCrush : 0, ox + bw - 2 * kIconColumn, 0);
                iconAt(e.flags & (Friend | BestFriend) ? iconFriend : e.flags & Enemy ? iconEnemy : 0, ox + bw - kIconColumn, 1);
            }
            L.regions.push_back({{L.x0 + ox, L.y0 + ry, L.x0 + ox + bw, L.y0 + ry + rowH}, Hit::Row,
                                 (list.list << 16) | int(i)});
        }
    }
    auto region = [&](uint32_t id, Hit kind, int value) {
        float ox = 0, oy = 0;
        const UiElement* found = nullptr;
        std::function<void(const UiElement&, float, float)> walk = [&](const UiElement& e, float px, float py) {
            const auto r = e.area();
            if (e.id() == id && !found) {
                found = &e, ox = px + float(r[0]), oy = py + float(r[1]);
                return;
            }
            for (const auto& c : e.children)
                walk(c, px + float(r[0]), py + float(r[1]));
        };
        for (const auto& c : root.children)
            walk(c, 0, 0);
        if (!found)
            return;
        const auto r = found->area();
        L.regions.push_back({{L.x0 + ox, L.y0 + oy, L.x0 + ox + float(r[2] - r[0]), L.y0 + oy + float(r[3] - r[1])}, kind, value});
        if (kind == Hit::Button)
            L.buttonIds.push_back(id);
    };
    region(kPhoneOk, Hit::Button, 0);
    region(kPhoneCancel, Hit::Button, 1);
    if (d.cls.cls == C::PhoneParty) {
        region(0x2000, Hit::Add, 0);
        region(0x1000, Hit::Remove, 0);
        for (uint32_t id : {0x3000u, 0x4000u, 0x7000u, 0x8000u})
            region(id, Hit::Sort, int(id));
    }
    if (d.cls.cls == C::PhoneNeighbors)
        for (uint32_t id : {0x1000u, 0x2000u, 0x3000u, 0x4000u})
            region(id, Hit::Sort, int(id));
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcSetAspirationDialog ------------------------------------------

// Script 0xCC16A49E: "%s is no longer a child..." (0x2D9B8E78 for a girl,
// 0x2D9B8E7E for a boy), the Sim's head (0x2D9C8C9E), the five
// aspirations (0x0CDC000n = bit 1 << (n - 1)) and Accept 0xA000. The
// slide with the aspiration's description (0x2000) is not shown yet.
DialogWindow::Layout DialogWindow::aspirationDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(d.type == 0x22 ? 0xCAFE0045 : 0xCC16A49E);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    for (auto& c : root.children) {
        const uint32_t id = c.id();
        if (id == 0x2000) {
            // RefreshInfoPanel: once an aspiration is picked the slide comes
            // out to the right (onTick moves its 0x0DA39A45 to the parent's
            // width less its own; here at once, inferred) with the
            // aspiration's name (0x4D9B8E8F) and description (0xCDA394CA)
            // from Live.package's STR# 0x91 (romance 13 / 6, family 9 / 1,
            // fortune 11 / 3, popularity 10 / 2, knowledge 12 / 4); under
            // the description the "Big Life Moments" wants catalog
            // (0xCDA395BB, cTSWinProcWantsCatalog: SetAspiration ->
            // LoadPowerWantsByAspiration, the want simulator's lifetime
            // goals of the aspiration).
            int title = 0, text = 0;
            switch (d.aspiration) {
            case 0x01: title = 13, text = 6; break;
            case 0x02: title = 9, text = 1; break;
            case 0x04: title = 11, text = 3; break;
            case 0x10: title = 10, text = 2; break;
            case 0x20: title = 12, text = 4; break;
            case 0x40: title = 14, text = 7; break;
            case 0x80: title = 40, text = 42; break;
            default: break;
            }
            c.set("winflag_visible", title ? "yes" : "no");
            const auto pa = c.area();
            if (UiElement* slide = c.child(0x0DA39A45)) {
                const auto sa = slide->area();
                const int dx = (pa[2] - pa[0]) - (sa[2] - sa[0]);
                slide->set("area", uiArea(dx, sa[1], dx + sa[2] - sa[0], sa[3]));
                for (auto& k : slide->children)
                    if (k.id() == 0x300A || (k.id() == 0xCDA395BB && (!title || !lifetimeWants)))
                        k.set("winflag_visible", "no");
            }
            auto live = [&](int n) {
                std::optional<std::string> t;
                if (res_ && n)
                    t = res_->string(UiStringRef{0x7F96C284, 0x91, uint32_t(n - 1)}); // one-based in the binary
                return t.value_or(std::string());
            };
            if (title) {
                L.captions[0x4D9B8E8F] = live(title);
                L.captions[0xCDA394CA] = live(text);
                // The description under the name, as tall as its text
                // (RefreshInfoPanel sizes and stacks them).
                if (UiElement* slide = c.child(0x0DA39A45)) {
                    const UiElement* head = slide->child(0x4D9B8E8F);
                    if (UiElement* desc = slide->child(0xCDA394CA); desc && head) {
                        const auto da = desc->area();
                        const int top = head->area()[3];
                        const auto lines = wrap(desc->get("font"), L.captions[0xCDA394CA], float(da[2] - da[0]));
                        const int h = int(std::ceil(float(lines.size()) * lineHeight(desc->get("font")))) + 4;
                        desc->set("area", uiArea(da[0], top, da[2], std::max(da[3], top + h)));
                    }
                    if (lifetimeWants)
                        wantsCatalog(L, *slide, {L.x0 + float(pa[0]) + float(slide->area()[0]), L.y0 + float(pa[1])},
                                     lifetimeWants(d.aspiration));
                }
            }
        }
        if (id == 0x2D9B8E78 || id == 0x2D9B8E7E) {
            const bool mine = (id == 0x2D9B8E78) == d.female;
            c.set("winflag_visible", mine ? "yes" : "no");
            if (mine) {
                std::string text = c.get("caption");
                if (res_)
                    if (auto ref = uiStringRef(c.get("captionres")))
                        if (auto t = res_->string(*ref))
                            text = *t;
                if (auto p = text.find("%s"); p != std::string::npos)
                    text.replace(p, 2, d.text); // the Sim's first name
                L.captions[id] = text;
            }
        }
        if ((id & 0xFFFF0000u) == 0x0CDC0000u) {
            const auto r = c.area();
            L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])}, Hit::Choice,
                                 1 << ((id & 0xFFFF) - 1)});
        }
        if (id == 0x2D9C8C9E && thumbnail)
            if (const RgbaImage* head = thumbnail(0, d.aspirationNeighbor))
                if (UiElement* t = c.child(1)) {
                    // The head inside the round frame (the window's image's alpha).
                    const auto ref = uiResourceRef(t->get("image"));
                    const RgbaImage* mask = ref && res_ ? res_->image(*ref) : nullptr;
                    auto& slot = icons_[d.id ^ 0x80000000u];
                    if (!slot && mask && mask->width) {
                        slot = std::make_unique<RgbaImage>(*mask);
                        for (uint32_t y = 0; y < slot->height; ++y)
                            for (uint32_t x = 0; x < slot->width; ++x) {
                                const uint32_t sx = x * head->width / slot->width, sy = y * head->height / slot->height;
                                const uint8_t* src = &head->pixels[(size_t(sy) * head->width + sx) * 4];
                                uint8_t* p = &slot->pixels[(size_t(y) * slot->width + x) * 4];
                                p[0] = src[0], p[1] = src[1], p[2] = src[2];
                                p[3] = uint8_t(int(p[3]) * int(src[3]) / 255);
                            }
                    }
                    t->set("id", hex(0x2D9C8C01));
                    L.images[0x2D9C8C01] = slot ? slot.get() : head;
                }
        if (id == 0xA000) {
            const auto r = c.area();
            L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])}, Hit::Button, 0});
            L.buttonIds.push_back(0xA000);
        }
    }
    L.root = std::move(root);
    return L;
}

// cTSWinProcWantsCatalog under the aspiration's description
// (RefreshInfoPanel, BV 0x3cf954): the catalog moved to the description's
// bottom, the slide as tall as the catalog's bottom plus the margin it had
// in the script (onAttach: slide height - catalog bottom) and its picture
// with it. The grid 0x1006 holds the wants in kCollapsedColumns 5 x
// kCollapsedRows 2 cells of script 0x49065095 (CreateThumbnail) with the
// script's margins (1, 3; inferred spacing); SetWant (BV 0xaa6748): window 1
// the want background or 2 the fear one, 3 the power want burst for a Power
// want, 4 the primary icon under the window's own image as alpha mask
// (nTSUI::ApplyAlphaMask), 5 the modifier icon (the secondary).
void DialogWindow::wantsCatalog(Layout& L, UiElement& slide, std::array<float, 2> slideAbs,
                                const std::vector<LifetimeWant>& wants) {
    UiElement* cat = slide.child(0xCDA395BB);
    const UiElement* desc = slide.child(0xCDA394CA);
    if (!cat || !desc)
        return;
    const auto sa = slide.area(), ca = cat->area();
    const int margin = (sa[3] - sa[1]) - ca[3];
    const int top = desc->area()[3];
    cat->set("area", uiArea(ca[0], top, ca[2], top + (ca[3] - ca[1])));
    const int bottom = top + (ca[3] - ca[1]) + margin;
    slide.set("area", uiArea(sa[0], sa[1], sa[2], sa[1] + std::max(bottom, sa[3] - sa[1])));
    if (UiElement* bg = slide.child(1)) {
        const auto ba = bg->area();
        bg->set("area", uiArea(ba[0], ba[1], ba[2], ba[1] + std::max(bottom, sa[3] - sa[1])));
    }
    UiElement* grid = cat->child(0x1006);
    if (!grid)
        return;
    const UiScript* cs = script(0x49065095);
    if (!cs || cs->roots.empty())
        return;
    const auto ga = grid->area();
    (void)slideAbs;
    constexpr int kCols = 5, kRows = 2, kMarginX = 1, kMarginY = 3;
    const auto ra = cs->roots.front().area();
    const int cw = ra[2] - ra[0], ch = ra[3] - ra[1];
    for (size_t k = 0; k < wants.size() && k < size_t(kCols * kRows); ++k) {
        const LifetimeWant& w = wants[k];
        UiElement cell = cs->roots.front();
        const int x = kMarginX + int(k % kCols) * (cw + kMarginX), y = kMarginY + int(k / kCols) * (ch + kMarginY);
        cell.set("area", uiArea(x, y, x + cw, y + ch));
        const uint32_t base = 0x00D00000u | (uint32_t(k) << 8);
        cell.set("id", hex(base));
        for (auto& c : cell.children) {
            const uint32_t id = c.id();
            if (id == 1)
                c.set("winflag_visible", w.fear ? "no" : "yes");
            if (id == 2)
                c.set("winflag_visible", w.fear ? "yes" : "no");
            if (id == 3)
                c.set("winflag_visible", w.power ? "yes" : "no");
            if (id == 4) {
                const auto ref = uiResourceRef(c.get("image"));
                if (const RgbaImage* icon = maskedIcon(w.icon, ref ? ref->instance : 0))
                    L.images[base | 4] = icon;
                else
                    c.set("winflag_visible", "no");
            }
            if (id == 5) {
                const RgbaImage* m = w.modifier && res_ ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, w.modifier}) : nullptr;
                if (m)
                    L.images[base | 5] = m;
                else
                    c.set("winflag_visible", "no");
            }
            if (id == 6)
                c.set("winflag_visible", "no"); // the click strip (the catalog only shows them)
            c.set("id", hex(base | id));
        }
        grid->children.push_back(std::move(cell));
    }
    (void)ga;
}

// A want's icon scaled to its mask's size with the mask's alpha.
const RgbaImage* DialogWindow::maskedIcon(uint32_t icon, uint32_t mask) {
    if (!icon || !res_)
        return nullptr;
    const uint64_t key = (uint64_t(icon) << 32) | mask;
    if (auto it = masked_.find(key); it != masked_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> out;
    const RgbaImage* src = res_->image(UiResourceRef{kTypeUiImage, kImageGroup, icon});
    const RgbaImage* m = mask ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, mask}) : nullptr;
    if (src && src->width && src->height) {
        out = std::make_unique<RgbaImage>();
        const uint32_t w = m && m->width ? m->width : src->width, h = m && m->height ? m->height : src->height;
        out->width = w, out->height = h;
        out->pixels.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* sp = &src->pixels[(size_t(y * src->height / h) * src->width + x * src->width / w) * 4];
                uint8_t* p = &out->pixels[(size_t(y) * w + x) * 4];
                p[0] = sp[0], p[1] = sp[1], p[2] = sp[2];
                p[3] = m && m->width ? uint8_t(int(sp[3]) * int(m->pixels[(size_t(y) * w + x) * 4 + 3]) / 255) : sp[3];
            }
    }
    return (masked_[key] = std::move(out)).get();
}

// --- cTSWinProcFillDialog ----------------------------------------------------

// Script 0x2CB242F8 ("Comprar Comida"): the fill bar (0xCCB5FD79: up 2,
// down 1, the bar 3) from the units the Sim has to the most it takes, the
// food cost (0x4DADF755), the delivery (0x4CB613E7) and the total
// (0x4CB613F8), OK 0xACB50208 and cancel 0xACB50200. The bar's step per
// click (10 units) and dragging on it are inferred.
DialogWindow::Layout DialogWindow::fillDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x2CB242F8);
    if (!s || d.type != 14)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    const int added = std::max(0, d.fillValue - d.fillUnits);
    const int total = fillCost(d, added);
    L.captions[0x4DADF755] = simoleons(total - d.fillDelivery);
    L.captions[0x4CB613E7] = simoleons(d.fillDelivery);
    L.captions[0x4CB613F8] = simoleons(total);
    L.captions[0x0CB613B4] = std::to_string(added);
    std::array<float, 4> fillRect{0, 0, 0, 0};
    for (auto& c : root.children) {
        const auto r = c.area();
        const std::array<float, 4> rr{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        if (c.id() == 0xCCB5FD79) {
            for (auto& k : c.children) {
                const auto q = k.area();
                const std::array<float, 4> kr{rr[0] + float(q[0]), rr[1] + float(q[1]), rr[0] + float(q[2]), rr[1] + float(q[3])};
                if (k.id() == 2)
                    L.regions.push_back({kr, Hit::Next, 0});
                if (k.id() == 1)
                    L.regions.push_back({kr, Hit::Prev, 0});
                if (k.id() == 3) {
                    // The bar filled from the bottom to the value, in the
                    // dark blue of the "gradient" strip 0x8E900055 (how the
                    // control cuts that strip is not known: a flat fill).
                    const float full = float(q[3] - q[1]);
                    const float f = d.fillMax > 0 ? std::clamp(float(d.fillValue) / float(d.fillMax), 0.0f, 1.0f) : 0;
                    fillRect = {float(q[0]) + 3, float(q[3]) - std::round(full * f), float(q[2]) - 3, float(q[3])};
                    L.regions.push_back({kr, Hit::Bar, 0});
                }
            }
        }
        if (c.id() == 0xCCB5FD79 && fillRect[3] > fillRect[1]) {
            UiElement bar = element("GZWinFlatRect", "IGZWinFlatRect", fillRect[0], fillRect[1], fillRect[2], fillRect[3]);
            bar.set("fillcolor", "(5,6,113)");
            c.children.push_back(bar);
        }
        if (c.id() == 0xACB50208) {
            L.regions.push_back({rr, Hit::Button, 0});
            L.buttonIds.push_back(0xACB50208);
        }
        if (c.id() == 0xACB50200) {
            L.regions.push_back({rr, Hit::Button, 1});
            L.buttonIds.push_back(0xACB50200);
        }
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcSelectItemsDialog ---------------------------------------------

namespace {
// What the racks' picks cost.
int pickedCost(const vm::Dialog& d) {
    int sum = 0;
    for (const auto& e : d.entries)
        if (contains(d.selection, e.id))
            sum += e.price;
    return sum;
}
constexpr float kItem = 64, kItemMargin = 5; // the grid's item (inferred size; margins from the script)
} // namespace

// Script 0x2CB242F6 ("Fazer Seleção"): one row of the items' thumbnails in
// 0x1006 (kRowCount=1, kItemMarginX/Y=5), previous / next (0x1004/5), OK
// 0xA000, cancel 0xA001. An item the family cannot pay is greyed out when
// picking several (UpdateFunds).
DialogWindow::Layout DialogWindow::selectItemsDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x2CB242F6);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    int& first = pages_[d.id];
    const int spent = pickedCost(d);
    std::vector<UiElement> extra;
    for (auto& c : root.children) {
        const auto r = c.area();
        const std::array<float, 4> rr{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        const uint32_t id = c.id();
        if (id == 0x1006) {
            const float gw = float(r[2] - r[0]), gh = float(r[3] - r[1]);
            const int per = std::max(1, int((gw + kItemMargin) / (kItem + kItemMargin)));
            first = std::clamp(first, 0, std::max(0, int(d.entries.size()) - per));
            const float used = float(per) * kItem + float(per - 1) * kItemMargin;
            for (int i = 0; i < per && first + i < int(d.entries.size()); ++i) {
                const vm::DialogEntry& e = d.entries[size_t(first + i)];
                const float x = float(r[0]) + std::round((gw - used) / 2) + float(i) * (kItem + kItemMargin);
                const float y = float(r[1]) + std::round((gh - kItem) / 2);
                const bool picked = contains(d.selection, e.id);
                const bool affordable = !d.multiSelect || picked ||
                                        e.price <= (d.obeyPrice ? d.fillFunds - spent : d.fillFunds);
                if (picked) {
                    // A green frame (the lists' selected colour; inferred look).
                    for (int k = 0; k < 2; ++k) {
                        UiElement sel = element("GZWinFlatRect", "IGZWinFlatRect", x - 2 + float(k), y - 2 + float(k),
                                                x + kItem + 2 - float(k), y + kItem + 2 - float(k));
                        sel.set("style", "nofill");
                        sel.set("outline", "yes");
                        for (const char* side : {"colorleft", "colortop", "colorright", "colorbottom"})
                            sel.set(side, "(0,255,0)");
                        extra.push_back(sel);
                    }
                }
                UiElement p = element("GZWinBMP", "IGZWinBMP", x, y, x + kItem, y + kItem);
                const uint32_t pid = 0x00D00000u | uint32_t(i);
                p.set("id", hex(pid));
                extra.push_back(p);
                if (thumbnail)
                    if (const RgbaImage* t = thumbnail(e.id, 0))
                        L.images[pid] = t;
                if (!affordable)
                    L.colors[pid] = {1, 1, 1, 0.35f};
                if (affordable)
                    L.regions.push_back({{L.x0 + x, L.y0 + y, L.x0 + x + kItem, L.y0 + y + kItem}, Hit::Row, first + i});
            }
        }
        if (id == 0x1004)
            L.regions.push_back({rr, Hit::Prev, 0});
        if (id == 0x1005)
            L.regions.push_back({rr, Hit::Next, 0});
        if (id == 0xA000 || id == 0xA001) {
            L.regions.push_back({rr, Hit::Button, id == 0xA000 ? 0 : 1});
            L.buttonIds.push_back(id);
        }
    }
    // The items go over the grid window, under the frame (index 5 is the frame's image).
    root.children.insert(root.children.begin() + std::min<size_t>(5, root.children.size()), extra.begin(), extra.end());
    L.root = std::move(root);
    return L;
}

} // namespace os2
