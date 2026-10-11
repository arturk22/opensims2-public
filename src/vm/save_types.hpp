#pragma once

#include "vm.hpp"

#include "formats/lot_objects.hpp"

#include <optional>
#include <string>

namespace os2::vm {

// The objt record of an object definition, as ObjectSaveTypeTable::
// WriteSelector writes it (docs/save-mac.md §5): for objects created during
// play whose definition the lot's table lacks. Nothing if no OBJD has the
// GUID. The save type number is the caller's.
std::optional<LotObjects::SaveType> saveTypeRecord(CodeLibrary& code, uint32_t guid);

} // namespace os2::vm
