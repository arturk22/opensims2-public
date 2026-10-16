// The dialog window of the app (PieMenuHost): keys into a text-entry dialog,
// Return and Esc, and the layout of the game's scripts (src/app/pie_menu.cpp).

#include "app/pie_menu.hpp"
#include "vm/vm.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"

#include <cstdlib>
#include <memory>
#include <filesystem>
#include <optional>

#include <cstdio>
#include <cmath>
#include <initializer_list>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint32_t kObjGroup = 0x7F000001;
using Target = Behavior::Target;
using Kind = Behavior::TargetKind;

static Behavior::Instruction instr(uint16_t opcode, std::initializer_list<uint8_t> operands) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = {Kind::True, 0};
    in.onFalse = {Kind::False, 0};
    in.nodeVersion = 1;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

static Behavior tree(std::vector<Behavior::Instruction> code) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.instructions = std::move(code);
    return b;
}

static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    t.format = 0xFFFD;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

// A dialog of `type` waits in the VM; returns its host.
struct Fixture {
    CodeLibrary code;
    VM vm{code};
    Object* me = nullptr;
    Thread t;
    explicit Fixture(int type) {
        code.add(kObjGroup, 0x12D, strings({"Title", "Pick a name for the baby", "Yes", "No", "Cancel"}));
        code.add(kObjGroup, 0x1000,
                 tree({instr(0x24, {0, 0, 2, 3, 4, uint8_t(type), 1, 0x00, 0x00, 0, 0, 0, 0, 2, 0})}));
        me = &vm.createObject(kObjGroup);
        t = vm.start(me->id, 0x1000);
        vm.run(t);
    }
};

int main() {
    {
        // Picking: a small object with a thin post (a mailbox) is picked by a
        // click a few pixels beside it; a big object is not, nor is a click far
        // from the small one.
        Fixture f(dialogtype::Message);
        PieMenuHost host(f.vm, "no-fonts");
        auto post = std::make_shared<PickMesh>();
        post->corners = {simd_make_float3(0, 0, 0), simd_make_float3(0.02f, 0, 0), simd_make_float3(0, 0, 1.7f)};
        post->triangleMask = {-1};
        post->uvs.assign(3, simd_make_float2(0, 0));
        post->lo = simd_make_float3(0, 0, 0), post->hi = simd_make_float3(0.02f, 0, 1.7f);
        const ObjectId mailbox = f.vm.createObject(kObjGroup).id;
        host.addPickBox(mailbox, post->lo, post->hi, matrix_identity_float4x4, post);
        const simd_float3 eye = simd_make_float3(0.003f, -10, 1);
        auto ray = [&](float x) { return simd_normalize(simd_make_float3(x, 0, 1) - eye); };
        check(host.pick(eye, ray(0.003f)) == mailbox, "pick: a click on the post");
        check(host.pick(eye, ray(0.05f)) == mailbox, "pick: a click a few pixels beside a small object");
        check(host.pick(eye, ray(0.6f)) == 0, "pick: a click far from it");
        auto wall = std::make_shared<PickMesh>();
        wall->corners = {simd_make_float3(5, 0, 0), simd_make_float3(5.02f, 0, 0), simd_make_float3(5, 0, 4)};
        wall->triangleMask = {-1};
        wall->uvs.assign(3, simd_make_float2(0, 0));
        wall->lo = simd_make_float3(5, 0, 0), wall->hi = simd_make_float3(5.02f, 0, 4);
        const ObjectId bigObject = f.vm.createObject(kObjGroup).id;
        host.addPickBox(bigObject, wall->lo, wall->hi, matrix_identity_float4x4, wall);
        check(host.pick(eye, simd_normalize(simd_make_float3(5.0f + 0.05f, 0, 1) - eye)) == 0,
              "pick: no tolerance for a big object");
        check(host.hasPickBox(mailbox) && host.hasPickBox(bigObject), "pick: the boxes are known");
        host.removePickBox(bigObject);
        check(host.hasPickBox(mailbox) && !host.hasPickBox(bigObject), "pick: a box is removed alone");
        // A big neighbour behind (a fireplace) that the exact ray hits must not
        // beat the small object nearer the eye whose edge is a few pixels off the ray.
        auto hearth = std::make_shared<PickMesh>();
        hearth->corners = {simd_make_float3(-3, 2, 0), simd_make_float3(3, 2, 0), simd_make_float3(-3, 2, 4),
                           simd_make_float3(3, 2, 0), simd_make_float3(3, 2, 4), simd_make_float3(-3, 2, 4)};
        hearth->triangleMask = {-1, -1};
        hearth->uvs.assign(6, simd_make_float2(0, 0));
        hearth->lo = simd_make_float3(-3, 2, 0), hearth->hi = simd_make_float3(3, 2, 4);
        const ObjectId fireplace = f.vm.createObject(kObjGroup).id;
        host.addPickBox(fireplace, hearth->lo, hearth->hi, matrix_identity_float4x4, hearth);
        check(host.pick(eye, ray(0.05f)) == mailbox, "pick: a small object beside the ray beats a big one behind it");
        check(host.pick(eye, ray(0.6f)) == fireplace, "pick: the big one where the ray misses the small one");
        host.removePickBox(fireplace);
        // A Sim out of the world (at work) takes no orders: no pie menu opens.
        host.setSim(f.me->id);
        host.simAway = [&](ObjectId s) { return s == f.me->id; };
        check(!host.open(mailbox, simd_make_float2(10, 10)), "pie: no menu for a Sim at work");
        check(!host.isOpen(), "pie: and none open");
    }
    {
        // Text entry (type 3): typing, Backspace, Return = OK.
        Fixture f(dialogtype::TextEntry);
        PieMenuHost host(f.vm, "no-fonts");
        check(host.dialogOpen() && host.dialogHasTextBox(), "text entry: open with a text box");
        for (char c : std::string("Ana"))
            host.dialogKey(0, std::string(1, c));
        check(host.dialog()->text == "Ana", "text entry: typed text");
        host.dialogKey(51, {}); // Backspace
        check(host.dialog()->text == "An", "text entry: backspace");
        host.dialogKey(0, std::string(1, '\x01')); // a control character is not text
        check(host.dialog()->text == "An", "text entry: control characters ignored");
        host.dialogKey(0, std::string(80, 'x'));
        check(host.dialog()->text.size() == 64, "text entry: 64 characters at most (AddEditBox)");
        host.dialogKey(36, {}); // Return
        check(!host.dialogOpen() && f.vm.dialogs.at(f.me->id).answer == 0, "text entry: Return answers OK");
    }
    {
        // Esc is the last button (SetupDialog's SetKey): the text entry's OK.
        Fixture f(dialogtype::TextEntry);
        PieMenuHost host(f.vm, "no-fonts");
        host.dialogKey(53, {});
        check(f.vm.dialogs.at(f.me->id).answer == 0, "text entry: Esc answers OK");
    }
    {
        // Yes/no: Esc answers No, space does nothing; keys type nothing.
        Fixture f(dialogtype::YesNo);
        PieMenuHost host(f.vm, "no-fonts");
        check(host.dialogOpen() && !host.dialogHasTextBox(), "yes/no: open without a text box");
        host.dialogKey(0, "x");
        check(host.dialog()->text.empty(), "yes/no: keys type nothing");
        host.dialogKey(49, " ");
        check(host.dialogOpen(), "yes/no: space does nothing");
        host.dialogKey(53, {});
        check(f.vm.dialogs.at(f.me->id).answer == 1, "yes/no: Esc answers No");
    }
    {
        Fixture f(dialogtype::YesNoCancel);
        PieMenuHost host(f.vm, "no-fonts");
        host.dialogKey(53, {});
        check(f.vm.dialogs.at(f.me->id).answer == 2, "yes/no/cancel: Esc answers Cancel");
    }
    {
        Fixture f(dialogtype::Message);
        PieMenuHost host(f.vm, "no-fonts");
        host.dialogKey(49, " ");
        check(f.vm.dialogs.at(f.me->id).answer == 0, "message: space answers OK");
    }
    {
        // cTSWinPictureDialog::DoLayout: buttons as wide as their text + 16,
        // at least 82, 12 apart, 34 high, centred; the window centred.
        Fixture f(dialogtype::YesNoCancel);
        DialogWindow w(f.vm, nullptr, nullptr);
        const auto L = w.layout(*PieMenuHost(f.vm, "no-fonts").dialog(), 1024, 768);
        std::vector<std::array<float, 4>> b;
        for (const auto& r : L.regions)
            if (r.kind == DialogWindow::Hit::Button)
                b.push_back(r.rect);
        check(b.size() == 3, "layout: three buttons");
        if (b.size() == 3) {
            check(b[0][2] - b[0][0] >= 82 && b[0][3] - b[0][1] == 34, "layout: buttons at least 82 wide, 34 high");
            check(b[1][0] - b[0][2] == 12 && b[2][0] - b[1][2] == 12, "layout: 12 px between buttons");
            check(std::abs((b[0][0] + b[2][2]) / 2 - (L.x0 + L.x1) / 2) <= 1, "layout: buttons centred");
            check(L.y1 - b[0][3] == 14, "layout: 14 px under the buttons");
        }
        check(L.x1 - L.x0 >= 128 && std::abs((L.x0 + L.x1) / 2 - 512) <= 1, "layout: at least 128 wide, centred");
    }
    {
        // "Continue" (byte 7 bit 7): button 1 is SetupDialog's close box, a
        // window outside the row FUN_0025b1fc sizes (the app looped forever
        // here: the 40 px box in the row was never widened); nothing places
        // it, so it is not drawn and takes no click; no row, no row height.
        CodeLibrary code;
        VM vm{code};
        DialogWindow w(vm, nullptr, nullptr);
        Dialog d;
        d.type = dialogtype::Message;
        d.cls = dialogClass(d.type);
        d.message = "A message that continues.";
        d.buttons = {"OK"};
        d.continueOthers = true;
        const auto L = w.layout(d, 1024, 768);
        size_t buttons = 0;
        for (const auto& r : L.regions)
            buttons += r.kind == DialogWindow::Hit::Button || r.kind == DialogWindow::Hit::Close;
        check(buttons == 0, "continue: no button drawn");
        check(L.y1 - L.y0 == 118 + 14, "continue: no row (118 - 0 + 14 high)");
        // Yes/no with "continue": only "No" in the row, at least 82 wide.
        d.type = dialogtype::YesNo;
        d.cls = dialogClass(d.type);
        d.buttons = {"Yes", "No"};
        const auto L2 = w.layout(d, 1024, 768);
        std::vector<DialogWindow::Region> row;
        for (const auto& r : L2.regions)
            if (r.kind == DialogWindow::Hit::Button)
                row.push_back(r);
        check(row.size() == 1 && row[0].value == 1 && row[0].rect[2] - row[0].rect[0] == 82,
              "continue: the second button alone in the row, 82 wide");
        // FUN_0025b1fc: 100 and 90 (within 20) both 100, then 50 raised to 82.
        d.type = dialogtype::YesNoCancel;
        d.cls = dialogClass(d.type);
        d.continueOthers = false;
        d.buttons = {std::string(12, 'W'), std::string(11, 'W'), "X"}; // 7 px a letter without fonts: 100, 93, 23
        const auto L3 = w.layout(d, 1024, 768);
        std::vector<float> widths;
        for (const auto& r : L3.regions)
            if (r.kind == DialogWindow::Hit::Button)
                widths.push_back(r.rect[2] - r.rect[0]);
        check(widths.size() == 3 && widths[0] == 100 && widths[1] == 100 && widths[2] == 82,
              "buttons: within 20 of a wider one made as wide, the rest at least 82");
    }
    {
        // Names: twins, Tab between the boxes, the first key replaces the
        // default, OK off while a box is empty.
        Fixture f(dialogtype::BabyName);
        PieMenuHost host(f.vm, "no-fonts");
        Dialog& d = f.vm.dialogs.at(f.me->id);
        d.names = {{1, "Bebezinho", false}, {2, "Bebezinha", true}};
        d.namePanel = 0xCDC2CAA6;
        check(host.dialogHasTextBox(), "names: a text box");
        host.dialogKey(0, "J");
        host.dialogKey(0, "o");
        check(d.names[0].text == "Jo", "names: the first key replaces the default");
        host.dialogKey(48, {}); // Tab
        for (int i = 0; i < 9; ++i)
            host.dialogKey(51, {});
        check(d.names[1].text.empty() && d.nameFocus == 1, "names: Tab to the second box");
        host.dialogKey(36, {});
        check(d.answer < 0, "names: OK off with an empty box");
        host.dialogKey(0, "Bia");
        host.dialogKey(36, {});
        check(d.answer == 0, "names: OK once every box has a name");
    }
    {
        // Party: a row clicked moves to the guests with Add (headless layout).
        Fixture f(dialogtype::PhoneParty);
        PieMenuHost host(f.vm, "no-fonts");
        Dialog& d = f.vm.dialogs.at(f.me->id);
        check(d.cls.cls == DialogClass::PhoneParty, "party: its class");
        d.entries = {{11, 0, "A", "", 0, 0}, {12, 0, "B", "", 0, 0}};
        d.maxSelection = 1;
        DialogWindow& w = host.dialogWindow();
        check(DialogWindow::okEnabled(d), "party: OK on");
        // Without the script, no rows to click: the phone books need ui.package.
        check(!DialogWindow::hasTextBox(d), "party: no text box");
        host.dialogKey(53, {});
        check(d.answer == 1, "party: Esc cancels");
        (void)w;
    }
    // With the game's data (OS2_DATA): the clothing rack's script, a pick,
    // "add to cart", the cart, OK; the try-on's choice; the vanity's tabs.
    if (const char* data = std::getenv("OS2_DATA")) {
        const std::filesystem::path res = std::filesystem::path(data) / "TSData/Res";
        ResourceIndex index(res);
        UiResources ui(index, res);
        {
            // DoLayout with an icon: the text block at least as tall as the icon
            // (the row overlapped a titled dialog's icon), and a
            // long text goes on under the icon in a second window (id 5).
            CodeLibrary code;
            VM vm{code};
            DialogWindow iw(vm, &ui, nullptr);
            Dialog m;
            m.type = dialogtype::Message;
            m.cls = dialogClass(m.type);
            m.id = 1;
            m.title = "A title";
            m.message = "Short.";
            m.buttons = {"OK"};
            m.iconImage = 0xE9000005;
            const auto L = iw.layout(m, 1280, 720);
            std::optional<float> iconBottom, rowTop;
            const auto* icon = L.root.child(3);
            if (icon)
                iconBottom = float(icon->area()[3]) + L.y0;
            for (const auto& r : L.regions)
                if (r.kind == DialogWindow::Hit::Button)
                    rowTop = r.rect[1];
            // The text starts 2 px over the icon (title + 26 against + 28) and
            // the row is 12 px under the text block: 10 under the icon.
            check(iconBottom && rowTop && *rowTop == *iconBottom + 10, "icon: the buttons 10 px under the icon");
            m.id = 2;
            m.message.clear();
            for (int k = 0; k < 40; ++k)
                m.message += "word ";
            const auto L2 = iw.layout(m, 1280, 720);
            check(L2.captions.count(5) && !L2.captions.at(5).empty() && L2.captions.at(2).size() < m.message.size(),
                  "icon: a long text goes on under the icon");
        }
        Fixture f(dialogtype::ClothingPurchase);
        Dialog& d = f.vm.dialogs.at(f.me->id);
        d.outfitSeparates = true, d.pricePerItem = 100, d.fillFunds = 250;
        DialogWindow w(f.vm, &ui, nullptr);
        w.log = false;
        w.shopItems = [](int16_t, uint32_t, uint32_t part, uint32_t, uint32_t) {
            std::vector<DialogWindow::ShopItem> l;
            for (uint32_t k = 0; k < 9; ++k)
                l.push_back({"part" + std::to_string(part) + "_" + std::to_string(k), Tgi{0xEBCF3E27, 1, part * 100 + k, 0}});
            return l;
        };
        auto centre = [](const std::array<float, 4>& r) { return std::array<float, 2>{(r[0] + r[2]) / 2, (r[1] + r[3]) / 2}; };
        auto find = [&](DialogWindow::Hit kind, int value) -> std::optional<std::array<float, 2>> {
            for (const auto& r : w.layout(d, 1280, 720).regions)
                if (r.kind == kind && r.value == value)
                    return centre(r.rect);
            return std::nullopt;
        };
        auto clickAt = [&](std::optional<std::array<float, 2>> p) {
            if (p)
                w.click(d, (*p)[0], (*p)[1], 1280, 720);
            return p.has_value();
        };
        check(clickAt(find(DialogWindow::Hit::Row, 4)), "rack: a grid of items");
        check(clickAt(find(DialogWindow::Hit::Add, 0)), "rack: add to cart once an item is picked");
        check(d.cart.size() == 1 && d.cart[0].instance == 4 * 100 + 4, "rack: the cart has the top picked");
        check(!find(DialogWindow::Hit::Add, 0), "rack: the same item not twice");
        check(clickAt(find(DialogWindow::Hit::Tab, 1)) && clickAt(find(DialogWindow::Hit::Row, 0)) &&
                  clickAt(find(DialogWindow::Hit::Add, 0)) && clickAt(find(DialogWindow::Hit::Tab, 2)) &&
                  clickAt(find(DialogWindow::Hit::Row, 1)) && clickAt(find(DialogWindow::Hit::Add, 0)),
              "rack: bottoms and whole outfits");
        check(d.cart.size() == 3 && !DialogWindow::okEnabled(d), "rack: OK off past the family's funds");
        check(clickAt(find(DialogWindow::Hit::Choice, 0x401)) && clickAt(find(DialogWindow::Hit::Row, 0x10000)) &&
                  clickAt(find(DialogWindow::Hit::Remove, 0)),
              "rack: the cart, an item taken out");
        check(d.cart.size() == 2 && DialogWindow::okEnabled(d), "rack: OK on");
        check(clickAt(find(DialogWindow::Hit::Button, 0)) && d.answer == 0, "rack: OK");

        // SaveCartStandard's question: a cart that completes no outfit asks
        // (yes buys; no closes the rack with nothing bought).
        for (const bool yes : {true, false}) {
            Fixture q(dialogtype::ClothingPurchase);
            Dialog& qd = q.vm.dialogs.at(q.me->id);
            qd.pricePerItem = 10, qd.fillFunds = 1000;
            qd.cart = {Tgi{0xEBCF3E27, 1, 404, 0}};
            DialogWindow qw(q.vm, &ui, nullptr);
            qw.log = false;
            qw.shopItems = w.shopItems;
            qw.cartCompletesOutfits = [](int16_t, uint32_t, const std::vector<Tgi>&) { return false; };
            for (const auto& r : qw.layout(qd, 1280, 720).regions)
                if (r.kind == DialogWindow::Hit::Button && r.value == 0) {
                    qw.click(qd, (r.rect[0] + r.rect[2]) / 2, (r.rect[1] + r.rect[3]) / 2, 1280, 720);
                    break;
                }
            check(qw.askingCart(qd) && qd.answer < 0, "rack: OK asks when nothing completes an outfit");
            qw.key(qd, yes ? 36 : 53, {});
            check(!qw.askingCart(qd) && qd.answer == 0 && qd.cart.size() == (yes ? 1u : 0u),
                  yes ? "rack: yes buys the cart" : "rack: no closes with nothing bought");
        }

        {
            // Party book: a long name ends in "..." within its column (the
            // list's width - 33: onAttach's columns 3, w - 33, 15, 15), a
            // short one is whole (without fonts a letter is 7 px wide).
            Fixture pf(dialogtype::PhoneParty);
            Dialog& pd = pf.vm.dialogs.at(pf.me->id);
            pd.entries = {{11, 0, "Drone Colono 1000101 de Souza", "", 0, 0}, {12, 0, "Ana", "", 0, 0}};
            pd.entries[0].flags = 16; // a friend: its icon in the last column
            DialogWindow pw(pf.vm, &ui, nullptr);
            pw.log = false;
            const auto L = pw.layout(pd, 1280, 720);
            bool cut = false, whole = false;
            for (const auto& [cid, text] : L.captions) {
                cut |= text.size() > 3 && text.ends_with("...") && text.starts_with("Drone") && 7.0f * float(text.size()) <= 175 - 33 &&
                       7.0f * float(text.size()) > 175 - 33 - 7 * 3;
                whole |= text == "Ana";
            }
            check(cut && whole, "party: a long name ends in ... at its column, a short one is whole");
        }
        {
            // The party's lists by name (ListBoxSortPredicate_Name ->
            // CompareByName, BV 0xa7a118): the last names, then the first
            // names, each byte lowered sign-extended (FUN_0003d7dc), so an
            // accented first letter (UTF-8 0xC3...) comes before "A".
            Fixture pf(dialogtype::PhoneParty);
            Dialog& pd = pf.vm.dialogs.at(pf.me->id);
            auto entry = [](int16_t id, const char* first, const char* last) {
                DialogEntry e;
                e.id = id, e.first = first, e.last = last, e.name = std::string(first) + " " + last;
                return e;
            };
            pd.entries = {entry(1, "Bruno", "Zeta"), entry(2, "abel", "Lima"), entry(3, "\xC3\x81gata", "Lima"),
                          entry(4, "Caio", "alfa")};
            DialogWindow pw(pf.vm, &ui, nullptr);
            pw.log = false;
            const auto L = pw.layout(pd, 1280, 720);
            std::vector<std::string> order;
            for (const auto& [cid, text] : L.captions)
                if ((cid >> 20) == 0xF && ((cid >> 12) & 0xFF) == 0)
                    order.push_back(text);
            check(order == std::vector<std::string>{"Caio alfa", "\xC3\x81gata Lima", "abel Lima", "Bruno Zeta"},
                  "party: by last name, then first name, case-blind, accented bytes first");
        }
        Fixture g(dialogtype::ClothingTryOn);
        Dialog& t = g.vm.dialogs.at(g.me->id);
        DialogWindow tw(g.vm, &ui, nullptr);
        tw.log = false;
        tw.shopItems = w.shopItems;
        std::array<std::string, 3> applied;
        tw.applyOutfit = [&](int16_t, uint32_t cat, const std::string& top, const std::string& bottom, const std::string& body) {
            check(cat == 0x400, "try-on: OK writes the try-on outfit");
            applied = {top, bottom, body};
        };
        for (const auto& r : tw.layout(t, 1280, 720).regions)
            if (r.kind == DialogWindow::Hit::Tab && r.value == 2) {
                tw.click(t, (r.rect[0] + r.rect[2]) / 2, (r.rect[1] + r.rect[3]) / 2, 1280, 720);
                break;
            }
        for (const auto& r : tw.layout(t, 1280, 720).regions)
            if (r.kind == DialogWindow::Hit::Row && r.value == 2) {
                tw.click(t, (r.rect[0] + r.rect[2]) / 2, (r.rect[1] + r.rect[3]) / 2, 1280, 720);
                break;
            }
        for (const auto& r : tw.layout(t, 1280, 720).regions)
            if (r.kind == DialogWindow::Hit::Button && r.value == 0) {
                tw.click(t, (r.rect[0] + r.rect[2]) / 2, (r.rect[1] + r.rect[3]) / 2, 1280, 720);
                break;
            }
        check(t.answer == 0 && applied[2] == "part8_2", "try-on: the whole outfit picked");

        Fixture h(dialogtype::Vanity);
        Dialog& v = h.vm.dialogs.at(h.me->id);
        DialogWindow vw(h.vm, &ui, nullptr);
        vw.log = false;
        std::vector<std::pair<int, int>> picks;
        bool done = false;
        vw.vanityItems = [](int16_t, int group) {
            return std::vector<DialogWindow::VanityItem>(size_t(group == DialogWindow::VanityHairColor ? 4 : 20));
        };
        vw.vanityPick = [&](int16_t, int group, int i) { picks.push_back({group, i}); };
        vw.vanityDone = [&](int16_t, bool ok) { done = ok; };
        auto vfind = [&](DialogWindow::Hit kind, int value) -> std::optional<std::array<float, 2>> {
            for (const auto& r : vw.layout(v, 1280, 720).regions)
                if (r.kind == kind && r.value == value)
                    return centre(r.rect);
            return std::nullopt;
        };
        auto vclick = [&](std::optional<std::array<float, 2>> p) {
            if (p)
                vw.click(v, (*p)[0], (*p)[1], 1280, 720);
            return p.has_value();
        };
        check(vclick(vfind(DialogWindow::Hit::Row, (DialogWindow::VanityHair << 16) | 3)) &&
                  vclick(vfind(DialogWindow::Hit::Choice, 0x302)),
              "vanity: a hair style and the blond colour");
        check(vclick(vfind(DialogWindow::Hit::Tab, 1)) &&
                  vclick(vfind(DialogWindow::Hit::Row, (DialogWindow::VanityLipstick << 16) | 1)),
              "vanity: makeup, a lipstick");
        check(vclick(vfind(DialogWindow::Hit::Tab, 6)) && vclick(vfind(DialogWindow::Hit::Next, DialogWindow::VanityBeards)) &&
                  vclick(vfind(DialogWindow::Hit::Row, (DialogWindow::VanityBeards << 16) | 12)),
              "vanity: beards, the second page");
        check(vfind(DialogWindow::Hit::Choice, 0x301).has_value(), "vanity: the facial hair colour row on the beards tab");
        check(vclick(vfind(DialogWindow::Hit::Tab, 0)) && vclick(vfind(DialogWindow::Hit::Choice, 0x310)) &&
                  vclick(vfind(DialogWindow::Hit::Row, (DialogWindow::VanityHairCustom << 16) | 0)),
              "vanity: Custom shows the custom tones' hairs");
        check(vclick(vfind(DialogWindow::Hit::Tab, 6)), "vanity: back to the beards");
        check(picks == (std::vector<std::pair<int, int>>{{DialogWindow::VanityHair, 3},
                                                          {DialogWindow::VanityHairColor, 2},
                                                          {DialogWindow::VanityLipstick, 1},
                                                          {DialogWindow::VanityBeards, 12},
                                                          {DialogWindow::VanityHairCustom, 0}}),
              "vanity: the picks in order");
        check(vclick(vfind(DialogWindow::Hit::Button, 0)) && done && v.answer == 0, "vanity: OK writes the look");

        // The aspiration dialog's "Big Life Moments": the catalog under the
        // description with a cell per lifetime want (window 4 the icon).
        Fixture a(dialogtype::SetAspiration);
        Dialog& ad = a.vm.dialogs.at(a.me->id);
        DialogWindow aw(a.vm, &ui, nullptr);
        aw.log = false;
        aw.lifetimeWants = [](uint32_t asp) {
            std::vector<DialogWindow::LifetimeWant> l;
            for (int k = 0; k < (asp == 2 ? 8 : 5); ++k)
                l.push_back({0x000b2011, 0, false, true, "want"});
            return l;
        };
        ad.aspiration = 2;
        const auto AL = aw.layout(ad, 1280, 720);
        int cells = 0;
        for (const auto& [id, img] : AL.images)
            if ((id & 0xFFF000FFu) == 0x00D00004u && img)
                ++cells;
        check(cells == 8, "aspiration: a catalog cell per Family lifetime want");
    }
    {
        // Picking (cTSWinSceneGraph::ObjectAtGameWindowPoint): the nearest hit,
        // Sim or object, and a floor stops the ray.
        CodeLibrary code;
        code.add(kObjGroup, 0x1000, tree({instr(0x02, {0, 0, 0, 0, 0, 0, 0, 0})}));
        VM vm{code};
        const ObjectId wardrobe = vm.createObject(kObjGroup).id, chair = vm.createObject(kObjGroup).id;
        PieMenuHost host(vm, "no-fonts");
        const simd_float4x4 at = matrix_identity_float4x4;
        host.addPickBox(wardrobe, simd_make_float3(4, 4, 3), simd_make_float3(5, 5, 5), at, nullptr); // upstairs
        host.setMovingBox(7, simd_make_float3(4.2f, 4.2f, 0), simd_make_float3(4.8f, 4.8f, 1.7f)); // a Sim under it
        const simd_float3 eye = simd_make_float3(4.5f, 4.5f, 20), down = simd_make_float3(0, 0, -1);
        check(host.pick(eye, down) == wardrobe, "pick: the object above wins over a Sim farther down the ray");
        host.addPickBox(chair, simd_make_float3(6, 6, 0), simd_make_float3(7, 7, 1), at, nullptr); // downstairs
        host.setMovingBox(11, simd_make_float3(6.2f, 6.2f, 0), simd_make_float3(6.8f, 6.8f, 1.7f)); // a Sim on it
        check(host.pick(simd_make_float3(6.5f, 6.5f, 20), down) == 11, "pick: a Sim in front of an object");
        // The upper floor at z = 3 stops the ray: the Sim below is not picked.
        host.floorHit = [](simd_float3 o, simd_float3 d) -> std::optional<float> {
            if (d.z >= 0 || o.z < 3)
                return std::nullopt;
            return (3 - o.z) / d.z;
        };
        check(host.pick(simd_make_float3(6.5f, 6.5f, 20), down) == 0, "pick: nothing behind a floor");
        check(host.pick(eye, down) == wardrobe, "pick: what stands on the floor still is");
        check(host.pick(simd_make_float3(6.5f, 6.5f, 2.5f), down) == 11, "pick: under the floor, the Sim");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog window: all passed\n");
    return failures ? 1 : 0;
}
