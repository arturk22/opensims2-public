#include "formats/qfs.hpp"

#include <cstdio>
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

static void roundTrip(const std::vector<uint8_t>& in, const char* name) {
    auto c = qfsCompress(in);
    check(qfsIsCompressed(c), name);
    check(qfsUncompressedSize(c) == in.size(), name);
    check(qfsDecompress(c) == in, name);
}

int main() {
    roundTrip({}, "empty");
    roundTrip({'a'}, "single byte");

    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "The Sims 2 SimAntics behaviour tree #" + std::to_string(i % 37) + "\n";
    roundTrip(std::vector<uint8_t>(text.begin(), text.end()), "repetitive text");

    std::vector<uint8_t> zeros(200000, 0);
    roundTrip(zeros, "long run");

    std::mt19937 rng(42);
    std::vector<uint8_t> noise(70000);
    for (auto& b : noise)
        b = uint8_t(rng());
    roundTrip(noise, "random");

    // Mixed: random blocks repeated at long distances (exercises 4-byte opcodes).
    std::vector<uint8_t> mixed;
    for (int r = 0; r < 4; ++r)
        mixed.insert(mixed.end(), noise.begin(), noise.begin() + 40000);
    roundTrip(mixed, "far matches");

    // Hand-built stream: literal "abcd" then copy 4 bytes from offset 4 -> "abcdabcd".
    std::vector<uint8_t> manual = {0, 0, 0, 0, 0x10, 0xFB, 0, 0, 8, 0xE0, 'a', 'b', 'c', 'd', 0x04, 0x03, 0xFC};
    auto out = qfsDecompress(manual);
    check(std::string(out.begin(), out.end()) == "abcdabcd", "manual stream");

    if (failures == 0)
        std::printf("qfs: all tests passed\n");
    return failures ? 1 : 0;
}
