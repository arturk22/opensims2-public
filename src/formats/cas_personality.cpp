#include "cas_personality.hpp"

#include <algorithm>

namespace os2::cas {

namespace {

struct Sign {
    const char* name;
    int outgoing, nice, playful, neat, active;
    int attract[2], repel[2];
    uint32_t aspiration;
};

// BV 0x277a68c (points), 0x277a62c (attracts), 0x277a5cc (repels),
// ComputeAspirations (BV 0x8cc150).
constexpr Sign kSigns[12] = {
    {"Aries", 8, 3, 3, 5, 6, {3, 2}, {4, 7}, 0x10},        {"Taurus", 5, 4, 8, 5, 3, {1, 7}, {6, 4}, 0x01},
    {"Gemini", 7, 3, 3, 4, 8, {12, 6}, {10, 1}, 0x04},     {"Cancer", 3, 6, 4, 6, 6, {2, 8}, {3, 1}, 0x02},
    {"Leo", 10, 3, 4, 4, 4, {9, 4}, {10, 3}, 0x10},        {"Virgo", 2, 5, 3, 9, 6, {11, 9}, {5, 2}, 0x20},
    {"Libra", 8, 7, 6, 2, 2, {6, 4}, {12, 8}, 0x01},       {"Scorpio", 5, 3, 3, 6, 8, {12, 5}, {7, 11}, 0x04},
    {"Sagittarius", 3, 4, 7, 2, 9, {12, 10}, {7, 8}, 0x20}, {"Capricorn", 4, 5, 8, 7, 1, {11, 2}, {5, 3}, 0x04},
    {"Aquarius", 4, 6, 7, 4, 4, {10, 9}, {8, 6}, 0x02},    {"Pisces", 3, 7, 3, 5, 7, {8, 3}, {5, 1}, 0x20},
};

const Sign& sign(int s) { return kSigns[std::clamp(s, 1, 12) - 1]; }

} // namespace

int& Personality::operator[](int i) {
    switch (i) {
    case 0: return outgoing;
    case 1: return nice;
    case 2: return playful;
    case 3: return neat;
    default: return active;
    }
}

int Personality::operator[](int i) const { return const_cast<Personality&>(*this)[i]; }

void setTrait(Personality& p, int trait, int points) {
    points = std::clamp(points, 0, 10);
    p[trait] = points;
    const int over = p.total() - kPersonalityPoints;
    if (over > 0)
        p[trait] -= over;
}

int zodiacSign(const Personality& p) {
    int best = 1;
    float bestD = -1;
    for (int s = 1; s <= 12; ++s) {
        const Sign& z = sign(s);
        const float d[5] = {float(p.outgoing - z.outgoing), float(p.nice - z.nice), float(p.playful - z.playful),
                            float(p.neat - z.neat), float(p.active - z.active)};
        float sum = 0;
        for (float v : d)
            sum += v * v * 1e-4f; // the binary works in points * 0.01
        if (bestD < 0 || sum < bestD)
            best = s, bestD = sum;
    }
    return best;
}

Personality zodiacPersonality(int s) {
    const Sign& z = sign(s);
    return Personality{z.outgoing, z.nice, z.playful, z.neat, z.active};
}

const char* zodiacName(int s) { return sign(s).name; }
uint32_t suggestedAspiration(int s) { return sign(s).aspiration; }
std::array<int, 2> compatibleSigns(int s) { return {sign(s).attract[0], sign(s).attract[1]}; }
std::array<int, 2> incompatibleSigns(int s) { return {sign(s).repel[0], sign(s).repel[1]}; }

} // namespace os2::cas
