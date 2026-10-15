#pragma once

// The Wants & Fears strip of the live panel (docs/ui-live-mac.md §7.4): the
// selected Sim's 4 wants and 3 fears (the base game's layout) with their
// icons from the want definitions (ui.package images; a Sim subject shows
// its portrait with the definition's secondary icon), the points under each
// and the want's text as the tooltip. What is shown comes from the want
// simulator (vm/wants.hpp); the look is ours (the game's subpanel scripts
// for this are not laid out here): a strip in the panel's colours above
// the needs subpanel.

#include "app/live_panels.hpp"

namespace os2 {

// Lays the strip out with its top-left corner at (x, y), `width` wide;
// returns its height (0 when the Sim has no want state).
float layoutWantsPanel(PanelContext& c, vm::ObjectId sim, float x, float y, float width);

// A Sim's memories for the panel (MemoryEntry, live_panels.hpp).
std::vector<MemoryEntry> simMemories(PanelContext& c, int16_t neighborId);
std::vector<MemoryEntry> simMemories(vm::VM& machine, int16_t neighborId);

} // namespace os2
