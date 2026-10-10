#include "strings.hpp"

#include "binreader.hpp"

namespace os2 {

StringTable StringTable::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    StringTable t;
    t.fileName = r.fixed(64);
    if (r.atEnd())
        return t;
    if (r.remaining() >= 2 && data[64] == 0x00) {
        // Legacy Pascal table: marker byte, u8 count.
        r.u8();
        const uint8_t n = r.atEnd() ? 0 : r.u8();
        for (uint8_t i = 0; i < n; ++i)
            t.entries.push_back({lang::EnglishUS, r.str8(), {}, 0});
        r.expectEnd("STR#");
        return t;
    }
    t.format = r.u16();
    const uint16_t n = r.u16();
    switch (t.format) {
    case 0xFFFF:
        for (uint16_t i = 0; i < n; ++i)
            t.entries.push_back({lang::EnglishUS, r.cstr(), {}, 0});
        break;
    case 0xFFFE:
        for (uint16_t i = 0; i < n; ++i) {
            Entry e;
            e.value = r.cstr();
            e.description = r.cstr();
            t.entries.push_back(std::move(e));
        }
        break;
    case 0xFFFD:
        for (uint16_t i = 0; i < n; ++i) {
            Entry e;
            e.language = r.u8();
            e.value = r.cstr();
            e.description = r.cstr();
            t.entries.push_back(std::move(e));
        }
        break;
    case 0xFFF6:
        for (uint16_t g = 0; g < n; ++g) {
            const uint8_t group = r.u8();
            const uint16_t items = r.u16();
            for (uint16_t i = 0; i < items; ++i) {
                Entry e;
                e.group = group;
                e.language = r.u8();
                r.u8(); // unknown flag (0x00 or 0xFF)
                e.value = r.str7();
                e.description = r.str7();
                t.entries.push_back(std::move(e));
            }
        }
        break;
    default:
        throw FormatError("STR#: unknown format " + hexString(t.format));
    }
    r.expectEndOrFill("STR#");
    return t;
}

size_t StringTable::count(uint8_t language) const {
    size_t n = 0;
    for (const auto& e : entries)
        n += e.language == language;
    return n;
}

std::string StringTable::get(size_t index, uint8_t language) const {
    const uint8_t secondary = language == lang::BrazilianPortuguese ? lang::Portuguese : language;
    for (uint8_t l : {language, secondary, lang::EnglishUS}) {
        size_t i = 0;
        for (const auto& e : entries)
            if (e.language == l && i++ == index)
                return e.value;
    }
    return index < entries.size() ? entries[index].value : std::string();
}

} // namespace os2
