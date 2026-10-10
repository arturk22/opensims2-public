#pragma once

#include <string>

namespace os2 {

// Runs a sound command; -1 if `cmd` (with these arguments) is not one.
int audioCommand(const std::string& cmd, int argc, char** argv);
const char* audioUsage();

} // namespace os2
