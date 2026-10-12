// The dialog window of the app (PieMenuHost): keys into a text-entry dialog,
// Return and Esc, and the layout of the game's scripts (src/app/pie_menu.cpp).

#include "app/pie_menu.hpp"
#include "vm/vm.hpp"

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
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog window: all passed\n");
    return failures ? 1 : 0;
}
