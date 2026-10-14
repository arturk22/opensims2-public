#include "log_once.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace os2 {

void logOnce(std::string_view where, std::string_view key, std::string_view what) {
    struct Site {
        std::set<std::string, std::less<>> keys;
        bool full = false; // the "not shown" line is out
    };
    // Never destroyed: an error at exit (a scene torn down by the quit
    // handler) can still be logged after the C++ runtime has begun
    // destroying statics.
    static auto* lock = new std::mutex;
    static auto* sites = new std::map<std::string, Site, std::less<>>;
    std::lock_guard guard(*lock);
    auto it = sites->find(where);
    if (it == sites->end())
        it = sites->emplace(std::string(where), Site{}).first;
    Site& site = it->second;
    if (site.keys.find(key) != site.keys.end())
        return;
    const int w = int(where.size());
    if (site.keys.size() >= size_t(kLogOnceKeys)) {
        if (!site.full)
            std::fprintf(stderr, "opensims2: %.*s: more errors like these are not shown\n", w, where.data());
        site.full = true;
        return;
    }
    site.keys.emplace(key);
    if (key.empty())
        std::fprintf(stderr, "opensims2: %.*s: %.*s\n", w, where.data(), int(what.size()), what.data());
    else
        std::fprintf(stderr, "opensims2: %.*s (%.*s): %.*s\n", w, where.data(), int(key.size()), key.data(),
                     int(what.size()), what.data());
}

} // namespace os2
