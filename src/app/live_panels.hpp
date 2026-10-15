#pragma once

// The live panel's subpanels other than the needs (LiveUi draws those), from
// the scripts cTSWinProcLivePanel::CreateAndAddSubPanel (BV 0x262d04) loads:
//
//   1  0x49065002  relationships (cTSWinProcPanelRelationships), one
//                  0x49065005 thumbnail (cTSWinProcRelationshipThumbnail) per
//                  Sim the selected one has a relationship with
//   2  0x49065003  skills and career (cTSWinProcPanelJob, cTSUISkillsCtrl)
//   3  0x49064820  "Simology" (cTSWinProcPanelSimology): personality
//                  (cTSWinProcPanelPersonality), interests, memories
//   4  0x49064800  rewards (cTSWinProcPanelRewards): career rewards
//
// Read from the VM: person data (skills 9-18, personality 2-7 and the
// originals 47-51, job GUID 89/90, level 57, performance 63, PTO 69,
// aspiration 46), the relationship variables (daily, flags, lifetime) and
// the job object's tables (STR# 0x93 titles, BCON 0x1000 wages, 0x1001
// start hour, 0x1002 hours, 0x1003 family friends, 0x101A work days).
// Details and what is inferred: docs/ui-live-mac.md §7 (binary) and §8 (this code).

#include "app/ui_panel_util.hpp"
#include "formats/ui_screen.hpp"
#include "vm/vm.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace os2 {

class FontLibrary;

// A Sim's memories as cTSMemorySimulator::GetMemories gives them to the
// panel (docs/relationships-mac.md §4): the visible memory tokens of the
// neighbor's inventory (vm/memories.hpp), newest first. From the memory
// object (cTSMemoryDefinition::Load, BV 0xb19bb0): its catalog strings, CTSS
// instance = OBJD field 41 (0x7D0 in the base game), or the next one when
// the subject is a female Sim (person data 65; cTSMemoryInstance::
// SetDefinition BV 0xb18ec8), entry 0 the title and 1 the description,
// "$Subject" the subject's name (ReplaceVariables BV 0xb18ab0: its catalog
// name; for a Sim the first name); its icon, the UI image of its group,
// instance 1. Good or bad by the kind (property 0).
struct MemoryEntry {
    uint32_t guid = 0;
    std::string text, description, subject;
    const RgbaImage* icon = nullptr; // unmasked, cached for the app's life
    bool good = true;
};

class UiResources;

// What a subpanel needs from the panel that draws it.
struct PanelContext {
    vm::VM& vm;
    UiResources& res;
    FontLibrary& fonts;
    UiScreen& out;
    std::map<const UiElement*, std::array<float, 4>>& rects; // filled as windows are laid out
    std::function<const RgbaImage*(RgbaImage)> keep;          // an image that lives as long as the screen
    std::function<void(std::array<float, 4>, std::function<void()>)> click; // a button
    std::function<void(std::array<float, 4>)> solid;                       // takes clicks, does nothing
    std::function<const RgbaImage*(int16_t neighborId)> portrait;          // null if none
    std::function<void(const std::string&)> sound;                         // UI sound by name
    std::function<void(std::array<float, 4>, std::string)> tip;           // a tooltip over a rectangle
    // A popup script (e.g. 0x4906502B) with the texts and pictures of its
    // child windows (by id; the pictures must outlive the popup), its tail
    // on the top centre of the anchor rectangle.
    std::function<void(uint32_t script, std::array<float, 4> anchor, std::vector<std::pair<uint32_t, std::string>>,
                       std::vector<std::pair<uint32_t, const RgbaImage*>>)>
        popup;
};

// Lays a window out with its area's top-left corner at (x, y).
void placeWindow(PanelContext& c, const UiElement& e, float x, float y, UiDrawHooks& hk);
// A gradient fill bar window (children 3 = mask, 4 = gradient; §5): the
// fill replaces child 3's image. By columns when the gradient is wider than
// the mask (the gradient's columns are the values) or the mask is taller
// than wide.
void fillBar(PanelContext& c, const UiElement& bar, float value, float min, float max, UiDrawHooks& hk);
// A stepped bar (child 3 = the full bar), 0..1000 in 10 divisions.
void stepBar(PanelContext& c, const UiElement& bar, float value, UiDrawHooks& hk);

class LivePanels {
public:
    enum Panel { Needs = 0, Relationships, Job, Simology, Rewards, Count };
    explicit LivePanels(UiResources& res);
    bool ok(Panel p) const;

    // The panel laid out with the left edge of its background at x and its
    // bottom at `bottom` (canvas pixels); returns the background's right edge.
    float layout(Panel p, PanelContext& c, vm::ObjectId sim, float x, float bottom);

    // Relationship thumbnails: filters (cTSWinProcPanelRelationships::
    // LoadRelationships) and paging.
    enum class Filter { Household = 1, Relatives = 2, Friends = 3, All = 5 };
    struct RelationshipRow {
        int16_t neighbor = 0;
        int daily = 0, lifetime = 0;
        int flags = 0, familial = 0;
        bool household = false;
    };
    static std::vector<RelationshipRow> relationshipsOf(vm::VM& vm, vm::ObjectId sim, Filter f);

    void resetPages() { relPage_ = 0, memPage_ = 0, memSelected_ = -1; }

private:
    float relationships(PanelContext& c, vm::ObjectId sim, float x, float bottom);
    float job(PanelContext& c, vm::ObjectId sim, float x, float bottom);
    float simology(PanelContext& c, vm::ObjectId sim, float x, float bottom);
    float rewards(PanelContext& c, vm::ObjectId sim, float x, float bottom);
    void memories(PanelContext& c, vm::ObjectId sim, const UiElement& tab);

    UiResources* res_;
    std::array<std::optional<UiScript>, Count> scripts_;
    std::optional<UiScript> relThumb_;
    Filter filter_ = Filter::All;
    int relPage_ = 0;
    int simologyTab_ = 0; // personality, interests, memories
    // The memories tab (cTSWinProcPanelMemories): its thumbnail script, page,
    // the selected memory (whose description is open) and the icons with
    // the thumbnail's alpha mask applied, by GUID.
    std::optional<UiScript> memThumb_;
    int memPage_ = 0, memSelected_ = -1;
    std::map<uint32_t, std::unique_ptr<RgbaImage>> maskedIcons_;
    std::vector<MemoryEntry> memList_;
};

} // namespace os2
