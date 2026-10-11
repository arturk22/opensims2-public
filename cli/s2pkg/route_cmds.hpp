#pragma once

#include <string>

namespace os2 {

// Runs the route command; -1 if `cmd` (with these arguments) is not one.
int routeCommand(const std::string& cmd, int argc, char** argv);
const char* routeUsage();

} // namespace os2
