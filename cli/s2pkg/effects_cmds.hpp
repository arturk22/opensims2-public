#pragma once

#include <string>

namespace os2 {

// Runs an effects command (effects.package survey); -1 if `cmd` is not one.
int effectsCommand(const std::string& cmd, int argc, char** argv);
const char* effectsUsage();

} // namespace os2
