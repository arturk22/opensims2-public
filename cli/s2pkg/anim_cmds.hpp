#pragma once

#include <string>

namespace os2 {

// Runs a skeleton/animation/Sim command; -1 if `cmd` (with these arguments) is not one.
int animCommand(const std::string& cmd, int argc, char** argv);
const char* animUsage();

} // namespace os2
