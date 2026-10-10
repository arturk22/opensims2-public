// s2pkg: inspect and extract The Sims 2 .package (DBPF) files.
#include "formats/dbpf.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace os2;

namespace {

std::string typeLabel(uint32_t t) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", t);
    if (auto n = typeName(t); !n.empty())
        return std::string(n);
    // Many Sims 1-era types are big-endian FourCCs ("SPR2", "DGRP", "BMP_").
    std::string cc;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = char((t >> shift) & 0xFF);
        if (c < 0x20 || c > 0x7E)
            return hex;
        cc += c;
    }
    return cc;
}

std::string tgiString(const Tgi& k) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X-%08X-%08X-%08X", k.type, k.group, k.instanceHi, k.instance);
    return buf;
}

int cmdLs(DbpfPackage& pkg) {
    for (const auto& e : pkg.entries()) {
        std::printf("%-6s %s  off=%-10u size=%-9u", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(),
                    e.offset, e.size);
        if (e.rawSize)
            std::printf(" raw=%u", *e.rawSize);
        std::printf("\n");
    }
    return 0;
}

int cmdStats(DbpfPackage& pkg) {
    struct Acc { size_t count = 0; uint64_t bytes = 0; size_t compressed = 0; };
    std::map<uint32_t, Acc> byType;
    for (const auto& e : pkg.entries()) {
        auto& a = byType[e.tgi.type];
        ++a.count;
        a.bytes += e.size;
        a.compressed += e.compressed();
    }
    const auto& h = pkg.header();
    std::printf("DBPF %u.%u  index %u.%u  entries=%u\n", h.major, h.minor, h.indexMajor,
                h.indexMinor ? h.indexMinor - 1 : 0, h.indexCount);
    std::printf("%-12s %8s %12s %10s\n", "type", "count", "bytes", "qfs");
    for (const auto& [t, a] : byType)
        std::printf("%-12s %8zu %12llu %10zu\n", typeLabel(t).c_str(), a.count, (unsigned long long)a.bytes,
                    a.compressed);
    return 0;
}

int cmdVerify(DbpfPackage& pkg) {
    size_t ok = 0, failed = 0;
    for (const auto& e : pkg.entries()) {
        try {
            pkg.read(e);
            ++ok;
        } catch (const std::exception& ex) {
            ++failed;
            std::fprintf(stderr, "%s: %s\n", tgiString(e.tgi).c_str(), ex.what());
        }
    }
    std::printf("%s: %zu ok, %zu failed\n", pkg.path().string().c_str(), ok, failed);
    return failed ? 1 : 0;
}

int cmdExtract(DbpfPackage& pkg, const std::filesystem::path& outDir) {
    std::filesystem::create_directories(outDir);
    for (const auto& e : pkg.entries()) {
        auto data = pkg.read(e);
        auto name = typeLabel(e.tgi.type) + "_" + tgiString(e.tgi) + ".bin";
        std::ofstream(outDir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                             std::streamsize(data.size()));
    }
    std::printf("extracted %zu resources to %s\n", pkg.entries().size(), outDir.string().c_str());
    return 0;
}

int usage() {
    std::fprintf(stderr, "usage: s2pkg ls|stats|verify <file.package>\n"
                         "       s2pkg extract <file.package> <out-dir>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string cmd = argv[1];
    try {
        DbpfPackage pkg(argv[2]);
        if (cmd == "ls") return cmdLs(pkg);
        if (cmd == "stats") return cmdStats(pkg);
        if (cmd == "verify") return cmdVerify(pkg);
        if (cmd == "extract" && argc == 4) return cmdExtract(pkg, argv[3]);
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
