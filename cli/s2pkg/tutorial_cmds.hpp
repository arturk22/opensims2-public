#pragma once

#include <string>

namespace os2 {

// s2pkg tutorial; -1 if `cmd` (with these arguments) is not it.
int tutorialCommand(const std::string& cmd, int argc, char** argv);
const char* tutorialUsage();

} // namespace os2
