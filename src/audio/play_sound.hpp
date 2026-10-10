#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace os2 {

// The app's --play-sound: resolves `name` (track or sample name, or a
// TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII key) among the sound packages under
// `resDir` (TSData/Res), decodes the `index`-th sample, prints its format
// and levels, writes it to `wavOut` when not empty, then plays it to the end
// on the default output. Returns a process exit code.
int playSoundCommand(const std::filesystem::path& resDir, const std::string& name, size_t index,
                     const std::string& wavOut);

} // namespace os2
