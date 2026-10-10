#pragma once

#include <map>
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

} // namespace os2
