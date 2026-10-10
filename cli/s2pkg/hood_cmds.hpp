#pragma once

#include <string>

namespace os2 {

// Runs a neighborhood command; -1 if `cmd` (with these arguments) is not one.
int hoodCommand(const std::string& cmd, int argc, char** argv);
const char* hoodUsage();

} // namespace os2
