#include "formats/dbpf.hpp"
#include "formats/qfs.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static std::vector<uint8_t> text(int lines) {
    std::string s;
    for (int i = 0; i < lines; ++i)
        s += "Object definition " + std::to_string(i % 23) + "\n";
    return {s.begin(), s.end()};
}

static std::filesystem::path tempPackage(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string("os2_test_") + name + ".package");
}

static void testEmpty() {
    const auto path = tempPackage("empty");
    DbpfWriter().save(path);
    DbpfPackage pkg(path);
    check(pkg.entries().empty(), "empty package has no entries");
    check(pkg.header().major == 1 && pkg.header().minor == 1, "empty package is DBPF 1.1");
    std::filesystem::remove(path);
}

static void testRoundTrip() {
    std::mt19937 rng(7);
    std::vector<uint8_t> noise(5000);
    for (auto& b : noise)
        b = uint8_t(rng());

    const Tgi plain{0x53545223, 0x7F000001, 0x00000081, 0};
    const Tgi packed{0x42484156, 0x7F000001, 0x00001000, 0};
    const Tgi wide{0xAC4F8687, 0x1C0532FA, 0x12345678, 0xFF00AA55};
    const Tgi incompressible{0x856DDBAC, 0x7F000002, 0x00000001, 0};
    const Tgi stored{0x4F424A44, 0x7F000003, 0x00000041, 0};

    DbpfWriter w;
    w.add(plain, text(10));
    w.add(packed, text(500), true);
    w.add(wide, text(300), true);
    w.add(incompressible, noise, true); // does not shrink: stored as is
    w.addStored(stored, qfsCompress(text(50)), uint32_t(text(50).size()));
    w.addStored(kDirTgi, {1, 2, 3}); // ignored: the writer makes its own DIR
    check(w.size() == 5, "caller DIR dropped");

    const auto path = tempPackage("roundtrip");
    w.save(path);
    DbpfPackage pkg(path);
    const auto& h = pkg.header();
    check(h.major == 1 && h.minor == 1, "DBPF 1.1");
    check(h.indexMajor == 7 && h.indexMinor == 2, "index 7.1");
    check(pkg.entries().size() == 6, "5 resources + DIR");
    check(pkg.entries().back().tgi == kDirTgi, "DIR written last");

    auto readBack = [&](const Tgi& k) {
        const auto* e = pkg.find(k);
        return e ? pkg.read(*e) : std::vector<uint8_t>{};
    };
    check(readBack(plain) == text(10), "uncompressed resource");
    check(readBack(packed) == text(500), "compressed resource");
    check(readBack(wide) == text(300), "compressed resource with high instance");
    check(readBack(incompressible) == noise, "incompressible resource");
    check(readBack(stored) == text(50), "pre-compressed resource");

    check(!pkg.find(plain)->compressed(), "plain not in DIR");
    check(pkg.find(packed)->compressed(), "packed in DIR");
    check(pkg.find(wide)->compressed(), "wide in DIR");
    check(!pkg.find(incompressible)->compressed(), "incompressible not in DIR");
    check(pkg.find(packed)->size < text(500).size(), "packed shrank");
    const auto* dir = pkg.find(kDirTgi);
    check(dir && dir->size == 3 * 20, "DIR has one 20-byte record per compressed resource");

    // Repack: stored bytes copied over unchanged give an identical file.
    DbpfWriter again;
    for (const auto& e : pkg.entries())
        again.addStored(e.tgi, pkg.readRaw(e), e.rawSize);
    check(again.build() == w.build(), "repack is byte-identical");
    std::filesystem::remove(path);
}

// The game's key type and compression rule (cGZDBSegmentPackedFile,
// docs/save-mac.md §9.2): no high instance -> 20-byte index entries, 1 at
// 0x3C and 16-byte DIR records; compression tried from 50 bytes and kept
// only under 0.8 of the raw size.
static void testGameLayout() {
    DbpfWriter w;
    const Tgi big{0x42484156, 0x7F000001, 0x00001000, 0};
    const Tgi small{0x53545223, 0x7F000001, 0x00000081, 0};
    const Tgi weak{0x4F424A44, 0x7F000001, 0x00000041, 0};
    std::vector<uint8_t> tiny(49, 'a'); // compresses well, but under 50 bytes
    std::mt19937 rng(11);
    std::vector<uint8_t> half(400); // half noise: QFS saves ~10-20%, under the 20% the game asks
    for (size_t i = 0; i < half.size(); ++i)
        half[i] = i < 330 ? uint8_t(rng()) : 'b';
    w.add(big, text(200), true);
    w.add(small, tiny, true);
    w.add(weak, half, true);
    const auto bytes = w.build();
    auto le = [&](size_t at) { return uint32_t(bytes[at]) | bytes[at + 1] << 8 | bytes[at + 2] << 16 | uint32_t(bytes[at + 3]) << 24; };
    check(le(0x3C) == 1, "key type 1 without high instances");
    check(le(0x2C) == le(0x24) * 20, "20-byte index entries");
    check(le(0x18) == 0 && le(0x1C) == 0, "zero dates");
    const auto path = tempPackage("layout");
    w.save(path);
    {
        DbpfPackage pkg(path);
        check(pkg.find(big) && pkg.find(big)->compressed(), "a large text is compressed");
        check(pkg.find(small) && !pkg.find(small)->compressed(), "under 50 bytes stays raw");
        check(pkg.find(weak) && !pkg.find(weak)->compressed(), "a record that keeps 0.8 of its size stays raw");
        const auto* dir = pkg.find(kDirTgi);
        check(dir && dir->size == 16, "one 16-byte DIR record");
        check(pkg.read(*pkg.find(big)) == text(200), "compressed record reads back");
    }
    std::filesystem::remove(path);
    check(!worthCompressing(0x42484156, 49, 10), "49 bytes: not tried");
    check(worthCompressing(0x42484156, 50, 39), "50 bytes at 0.78: kept");
    check(!worthCompressing(0x42484156, 50, 40), "exactly 0.8: raw");
    check(!worthCompressing(type::DIR, 1000, 10), "the DIR is never compressed");
}

// The Sim parts are read on several threads (loadSimParts' parallelFor): a
// package that cannot be reopened (it went, or the process ran out of open
// files, as an app opened from the Finder did with its 256) throws in the
// workers. That called std::terminate; now the caller gets the exception.
static void testParallelReadError() {
    const auto dir = std::filesystem::temp_directory_path() / "os2_test_parallel_read";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    DbpfWriter w;
    for (uint32_t i = 0; i < 200; ++i) { // over parallelFor's 64: several threads
        w.add({0xEBCF3E27, 0x1C0532FA, i, 0}, text(3)); // GZPS
        w.add({0xAC506764, 0x1C0532FA, i, 0}, text(3)); // its 3IDR
    }
    w.save(dir / "parts.package");
    ResourceIndex index(dir);
    std::filesystem::remove_all(dir); // the index's packages are closed after indexing
    bool threw = false;
    try {
        loadSimParts(index);
    } catch (const DbpfError&) {
        threw = true;
    }
    check(threw, "a read error in the parallel Sim-part read reaches the caller");
}

int main() {
    testEmpty();
    testRoundTrip();
    testGameLayout();
    testParallelReadError();
    if (failures == 0)
        std::printf("dbpf: all tests passed\n");
    return failures ? 1 : 0;
}
