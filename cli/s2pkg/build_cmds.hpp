#pragma once

#include <string>
#include <utility>
#include <vector>

namespace os2 {

// Build-mode commands (lot geometry round trip, rooms, edits); -1 if `cmd`
// is not one.
int buildCommand(const std::string& cmd, int argc, char** argv);
const char* buildUsage();

} // namespace os2
