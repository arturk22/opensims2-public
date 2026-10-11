#pragma once

#include <string>

namespace os2 {

// Buy-mode commands (catalog, buying into a lot); -1 if `cmd` is not one.
int buyCommand(const std::string& cmd, int argc, char** argv);
const char* buyUsage();

} // namespace os2
