#pragma once

#include <string>

namespace os2 {

// Runs a shadow command (the objects' GUOB planes); -1 if `cmd` is not one.
int shadowCommand(const std::string& cmd, int argc, char** argv);
const char* shadowUsage();

} // namespace os2
