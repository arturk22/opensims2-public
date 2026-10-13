#include "app/buy_mode.hpp"

#include "app/design_tool.hpp"

#include "app/lot_vm.hpp"
#include "app/vm_objects.hpp"
#include "formats/font.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace os2 {

namespace {

constexpr uint32_t kPanelScript = 0x49060004, kPuckScript = 0x4905F85B, kDescriptionScript = 0x49063004;
constexpr uint32_t kUiTextGroup = 0x7FA7DC7C; // Text/UIText.package
constexpr uint32_t kPlacementErrors = 0x89;   // its placement error texts (code - 2)

// Buy panel windows (0x49060004, docs/buy-build-mac.md §1.4).
constexpr uint32_t kTabFunction = 0x2C584E6C, kTabRoom = 0x4C578F23, kTabCollections = 0x2CACE0CD;
constexpr uint32_t kBgFunction = 0x4CC48AA1, kBgRoom = 0x4CC48AA2, kBgCollections = 0x4CC48AA3;
constexpr uint32_t kFunctionSort = 0x0A00, kFunctionButtons = 0xA000, kRoomSort = 0x0C00, kRoomButtons = 0xC000,
                   kRoomFunctions = 0xD000, kRoomBack = 0xDA00, kCollections = 0x0E00;
constexpr uint32_t kCatalog = 0x898F42CD, kCollapsed = 0x1000, kPrev = 0x1004, kNext = 0x1005;
constexpr uint32_t kToolHand = 0xCC607031, kToolEyedropper = 0xCC607032, kToolDesign = 0xCC607030, kUndo = 0xCC607033,
                   kRedo = 0xCC607034, kDayNight = 0xCC607035;
// The collapsed grid (window 0x1006 of 0x1000: 176 x 100). The columns,
// rows and margins are the collections window's constants on the same
// panel (kCollapsedColumns=3, kCollapsedRows=2, margins 2 and 6;
// inferred: the catalog grid's own constants are in code).
constexpr float kGridX = 52, kGridY = 19, kGridW = 176, kGridH = 100;
constexpr int kColumns = 3, kRows = 2;
// The expanded catalog (window 0x2000, opened by "Expandir" 0x1009 and shut
// by "Recolher" 0x2009): its grid window 0x2006 is 176 x 196. The script has
// no constants for it; 4 rows keep the thumbnails the collapsed size (3
// columns, the same margins; inferred).
constexpr uint32_t kExpanded = 0x2000, kExpandedPrev = 0x2004, kExpandedNext = 0x2005, kExpand = 0x1009,
                   kCollapse = 0x2009;
constexpr float kExpandedGridH = 196;
constexpr int kExpandedRows = 4;
// The expanded catalog's top over the panel's origin; the collapsed one's
// is 96 (window 0x1000).
constexpr float kExpandedTop = 9;
constexpr float kMarginX = 2, kMarginY = 6;

// The room buttons 0xC100-0xC900: room sort bit of each (§1.4).
constexpr std::array<int, 9> kRoomBits = {3, 5, 4, 6, 7, 1, 0, 8, 2};

// The subsort a button 0xB<k><n>0 of function k selects (§1.4): its bits,
// 0 for "all", -1 for none (back, or not a button).
int subSortOf(int k, int n) {
    if (n == 7 && k != 7)
        return 0;
    if (n == 6 && k != 7)
        return 0x80;
    switch (k) {
    case 6: { const int t[] = {-1, -1, 1, 2, 0x20, 8, -1, -1, 0x10, 4}; return n < 10 ? t[n] : -1; }
    case 7: { const int t[] = {-1, -1, -1, -1, 8, 2, 0x10, 0x80, 0, -1}; return n < 10 ? t[n] : -1; }
    case 8: { const int t[] = {-1, -1, 1, 2, 8, 4, -1, -1, 0x10, -1}; return n < 10 ? t[n] : -1; }
    case 9: { const int t[] = {-1, -1, 1, -1, 2, 4, -1, -1, 8, -1}; return n < 10 ? t[n] : -1; }
    default: { const int t[] = {-1, -1, 1, 2, 4, 8, -1, -1, 0x10, -1}; return n < 10 ? t[n] : -1; }
    }
}

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

std::string money(int32_t v) {
    std::string digits = std::to_string(v < 0 ? -int64_t(v) : int64_t(v)), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

std::string area(int x0, int y0, int x1, int y1) {
    return "(" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(x1) + "," + std::to_string(y1) +
           ")";
}

// A flat unit tile, alpha blended and unlit, for the footprint.
ObjectModel tileModel(const std::string& name, const char* rgb) {
    ObjectModel m;
    m.name = name;
    ObjectModel::Part part;
    part.node = part.shape = name;
    part.material = name;
    const float e = 0.04f;
    part.mesh.positions = {{e, e, 0}, {1 - e, e, 0}, {1 - e, 1 - e, 0}, {e, 1 - e, 0}};
    part.mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    part.mesh.triangles = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    m.parts.push_back(std::move(part));
    MaterialDefinition md;
    md.materialType = "StandardMaterial";
    md.properties = {{"stdMatDiffCoef", rgb},
                     {"stdMatEmissiveCoef", rgb},
                     {"stdMatUntexturedDiffAlpha", "0.45"},
                     {"stdMatAlphaBlendMode", "blend"},
                     {"stdMatLightingEnabled", "false"},
                     {"stdMatCullMode", "none"},
                     {"stdMatLayer", "1"}};
    m.materials[name] = md;
    return m;
}

simd_float4x4 at(float x, float y, float z) {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(x, y, z, 1);
    return m;
}

} // namespace

BuyMode::BuyMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
                 const std::filesystem::path& resDir)
    : sim_(simulation), vm_(simulation.machine()), view_(view), renderer_(renderer), index_(index),
      res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")), catalog_(Catalog::load(index)) {
    catalog_.setThumbnailPackage(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return !into.roots.empty();
        }
        std::fprintf(stderr, "opensims2: buy mode: no UI script %08X\n", id);
        return false;
    };
    ok_ = load(kPanelScript, panel_) && load(kPuckScript, puck_) && load(kDescriptionScript, description_);
    design_ = std::make_unique<DesignTool>(vm_, view_, index_, *res_, *fonts_);
    design_->say = [this](const std::string& m) { say(m); };
    design_->playSound = [this](const std::string& n) { sound(n.c_str()); };
    design_->onAccepted = [this](const DesignTool::Change& c) {
        Op op;
        op.kind = Op::Kind::Design, op.id = c.id, op.cost = c.cost, op.before = c.before, op.after = c.after;
        undo_.push_back(std::move(op));
        redo_.clear();
        dirty_ = true;
    };
    try {
        tileModels_[0] = renderer_.addObject(tileModel("os2_buy_ok", "0.2,1,0.2"), index_);
        tileModels_[1] = renderer_.addObject(tileModel("os2_buy_no", "1,0.15,0.1"), index_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: buy mode: %s\n", e.what());
    }
}

BuyMode::~BuyMode() = default;

void BuyMode::say(const std::string& m) {
    message_ = m;
    std::printf("opensims2: buy mode: %s\n", m.c_str());
    std::fflush(stdout);
    dirty_ = true;
}

void BuyMode::enter() {
    if (active_)
        return;
    active_ = true;
    savedPaused_ = vm_.globals[vm::global::Paused];
    vm_.globals[vm::global::Paused] = 1;
    // The level of the selected Sim, else the lot's ground.
    level_ = vm_.world ? vm_.world->firstLevel() : 1;
    if (const vm::Object* s = vm_.object(vm_.globals[vm::global::SelectedPersonId]); s && s->position.inWorld())
        level_ = s->position.level;
    say("entered (" + std::to_string(catalog_.entries().size()) + " catalog items, funds " + money(vm::funds(vm_)) +
        ")");
}

void BuyMode::exit() {
    if (!active_)
        return;
    cancel();
    design_->cancel();
    tool_ = Tool::Hand;
    // The history is reset: what was sold dies now (ResetSelf).
    for (const Op& op : undo_)
        if (op.kind == Op::Kind::Deleted && vm_.object(op.id)) {
            for (vm::ObjectId part : vm::groupOf(vm_, op.id))
                view_.forget(part);
            vm::deleteGroup(vm_, op.id);
        }
    undo_.clear(), redo_.clear();
    vm_.globals[vm::global::Paused] = savedPaused_;
    active_ = false;
    selected_ = nullptr;
    update();
    say("left");
}

// --- The object tool --------------------------------------------------------

bool BuyMode::select(const CatalogEntry& entry) {
    cancel();
    try {
        held_ = vm::createGhost(vm_, entry.group, entry.instance);
    } catch (const std::exception& e) {
        say(entry.name + ": " + e.what());
        held_ = 0;
        return false;
    }
    heldNew_ = true;
    selected_ = &entry;
    last_ = {};
    say("holding " + entry.name + " (" + money(entry.price) + ")");
    return true;
}

vm::Placement BuyMode::current(vm::ObjectId id) {
    vm::Placement p;
    const vm::Object* lead = vm_.object(id);
    if (!lead)
        return p;
    p.ok = lead->position.inWorld();
    p.level = lead->position.level;
    p.direction = lead->data[vm::objdata::Direction];
    p.container = lead->data[vm::objdata::ContainerId];
    p.slot = p.container ? lead->data[vm::objdata::SlotNumber] : -1;
    for (vm::ObjectId part : vm::groupOf(vm_, id))
        if (const vm::Object* o = vm_.object(part))
            p.parts.push_back({part, vm::VM::tileOf(*o)});
    return p;
}

// Shows the held object where `p` says, whether it fits or not (the
// trees run only when it is placed).
void BuyMode::preview(const vm::Placement& p) {
    for (const auto& part : p.parts) {
        vm::Object* o = vm_.object(part.id);
        if (!o)
            continue;
        o->data[vm::objdata::Direction] = int16_t(p.direction);
        o->position = {float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level};
        o->data[vm::objdata::ContainerId] = p.ok ? p.container : 0;
        o->data[vm::objdata::SlotNumber] = int16_t(p.ok && p.container ? p.slot : -1);
        if (p.ok && p.container)
            if (const vm::Object* c = vm_.object(p.container))
                o->position = c->position;
    }
}

bool BuyMode::changeLevel(int delta) {
    if (!active_ || !vm_.world)
        return false;
    // Not the pool's level (0, under the ground): the floors stop at the ground.
    const int first = vm_.world->firstLevel(), hi = first + std::max(1, vm_.world->levelCount()) - 1;
    const int lo = std::min(std::max(first, 1), hi);
    const int level = std::clamp(level_ + delta, lo, hi);
    if (level == level_)
        return false;
    level_ = level;
    say("level " + std::to_string(level_));
    // What is held goes to the new floor at the same tile.
    if (held_ && !last_.parts.empty()) {
        const vm::TilePos lead = last_.parts.front().tile;
        holdAt(lead.x, lead.y, direction_);
    }
    return true;
}

// OnRelease's sound (docs/buy-build-mac.md §6): one for doors and windows
// (object types 8 and 9), one for plants, one for the rest. The sound names
// are inferred from the executable's ui_* list; plants = decorative
// function (0x20) with subsort 8 (the "plants" button, §1.4).
const char* BuyMode::dropSound(vm::ObjectId id) const {
    const vm::Object* o = vm_.object(id);
    if (!o || !o->definition)
        return "ui_object_plop";
    const auto& d = *o->definition;
    const int type = d.field(objd::ObjectType);
    if (type == 8 || type == 9)
        return "ui_build_door_plop";
    if ((d.field(objd::FunctionSort) & 0x20) && (d.field(objd::FunctionSubSort) & 8))
        return "ui_build_garden_plop";
    return "ui_object_plop";
}

std::optional<vm::TilePos> BuyMode::tileUnder(simd_float3 eye, simd_float3 dir) const {
    if (!vm_.world || std::fabs(dir.z) < 1e-5f)
        return std::nullopt;
    // The ray against the floor of the current level, twice (the floor's
    // height where the first guess lands).
    float z = sim_.floorHeight(level_, float(vm_.world->width()) / 2, float(vm_.world->height()) / 2);
    simd_float3 hit{};
    for (int i = 0; i < 3; ++i) {
        const float t = (z - eye.z) / dir.z;
        if (t <= 0)
            return std::nullopt;
        hit = eye + dir * t;
        z = sim_.floorHeight(level_, hit.x, hit.y);
    }
    return vm::TilePos{int(std::floor(hit.x)), int(std::floor(hit.y)), level_};
}

void BuyMode::holdAt(int x, int y, int direction) {
    direction_ = direction;
    if (!held_)
        return;
    last_ = vm::checkPlacement(vm_, held_, {x, y, level_}, direction_);
    preview(last_);
    dirty_ = true;
}

void BuyMode::pointAt(simd_float3 eye, simd_float3 dir) {
    if (!active_ || !held_)
        return;
    if (auto t = tileUnder(eye, dir))
        holdAt(t->x, t->y, direction_);
}

std::string BuyMode::errorText(const vm::Placement& p) {
    if (p.error >= 2 && vm::placementerror::textIndex(p.error) >= 0)
        if (const StringTable* t = res_->table(kUiTextGroup, kPlacementErrors)) {
            std::string s = t->get(size_t(vm::placementerror::textIndex(p.error)));
            if (!s.empty())
                return s + " (" + p.reason + ")";
        }
    return p.reason;
}

bool BuyMode::placeAt(int x, int y, int direction) {
    if (!held_)
        return false;
    holdAt(x, y, direction);
    const vm::Placement p = last_;
    if (!p.ok) {
        say("cannot place at (" + std::to_string(x) + ", " + std::to_string(y) + ", level " + std::to_string(level_) +
            "): " + errorText(p));
        sound("ui_error");
        return false;
    }
    const vm::ObjectId id = held_;
    if (heldNew_) {
        const int price = selected_ ? selected_->price : 0;
        std::string why;
        if (!vm::buy(vm_, id, p, price, &why)) {
            say("not bought: " + why);
            sound("ui_error");
            return false;
        }
        undo_.push_back({Op::Kind::Created, id, p, price});
        sound(dropSound(id));
        say("bought " + (selected_ ? selected_->name : std::string("?")) + " as object " + std::to_string(id) +
            " for " + money(price) + "; funds " + money(vm::funds(vm_)));
        held_ = 0;
        redo_.clear();
        // "Keep Buying" (OBJD field 96): another one is held at once.
        if (selected_ && vm_.object(id) && vm_.object(id)->definition &&
            vm_.object(id)->definition->field(objd::KeepBuying))
            select(*selected_);
    } else {
        vm::applyPlacement(vm_, id, p);
        vm::runUserPlacement(vm_, id);
        undo_.push_back({Op::Kind::Moved, id, heldFrom_, 0});
        sound(dropSound(id));
        redo_.clear();
        held_ = 0;
        say("moved object " + std::to_string(id));
    }
    dirty_ = true;
    return true;
}

bool BuyMode::grab(vm::ObjectId id) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    cancel();
    const vm::ObjectId lead = vm::multiTileGroup(*o);
    heldFrom_ = current(lead);
    // The user pickup tree (12) runs on single-tile objects (the group
    // handles multi-tile ones, §4).
    if (vm::groupOf(vm_, lead).size() == 1)
        try {
            vm_.runFunction(lead, vm::entry::UserPickup);
        } catch (const std::exception&) {
        }
    held_ = lead;
    heldNew_ = false;
    direction_ = heldFrom_.direction;
    sound("ui_object_pickup");
    say("picked up object " + std::to_string(lead) + " (" + vm_.object(lead)->definition->fileName + ", worth " +
        money(vm::saleValue(vm_, lead)) + ")");
    return true;
}

// What the tools act on: the object under the mouse ray, else (and for a
// slot's contents first) what stands on the tile under it. People are never
// picked.
vm::ObjectId BuyMode::objectAt(simd_float3 eye, simd_float3 dir, const std::optional<vm::TilePos>& tile) const {
    auto usable = [&](vm::ObjectId id) {
        const vm::Object* o = vm_.object(id);
        return o && !o->person && o->definition;
    };
    if (pickObject)
        if (const vm::ObjectId id = pickObject(eye, dir); usable(id))
            return id;
    if (!tile)
        return 0;
    // An object in a slot of something on the tile first, then what stands on it.
    const std::vector<vm::ObjectId> on = vm_.objectsOnTile(*tile);
    for (vm::ObjectId id : on)
        for (const auto& [other, x] : vm_.objects())
            if (x.data[vm::objdata::ContainerId] == id && !x.person && x.definition)
                return other;
    for (vm::ObjectId id : on)
        if (usable(id))
            return id;
    return 0;
}

void BuyMode::setTool(Tool t) {
    cancel();
    design_->cancel();
    tool_ = t;
    dirty_ = true;
}

// The catalog entry of an object (its group's lead): by GUID, else by file name.
// A multi-tile object is in the catalog by its master OBJD, whose GUID none of
// the parts has ("Sofa - Loveseat - Art Nouveau (0,0)"): the multi-tile entry
// of the parts' group.
const CatalogEntry* BuyMode::catalogEntryOf(const vm::Object* l) const {
    if (!l || !l->definition)
        return nullptr;
    const CatalogEntry* entry = catalog_.byGuid(l->guid());
    if (!entry)
        entry = catalog_.find(l->definition->fileName);
    if (!entry)
        for (const auto& e : catalog_.entries())
            if (e.multiTile && e.group == l->group) {
                entry = &e;
                break;
            }
    return entry;
}

bool BuyMode::eyedrop(vm::ObjectId id) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::ObjectId lead = vm::multiTileGroup(*o);
    const vm::Object* l = vm_.object(lead);
    const CatalogEntry* entry = catalogEntryOf(l);
    if (!entry) {
        say(std::string(o->definition->fileName) + " is not in the catalog");
        sound("ui_error");
        return false;
    }
    if (!select(*entry))
        return false;
    tool_ = Tool::Hand; // the object tool takes over, as after a catalog click
    return true;
}

bool BuyMode::design(vm::ObjectId id, simd_float2 at) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::Object* lead = vm_.object(vm::multiTileGroup(*o));
    const CatalogEntry* entry = catalogEntryOf(lead);
    const std::string name = entry ? entry->name : o->definition->fileName;
    dirty_ = true;
    return design_->open(id, at, name);
}

bool BuyMode::click(simd_float3 eye, simd_float3 dir, std::optional<simd_float2> at) {
    if (!active_)
        return false;
    if (design_->isOpen()) // a click off the dialog closes it, as Cancel
        design_->cancel(), dirty_ = true;
    const auto t = tileUnder(eye, dir);
    if (held_) {
        if (!t)
            return false;
        return placeAt(t->x, t->y, direction_);
    }
    const vm::ObjectId id = objectAt(eye, dir, t);
    if (!id)
        return false;
    switch (tool_) {
    case Tool::Eyedropper: return eyedrop(id);
    case Tool::Design:
        return design(id, at.value_or(mouse_.value_or(simd_make_float2(float(canvasW_) / 2, float(canvasH_) / 2))));
    case Tool::Hand: return grab(id);
    }
    return false;
}

void BuyMode::rotate(int steps) {
    direction_ = ((direction_ + steps) % 8 + 8) % 8;
    if (held_)
        sound("ui_object_rotate");
    if (held_ && !last_.parts.empty()) {
        const vm::TilePos lead = last_.parts.front().tile;
        holdAt(lead.x, lead.y, direction_);
    }
}

void BuyMode::cancel() {
    if (!held_)
        return;
    if (heldNew_) {
        for (vm::ObjectId part : vm::groupOf(vm_, held_))
            view_.forget(part);
        vm::deleteGroup(vm_, held_);
    } else {
        if (heldFrom_.ok)
            vm::applyPlacement(vm_, held_, heldFrom_);
    }
    held_ = 0;
    last_ = {};
    dirty_ = true;
}

bool BuyMode::deleteHeld() {
    if (!held_)
        return false;
    if (heldNew_) { // never bought: it just goes
        cancel();
        return true;
    }
    const vm::ObjectId id = held_;
    // Bought in this session: destroyed at once, the price back (the
    // purchase leaves the history, §2).
    auto created = std::find_if(undo_.begin(), undo_.end(),
                                [&](const Op& op) { return op.kind == Op::Kind::Created && op.id == id; });
    if (created != undo_.end()) {
        vm::chargeFunds(vm_, created->cost);
        undo_.erase(std::remove_if(undo_.begin(), undo_.end(), [&](const Op& op) { return op.id == id; }),
                    undo_.end());
        for (vm::ObjectId part : vm::groupOf(vm_, id))
            view_.forget(part);
        vm::deleteGroup(vm_, id);
        held_ = 0;
        sound("ui_object_sell");
        say("returned object " + std::to_string(id) + " for its full price; funds " + money(vm::funds(vm_)));
        return true;
    }
    if (heldFrom_.ok)
        vm::applyPlacement(vm_, id, heldFrom_); // back where it was, for undo
    const int value = vm::sell(vm_, id);
    sound("ui_object_sell");
    undo_.push_back({Op::Kind::Deleted, id, heldFrom_, -value});
    redo_.clear();
    held_ = 0;
    say("sold object " + std::to_string(id) + " for " + money(value) + "; funds " + money(vm::funds(vm_)));
    return true;
}

bool BuyMode::undo() {
    cancel();
    if (undo_.empty())
        return false;
    Op op = undo_.back();
    undo_.pop_back();
    switch (op.kind) {
    case Op::Kind::Created: // out of the world, the price back
        op.placement = current(op.id);
        vm::pickUp(vm_, op.id);
        vm::chargeFunds(vm_, op.cost);
        break;
    case Op::Kind::Deleted: // back where it was, the refund paid back
        vm::applyPlacement(vm_, op.id, op.placement);
        vm::chargeFunds(vm_, op.cost);
        break;
    case Op::Kind::Moved: {
        const vm::Placement now = current(op.id);
        vm::applyPlacement(vm_, op.id, op.placement);
        op.placement = now;
        break;
    }
    case Op::Kind::Design: // the old design back, the price back
        DesignTool::apply(vm_, view_, op.id, op.before);
        vm::chargeFunds(vm_, op.cost);
        break;
    }
    redo_.push_back(op);
    sound("ui_undo");
    say("undone; funds " + money(vm::funds(vm_)));
    return true;
}

bool BuyMode::redo() {
    cancel();
    if (redo_.empty())
        return false;
    Op op = redo_.back();
    redo_.pop_back();
    switch (op.kind) {
    case Op::Kind::Created:
        if (!vm::canAfford(vm_, op.cost))
            return redo_.push_back(op), false;
        vm::applyPlacement(vm_, op.id, op.placement);
        vm::chargeFunds(vm_, -op.cost);
        break;
    case Op::Kind::Deleted:
        op.placement = current(op.id);
        vm::pickUp(vm_, op.id);
        vm::chargeFunds(vm_, -op.cost);
        break;
    case Op::Kind::Moved: {
        const vm::Placement now = current(op.id);
        vm::applyPlacement(vm_, op.id, op.placement);
        op.placement = now;
        break;
    }
    case Op::Kind::Design:
        if (!vm::canAfford(vm_, op.cost))
            return redo_.push_back(op), false;
        DesignTool::apply(vm_, view_, op.id, op.after);
        vm::chargeFunds(vm_, -op.cost);
        break;
    }
    undo_.push_back(op);
    sound("ui_redo");
    say("redone; funds " + money(vm::funds(vm_)));
    return true;
}

void BuyMode::update() {
    size_t used = 0;
    if (active_ && held_ && tileModels_[0] >= 0)
        for (const auto& part : last_.parts) {
            const int model = tileModels_[last_.ok ? 0 : 1];
            const float z = sim_.floorHeight(part.tile.level, float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f) +
                            0.02f;
            const simd_float4x4 m = at(float(part.tile.x), float(part.tile.y), z);
            if (used == tileInstances_.size())
                tileInstances_.push_back(renderer_.addInstance(model, m));
            else
                renderer_.setInstanceModel(tileInstances_[used], model), renderer_.setInstanceTransform(tileInstances_[used], m);
            ++used;
        }
    for (size_t i = used; i < tileInstances_.size(); ++i)
        renderer_.setInstanceTransform(tileInstances_[i], at(0, 0, -1000));
}

// --- The panel ---------------------------------------------------------------

std::vector<const CatalogEntry*> BuyMode::items() const {
    if (sort_ == Sort::Function && function_ >= 0) {
        std::vector<const CatalogEntry*> out;
        for (const auto* e : catalog_.byFunction(function_))
            if (subSort_ == 0 || (e->functionSubSort & subSort_))
                out.push_back(e);
        return out;
    }
    if (sort_ == Sort::Room && room_ >= 0) {
        std::vector<const CatalogEntry*> out;
        for (const auto* e : catalog_.byRoom(room_))
            if (roomFunction_ == 0 || (e->functionSort & roomFunction_))
                out.push_back(e);
        return out;
    }
    return {};
}

UiScreen BuyMode::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    frameImages_.clear();
    if (!ok_ || !active_)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
    auto place = [&](const UiElement& e, float x, float y, UiDrawHooks& hk) {
        const auto a = e.area();
        hk.rects = &rects;
        layoutUiElement(e, x - float(a[0]), y - float(a[1]), *res_, *fonts_, out, hk);
    };
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        if (!e)
            return std::nullopt;
        auto it = rects.find(e);
        return it == rects.end() ? std::nullopt : std::optional(it->second);
    };
    auto target = [&](const UiElement* e, Action a, int v) {
        if (auto r = rectOf(e))
            targets_.push_back({*r, a, v});
    };
    auto text = [&](const std::string& s, float x, float y, float w, float h, const char* align,
                    const char* color = "(5,6,113)") {
        UiElement t;
        t.tag = "LEGACY";
        t.attrs = {{"clsid", "GZWinText"}, {"caption", s},          {"font", "LiveModePanelPuck"},
                   {"forecolor", color},    {"align", align},       {"winflag_visible", "yes"},
                   {"area", area(0, 0, int(w), int(h))}};
        UiDrawHooks th;
        layoutUiElement(t, x, y, *res_, *fonts_, out, th);
    };

    // Joined to the puck as the live panel is (live_ui.cpp, inferred): the
    // panel's hidden rectangle captioned "4bdeffaa:bl" over the puck's
    // window 0x4BDEFFAA, the puck at the bottom left.
    const UiElement& puck = puck_.roots.front();
    // The catalog follows the open sort area (inferred: the script keeps it
    // at x 447 under the collections' layout): it starts where that area
    // ends, the dark green strip runs up to it and the end cap gives way
    // to the catalog's own joint ("center piece").
    {
        UiElement& r = panel_.roots.front();
        const UiElement* sortArea = r.child(sort_ == Sort::Function ? kFunctionSort : kRoomSort);
        const int right = sortArea ? sortArea->area()[2] : 447;
        if (UiElement* c = r.child(kCatalog)) {
            const auto a = c->area();
            c->set("area", area(right, a[1], right + (a[2] - a[0]), a[3]));
        }
        if (UiElement* g = r.child(0x4CC48AA6)) {
            const auto a = g->area();
            g->set("area", area(a[0], a[1], right, a[3]));
        }
        if (UiElement* e = r.child(0x4CC48AA4))
            e->set("winflag_visible", "no");
    }
    const UiElement& root = panel_.roots.front();
    const auto pa = puck.area();
    const float puckX = 8, puckY = float(height) - float(pa[3] - pa[1]);
    float x0 = puckX, y0 = puckY - 137;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = root.child(0x0000AAAA))
            x0 = puckX + float(a->area()[0] - b->area()[0]), y0 = puckY + float(a->area()[1] - b->area()[1]);

    UiDrawHooks hk;
    const bool byFunction = sort_ == Sort::Function;
    for (uint32_t id : {kBgFunction, kBgRoom, kBgCollections, kCollections})
        if (const UiElement* e = root.child(id))
            (id == (byFunction ? kBgFunction : kBgRoom) ? hk.show : hk.hide).insert(e);
    if (const UiElement* e = root.child(kTabFunction))
        hk.buttonState[e] = byFunction ? 2 : 1;
    if (const UiElement* e = root.child(kTabRoom))
        hk.buttonState[e] = byFunction ? 1 : 2;
    if (const UiElement* e = root.child(kTabCollections)) // collections are Bon Voyage's
        hk.hide.insert(e);
    if (const UiElement* e = root.child(kToolHand))
        hk.buttonState[e] = (held_ && !heldNew_) || (tool_ == Tool::Hand && !held_) ? 2 : 1;
    if (const UiElement* e = root.child(kToolEyedropper))
        hk.buttonState[e] = tool_ == Tool::Eyedropper ? 2 : 1;
    if (const UiElement* e = root.child(kToolDesign))
        hk.buttonState[e] = tool_ == Tool::Design ? 2 : 1;
    if (const UiElement* e = root.child(kUndo))
        hk.buttonState[e] = undo_.empty() ? 0 : 1;
    if (const UiElement* e = root.child(kRedo))
        hk.buttonState[e] = redo_.empty() ? 0 : 1;
    // The sort area: the category buttons, or the chosen category's subsorts.
    if (const UiElement* fs = root.child(kFunctionSort)) {
        (byFunction ? hk.show : hk.hide).insert(fs);
        const uint32_t open = function_ < 0 ? kFunctionButtons : uint32_t(0xB000 + (function_ + 1) * 0x100);
        for (const auto& c : fs->children)
            (c.id() == open ? hk.show : hk.hide).insert(&c);
        if (function_ >= 0)
            if (const UiElement* panel = fs->child(uint32_t(0xB000 + (function_ + 1) * 0x100)))
                for (const auto& b : panel->children) {
                    const int n = int(b.id() >> 4 & 0xF);
                    if (subSortOf(function_ + 1, n) == subSort_ && n != 1)
                        hk.buttonState[&b] = 2;
                }
    }
    if (const UiElement* rs = root.child(kRoomSort)) {
        (byFunction ? hk.hide : hk.show).insert(rs);
        if (const UiElement* c = rs->child(kRoomButtons))
            (room_ < 0 ? hk.show : hk.hide).insert(c);
        if (const UiElement* d = rs->child(kRoomFunctions)) {
            (room_ < 0 ? hk.hide : hk.show).insert(d);
            for (const auto& c : d->children) {
                const uint32_t id = c.id();
                if (id >= 0xE100 && id <= 0xE900) { // the chosen room's header
                    const int k = int(id >> 8 & 0xF);
                    (room_ >= 0 && kRoomBits[size_t(k - 1)] == room_ ? hk.show : hk.hide).insert(&c);
                }
                if (id >= 0xD100 && id <= 0xD900 && roomFunction_ == (1 << ((id >> 8 & 0xF) - 1)))
                    hk.buttonState[&c] = 2;
            }
        }
    }
    // The grid: thumbnails of the current page with their prices, the
    // selected one marked, the ones the family cannot afford dimmed
    // (UpdateAffordability, §1.5: frame 0, inferred).
    const auto list = items();
    const int rows = expanded_ ? kExpandedRows : kRows;
    const float gridH = expanded_ ? kExpandedGridH : kGridH;
    const size_t perPage = size_t(kColumns * rows);
    const size_t pages = std::max<size_t>(1, (list.size() + perPage - 1) / perPage);
    page_ = std::min(page_, pages - 1);
    const int64_t funds = vm::noCharge(vm_) ? INT64_MAX : vm::funds(vm_); // no charge: nothing is too dear
    std::vector<std::pair<std::array<float, 4>, const CatalogEntry*>> cells;
    const UiElement* catalog = root.child(kCatalog);
    const UiElement* collapsed = catalog ? catalog->child(expanded_ ? kExpanded : kCollapsed) : nullptr;
    if (catalog)
        if (const UiElement* other = catalog->child(expanded_ ? kCollapsed : kExpanded))
            hk.hide.insert(other);
    auto drawGrid = [&](UiScreen& s, float x, float y) {
            const float cw = (kGridW - kMarginX * (kColumns - 1)) / kColumns;
            const float ch = (gridH - kMarginY * float(rows - 1)) / float(rows);
            const float thumb = std::min(cw, ch - 12);
            for (size_t i = 0; i < perPage && page_ * perPage + i < list.size(); ++i) {
                const CatalogEntry* e = list[page_ * perPage + i];
                const float cx = x + kGridX + float(i % kColumns) * (cw + kMarginX);
                const float cy = y + kGridY + float(i / kColumns) * (ch + kMarginY);
                const float tx = cx + (cw - thumb) / 2;
                if (e == selected_ || e == hovered_) {
                    UiQuad q;
                    q.x0 = tx - 2, q.y0 = cy - 2, q.x1 = tx + thumb + 2, q.y1 = cy + thumb + 2;
                    q.color = e == selected_ ? std::array<float, 4>{0.6f, 1, 0.6f, 0.7f}
                                             : std::array<float, 4>{1, 1, 1, 0.45f};
                    s.quads.push_back(q);
                }
                if (const RgbaImage* img = catalog_.thumbnail(*e)) {
                    UiQuad q;
                    q.image = img;
                    q.x0 = tx, q.y0 = cy, q.x1 = tx + thumb, q.y1 = cy + thumb;
                    if (e->price > funds)
                        q.color = {0.45f, 0.45f, 0.45f, 0.8f};
                    s.quads.push_back(q);
                }
                cells.push_back({{tx, cy, tx + thumb, cy + ch}, e});
            }
        };
    place(root, x0, y0, hk);
    if (auto r = rectOf(collapsed))
        drawGrid(out, (*r)[0], (*r)[1]);
    for (const auto& [r, e] : cells) {
        text(money(e->price), r[0] - 8, r[3] - 13, r[2] - r[0] + 16, 13, "center",
             e->price > funds ? "(150,40,40)" : "(5,6,113)");
        targets_.push_back({r, Action::Item, int(std::find(list.begin(), list.end(), e) - list.begin())});
    }
    // Targets.
    target(root.child(kTabFunction), Action::Tab, 0);
    target(root.child(kTabRoom), Action::Tab, 1);
    target(root.child(kToolHand), Action::Tool, 0);
    target(root.child(kToolEyedropper), Action::Tool, 1);
    target(root.child(kToolDesign), Action::Tool, 2);
    target(root.child(kDayNight), Action::DayNight, 0);
    target(root.child(kUndo), Action::Undo, 0);
    target(root.child(kRedo), Action::Redo, 0);
    if (collapsed) {
        target(collapsed->child(expanded_ ? kExpandedPrev : kPrev), Action::Prev, 0);
        target(collapsed->child(expanded_ ? kExpandedNext : kNext), Action::Next, 0);
        target(collapsed->child(expanded_ ? kCollapse : kExpand), Action::Expand, 0);
    }
    if (const UiElement* fs = root.child(kFunctionSort)) {
        if (const UiElement* c = fs->child(kFunctionButtons))
            for (const auto& b : c->children)
                target(&b, Action::Function, int(b.id() >> 8 & 0xF) - 1);
        if (function_ >= 0)
            if (const UiElement* panel = fs->child(uint32_t(0xB000 + (function_ + 1) * 0x100)))
                for (const auto& b : panel->children) {
                    const int n = int(b.id() >> 4 & 0xF);
                    if (n == 1)
                        target(&b, Action::Back, 0);
                    else if (const int ss = subSortOf(function_ + 1, n); ss >= 0)
                        target(&b, Action::SubSort, ss);
                }
    }
    if (const UiElement* rs = root.child(kRoomSort)) {
        if (const UiElement* c = rs->child(kRoomButtons))
            for (const auto& b : c->children)
                target(&b, Action::Room, kRoomBits[size_t((b.id() >> 8 & 0xF) - 1)]);
        if (const UiElement* d = rs->child(kRoomFunctions))
            for (const auto& b : d->children) {
                const uint32_t id = b.id();
                if (id == kRoomBack)
                    target(&b, Action::Back, 0);
                else if (id >= 0xD100 && id <= 0xD900)
                    target(&b, Action::RoomFunction, 1 << ((id >> 8 & 0xF) - 1));
            }
    }
    for (const auto& [e, r] : rects)
        if (e->clsid() == "GZWinBMP" || e->clsid() == "GZWinGen" || e->clsid().rfind("0x", 0) == 0)
            solid_.push_back(r);

    // The catalog description of the item under the mouse, above the
    // panel (inferred placement).
    if (const CatalogEntry* e = hovered_) {
        UiElement& dm = description_.roots.front();
        if (UiElement* n = dm.child(0x4000))
            n->set("area", "(158,36,456,120)"), n->set("wrapped", "yes");
        if (UiElement* n = dm.child(0x5000))
            n->set("winflag_visible", "no");
        const UiElement& d = dm;
        const auto da = d.area();
        const float dx = x0 + 205, dy = y0 + (expanded_ ? kExpandedTop : 96) - float(da[3] - da[1]) - 4;
        UiDrawHooks dh;
        if (const UiElement* n = d.child(0x3000))
            dh.captions[n] = e->name;
        if (const UiElement* n = d.child(0x4000))
            dh.captions[n] = e->description;
        // The ratings (GetCatalogRating: fields 82-88), named as the needs
        // panel names the motives (Live.package STR# 0x82).
        std::string ratings;
        const std::array<int, 7> captions = {2, 6, 10, 14, 4, 8, 16};
        for (size_t i = 0; i < e->ratings.size(); ++i)
            if (e->ratings[i])
                if (auto s = res_->string({0x7F96C284, 0x82, uint32_t(captions[i])}))
                    ratings += (ratings.empty() ? "" : "   ") + s->substr(0, s->find('|')) + " " +
                               (e->ratings[i] > 0 ? "+" : "") + std::to_string(e->ratings[i]);
        // The description wraps down to the ratings line, which takes the
        // debug line's place (inferred: the game's text layout is in code).
        if (const UiElement* n = d.child(0x6000))
            dh.captions[n] = ratings;
        if (const UiElement* f = d.child(0x8000))
            if (const UiElement* img = f->child(0x8001))
                if (const RgbaImage* t = catalog_.thumbnail(*e))
                    dh.images[img] = t;
        for (uint32_t id : {0x9000u, 0xA000u})
            if (const UiElement* g = d.child(id))
                dh.hide.insert(g);
        dh.rects = &rects;
        layoutUiElement(d, dx - float(da[0]), dy - float(da[1]), *res_, *fonts_, out, dh);
        if (const UiElement* pb = d.child(0x0002))
            if (auto r = rectOf(pb))
                text(money(e->price), (*r)[0], (*r)[1], (*r)[2] - (*r)[0], (*r)[3] - (*r)[1], "center",
                     e->price > funds ? "(150,40,40)" : "(5,6,113)");
    }
    // Why the held object does not fit, by the mouse (the game's error
    // callout; its look is inferred).
    if (held_ && !last_.ok && last_.error) {
        const std::string why = errorText(last_);
        const std::string shown = why.substr(0, why.find(" ("));
        const simd_float2 m = mouse_.value_or(simd_make_float2(float(width) / 2, float(height) / 2));
        UiQuad q;
        const float boxW = 8 * float(shown.size()) + 12;
        q.x0 = std::max(4.0f, std::min(m.x + 14, float(width) - boxW - 4)); // never past the window's edge
        q.y0 = std::min(m.y + 14, float(height) - 26);
        q.x1 = q.x0 + boxW, q.y1 = q.y0 + 22;
        q.color = {178 / 255.0f, 193 / 255.0f, 1, 0.9f};
        out.quads.push_back(q);
        text(shown, q.x0, q.y0 + 3, q.x1 - q.x0, 18, "center");
    }
    // The object design dialog over everything.
    design_->layout(out, width, height);
    windows_.capture(rects); // for the tutorial (controlRect)
    return out;
}

void BuyMode::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty() || dirty_))
        screen(canvasW_, canvasH_);
}

bool BuyMode::press(simd_float2 p) {
    if (!ok_ || !active_)
        return false;
    layoutIfNeeded();
    if (design_->press(p)) {
        dirty_ = true;
        return true;
    }
    // A control the tutorial turned off takes the click and does nothing.
    if (allowPress && !allowPress(p))
        for (const Target& t : targets_)
            if (inside(t.rect, p))
                return true;
    for (const Target& t : std::vector<Target>(targets_)) {
        if (!inside(t.rect, p))
            continue;
        switch (t.action) {
        case Action::DayNight:
            if (onDayNight)
                onDayNight();
            break;
        case Action::Tab:
            sort_ = t.value == 0 ? Sort::Function : Sort::Room;
            function_ = room_ = -1, subSort_ = roomFunction_ = 0, page_ = 0;
            break;
        case Action::Function: function_ = t.value, subSort_ = 0, page_ = 0; break;
        case Action::SubSort: subSort_ = t.value, page_ = 0; break;
        case Action::Room: room_ = t.value, roomFunction_ = 0, page_ = 0; break;
        case Action::RoomFunction:
            roomFunction_ = roomFunction_ == t.value ? 0 : t.value, page_ = 0;
            if (items().empty()) // AnyObjectsByRoom: nothing there, back to all
                roomFunction_ = 0;
            break;
        case Action::Back:
            if (sort_ == Sort::Function)
                function_ = -1, subSort_ = 0;
            else
                room_ = -1, roomFunction_ = 0;
            page_ = 0;
            break;
        case Action::Expand: {
            // The first item shown stays on the page.
            const size_t first = page_ * size_t(kColumns * (expanded_ ? kExpandedRows : kRows));
            expanded_ = !expanded_;
            page_ = first / size_t(kColumns * (expanded_ ? kExpandedRows : kRows));
            sound(expanded_ ? "ui_panel_expand" : "ui_panel_collapse");
            break;
        }
        case Action::Prev: page_ = page_ ? page_ - 1 : 0; break;
        case Action::Next: ++page_; break;
        case Action::Item: {
            const auto list = items();
            if (size_t(t.value) < list.size())
                select(*list[size_t(t.value)]);
            break;
        }
        case Action::Tool: setTool(Tool(t.value)); break;
        case Action::Undo: undo(); break;
        case Action::Redo: redo(); break;
        case Action::None: break;
        }
        std::printf("opensims2: buy panel: %s %d (%zu items)\n",
                    t.action == Action::Item ? "item" : t.action == Action::Function ? "function"
                    : t.action == Action::SubSort ? "subsort" : t.action == Action::Room ? "room" : "button",
                    t.value, items().size());
        std::fflush(stdout);
        dirty_ = true;
        return true;
    }
    for (const auto& r : solid_)
        if (inside(r, p))
            return true;
    return false;
}

void BuyMode::hover(simd_float2 p) {
    mouse_ = p;
    if (!active_)
        return;
    layoutIfNeeded();
    const CatalogEntry* over = nullptr;
    const auto list = items();
    for (const Target& t : targets_)
        if (t.action == Action::Item && inside(t.rect, p) && size_t(t.value) < list.size())
            over = list[size_t(t.value)];
    if (over != hovered_)
        hovered_ = over, dirty_ = true;
    if (held_ && !last_.ok)
        dirty_ = true;
}

void BuyMode::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void BuyMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || !ok_ || !active_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    if (cw != laidW_ || ch != laidH_)
        dirty_ = true;
    canvasW_ = cw, canvasH_ = ch;
    if (dirty_) {
        layer_ = (layer_ + 1) % layers_.size();
        layers_[layer_]->setScreen(screen(cw, ch));
        dirty_ = false;
    }
    layers_[layer_]->encode(enc, width, height);
}

std::optional<std::array<float, 4>> BuyMode::controlRect(uint32_t window, uint32_t control) {
    if (!active_)
        return std::nullopt;
    layoutIfNeeded();
    return windows_.find(window, control);
}

} // namespace os2
