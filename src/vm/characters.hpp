#pragma once

// Make New Character (0x13): new neighbors from a template or two parents
// (characters.cpp).

#include "vm.hpp"

namespace os2::vm {

void registerCharacterPrimitives(VM& vm);

} // namespace os2::vm
