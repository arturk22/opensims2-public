#include "port/process.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace os2 {

int processId() {
#if defined(_WIN32)
    return _getpid();
#else
    return ::getpid();
#endif
}

bool processAlive(long pid) {
    if (pid <= 0)
        return false;
#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const BOOL ok = GetExitCodeProcess(handle, &code);
    CloseHandle(handle);
    return ok && code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

} // namespace os2
