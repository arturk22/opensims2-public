// The dialog window of the app (PieMenuHost): keys into a text-entry dialog,
// Return and Esc, and the layout of the game's scripts (src/app/pie_menu.cpp).

#include "app/pie_menu.hpp"
#include "vm/vm.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"

#include <cstdlib>
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
                 tree({instr(0x24, {0, 0, 2, 3, 4, uint8_t(type), 1, 0x00, 0x01, 0, 0, 0, 0, 2, 0})}));
        me = &vm.createObject(kObjGroup);
        t = vm.start(me->id, 0x1000);
        vm.run(t);
    }
};

int main() {
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
            // Party book: a long name ends in "..." before its row's icons,
            // a short one is whole (without fonts a letter is 7 px wide).
            Fixture pf(dialogtype::PhoneParty);
            Dialog& pd = pf.vm.dialogs.at(pf.me->id);
            pd.entries = {{11, 0, "Drone Colono 1000101 de Souza", "", 0, 0}, {12, 0, "Ana", "", 0, 0}};
            pd.entries[0].flags = 16; // a friend: the text stops before the friend column
            DialogWindow pw(pf.vm, &ui, nullptr);
            pw.log = false;
            const auto L = pw.layout(pd, 1280, 720);
            bool cut = false, whole = false;
            for (const auto& [cid, text] : L.captions) {
                cut |= text.size() > 3 && text.ends_with("...") && text.starts_with("Drone") && 7.0f * float(text.size()) <= 175 - 48;
                whole |= text == "Ana";
            }
            check(cut && whole, "party: a long name ends in ... before the icons, a short one is whole");
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
