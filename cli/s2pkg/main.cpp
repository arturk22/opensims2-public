// s2pkg: inspect and extract The Sims 2 .package (DBPF) files.
#include "formats/dbpf.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"

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

Tgi parseTgi(const std::string& s) {
    Tgi k;
    if (std::sscanf(s.c_str(), "%x-%x-%x-%x", &k.type, &k.group, &k.instanceHi, &k.instance) != 4)
        throw std::runtime_error("bad TGI '" + s + "' (expected TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII)");
    return k;
}

bool isRcolType(uint32_t t) {
    switch (t) {
    case 0x1C4A276C: case 0x49596978: case 0x7BA3838C: case 0xAC4F8687: case 0xE519C933:
    case 0xED534136: case 0xFC6EB1F7: case 0x4D51F042: case 0xC9C81B9B: case 0xC9C81BA3:
    case 0xC9C81BA9: case 0xC9C81BAD: case 0xED534137: case 0x253D2018:
        return true;
    default:
        return false;
    }
}

// Decodes one resource; returns a textual dump.
std::string decode(uint32_t type, std::span<const uint8_t> data) {
    std::string out;
    char line[256];
    if (isRcolType(type)) {
        Rcol rc(data);
        for (const auto& l : rc.links) {
            std::snprintf(line, sizeof line, "link %s\n", tgiString(l).c_str());
            out += line;
        }
        for (const auto& b : rc.blocks) {
            std::snprintf(line, sizeof line, "block %s 0x%08X  %zu bytes\n", b.className.c_str(), b.typeId,
                          b.body.size());
            out += line;
        }
        return out;
    }
    throw std::runtime_error("no decoder for " + typeLabel(type));
}

int cmdDump(DbpfPackage& pkg, const Tgi& tgi) {
    const auto* e = pkg.find(tgi);
    if (!e)
        throw std::runtime_error("resource not found");
    std::fputs(decode(tgi.type, pkg.read(*e)).c_str(), stdout);
    return 0;
}

int cmdDecodeAll(DbpfPackage& pkg) {
    std::map<std::string, std::pair<size_t, size_t>> byType; // ok, failed
    for (const auto& e : pkg.entries()) {
        if (!isRcolType(e.tgi.type))
            continue;
        auto& acc = byType[typeLabel(e.tgi.type)];
        try {
            decode(e.tgi.type, pkg.read(e));
            ++acc.first;
        } catch (const std::exception& ex) {
            ++acc.second;
            std::fprintf(stderr, "%s %s: %s\n", typeLabel(e.tgi.type).c_str(), tgiString(e.tgi).c_str(), ex.what());
        }
    }
    size_t failed = 0;
    for (const auto& [t, acc] : byType) {
        std::printf("%s: %-6s %zu ok, %zu failed\n", pkg.path().filename().string().c_str(), t.c_str(), acc.first,
                    acc.second);
        failed += acc.second;
    }
    return failed ? 1 : 0;
}

int cmdFind(const std::string& root, const std::string& name, const std::string& type) {
    ResourceIndex idx(root);
    std::printf("%zu packages, %zu resources, %zu duplicate TGIs\n", idx.packageCount(), idx.resourceCount(),
                idx.duplicateCount());
    const uint32_t t = uint32_t(std::stoul(type, nullptr, 16));
    std::printf("hash: instance=0x%08X instanceHi=0x%08X\n", nameInstanceHash(name), nameInstanceHiHash(name));
    auto loc = idx.findByName(name, t);
    if (!loc) {
        std::printf("not found\n");
        return 1;
    }
    std::printf("%s in %s\n", tgiString(loc->entry->tgi).c_str(), loc->package->path().string().c_str());
    return 0;
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
    std::fprintf(stderr, "usage: s2pkg ls|stats|verify|decode-all <file.package>\n"
                         "       s2pkg dump <file.package> <TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII>\n"
                         "       s2pkg extract <file.package> <out-dir>\n"
                         "       s2pkg find <Res-dir> <name> <type-hex>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string cmd = argv[1];
    try {
        if (cmd == "find" && argc == 5) return cmdFind(argv[2], argv[3], argv[4]);
        DbpfPackage pkg(argv[2]);
        if (cmd == "ls") return cmdLs(pkg);
        if (cmd == "stats") return cmdStats(pkg);
        if (cmd == "verify") return cmdVerify(pkg);
        if (cmd == "decode-all") return cmdDecodeAll(pkg);
        if (cmd == "dump" && argc == 4) return cmdDump(pkg, parseTgi(argv[3]));
        if (cmd == "extract" && argc == 4) return cmdExtract(pkg, argv[3]);
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
