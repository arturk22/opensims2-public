#pragma once

// Errors a caller recovers from (a missing or unreadable piece of the game's
// data, a fallback taken) on stderr, so that they do not vanish
// (round 24, docs/game-flow-mac.md §5): "opensims2: <where> (<key>): <what>", once per
// (where, key), so that a block that runs every frame or tick prints its
// error once; at most kLogOnceKeys keys per `where`, then one line saying
// the rest are not shown. Thread-safe, and safe while the process exits
// (what it has printed is never destroyed).

#include <exception>
#include <stdexcept> // (std::logic_error: the number parsing callers catch)
#include <string_view>

namespace os2 {

inline constexpr int kLogOnceKeys = 20;

void logOnce(std::string_view where, std::string_view key, std::string_view what);
inline void logOnce(std::string_view where, std::string_view key, const std::exception& e) {
    logOnce(where, key, std::string_view(e.what()));
}

} // namespace os2
