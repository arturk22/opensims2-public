#pragma once

#include <string>

namespace os2 {

// Runs the pie menu command; -1 if `cmd` (with these arguments) is not one.
int pieCommand(const std::string& cmd, int argc, char** argv);
const char* pieUsage();

} // namespace os2
