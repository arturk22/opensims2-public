#pragma once

#include <string>

namespace os2 {

// s2pkg lot-state: the SimAntics state saved in a lot package (XOBJ/PERS:
// tree stacks, timers, the Sims' motives, ...); -1 if `cmd` is not one.
int stateCommand(const std::string& cmd, int argc, char** argv);
const char* stateUsage();

} // namespace os2
