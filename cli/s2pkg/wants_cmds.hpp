#pragma once

#include <cstdint>
#include <string>

namespace os2 {
namespace vm {
class VM;
struct WantEvent;
}

// s2pkg wants: each Sim's wants, fears, aspiration and memories.
int wantsCommand(const std::string& cmd, int argc, char** argv);
const char* wantsUsage();

// One line about a want event (for s2pkg autonomy).
std::string describeWantEvent(vm::VM& machine, const vm::WantEvent& e);

} // namespace os2
