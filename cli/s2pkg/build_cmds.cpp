// s2pkg build-roundtrip: Build-mode geometry written back (formats/lot_write.hpp)
// and the rooms worked out again (formats/lot_edit.hpp), checked against
// the saved lots.

#include "build_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "formats/lot_write.hpp"

#include <cstdio>
#include <filesystem>
#include <map>
#include <tuple>
#include <stdexcept>

namespace os2 {

namespace fs = std::filesystem;

namespace {

std::string key(uint32_t type, uint32_t instance) {
    char b[32];
    std::snprintf(b, sizeof b, "%08X-%02X", type, instance);
    return b;
}

// Writes the geometry of each lot as read (and with --rooms after working
// the rooms out again) and compares it, record by record, with the lot's.
int cmdRoundtrip(const std::vector<std::string>& args) {
    bool rooms = false;
    std::vector<fs::path> lots;
    for (const auto& a : args)
        if (a == "--rooms")
            rooms = true;
        else
            lots.emplace_back(a);
    size_t same = 0, differ = 0;
    std::map<std::string, size_t> wrong;
    for (const auto& path : lots) {
        DbpfPackage pkg(path);
        Lot lot;
        try {
            lot = Lot::load(pkg);
        } catch (const std::exception& ex) {
            std::printf("%s: skipped (%s)\n", path.filename().string().c_str(), ex.what());
            continue;
        }
        const Lot before = lot;
        if (rooms) {
            updateRooms(lot);
            // What changed, quarter by quarter: (level, old, new) -> count.
            std::map<std::tuple<int, int, int>, size_t> changes;
            for (size_t i = 0; i < lot.rooms.size(); ++i)
                for (int q = 0; q < 4; ++q)
                    if (lot.rooms[i][q] != before.rooms[i][q])
                        ++changes[{int(i / (size_t(lot.width) * lot.height)) + lot.minLevel, before.rooms[i][q],
                                   lot.rooms[i][q]}];
            size_t shown = 0;
            for (const auto& [k, n] : changes)
                if (shown++ < 8)
                    std::printf("  %s: level %d room %d -> %d: %zu quarters\n", path.filename().string().c_str(),
                                std::get<0>(k), std::get<1>(k), std::get<2>(k), n);
        }
        const auto records = writeLotGeometry(lot, pkg);
        std::vector<std::string> bad;
        for (const auto& r : records) {
            const DbpfEntry* e = nullptr;
            for (const auto& x : pkg.entries())
                if (x.tgi.type == r.type && x.tgi.instance == r.instance && x.tgi.group == r.group)
                    e = &x;
            if (!e) {
                if (r.bytes.size() > 0)
                    bad.push_back(key(r.type, r.instance) + " new");
                continue;
            }
            const auto original = pkg.read(*e);
            if (original != r.bytes) {
                size_t at = 0;
                while (at < original.size() && at < r.bytes.size() && original[at] == r.bytes[at])
                    ++at;
                bad.push_back(key(r.type, r.instance) + " at " + std::to_string(at) + " (" +
                              std::to_string(original.size()) + " vs " + std::to_string(r.bytes.size()) + ")");
                ++wrong[key(r.type, r.instance)];
            }
        }
        if (bad.empty()) {
            ++same;
        } else {
            ++differ;
            std::printf("%s:", path.filename().string().c_str());
            for (const auto& b : bad)
                std::printf(" %s;", b.c_str());
            std::printf("\n");
        }
    }
    std::printf("%zu lots written back the same, %zu differ\n", same, differ);
    for (const auto& [k, n] : wrong)
        std::printf("  %s differs in %zu\n", k.c_str(), n);
    return differ ? 1 : 0;
}

} // namespace

int buildCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "build-roundtrip" && argc >= 3)
        return cmdRoundtrip({argv + 2, argv + argc});
    return -1;
}

const char* buildUsage() {
    return "       s2pkg build-roundtrip <lot.package>... [--rooms]\n";
}

} // namespace os2
