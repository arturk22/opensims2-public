#pragma once

// Personality, zodiac and aspiration in Create-a-Sim (docs/cas-mac.md §6).
// Five traits in person data, 0..1000 each, 100 per point, 25 points in all
// (kMaxPersonalityTraitsValue = 2500; CB980E83's constants). The sign is the
// one of the 12 (Outgoing, Nice, Playful, Neat, Active) vectors nearest to
// the Sim's points (ComputeZodiacSign, squared Euclidean distance, Aries on a
// tie or none); picking a sign sets its points (SetZodiacSign). Aspirations:
// person data 46, 1 << (k - 1) for the button 0x0CDC000k: Romance 0x01,
// Family 0x02, Fortune 0x04, Reputation 0x10, Knowledge 0x20, Grow Up 0x40
// (toddlers and children).

#include <array>
#include <cstdint>

namespace os2::cas {

// Points 0..10 in the order of the zodiac table.
struct Personality {
    int outgoing = 5, nice = 5, playful = 5, neat = 5, active = 5;

    int total() const { return outgoing + nice + playful + neat + active; }
    int& operator[](int i);
    int operator[](int i) const;
};

constexpr int kPersonalityPoints = 25;

// Sets trait i to `points` (0..10), taking back what passes the 25-point
// total from that trait (UpdatePersonality).
void setTrait(Personality& p, int trait, int points);

int zodiacSign(const Personality& p);     // 1 Aries .. 12 Pisces
Personality zodiacPersonality(int sign);  // the sign's points
const char* zodiacName(int sign);         // "Aries", ...
uint32_t suggestedAspiration(int sign);   // ComputeAspirations' table (its use was not read)
std::array<int, 2> compatibleSigns(int sign);
std::array<int, 2> incompatibleSigns(int sign);

namespace aspiration {
constexpr uint32_t Romance = 0x01, Family = 0x02, Fortune = 0x04, Reputation = 0x10, Knowledge = 0x20, GrowUp = 0x40;
}

// Person data indices (STR# 0xC8 labels) the traits go to.
namespace traitdata {
constexpr int Nice = 2, Active = 3, Playful = 5, Outgoing = 6, Neat = 7, Aspiration = 46, Zodiac = 70;
}

} // namespace os2::cas
