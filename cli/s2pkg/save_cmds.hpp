#pragma once

#include <string>

namespace os2 {

// s2pkg lot-save: loads a lot into the VM, runs it, and writes it back in
// the game's format (vm/lot_save.hpp), checking the round trip; -1 if `cmd`
// is not one.
int saveCommand(const std::string& cmd, int argc, char** argv);
const char* saveUsage();

} // namespace os2
