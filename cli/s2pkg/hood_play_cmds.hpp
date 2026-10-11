#pragma once

#include <string>

namespace os2 {

// Runs a neighborhood play command (lots, family bin, moving); -1 if `cmd` (with these arguments) is not one.
int hoodPlayCommand(const std::string& cmd, int argc, char** argv);
const char* hoodPlayUsage();

} // namespace os2
