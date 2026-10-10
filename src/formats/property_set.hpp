#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace os2 {

// cGZPropertySetString: the game's XML property sets (catalog entries for
// floor and wall patterns, fences, ...):
//   <cGZPropertySetString>
//      <AnyUint32 key="guid" type="0xeb61e4f7">3435290776</AnyUint32>
//      <AnyString key="material" type="0x0b8bea18">floor_loftcoralsandstone</AnyString>
// Values are kept as text, keyed by `key`, with XML entities decoded.
std::map<std::string, std::string> parsePropertySet(std::string_view xml);

// A property set in either form: the XML one above or the binary one (CPF,
// magic 0xCBE750E0, used by the GZPS catalog entries of Sim outfits): u16
// version, u32 count, then per item u32 type, u32 name length, name, value
// (0xEB61E4F7 u32, 0x0C264712 i32, 0xABC78708 f32, 0xCBA908E1 bool u8,
// 0x0B8BEA18 string: u32 length + bytes). Numbers become decimal text.
std::map<std::string, std::string> readPropertySet(std::span<const uint8_t> data);

} // namespace os2
