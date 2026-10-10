#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace os2 {

// True if s2pkg can decode resources of this type.
bool hasDecoder(uint32_t type);

// Fully decodes a resource and returns a human-readable dump; throws on malformed data.
std::string describeResource(uint32_t type, std::span<const uint8_t> data);

} // namespace os2
