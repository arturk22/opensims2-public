#pragma once

#include <string>

namespace os2 {

// Runs the autonomy command; -1 if `cmd` (with these arguments) is not one.
int autonomyCommand(const std::string& cmd, int argc, char** argv);
const char* autonomyUsage();

} // namespace os2
