#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// Language codes of STR#/CTSS/TTAs entries (text is UTF-8).
namespace lang {
constexpr uint8_t EnglishUS = 1;
constexpr uint8_t Portuguese = 14;          // Portugal
constexpr uint8_t BrazilianPortuguese = 35; // localized names, e.g. "Andrade" for "Antonio"
// The language strings are read in by default: setPreferred's, else the
// language of defaultLocale() (OS2_LOCALE; Brazilian Portuguese).
uint8_t preferred();
void setPreferred(uint8_t language);
} // namespace lang

// String tables (STR#, CTSS catalog descriptions, TTAs pie-menu strings):
// 64-byte name, u16 format, then entries:
//   0x0000: u8 count, Pascal strings (legacy)
//   0xFFFF: u16 count, C strings
//   0xFFFE: u16 count, (value, description) C strings
//   0xFFFD: u16 count, (u8 language, value, description) C strings
//   0xFFF6: u16 count, groups of (u8, u8, value, description) .NET strings
struct StringTable {
    struct Entry {
        uint8_t language = lang::EnglishUS;
        std::string value;
        std::string description;
        uint16_t group = 0; // 0xFFF6 only
    };

    std::string fileName;
    uint16_t format = 0;
    std::vector<Entry> entries;

    static StringTable parse(std::span<const uint8_t> data);

    // The index-th string in a language: entries of each language are listed in
    // order. Brazilian Portuguese falls back to Portuguese (and Portuguese to
    // Brazilian Portuguese); every language falls back to US English, then to
    // the raw entry order. Empty if missing.
    std::string get(size_t index, uint8_t language = lang::preferred()) const;
    // Number of strings in a language.
    size_t count(uint8_t language) const;
};

} // namespace os2
