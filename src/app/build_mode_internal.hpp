#pragma once

// What Build mode's sources share (build_mode.cpp, build_mode_lot.cpp,
// build_mode_panel.cpp; nothing else includes it).

#include <cstdint>
#include <string>

namespace os2 {

// (Unnamed: each source has its own copy, as when they were one file;
// inline so that a source that does not use it is not warned.)
namespace {

inline std::string money(int32_t v) {
    std::string digits = std::to_string(v < 0 ? -int64_t(v) : int64_t(v)), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

} // namespace

} // namespace os2
