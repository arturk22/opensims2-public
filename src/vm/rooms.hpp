#pragma once

// Room values (data owner 0x17 "room [temp 0]"), as cTSObject::InterpValue
// computes them in the Mac binary.

#include "vm.hpp"

namespace os2::vm {

int16_t roomValue(const VM& vm, int16_t room, int16_t value);

} // namespace os2::vm
