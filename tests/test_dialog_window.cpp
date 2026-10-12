// The dialog window of the app (PieMenuHost): keys into a text-entry dialog,
// Return and Esc, and the layout of the game's scripts (src/app/pie_menu.cpp).

#include "app/pie_menu.hpp"
#include "vm/vm.hpp"

#include <cstdio>
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
        host.dialogKey(0, std::string(40, 'x'));
        check(host.dialog()->text.size() == 18, "text entry: 18 characters at most (maxtext)");
        host.dialogKey(53, {}); // Esc: not a way out of a text box
        check(host.dialogOpen(), "text entry: Esc leaves it open");
        host.dialogKey(36, {}); // Return
        check(!host.dialogOpen() && f.vm.dialogs.at(f.me->id).answer == 0, "text entry: Return answers OK");
    }
    {
        // Baby Name (23): the default name is selected, so the first key replaces it.
        Fixture f(dialogtype::BabyName);
        PieMenuHost host(f.vm, "no-fonts");
        f.vm.dialogs.at(f.me->id).text = "Bebezinho";
        host.dialogKey(0, "J");
        host.dialogKey(0, "o");
        check(host.dialog()->text == "Jo", "baby name: the first key replaces the default");
    }
    {
        // Yes/no: Esc answers No, Return Yes; a message dialog ignores Esc.
        Fixture f(dialogtype::YesNo);
        PieMenuHost host(f.vm, "no-fonts");
        check(host.dialogOpen() && !host.dialogHasTextBox(), "yes/no: open without a text box");
        host.dialogKey(0, "x");
        check(host.dialog()->text.empty(), "yes/no: keys type nothing");
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
        host.dialogKey(53, {});
        check(host.dialogOpen(), "message: Esc does nothing");
        host.dialogKey(76, {}); // the numeric Enter
        check(f.vm.dialogs.at(f.me->id).answer == 0, "message: Enter answers OK");
    }
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("dialog window: all passed\n");
    return failures ? 1 : 0;
}
