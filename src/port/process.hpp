#pragma once

// Process id and "is this pid still running", used by session folders.
// kill(pid, 0) on POSIX; OpenProcess on Windows (access denied counts as alive).

namespace os2 {

int processId();
bool processAlive(long pid);

} // namespace os2
