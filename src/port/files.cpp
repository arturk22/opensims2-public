#include "port/files.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <system_error>
#include <vector>
#endif

namespace os2 {

void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
    const std::wstring target = std::filesystem::absolute(to).wstring();
    HANDLE handle = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        const size_t nameBytes = target.size() * sizeof(wchar_t);
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + nameBytes);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->RootDirectory = nullptr;
        info->FileNameLength = DWORD(nameBytes);
        std::memcpy(info->FileName, target.c_str(), nameBytes);
        const BOOL ok = SetFileInformationByHandle(handle, FileRenameInfoEx, info, DWORD(buffer.size()));
        CloseHandle(handle);
        if (ok)
            return;
    }
    // Older Windows (no FileRenameInfoEx) or another volume: the plain replace.
    if (!MoveFileExW(from.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        throw std::filesystem::filesystem_error("rename", from, to,
                                                std::error_code(int(GetLastError()), std::system_category()));
#else
    std::filesystem::rename(from, to);
#endif
}

} // namespace os2
