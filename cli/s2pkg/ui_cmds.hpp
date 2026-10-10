#pragma once

#include <string>

namespace os2 {

// Runs a UI script/image/font command; -1 if `cmd` (with these arguments) is not one.
int uiCommand(const std::string& cmd, int argc, char** argv);
const char* uiUsage();

} // namespace os2
