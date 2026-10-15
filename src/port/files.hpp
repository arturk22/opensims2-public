#pragma once

// Replacing a saved file with its finished copy ("<file>.saving" renamed over
// it). POSIX rename replaces a file that is open for reading (the running
// lot's package is); on Windows the rename has POSIX semantics
// (FileRenameInfoEx), which needs the readers' FILE_SHARE_DELETE
// (DbpfPackage opens with it).

#include <filesystem>

namespace os2 {

// Renames `from` over `to`; throws std::filesystem::filesystem_error.
void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace os2
