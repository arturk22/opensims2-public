#pragma once

#include <string>

namespace os2 {

// Runs a primitive-survey command; -1 if `cmd` (with these arguments) is not one.
int primCommand(const std::string& cmd, int argc, char** argv);
const char* primUsage();

} // namespace os2
