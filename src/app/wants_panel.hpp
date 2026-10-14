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

// A Sim's memories (the neighbor inventory's tokens with flags 3, newest
// first): the memory object's text (CTSS 0x7D0, or 0x7D1 for a female
// Sim, "$Subject" = the Sim whose OBJD GUID is in properties 5/6), its
// description, its icon (the object's image 0x856DDBAC, instance 1) and
// whether it is a bad one (property 0 bit 2, inferred).
struct MemoryEntry {
    std::string text, description;
    const RgbaImage* icon = nullptr;
    bool bad = false;
};
std::vector<MemoryEntry> simMemories(PanelContext& c, int16_t neighborId);
std::vector<MemoryEntry> simMemories(vm::VM& machine, int16_t neighborId);
// The memories tab of the Simology panel: the icons in rows over `area`,
// with their texts as tooltips (our look).
void layoutMemories(PanelContext& c, vm::ObjectId sim, std::array<float, 4> area);

} // namespace os2
