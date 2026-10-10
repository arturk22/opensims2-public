#pragma once

#include <string>

namespace os2 {

// Runs a material command; -1 if `cmd` (with these arguments) is not one.
int materialCommand(const std::string& cmd, int argc, char** argv);
const char* materialUsage();

} // namespace os2
