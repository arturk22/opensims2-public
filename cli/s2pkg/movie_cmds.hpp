#pragma once

#include <string>

namespace os2 {

// Runs a movie command; -1 if `cmd` (with these arguments) is not one.
int movieCommand(const std::string& cmd, int argc, char** argv);
const char* movieUsage();

} // namespace os2
