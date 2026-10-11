#pragma once

#include <string>

namespace os2 {

// Create-a-Sim commands (cas-catalog, cas-make, cas-baby); -1 if `cmd` is not one.
int casCommand(const std::string& cmd, int argc, char** argv);
const char* casUsage();

} // namespace os2
