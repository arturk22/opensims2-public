#pragma once

#include <string>

namespace os2 {

// Runs a Sim-appearance survey command; -1 if `cmd` is not one.
int simLookCommand(const std::string& cmd, int argc, char** argv);
const char* simLookUsage();

} // namespace os2
