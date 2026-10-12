#include "app/build_mode.hpp"

#include "app/design_tool.hpp"

#include "app/lot_vm.hpp"
#include "app/vm_objects.hpp"
#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/txmt.hpp"
#include "formats/ui_resources.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace os2 {

namespace {

constexpr uint32_t kPanelScript = 0x49060003, kPuckScript = 0x4905F85B, kDescriptionScript = 0x49063004;
constexpr uint32_t kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C;
// Build panel windows (0x49060003, docs/buy-build-mac.md §7).
constexpr uint32_t kSortArea = 0x000000A8, kCategories = 0xA000, kCatalogArea = 0xAD502BBF, kCollapsed = 0x1000,
                   kExpanded = 0x2000, kPrev = 0x1004, kNext = 0x1005, kInfoText = 0x4000;
constexpr uint32_t kDesignBtn = 0xAB, kDayNightBtn = 0xAA;
constexpr uint32_t kCategoriesBtn = 0xA6, kHand = 0xA5, kUndo = 0xA1, kRedo = 0xA2, kCollectionsBtn = 0xA7, kCollectionsWin = 0xA9,
                   kAutoRoof = 0xAC, kBgFunctions = 0xB1, kBgNoSubsort = 0xB2, kBgCollections = 0xB3, kBgTile = 0xB4,
                   kBgEndCap = 0xB5;
// The collapsed grid (window 0x1006 of 0x1000), as Buy mode's: 3 x 2
// cells with the collections constants' margins (inferred).
constexpr float kGridX = 52, kGridY = 19, kGridW = 176, kGridH = 100;
constexpr int kColumns = 3, kRows = 2;
constexpr float kMarginX = 2, kMarginY = 6;
// Wall tool and room tool (walls.txt "wall 1": thumbnailResource 0 and 1,
// catalog text "build" 129 strings 0/1 and 3/4 in Text/Build.package).
constexpr uint32_t kBuildThumbGroup = 0x499DB772, kWallToolThumb = 0xE9261348, kRoomToolThumb = 0xE9261347;
constexpr uint32_t kBuildText = 0x81;

// Floor and wall-covering subsort buttons 0xB4n0 / 0xB5n0 (n from 2):
// the property sets' "subsort" values.
const char* const kFloorSubsorts[] = {"stone", "brick", "wood", "carpet", "tile", "lino", "poured", "other"};
const char* const kWallSubsorts[] = {"brick", "masonry", "siding", "poured", "paint", "wallpaper", "tile", "paneling"};
// Doors and windows (build mode type 8): button 0xB3n0 -> subsort bits
// (doors 1, two-story windows 2, windows 4, arches 0x10; gates 8 are under
// Miscellaneous 0xBA60).
int doorSubsort(int n) {
    switch (n) {
    case 2: return 1;
    case 3: return 2;
    case 4: return 4;
    case 5: return 0x10;
    default: return 1 | 2 | 4 | 0x10;
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

// A flat unit square, alpha blended and unlit: the previews.
ObjectModel quadModel(const std::string& name, const char* rgb) {
    ObjectModel m;
    m.name = name;
    ObjectModel::Part part;
    part.node = part.shape = name;
    part.material = name;
    part.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    part.mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    part.mesh.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    part.mesh.triangles = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    m.parts.push_back(std::move(part));
    MaterialDefinition md;
    md.materialType = "StandardMaterial";
    md.properties = {{"stdMatDiffCoef", rgb},          {"stdMatEmissiveCoef", rgb},  {"stdMatUntexturedDiffAlpha", "0.5"},
                     {"stdMatAlphaBlendMode", "blend"}, {"stdMatLightingEnabled", "false"},
                     {"stdMatCullMode", "none"},        {"stdMatLayer", "1"}};
    m.materials[name] = md;
    return m;
}

// The unit square scaled to (sx, sy), turned by `angle` about Z and moved
// to (x, y, z); `centred` centres it across its width.
simd_float4x4 placeQuad(float x, float y, float z, float sx, float sy, float angle, bool centred) {
    const float c = std::cos(angle), s = std::sin(angle);
    simd_float4x4 m;
    m.columns[0] = simd_make_float4(c * sx, s * sx, 0, 0);
    m.columns[1] = simd_make_float4(-s * sy, c * sy, 0, 0);
    m.columns[2] = simd_make_float4(0, 0, 1, 0);
    const float oy = centred ? -0.5f * sy : 0;
    m.columns[3] = simd_make_float4(x - s * oy, y + c * oy, z, 1);
    return m;
}

simd_float4x4 hidden() {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(0, 0, -1000, 1);
    return m;
}

} // namespace

BuildMode::BuildMode(LotSimulation& simulation, VmObjectView& view, Renderer& renderer, ResourceIndex& index,
                     const std::filesystem::path& resDir, Lot& lot)
    : sim_(simulation), vm_(simulation.machine()), view_(view), renderer_(renderer), index_(index), lot_(lot),
      res_(std::make_unique<UiResources>(index, resDir)), fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")),
      patterns_(BuildCatalog::load(index)), objects_(Catalog::load(index, true)) {
    objects_.setThumbnailPackage(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
    editor_ = std::make_unique<vm::BuildEditor>(vm_, lot_, &patterns_);
    design_ = std::make_unique<DesignTool>(vm_, view_, index_, *res_, *fonts_);
    design_->say = [this](const std::string& m) { say(m); };
    design_->playSound = [this](const std::string& n) { sound(n); };
    design_->onAccepted = [this](const DesignTool::Change& c) {
        syncHistory();
        Entry e;
        e.kind = Entry::Kind::Design, e.id = c.id, e.cost = c.cost, e.before = c.before, e.after = c.after;
        undo_.push_back(std::move(e));
        redo_.clear();
        dirty_ = true;
    };
    auto load = [&](uint32_t id, UiScript& into) {
        if (auto s = res_->script(id)) {
            into = std::move(*s);
            return !into.roots.empty();
        }
        std::fprintf(stderr, "opensims2: build mode: no UI script %08X\n", id);
        return false;
    };
    ok_ = load(kPanelScript, panel_) && load(kPuckScript, puck_) && load(kDescriptionScript, description_);
    try {
        previewModels_[0] = renderer_.addObject(quadModel("os2_build_ok", "0.25,1,0.35"), index_);
        previewModels_[1] = renderer_.addObject(quadModel("os2_build_no", "1,0.15,0.1"), index_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: %s\n", e.what());
    }
}

BuildMode::~BuildMode() = default;

void BuildMode::redraw() {
    sim_.updateGeometry(lot_);
    if (onGeometryChanged)
        onGeometryChanged();
}

void BuildMode::say(const std::string& m) {
    message_ = m;
    std::printf("opensims2: build mode: %s\n", m.c_str());
    std::fflush(stdout);
    dirty_ = true;
}

void BuildMode::enter() {
    if (active_)
        return;
    active_ = true;
    savedPaused_ = vm_.globals[vm::global::Paused];
    vm_.globals[vm::global::Paused] = 1;
    level_ = vm_.world ? vm_.world->firstLevel() : 1;
    if (vm_.world && vm_.world->firstLevel() < 1)
        level_ = 1;
    if (const vm::Object* s = vm_.object(vm_.globals[vm::global::SelectedPersonId]); s && s->position.inWorld())
        level_ = s->position.level;
    say("entered (" + std::to_string(patterns_.floors().size()) + " floors, " + std::to_string(patterns_.walls().size()) +
        " wall coverings, " + std::to_string(objects_.entries().size()) + " build objects; funds " +
        money(vm::funds(vm_)) + ")");
}

void BuildMode::exit() {
    if (!active_)
        return;
    cancel();
    stopBrushLoop();
    design_->cancel();
    editor_->resetHistory([this](vm::ObjectId part) { view_.forget(part); });
    // Objects whose purchase was undone die with the history (ResetSelf).
    for (const Entry& e : redo_)
        if (e.kind == Entry::Kind::Object)
            if (const vm::Object* o = vm_.object(e.id); o && !o->position.inWorld()) {
                for (vm::ObjectId part : vm::groupOf(vm_, e.id))
                    view_.forget(part);
                vm::deleteGroup(vm_, e.id);
            }
    undo_.clear(), redo_.clear();
    editorDepth_ = 0;
    vm_.globals[vm::global::Paused] = savedPaused_;
    active_ = false;
    tool_ = Tool::None;
    update();
    say("left");
}

// --- Tools -------------------------------------------------------------------

void BuildMode::selectWallTool(bool room) {
    cancel();
    tool_ = room ? Tool::Room : Tool::Wall;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    say(room ? "room tool" : "wall tool");
}

void BuildMode::selectFence(const BuildFence& f) {
    cancel();
    tool_ = Tool::Fence;
    fence_ = &f;
    pattern_ = nullptr, object_ = nullptr;
    say("fence " + f.name + " (" + money(f.price) + " a section)");
}

void BuildMode::selectFoundationTool(int product) {
    cancel();
    tool_ = Tool::Foundation;
    deckProduct_ = product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    static const int prices[3] = {kFoundationTilePrice, kLatticeDeckTilePrice, kColumnDeckTilePrice};
    say(std::string(product ? "deck" : "foundation") + " tool (" + money(prices[std::clamp(product, 0, 2)]) +
        " a tile)");
}

void BuildMode::selectTerrainTool(vm::BuildEditor::TerrainOp op, const BuildTerrainPaint* paint) {
    cancel();
    tool_ = Tool::Terrain;
    terrainOp_ = paint ? vm::BuildEditor::TerrainOp::Paint : op;
    paint_ = paint;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr;
    static const char* const names[] = {"raise", "lower", "flatten", "smooth", "paint", "erase paint"};
    say(std::string("terrain tool: ") + names[int(terrainOp_)] + (paint ? " " + paint->name : std::string()));
}

void BuildMode::selectRoofTool(uint32_t type) {
    cancel();
    tool_ = Tool::Roof;
    roofType_ = type;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("roof tool, type " + std::to_string(type));
}

void BuildMode::selectPoolTool() {
    cancel();
    tool_ = Tool::Pool;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("pool tool (" + money(kPoolTilePrice) + " a tile)");
}

void BuildMode::selectStairsTool(const BuildStairs& product) {
    cancel();
    tool_ = Tool::Stairs;
    stairs_ = &product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say(product.name + " (" + money(product.price) + " a step)");
}

void BuildMode::selectDormerTool(const BuildRoofProduct& product) {
    cancel();
    tool_ = Tool::Dormer;
    dormer_ = &product;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say(product.name + " (" + money(product.price) + ")");
}

BuildMode::StairsDrag BuildMode::stairsDrag(simd_float2 from, simd_float2 to) const {
    StairsDrag d;
    d.top = {int(std::floor(from.x)), int(std::floor(from.y)), level_};
    const float cx = float(d.top.x) + 0.5f, cy = float(d.top.y) + 0.5f;
    const bool dragged = int(std::floor(to.x)) != d.top.x || int(std::floor(to.y)) != d.top.y;
    // Off the side of the tile the mouse is on (the floor tile's quarter,
    // cFloorTile +0xC: 1 -> -x, 2 -> +y, 4 -> +x, 8 -> -y), or the way of the drag.
    const float dx = (dragged ? to.x : from.x) - cx, dy = (dragged ? to.y : from.y) - cy;
    if (std::fabs(dx) >= std::fabs(dy))
        d.direction = dx < 0 ? 0 : 4;
    else
        d.direction = dy > 0 ? 2 : 6;
    if (dragged)
        d.end = std::array<float, 2>{to.x, to.y};
    return d;
}

void BuildMode::selectPattern(const BuildPattern& p) {
    cancel();
    tool_ = p.floor ? Tool::Floor : Tool::Wallpaper;
    pattern_ = &p;
    object_ = nullptr, fence_ = nullptr;
    say(std::string(p.floor ? "floor " : "wall covering ") + p.name + " (" + money(p.price) + ")");
}

bool BuildMode::selectObject(const CatalogEntry& e) {
    cancel();
    try {
        held_ = vm::createGhost(vm_, e.group, e.instance);
    } catch (const std::exception& ex) {
        say(e.name + ": " + ex.what());
        held_ = 0;
        return false;
    }
    tool_ = Tool::Object;
    object_ = &e;
    pattern_ = nullptr, fence_ = nullptr;
    last_ = {};
    say("holding " + e.name + " (" + money(e.price) + ")");
    return true;
}

void BuildMode::cancel() {
    if (held_) {
        for (vm::ObjectId part : vm::groupOf(vm_, held_))
            view_.forget(part);
        vm::deleteGroup(vm_, held_);
        held_ = 0;
    }
    last_ = {};
    lastOk_ = false;
    dragFrom_.reset();
    if (design_)
        design_->cancel();
    dirty_ = true;
}

void BuildMode::holdObjectAt(int x, int y, int direction) {
    direction_ = direction;
    if (!held_)
        return;
    last_ = vm::checkPlacement(vm_, held_, {x, y, level_}, direction_);
    // Turning green: ui_build_door_green / ui_build_stair_green.
    if (last_.ok && !lastOk_) {
        const std::string plop = objectSound(held_);
        if (plop == "ui_build_door_plop")
            sound("ui_build_door_green");
        else if (plop == "ui_build_stair_plop")
            sound("ui_build_stair_green");
    }
    lastOk_ = last_.ok;
    // Shown where it would go, whether it fits or not (as Buy mode does).
    for (const auto& part : last_.parts)
        if (vm::Object* o = vm_.object(part.id)) {
            o->data[vm::objdata::Direction] = int16_t(direction_);
            o->position = {float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level};
        }
    dirty_ = true;
}

bool BuildMode::placeObjectAt(int x, int y, int direction) {
    if (!held_ || !object_)
        return false;
    holdObjectAt(x, y, direction);
    if (!last_.ok) {
        std::string text = last_.reason;
        if (last_.error >= 2)
            if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
                if (std::string g = t->get(size_t(vm::placementerror::textIndex(last_.error))); !g.empty())
                    text = g + " (" + last_.reason + ")";
        say("cannot: " + text);
        sound("ui_error");
        return false;
    }
    const vm::ObjectId id = held_;
    std::string why;
    if (!vm::buy(vm_, id, last_, object_->price, &why)) {
        say("not bought: " + why);
        sound("ui_error");
        return false;
    }
    sound(objectSound(id));
    // A staircase opens the floor above its steps (before its own history
    // entry, so undo takes the stairs away first).
    if (objectSound(id) == "ui_build_stair_plop")
        if (const auto opened = editor_->openStairwell(id); opened.changed)
            say(opened.message);
    syncHistory();
    Entry entry;
    entry.kind = Entry::Kind::Object, entry.id = id, entry.cost = object_->price;
    held_ = 0;
    // A door or window cuts its hole in the wall it stands in (the lot
    // model finds the holes from the lot's cObjects, lot_model.cpp).
    if (const vm::Object* o = vm_.object(id)) {
        Lot::Object c;
        c.instance = uint32_t(id);
        c.definition = object_->fileName + "_objt";
        c.model = object_->model;
        if (c.model.size() > 5 && c.model.ends_with("_cres"))
            c.model.resize(c.model.size() - 5);
        c.position = {o->position.x, o->position.y, sim_.floorHeight(o->position.level, o->position.x, o->position.y)};
        // The model faces 90 - 45 x direction degrees about Z (world.hpp).
        const float yaw = (90.0f - 45.0f * float(o->data[vm::objdata::Direction])) * float(M_PI) / 180.0f;
        c.rotation = {0, 0, std::sin(yaw / 2), std::cos(yaw / 2)};
        lot_.objects.push_back(c);
        entry.cobject = c;
    }
    undo_.push_back(std::move(entry));
    redo_.clear();
    syncAfterObject();
    say("bought " + object_->name + " as object " + std::to_string(id) + " for " + money(object_->price) + "; funds " +
        money(vm::funds(vm_)));
    selectObject(*object_); // another of the same (doors and windows keep buying)
    return true;
}

void BuildMode::syncAfterObject() {
    editor_->syncWorld();
    redraw();
}

void BuildMode::rotate(int steps) {
    if (tool_ == Tool::Floor) {
        // The toolbar's rotation mode: 1 -> 2 -> 4 -> 8 (key 0xACB3A629).
        int k = 0;
        while ((1 << k) < floorRotation_ && k < 3)
            ++k;
        k = ((k + (steps > 0 ? 1 : -1)) % 4 + 4) % 4;
        floorRotation_ = 1 << k;
        say("floor rotation " + std::to_string(90 * k) + " degrees");
        return;
    }
    direction_ = ((direction_ + steps) % 8 + 8) % 8;
    if (held_ && !last_.parts.empty())
        holdObjectAt(last_.parts.front().tile.x, last_.parts.front().tile.y, direction_);
}

void BuildMode::syncHistory() {
    const size_t depth = editor_->undoDepth();
    if (depth > editorDepth_) {
        for (size_t k = editorDepth_; k < depth; ++k)
            undo_.push_back({});
        redo_.clear();
    }
    editorDepth_ = depth;
}

bool BuildMode::undo() {
    cancel();
    design_->cancel();
    syncHistory();
    bool ok = false;
    if (!undo_.empty()) {
        Entry e = std::move(undo_.back());
        undo_.pop_back();
        switch (e.kind) {
        case Entry::Kind::Editor:
            ok = editor_->undo();
            editorDepth_ = editor_->undoDepth();
            break;
        case Entry::Kind::Object: // out of the world, the price back (as Buy mode's)
            if (vm_.object(e.id)) {
                e.placement = vm::currentPlacement(vm_, e.id);
                vm::pickUp(vm_, e.id);
                vm::addFunds(vm_, e.cost);
                if (e.cobject)
                    std::erase_if(lot_.objects, [&](const Lot::Object& c) { return c.instance == e.cobject->instance; });
                syncAfterObject();
                ok = true;
            }
            break;
        case Entry::Kind::Design:
            DesignTool::apply(vm_, view_, e.id, e.before);
            vm::addFunds(vm_, e.cost);
            ok = true;
            break;
        }
        if (ok)
            redo_.push_back(std::move(e));
    }
    if (ok)
        redraw();
    sound(ok ? "ui_undo" : "ui_error");
    say(ok ? "undone; funds " + money(vm::funds(vm_)) : "nothing to undo");
    return ok;
}

bool BuildMode::redo() {
    cancel();
    design_->cancel();
    bool ok = false;
    if (!redo_.empty()) {
        Entry e = std::move(redo_.back());
        redo_.pop_back();
        switch (e.kind) {
        case Entry::Kind::Editor:
            ok = editor_->redo();
            editorDepth_ = editor_->undoDepth();
            break;
        case Entry::Kind::Object:
            if (vm_.object(e.id) && vm::funds(vm_) >= e.cost) {
                vm::applyPlacement(vm_, e.id, e.placement);
                vm::addFunds(vm_, -e.cost);
                if (e.cobject)
                    lot_.objects.push_back(*e.cobject);
                syncAfterObject();
                ok = true;
            }
            break;
        case Entry::Kind::Design:
            if (vm::funds(vm_) >= e.cost) {
                DesignTool::apply(vm_, view_, e.id, e.after);
                vm::addFunds(vm_, -e.cost);
                ok = true;
            }
            break;
        }
        if (ok)
            undo_.push_back(std::move(e));
        else
            redo_.push_back(std::move(e));
    }
    if (ok)
        redraw();
    sound(ok ? "ui_redo" : "ui_error");
    say(ok ? "redone; funds " + money(vm::funds(vm_)) : "nothing to redo");
    return ok;
}

vm::BuildEditor::Result BuildMode::autoRoof(int tool) {
    pressSound(Tool::Roof, false);
    const auto r = tool == 0 ? editor_->removeRoofs() : editor_->autoRoof(tool, preserveRoofs_);
    releaseSound(Tool::Roof, false, r);
    applied(r);
    return r;
}

void BuildMode::selectDesignTool() {
    cancel();
    design_->cancel();
    tool_ = Tool::Design;
    pattern_ = nullptr, object_ = nullptr, fence_ = nullptr, paint_ = nullptr;
    say("design tool");
}

bool BuildMode::design(vm::ObjectId id, simd_float2 at) {
    const vm::Object* o = vm_.object(id);
    if (!o || o->person || !o->definition)
        return false;
    const vm::Object* lead = vm_.object(vm::multiTileGroup(*o));
    const CatalogEntry* entry = lead ? objects_.byGuid(lead->guid()) : nullptr;
    dirty_ = true;
    return design_->open(id, at, entry ? entry->name : o->definition->fileName);
}

void BuildMode::applied(const vm::BuildEditor::Result& r) {
    // A refusal shows the game's text for its code (UIText STR# 0x89).
    std::string text = r.message;
    if (!r.ok && r.error >= 2)
        if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
            if (std::string g = t->get(size_t(vm::placementerror::textIndex(r.error))); !g.empty())
                text = g + " (" + r.message + ")";
    say((r.ok ? "" : "cannot: ") + text + "; funds " + money(vm::funds(vm_)));
    syncHistory();
    if (r.ok && r.changed)
        redraw();
}

// --- The lot -------------------------------------------------------------------

std::optional<simd_float2> BuildMode::groundPoint(simd_float3 eye, simd_float3 dir) const {
    if (!vm_.world || std::fabs(dir.z) < 1e-5f)
        return std::nullopt;
    float z = sim_.floorHeight(level_, float(vm_.world->width()) / 2, float(vm_.world->height()) / 2);
    simd_float3 hit{};
    for (int i = 0; i < 3; ++i) {
        const float t = (z - eye.z) / dir.z;
        if (t <= 0)
            return std::nullopt;
        hit = eye + dir * t;
        z = sim_.floorHeight(level_, hit.x, hit.y);
    }
    return simd_make_float2(hit.x, hit.y);
}

std::optional<std::pair<std::array<int, 4>, int>> BuildMode::faceAt(simd_float2 p) const {
    const Lot::Wall* best = nullptr;
    float bestD = 0.6f;
    for (const auto& w : lot_.walls) {
        if (w.a.level != level_ - 1 || Lot::isFence(w.type))
            continue;
        const simd_float2 a = simd_make_float2(w.a.x, w.a.y), b = simd_make_float2(w.b.x, w.b.y);
        const simd_float2 ab = b - a;
        const float t = std::clamp(simd_dot(p - a, ab) / simd_dot(ab, ab), 0.0f, 1.0f);
        const float d = simd_length(p - (a + ab * t));
        if (d < bestD)
            bestD = d, best = &w;
    }
    if (!best)
        return std::nullopt;
    const float cross = (best->b.x - best->a.x) * (p.y - best->a.y) - (best->b.y - best->a.y) * (p.x - best->a.x);
    return std::pair{std::array<int, 4>{int(std::lround(best->a.x)), int(std::lround(best->a.y)),
                                        int(std::lround(best->b.x)), int(std::lround(best->b.y))},
                     cross < 0 ? 1 : -1};
}

// The end of a wall drag from `from`: along the nearest of the eight
// directions, as long as the longer axis (cTSUserToolWall, inferred).
static std::array<int, 4> snapRun(simd_float2 from, simd_float2 to) {
    const int x0 = int(std::lround(from.x)), y0 = int(std::lround(from.y));
    const int dx = int(std::lround(to.x)) - x0, dy = int(std::lround(to.y)) - y0;
    if (!dx && !dy)
        return {x0, y0, x0, y0};
    const int k = (int(std::lround(std::atan2(float(dy), float(dx)) / float(M_PI / 4))) + 8) % 8;
    static const int steps[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const int n = std::max(std::abs(dx), std::abs(dy));
    return {x0, y0, x0 + steps[k][0] * n, y0 + steps[k][1] * n};
}

void BuildMode::pointAt(simd_float3 eye, simd_float3 dir) {
    if (!active_)
        return;
    pointer_ = groundPoint(eye, dir);
    if (tool_ == Tool::Object && held_ && pointer_)
        holdObjectAt(int(std::floor(pointer_->x)), int(std::floor(pointer_->y)), direction_);
    dirty_ = true;
}

bool BuildMode::mouseDown(simd_float3 eye, simd_float3 dir, bool control, bool shift) {
    if (!active_ || tool_ == Tool::None)
        return false;
    control_ = control, shift_ = shift;
    const auto p = groundPoint(eye, dir);
    if (!p)
        return false;
    pointer_ = p;
    if (tool_ == Tool::Object) {
        if (held_ && object_)
            placeObjectAt(int(std::floor(p->x)), int(std::floor(p->y)), direction_);
        return true;
    }
    if (tool_ == Tool::Design) {
        if (design_->isOpen()) // a click off the dialog closes it
            design_->cancel(), dirty_ = true;
        else if (const vm::ObjectId id = pickObject ? pickObject(eye, dir) : 0)
            design(id, mouse_.value_or(simd_make_float2(float(canvasW_) / 2, float(canvasH_) / 2)));
        return true;
    }
    if (tool_ == Tool::Dormer && dormer_) {
        pressSound(Tool::Roof, false);
        const auto r = editor_->placeDormer(dormer_->id, level_, int(std::lround(p->x)), int(std::lround(p->y)),
                                            (direction_ / 2) * 2 % 8);
        releaseSound(Tool::Roof, false, r);
        applied(r);
        return true;
    }
    if (tool_ == Tool::Wallpaper) {
        if (auto f = faceAt(*p); f && pattern_) {
            const auto& [s, side] = *f;
            const auto r = shift ? editor_->paintFlood(level_, s[0], s[1], s[2], s[3], side, control ? nullptr : pattern_)
                                 : editor_->paintWalls(level_, s[0], s[1], s[2], s[3], side, control ? nullptr : pattern_);
            releaseSound(tool_, control, r);
            applied(r);
        }
        return true;
    }
    if (tool_ == Tool::Terrain) {
        editor_->beginTerrainStroke(control ? vm::BuildEditor::TerrainOp::Erase : terrainOp_, p->x, p->y);
        stroking_ = true;
        lastBrush_ = 0;
        pressSound(tool_, control);
        brushTick(true);
        return true;
    }
    dragFrom_ = p;
    dragSections_ = 0;
    pressSound(tool_, control);
    return true;
}

void BuildMode::pressSound(Tool tool, bool erase) {
    switch (tool) {
    case Tool::Wall:
    case Tool::Room:
    case Tool::Fence: sound(erase ? "ui_build_walldelete_mdown" : "ui_build_wall_mdown"); break;
    case Tool::Foundation: sound("ui_build_deck_mdown"); break;
    case Tool::Pool: sound("ui_build_pool_mdown"); break;
    case Tool::Roof: sound("ui_build_roof_mdown"); break;
    case Tool::Terrain: {
        // Levelling has its own press; the brush loops while the mouse is down.
        const bool paint = paint_ != nullptr || erase;
        if (!paint && terrainOp_ == vm::BuildEditor::TerrainOp::Flatten)
            sound("ui_build_terrain_level_mdown");
        stopBrushLoop();
        if (startLoop)
            brushLoop_ = startLoop(paint ? "ui_build_terrain_paint_lp" : "ui_build_terrain_lp");
        break;
    }
    default: break;
    }
}

void BuildMode::releaseSound(Tool tool, bool erase, const vm::BuildEditor::Result& r) {
    if (!r.ok) {
        sound("ui_error");
        return;
    }
    switch (tool) {
    case Tool::Wall:
    case Tool::Room:
    case Tool::Fence:
        if (!erase)
            sound("ui_build_wall_mup");
        break;
    case Tool::Foundation: sound("ui_build_deck_mup"); break;
    case Tool::Pool: sound("ui_build_pool_mup"); break;
    case Tool::Roof: sound("ui_build_roof_mup"); break;
    case Tool::Stairs:
        if (!erase)
            sound("ui_build_stair_plop");
        break;
    case Tool::Floor:
        if (r.changed)
            sound("ui_build_flooring_plop");
        break;
    case Tool::Wallpaper:
        if (r.changed)
            sound("ui_build_wallcovering_plop");
        break;
    case Tool::Terrain:
        if (!erase && !paint_ && terrainOp_ == vm::BuildEditor::TerrainOp::Flatten)
            sound("ui_build_terrain_level_mup");
        break;
    default: break;
    }
}

void BuildMode::stopBrushLoop() {
    if (brushLoop_ && stopLoop)
        stopLoop(brushLoop_);
    brushLoop_ = 0;
}

std::string BuildMode::objectSound(vm::ObjectId id) const {
    const vm::Object* o = vm_.object(id);
    if (!o || !o->definition)
        return "ui_object_plop";
    const auto& d = *o->definition;
    const int type = d.field(objd::ObjectType);
    if (type == 8 || type == 9)
        return "ui_build_door_plop";
    if (d.fileName.rfind("Stairs - ", 0) == 0)
        return "ui_build_stair_plop";
    if ((d.field(objd::FunctionSort) & 0x20) && (d.field(objd::FunctionSubSort) & 8))
        return "ui_build_garden_plop";
    return "ui_object_plop";
}

// The brush while the mouse is down, every 100 ms (ApplyTerrainSculptBrush);
// the lot is redrawn at most every 0.3 s while stroking.
void BuildMode::brushTick(bool force) {
    if (!stroking_ || !pointer_)
        return;
    const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count()) /
                       1000.0;
    if (!force && now - lastBrush_ < 0.1)
        return;
    lastBrush_ = now;
    const auto& profile = terrainOp_ == vm::BuildEditor::TerrainOp::Lower ? patterns_.lowerProfile() : patterns_.raiseProfile();
    const auto r = editor_->applyTerrainBrush(pointer_->x, pointer_->y, brushSize_, profile, paint_ ? paint_->texture : "");
    if (!r.ok && r.error)
        applied(r);
    if (r.changed && now - lastRedraw_ > 0.3) {
        lastRedraw_ = now;
        redraw();
    }
}

void BuildMode::mouseDrag(simd_float3 eye, simd_float3 dir) {
    if (!active_)
        return;
    if (auto p = groundPoint(eye, dir))
        pointer_ = p;
    dirty_ = true;
}

bool BuildMode::mouseUp(simd_float3 eye, simd_float3 dir) {
    if (active_ && stroking_) {
        stroking_ = false;
        stopBrushLoop();
        const auto r = editor_->endTerrainStroke();
        releaseSound(Tool::Terrain, control_, r);
        applied(r);
        return true;
    }
    if (!active_ || !dragFrom_)
        return false;
    if (auto p = groundPoint(eye, dir))
        pointer_ = p;
    const simd_float2 a = *dragFrom_, b = pointer_.value_or(a);
    dragFrom_.reset();
    auto done = [&](const vm::BuildEditor::Result& r) {
        releaseSound(tool_, control_, r);
        applied(r);
    };
    if (tool_ == Tool::Fence && fence_) {
        const auto r = snapRun(a, b);
        if (r[0] == r[2] && r[1] == r[3])
            return true;
        done(control_ ? editor_->deleteFences(level_, r[0], r[1], r[2], r[3])
                         : editor_->drawFences(level_, r[0], r[1], r[2], r[3], *fence_));
    } else if (tool_ == Tool::Wall || tool_ == Tool::Room) {
        if (tool_ == Tool::Room || shift_) {
            const int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y)), x1 = int(std::lround(b.x)),
                      y1 = int(std::lround(b.y));
            if (control_)
                for (const auto& s : std::array<std::array<int, 4>, 4>{
                         {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
                    done(editor_->deleteWalls(level_, s[0], s[1], s[2], s[3]));
            else
                done(editor_->drawRoom(level_, x0, y0, x1, y1));
        } else {
            const auto r = snapRun(a, b);
            if (r[0] == r[2] && r[1] == r[3])
                return true;
            done(control_ ? editor_->deleteWalls(level_, r[0], r[1], r[2], r[3])
                             : editor_->drawWalls(level_, r[0], r[1], r[2], r[3]));
        }
    } else if (tool_ == Tool::Stairs && stairs_) {
        if (control_) {
            // The sledgehammer: the modular stair part nearest the mouse.
            vm::ObjectId best = 0;
            float bestD = 0.75f;
            for (const auto& [id, o] : vm_.objects())
                if (o.position.inWorld() && o.definition && o.definition->fileName.rfind("Stair Modular - ", 0) == 0 &&
                    (o.position.level == level_ || o.position.level == level_ - 1))
                    if (const float d = std::hypot(o.position.x - b.x, o.position.y - b.y); d < bestD)
                        bestD = d, best = id;
            done(best ? editor_->deleteStairs(best) : vm::BuildEditor::Result{false, 0, 0, 0, "no stairs there"});
        } else {
            const StairsDrag d = stairsDrag(a, b);
            done(editor_->placeStairs(*stairs_, d.top, d.direction, d.end));
        }
    } else if (tool_ == Tool::Roof) {
        const int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y)), x1 = int(std::lround(b.x)), y1 = int(std::lround(b.y));
        done(control_ ? editor_->deleteRoof(level_, b.x, b.y) : editor_->drawRoof(level_, x0, y0, x1, y1, roofType_));
    } else if (tool_ == Tool::Pool) {
        const int x0 = int(std::floor(a.x)), y0 = int(std::floor(a.y)), x1 = int(std::floor(b.x)), y1 = int(std::floor(b.y));
        done(control_ ? editor_->deletePool(x1, y1) : editor_->drawPool(x0, y0, x1, y1));
    } else if (tool_ == Tool::Foundation) {
        const int x0 = int(std::floor(a.x)), y0 = int(std::floor(a.y)), x1 = int(std::floor(b.x)), y1 = int(std::floor(b.y));
        done(control_ ? editor_->deleteFoundation(x0, y0, x1, y1)
                      : editor_->drawFoundation(x0, y0, x1, y1, deckProduct_));
    } else if (tool_ == Tool::Floor) {
        // A click without a drag floors the quarters under the mouse
        // (FUN_003000cc: the half on its side of a diagonal wall).
        const bool click = int(std::floor(a.x)) == int(std::floor(b.x)) && int(std::floor(a.y)) == int(std::floor(b.y));
        const int quarters = click ? editor_->clickQuarters(level_, b.x, b.y) : 0xF;
        done(editor_->paintFloor(level_, int(std::floor(a.x)), int(std::floor(a.y)), int(std::floor(b.x)),
                                    int(std::floor(b.y)), control_ ? nullptr : pattern_, quarters, floorRotation_));
    }
    return true;
}

void BuildMode::update() {
    brushTick(false);
    size_t used = 0;
    auto put = [&](bool ok, const simd_float4x4& m) {
        const int model = previewModels_[ok ? 0 : 1];
        if (model < 0)
            return;
        if (used == previewInstances_.size())
            previewInstances_.push_back(renderer_.addInstance(model, m));
        else
            renderer_.setInstanceModel(previewInstances_[used], model), renderer_.setInstanceTransform(previewInstances_[used], m);
        ++used;
    };
    if (active_ && pointer_) {
        const simd_float2 p = *pointer_;
        auto z = [&](float x, float y) { return sim_.floorHeight(level_, x, y) + 0.03f; };
        if (tool_ == Tool::Wall || tool_ == Tool::Room || tool_ == Tool::Fence || tool_ == Tool::Roof) {
            std::vector<std::array<int, 4>> runs;
            if (dragFrom_) {
                const simd_float2 a = *dragFrom_;
                if (tool_ == Tool::Room || tool_ == Tool::Roof || (shift_ && tool_ == Tool::Wall)) {
                    const int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y)), x1 = int(std::lround(p.x)),
                              y1 = int(std::lround(p.y));
                    runs = {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}};
                } else {
                    runs = {snapRun(a, p)};
                }
            }
            // A section more under the drag: its sound (ui_build_wall_section,
            // or walldelete_ with Ctrl; fences and rooms the same, inferred).
            size_t sections = 0;
            for (const auto& run : runs)
                sections += vm::wallSections(run[0], run[1], run[2], run[3]).size();
            if (dragFrom_ && tool_ != Tool::Roof && sections > dragSections_)
                sound(control_ ? "ui_build_walldelete_section" : "ui_build_wall_section");
            if (dragFrom_)
                dragSections_ = sections;
            for (const auto& run : runs)
                for (const auto& s : vm::wallSections(run[0], run[1], run[2], run[3])) {
                    const bool ok = control_ || editor_->wallBlocked(level_, s[0], s[1], s[2], s[3], nullptr,
                                                                     tool_ == Tool::Fence, true)
                                                    .empty();
                    const float len = std::hypot(float(s[2] - s[0]), float(s[3] - s[1]));
                    put(ok, placeQuad(float(s[0]), float(s[1]), z(float(s[0]), float(s[1])), len, 0.18f,
                                      std::atan2(float(s[3] - s[1]), float(s[2] - s[0])), true));
                }
            // The grid point under the mouse.
            put(true, placeQuad(std::round(p.x) - 0.12f, std::round(p.y) - 0.12f, z(p.x, p.y), 0.24f, 0.24f, 0, false));
        } else if (tool_ == Tool::Floor || tool_ == Tool::Foundation || tool_ == Tool::Pool) {
            const simd_float2 a = dragFrom_.value_or(p);
            const int x0 = int(std::floor(std::min(a.x, p.x))), x1 = int(std::floor(std::max(a.x, p.x)));
            const int y0 = int(std::floor(std::min(a.y, p.y))), y1 = int(std::floor(std::max(a.y, p.y)));
            // A foundation growing under the drag: ui_build_deck_section.
            const size_t tiles = size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1);
            if (dragFrom_ && tool_ == Tool::Foundation && tiles > dragSections_)
                sound("ui_build_deck_section");
            if (dragFrom_)
                dragSections_ = tiles;
            for (int x = x0; x <= x1 && used < 400; ++x)
                for (int y = y0; y <= y1 && used < 400; ++y)
                    put(!control_, placeQuad(float(x) + 0.04f, float(y) + 0.04f, z(float(x) + 0.5f, float(y) + 0.5f), 0.92f,
                                             0.92f, 0, false));
        } else if (tool_ == Tool::Wallpaper) {
            if (auto f = faceAt(p)) {
                const auto& [s, side] = *f;
                // A new face under the mouse shows the covering on it
                // (cTSUserToolWallpaper::Update): ui_build_wallcovering_appear.
                const std::array<int, 5> face{s[0], s[1], s[2], s[3], side};
                if (face != hoverFace_ && pattern_)
                    sound("ui_build_wallcovering_appear");
                hoverFace_ = face;
                const float ang = std::atan2(float(s[3] - s[1]), float(s[2] - s[0]));
                const float len = std::hypot(float(s[2] - s[0]), float(s[3] - s[1]));
                // A strip on the face's side (right of a -> b is -90°).
                const float off = side > 0 ? -0.3f : 0.0f;
                const float c = std::cos(ang), sn = std::sin(ang);
                put(true, placeQuad(float(s[0]) - sn * off, float(s[1]) + c * off, z(float(s[0]), float(s[1])), len, 0.3f,
                                    ang, false));
            }
        } else if (tool_ == Tool::Terrain) {
            // The brush's square (the profile is round inside it).
            const float w = 2.0f + 4.0f * float(std::clamp(brushSize_, 0, 2));
            put(true, placeQuad(p.x - w / 2, p.y - w / 2, z(p.x, p.y), w, w, 0, false));
        } else if (tool_ == Tool::Stairs && stairs_ && !control_) {
            // The top tile and the steps the flight would have (green, or
            // red when refused), with its cost (CalculatePreviewCost).
            const StairsDrag d = stairsDrag(dragFrom_.value_or(p), p);
            int error = 0;
            const int n = editor_->stairsSteps(d.top, d.direction, d.end, false, &error);
            put(n > 0, placeQuad(float(d.top.x) + 0.04f, float(d.top.y) + 0.04f,
                                 z(float(d.top.x) + 0.5f, float(d.top.y) + 0.5f), 0.92f, 0.92f, 0, false));
            const auto [sx, sy] = vm::directionStep(d.direction);
            const float ex = float(d.top.x) + 0.5f + float(sx) * 0.5f, ey = float(d.top.y) + 0.5f + float(sy) * 0.5f;
            const float topZ = sim_.floorHeight(level_, ex, ey);
            for (int i = 0; i < std::max(n, 1) && used < 400; ++i) {
                const float px = ex + float(sx) * 0.25f * float(i) + (sx < 0 ? -0.25f : 0.0f) + (sx == 0 ? -0.46f : 0.0f);
                const float py = ey + float(sy) * 0.25f * float(i) + (sy < 0 ? -0.25f : 0.0f) + (sy == 0 ? -0.46f : 0.0f);
                put(n > 0, placeQuad(px, py, topZ - 0.1875f * float(i + 1) + 0.03f, sx ? 0.25f : 0.92f, sy ? 0.25f : 0.92f, 0,
                                     false));
            }
            if (n > 0 && dragFrom_ && stairs_)
                message_ = std::to_string(n) + " steps: " + money(n * stairs_->price);
        } else if (tool_ == Tool::Object && held_) {
            for (const auto& part : last_.parts)
                put(last_.ok, placeQuad(float(part.tile.x) + 0.04f, float(part.tile.y) + 0.04f,
                                        z(float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f), 0.92f, 0.92f, 0, false));
        }
    }
    for (size_t i = used; i < previewInstances_.size(); ++i)
        renderer_.setInstanceTransform(previewInstances_[i], hidden());
}

// --- The panel -----------------------------------------------------------------

const RgbaImage* BuildMode::patternThumbnail(const BuildPattern& p) { return materialThumbnail(p.guid, p.material); }

// Fences have no catalog thumbnail in the game's cache: their rail's
// material texture stands in for it (as patterns show their texture).
const RgbaImage* BuildMode::fenceThumbnail(const BuildFence& f) {
    if (auto it = thumbs_.find(f.guid); it != thumbs_.end())
        return it->second.get();
    std::string material;
    try {
        const ObjectModel m = loadObjectModel(index_, f.rail);
        for (const auto& part : m.parts)
            if (!part.material.empty()) {
                material = part.material;
                break;
            }
    } catch (const std::exception&) {
    }
    if (material.empty())
        return thumbs_.emplace(f.guid, nullptr).first->second.get();
    return materialThumbnail(f.guid, material);
}

const RgbaImage* BuildMode::materialThumbnail(uint32_t key, const std::string& materialName) {
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    BuildPattern p;
    p.guid = key, p.material = materialName;
    std::shared_ptr<RgbaImage> img;
    // The material's base texture (the game renders its catalog swatches
    // from the material: wallthumbnailmat; here the texture itself).
    try {
        if (auto loc = index_.findByName(p.material + "_txmt", kTypeTxmt)) {
            const auto bytes = index_.read(*loc);
            Rcol rc(bytes);
            if (const auto* block = rc.find("cMaterialDefinition")) {
                const MaterialDefinition md = MaterialDefinition::parse(block->body);
                std::string tex = md.property("stdMatBaseTextureName");
                if (tex.empty() && !md.textureNames.empty())
                    tex = md.textureNames.front();
                if (!tex.empty())
                    if (auto t = index_.findByName(tex + "_txtr", kTypeTxtr)) {
                        // A level about the cell's size (the largest may be a LIFO).
                        const TextureLevels lv = loadTextureLevels(index_.read(*t), &index_);
                        size_t pick = 0;
                        for (size_t l = 0; l < lv.levels.size(); ++l)
                            if (lv.levels[l].width >= 64)
                                pick = l;
                        if (pick < lv.levels.size()) {
                            const auto& L = lv.levels[pick];
                            img = std::make_shared<RgbaImage>(decodeTexture(lv.format, L.width, L.height, L.data));
                        }
                    }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: thumbnail of %s: %s\n", p.material.c_str(), e.what());
    }
    thumbs_[p.guid] = std::move(img);
    return thumbs_[p.guid].get();
}

// A texture by name (a terrain paint's swatch).
const RgbaImage* BuildMode::textureThumbnail(uint32_t key, const std::string& texture) {
    if (auto it = thumbs_.find(key); it != thumbs_.end())
        return it->second.get();
    std::shared_ptr<RgbaImage> img;
    try {
        if (auto t = index_.findByName(texture + "_txtr", kTypeTxtr)) {
            const TextureLevels lv = loadTextureLevels(index_.read(*t), &index_);
            size_t pick = 0;
            for (size_t l = 0; l < lv.levels.size(); ++l)
                if (lv.levels[l].width >= 64)
                    pick = l;
            if (pick < lv.levels.size()) {
                const auto& L = lv.levels[pick];
                img = std::make_shared<RgbaImage>(decodeTexture(lv.format, L.width, L.height, L.data));
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: build mode: thumbnail of %s: %s\n", texture.c_str(), e.what());
    }
    thumbs_[key] = std::move(img);
    return thumbs_[key].get();
}

const RgbaImage* BuildMode::stairsThumbnail(const BuildStairs& b) {
    for (const uint32_t guid : {b.objects[2], b.objects[3]}) {
        const auto def = vm_.code().definitionByGuid(guid);
        if (!def)
            continue;
        CatalogEntry e;
        e.guid = guid;
        e.group = def->first;
        e.model = objectCresName(index_, Tgi{vm::kTypeObjd, def->first, def->second, 0});
        if (e.model.ends_with("_cres"))
            e.model.resize(e.model.size() - 5);
        if (const RgbaImage* img = objects_.thumbnail(e))
            return img;
    }
    return nullptr;
}

std::vector<BuildMode::Item> BuildMode::items() {
    std::vector<Item> out;
    auto patternItems = [&](bool floor, const char* const* names, size_t count) {
        const std::string sub = subsort_ >= 2 && size_t(subsort_ - 2) < count ? names[subsort_ - 2] : "";
        for (const BuildPattern* p : patterns_.bySubsort(floor, sub)) {
            Item it;
            it.tool = floor ? Tool::Floor : Tool::Wallpaper;
            it.pattern = p;
            it.name = p->name, it.description = p->description, it.price = p->price;
            out.push_back(std::move(it));
        }
    };
    auto objectItems = [&](int type, int bits) {
        for (const auto& e : objects_.entries())
            if (e.buildModeType == type && (e.buildModeSubsort & bits)) {
                Item it;
                it.tool = Tool::Object;
                it.object = &e;
                it.name = e.name, it.description = e.description, it.price = e.price;
                out.push_back(std::move(it));
            }
    };
    switch (category_) {
    case 1: {
        // Text/Build.package STR# 129 (catalogTextResource build 129): for
        // catalog index i, string 3i the price, 3i+1 the name, 3i+2 the text.
        const StringTable* t = res_->table(packageGroupHash("Build"), kBuildText);
        for (int k = 0; k < 2; ++k) {
            Item it;
            it.tool = k ? Tool::Room : Tool::Wall;
            it.price = kWallSectionPrice;
            it.image = res_->image({kTypeUiImage, kBuildThumbGroup, k ? kRoomToolThumb : kWallToolThumb});
            if (t) {
                it.price = std::atoi(t->get(size_t(3 * k), lang::EnglishUS).c_str());
                it.name = t->get(size_t(3 * k + 1));
                it.description = t->get(size_t(3 * k + 2));
            }
            if (!it.price)
                it.price = kWallSectionPrice;
            if (it.name.empty())
                it.name = k ? "Room Tool" : "Wall Tool";
            out.push_back(std::move(it));
        }
        break;
    }
    case 2: {
        // B200 foundations: catalog text "build" 138 (deckfoundations.txt
        // "foundation 0": catalogTextIndices 0 1), price 3i, name 3i+1, text 3i+2.
        // The decks: "deck 1" (catalogTextIndices 2 3, lattice) and "deck 2"
        // (4 5, columns): strings 3 x index.
        struct Kind {
            int text, price;
            const char* name;
            uint32_t key;
            const char* material;
        };
        const Kind kinds[3] = {{0, kFoundationTilePrice, "Foundation", 0xF0F0F0F0u, "floor_plywoodFoundation"},
                               {2, kLatticeDeckTilePrice, "Deck", 0xF0F0F0F1u, "wall_deckRedwoodAlpha_base"},
                               {4, kColumnDeckTilePrice, "Deck", 0xF0F0F0F2u, "floor_deckDarkRedwood"}};
        for (int k = 0; k < 3; ++k) {
            Item it;
            it.tool = Tool::Foundation;
            it.product = k;
            it.price = kinds[k].price;
            it.name = kinds[k].name;
            if (const StringTable* t = res_->table(packageGroupHash("Build"), 0x8A)) {
                const size_t i = size_t(3 * kinds[k].text);
                if (const int price = std::atoi(t->get(i, lang::EnglishUS).c_str()))
                    it.price = price;
                if (const std::string n = t->get(i + 1); !n.empty())
                    it.name = n;
                it.description = t->get(i + 2);
            }
            it.image = materialThumbnail(kinds[k].key, kinds[k].material);
            out.push_back(std::move(it));
        }
        break;
    }
    case 7: {
        // B720 sculpting: catalog text "build" 128 (strings 3i price, 3i+1
        // name, 3i+2 text; Raise 0, Lower 1, the proxy tool 2 and 3 =
        // flatten and smooth, inferred), thumbnails 0x499DB772/0xE9261349,
        // ...50, ...41 (userToolProxies.txt) and ...51 (inferred).
        // B730 the terrain paints (their texture as the swatch).
        if (subsort_ == 0 || subsort_ == 2) {
            const StringTable* t = res_->table(packageGroupHash("Build"), 0x80);
            static const uint32_t thumbs[4] = {0xE9261349, 0xE9261350, 0xE9261341, 0xE9261351};
            static const char* const fallback[4] = {"Raise Terrain", "Lower Terrain", "Flatten Terrain", "Smooth Terrain"};
            for (int k = 0; k < 4; ++k) {
                Item it;
                it.tool = Tool::Terrain;
                it.terrainOp = k;
                it.price = 1;
                it.name = fallback[k];
                if (t) {
                    if (const std::string n = t->get(size_t(3 * k + 1)); !n.empty())
                        it.name = n;
                    it.description = t->get(size_t(3 * k + 2));
                }
                it.image = res_->image({kTypeUiImage, kBuildThumbGroup, thumbs[k]});
                out.push_back(std::move(it));
            }
        }
        if (subsort_ == 0 || subsort_ == 3)
            for (const BuildTerrainPaint& p : patterns_.terrainPaints()) {
                Item it;
                it.tool = Tool::Terrain;
                it.paint = &p;
                it.name = p.name, it.description = p.description;
                it.image = textureThumbnail(p.guid, p.texture);
                out.push_back(std::move(it));
            }
        break;
    }
    case 9: {
        // B920 roof types: roofs.txt "roof 0-5" (Lot::RoofType 1-6), catalog
        // text "build" 140 at their catalogTextIndex (0 1 3 2 4 5), price 0,
        // thumbnails 0x499DB772/0xE9261580, ...85, ...60, ...65, ...70, ...75.
        // B930 the roof patterns (their top texture as the swatch).
        if (subsort_ == 0 || subsort_ == 2) {
            const StringTable* t = res_->table(packageGroupHash("Build"), 0x8C);
            static const int text[6] = {0, 1, 3, 2, 4, 5};
            static const uint32_t thumbs[6] = {0xE9261580, 0xE9261585, 0xE9261560, 0xE9261565, 0xE9261570, 0xE9261575};
            static const char* const fallback[6] = {"Shed Gable Roof", "Shed Hip Roof", "Short Gable Roof",
                                                    "Long Gable Roof", "Hip Roof", "Mansard Roof"};
            for (int k = 0; k < 6; ++k) {
                Item it;
                it.tool = Tool::Roof;
                it.roofType = uint32_t(k + 1);
                it.name = fallback[k];
                if (t) {
                    if (const std::string n = t->get(size_t(3 * text[k] + 1)); !n.empty())
                        it.name = n;
                    it.description = t->get(size_t(3 * text[k] + 2));
                }
                it.image = res_->image({kTypeUiImage, kBuildThumbGroup, thumbs[k]});
                out.push_back(std::move(it));
            }
        }
        if (subsort_ == 0 || subsort_ == 4) // B940 dormers (roofs.txt "roof 6-11", catalog text "build" 140)
            for (const BuildRoofProduct& d : patterns_.roofProducts())
                if (d.id >= 6 && d.id <= 11) {
                    Item it;
                    it.tool = Tool::Dormer;
                    it.dormer = &d;
                    it.name = d.name, it.description = d.description, it.price = d.price;
                    out.push_back(std::move(it));
                }
        if (subsort_ == 0 || subsort_ == 3)
            for (const BuildRoofPattern& p : patterns_.roofPatterns()) {
                Item it;
                it.tool = Tool::Roof;
                it.roofPattern = &p;
                it.name = p.name, it.description = p.description;
                it.image = textureThumbnail(p.guid, p.texture);
                out.push_back(std::move(it));
            }
        break;
    }
    case 3: objectItems(8, doorSubsort(subsort_)); break;
    case 6: // B630 staircases (build type 1, subsort 0x20; placed with the object tool)
        if (subsort_ == 0 || subsort_ == 3)
            objectItems(1, 0x20);
        if (subsort_ == 0 || subsort_ == 4) // B640 modular stairs (modularstairs.txt, price a step)
            for (const BuildStairs& b : patterns_.stairs()) {
                Item it;
                it.tool = Tool::Stairs;
                it.stairs = &b;
                it.name = b.name, it.description = b.description, it.price = b.price;
                it.image = stairsThumbnail(b);
                out.push_back(std::move(it));
            }
        break;
    case 4: patternItems(true, kFloorSubsorts, std::size(kFloorSubsorts)); break;
    case 5: patternItems(false, kWallSubsorts, std::size(kWallSubsorts)); break;
    case 10:
        if (subsort_ == 6)
            objectItems(8, 8); // gates
        if (subsort_ == 4) { // BA40 pools: the pool tool (catalog text "build" 138, index 6), then the pool objects
            Item it;
            it.tool = Tool::Pool;
            it.price = kPoolTilePrice;
            it.name = "Pool";
            if (const StringTable* t = res_->table(packageGroupHash("Build"), 0x8A)) {
                if (const int price = std::atoi(t->get(18, lang::EnglishUS).c_str()))
                    it.price = price;
                if (const std::string n = t->get(19); !n.empty())
                    it.name = n;
                it.description = t->get(20);
            }
            it.image = materialThumbnail(0xF0F0F0F1u, "floor_poolRoundedLipBlue");
            out.push_back(std::move(it));
            objectItems(1, 0x40);
        }
        if (subsort_ == 5) // BA50 fences
            for (const BuildFence& f : patterns_.fences()) {
                Item it;
                it.tool = Tool::Fence;
                it.fence = &f;
                it.name = f.name, it.description = f.description, it.price = f.price;
                out.push_back(std::move(it));
            }
        break;
    default: break;
    }
    return out;
}

void BuildMode::openCategory(int category, int subsort) {
    category_ = category, subsort_ = subsort, page_ = 0, dirty_ = true;
}

UiScreen BuildMode::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    if (!ok_ || !active_)
        return out;
    std::map<const UiElement*, std::array<float, 4>> rects;
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
        t.attrs = {{"clsid", "GZWinText"}, {"caption", s},    {"font", "LiveModePanelPuck"},
                   {"forecolor", color},    {"align", align}, {"winflag_visible", "yes"},
                   {"area", area(0, 0, int(w), int(h))}};
        UiDrawHooks th;
        layoutUiElement(t, x, y, *res_, *fonts_, out, th);
    };

    UiElement& root = panel_.roots.front();
    UiElement* sortArea = root.child(kSortArea);
    const uint32_t openId = category_ == 0 ? kCategories : uint32_t(0xB000 + category_ * 0x100);
    const UiElement* open = sortArea ? sortArea->child(openId) : nullptr;
    const auto list = items();
    // B950 opens the auto-roof panel (0xAC) where the catalog goes.
    const bool autoRoofPanel = category_ == 9 && subsort_ == 5;
    const bool showCatalog = category_ != 0 && !autoRoofPanel;
    // The catalog follows the open sort panel (as in Buy mode; the
    // script's place for it, x 484, is where the category buttons end).
    if (sortArea && open) {
        const int right = sortArea->area()[0] + open->area()[2] - 1;
        if (UiElement* c = root.child(kCatalogArea)) {
            const auto a = c->area();
            c->set("area", area(right, a[1], right + (a[2] - a[0]), a[3]));
        }
        if (UiElement* c = root.child(kAutoRoof)) {
            const auto a = c->area();
            c->set("area", area(right, a[1], right + (a[2] - a[0]), a[3]));
        }
        if (UiElement* t = root.child(kBgTile)) {
            const auto a = t->area();
            const int end = showCatalog ? right : sortArea->area()[0] + open->area()[2];
            t->set("area", area(a[0], a[1], std::max(a[0], end), a[3]));
        }
        if (UiElement* e = root.child(kBgEndCap)) {
            const auto a = e->area();
            const int x = sortArea->area()[0] + open->area()[2];
            e->set("area", area(x, a[1], x + (a[2] - a[0]), a[3]));
        }
    }
    const UiElement& puck = puck_.roots.front();
    const auto pa = puck.area();
    const float puckX = 8, puckY = float(height) - float(pa[3] - pa[1]);
    float x0 = puckX, y0 = puckY - 137;
    if (const UiElement* a = puck.child(0x4BDEFFAA))
        if (const UiElement* b = root.child(0x0000AAAA))
            x0 = puckX + float(a->area()[0] - b->area()[0]), y0 = puckY + float(a->area()[1] - b->area()[1]);

    UiDrawHooks hk;
    hk.rects = &rects;
    for (uint32_t id : {kBgNoSubsort, kBgCollections, kCollectionsBtn, kCollectionsWin})
        if (const UiElement* e = root.child(id))
            hk.hide.insert(e);
    if (const UiElement* e = root.child(kAutoRoof)) {
        (autoRoofPanel ? hk.show : hk.hide).insert(e);
        if (const UiElement* b = e->child(0xC400))
            hk.buttonState[b] = preserveRoofs_ ? 2 : 1;
    }
    if (const UiElement* e = root.child(kBgFunctions))
        hk.show.insert(e);
    if (const UiElement* e = root.child(kBgEndCap))
        (showCatalog ? hk.hide : hk.show).insert(e);
    if (const UiElement* t = root.child(kBgTile)) {
        const auto a = t->area();
        if (a[2] <= a[0])
            hk.hide.insert(t);
    }
    if (const UiElement* c = root.child(kCatalogArea)) {
        (showCatalog ? hk.show : hk.hide).insert(c);
        if (const UiElement* x = c->child(kExpanded))
            hk.hide.insert(x);
        if (const UiElement* x = c->child(kInfoText))
            hk.hide.insert(x);
    }
    if (sortArea)
        for (const auto& c : sortArea->children)
            (c.id() == openId ? hk.show : hk.hide).insert(&c);
    if (open && category_ != 0)
        for (const auto& b : open->children) {
            const int n = int(b.id() >> 4 & 0xF);
            if ((b.id() & 0xF00F) == 0xB000 && n == subsort_ && n != 1)
                hk.buttonState[&b] = 2;
        }
    if (sortArea)
        if (const UiElement* cats = sortArea->child(kCategories))
            for (const auto& b : cats->children)
                if (int(b.id() >> 8 & 0xF) == category_)
                    hk.buttonState[&b] = 2;
    if (const UiElement* e = root.child(kHand))
        hk.buttonState[e] = tool_ == Tool::None ? 2 : 1;
    if (const UiElement* e = root.child(kDesignBtn))
        hk.buttonState[e] = tool_ == Tool::Design ? 2 : 1;
    if (const UiElement* e = root.child(kUndo))
        hk.buttonState[e] = !undo_.empty() || editor_->undoDepth() > editorDepth_ ? 1 : 0;
    if (const UiElement* e = root.child(kRedo))
        hk.buttonState[e] = !redo_.empty() ? 1 : 0;
    layoutUiElement(root, x0 - float(root.area()[0]), y0 - float(root.area()[1]), *res_, *fonts_, out, hk);

    // The grid: thumbnails of the current page with their prices.
    const size_t perPage = size_t(kColumns * kRows);
    const size_t pages = std::max<size_t>(1, (list.size() + perPage - 1) / perPage);
    page_ = std::min(page_, pages - 1);
    const int32_t funds = vm::funds(vm_);
    const UiElement* catalog = root.child(kCatalogArea);
    const UiElement* collapsed = catalog ? catalog->child(kCollapsed) : nullptr;
    if (auto r = rectOf(collapsed); r && showCatalog) {
        const float cw = (kGridW - kMarginX * (kColumns - 1)) / kColumns;
        const float ch = (kGridH - kMarginY * (kRows - 1)) / kRows;
        const float thumb = std::min(cw, ch - 12);
        for (size_t i = 0; i < perPage && page_ * perPage + i < list.size(); ++i) {
            Item& it = const_cast<Item&>(list[page_ * perPage + i]);
            const float cx = (*r)[0] + kGridX + float(i % kColumns) * (cw + kMarginX);
            const float cy = (*r)[1] + kGridY + float(i / kColumns) * (ch + kMarginY);
            const float tx = cx + (cw - thumb) / 2;
            const bool selected = (it.pattern && it.pattern == pattern_) || (it.object && it.object == object_) ||
                                  (it.fence && it.fence == fence_) || (it.paint && it.paint == paint_ && tool_ == Tool::Terrain) ||
                                  (it.tool == Tool::Terrain && !it.paint && tool_ == Tool::Terrain && !paint_ &&
                                   it.terrainOp == int(terrainOp_)) ||
                                  (it.tool == Tool::Roof && !it.roofPattern && tool_ == Tool::Roof && it.roofType == roofType_) ||
                                  (it.stairs && it.stairs == stairs_ && tool_ == Tool::Stairs) ||
                                  (it.dormer && it.dormer == dormer_ && tool_ == Tool::Dormer) ||
                                  (!it.pattern && !it.object && !it.fence && !it.paint && !it.stairs && !it.dormer && it.tool != Tool::Terrain &&
                                   it.tool != Tool::Roof && it.tool == tool_);
            if (selected) {
                UiQuad q;
                q.x0 = tx - 2, q.y0 = cy - 2, q.x1 = tx + thumb + 2, q.y1 = cy + thumb + 2;
                q.color = {0.6f, 1, 0.6f, 0.7f};
                out.quads.push_back(q);
            }
            const RgbaImage* img = it.image;
            if (!img && it.pattern)
                img = patternThumbnail(*it.pattern);
            if (!img && it.fence)
                img = fenceThumbnail(*it.fence);
            if (!img && it.object)
                img = objects_.thumbnail(*it.object);
            if (img) {
                UiQuad q;
                q.image = img;
                q.x0 = tx, q.y0 = cy, q.x1 = tx + thumb, q.y1 = cy + thumb;
                if (it.price > funds)
                    q.color = {0.45f, 0.45f, 0.45f, 0.8f};
                out.quads.push_back(q);
            }
            text(money(it.price), tx - 8, cy + ch - 13, thumb + 16, 13, "center",
                 it.price > funds ? "(150,40,40)" : "(5,6,113)");
            targets_.push_back({{tx, cy, tx + thumb, cy + ch}, Action::Item, int(page_ * perPage + i)});
        }
        target(collapsed->child(kPrev), Action::Prev, 0);
        target(collapsed->child(kNext), Action::Next, 0);
    }
    target(root.child(kHand), Action::Hand, 0);
    if (const UiElement* e = root.child(kAutoRoof); e && autoRoofPanel) {
        static const std::pair<uint32_t, int> buttons[] = {{0xC100, 1}, {0xC200, 2}, {0xC500, 3}, {0xC600, 4},
                                                           {0xC700, 5}, {0xC300, 0}, {0xC400, -1}};
        for (const auto& [id, tool] : buttons)
            target(e->child(id), Action::AutoRoof, tool);
    }
    target(root.child(kDesignBtn), Action::Design, 0);
    target(root.child(kDayNightBtn), Action::DayNight, 0);
    target(root.child(kCategoriesBtn), Action::Back, 0);
    target(root.child(kUndo), Action::Undo, 0);
    target(root.child(kRedo), Action::Redo, 0);
    if (sortArea) {
        if (category_ == 0) {
            if (const UiElement* cats = sortArea->child(kCategories))
                for (const auto& b : cats->children)
                    target(&b, Action::Category, int(b.id() >> 8 & 0xF));
        } else if (open) {
            for (const auto& b : open->children)
                if ((b.id() & 0xF00F) == 0xB000) {
                    const int n = int(b.id() >> 4 & 0xF);
                    target(&b, n == 1 ? Action::Back : Action::Subsort, n);
                }
        }
    }
    for (const auto& [e, r] : rects)
        if (e->clsid() == "GZWinBMP" || e->clsid() == "GZWinGen" || e->clsid().rfind("0x", 0) == 0)
            solid_.push_back(r);

    // The catalog description of the item under the mouse (as Buy mode's).
    if (hovered_) {
        UiElement& dm = description_.roots.front();
        if (UiElement* n = dm.child(0x4000))
            n->set("area", "(158,36,456,120)"), n->set("wrapped", "yes");
        if (UiElement* n = dm.child(0x5000))
            n->set("winflag_visible", "no");
        const auto da = dm.area();
        const float dx = x0 + 205, dy = y0 + 96 - float(da[3] - da[1]) - 4;
        UiDrawHooks dh;
        if (const UiElement* n = dm.child(0x3000))
            dh.captions[n] = hovered_->name;
        if (const UiElement* n = dm.child(0x4000))
            dh.captions[n] = hovered_->description;
        if (const UiElement* n = dm.child(0x6000))
            dh.captions[n] = "";
        if (const UiElement* f = dm.child(0x8000))
            if (const UiElement* img = f->child(0x8001)) {
                const RgbaImage* t = hovered_->image;
                if (!t && hovered_->pattern)
                    t = patternThumbnail(*hovered_->pattern);
                if (!t && hovered_->fence)
                    t = fenceThumbnail(*hovered_->fence);
                if (!t && hovered_->object)
                    t = objects_.thumbnail(*hovered_->object);
                if (t)
                    dh.images[img] = t;
            }
        for (uint32_t id : {0x9000u, 0xA000u})
            if (const UiElement* g = dm.child(id))
                dh.hide.insert(g);
        dh.rects = &rects;
        layoutUiElement(dm, dx - float(da[0]), dy - float(da[1]), *res_, *fonts_, out, dh);
        if (const UiElement* pb = dm.child(0x0002))
            if (auto r = rectOf(pb))
                text(money(hovered_->price), (*r)[0], (*r)[1], (*r)[2] - (*r)[0], (*r)[3] - (*r)[1], "center");
    }
    // Why the last change was refused, by the mouse (the look is inferred).
    if (!message_.empty() && message_.rfind("cannot", 0) == 0) {
        // Only the game's text for the refusal: no "cannot:" prefix and no internal reason in brackets.
        std::string shown = message_.substr(0, message_.find(';'));
        shown.erase(0, std::min<size_t>(shown.size(), 8)); // "cannot: "
        if (size_t br = shown.find(" ("); br != std::string::npos)
            shown.resize(br);
        const simd_float2 m = mouse_.value_or(simd_make_float2(float(width) / 2, float(height) / 2));
        UiQuad q;
        const float boxW = 7 * float(shown.size()) + 12;
        q.x0 = std::max(4.0f, std::min(m.x + 14, float(width) - boxW - 4)); // never past the window's edge
        q.y0 = std::min(m.y + 14, float(height) - 26);
        q.x1 = q.x0 + boxW, q.y1 = q.y0 + 22;
        q.color = {178 / 255.0f, 193 / 255.0f, 1, 0.9f};
        out.quads.push_back(q);
        text(shown, q.x0, q.y0 + 3, q.x1 - q.x0, 18, "center");
    }
    design_->layout(out, width, height); // the object design dialog over everything
    windows_.capture(rects); // for the tutorial (controlRect)
    return out;
}

void BuildMode::layoutIfNeeded() {
    if (canvasW_ > 0 && canvasH_ > 0 && (laidW_ != canvasW_ || laidH_ != canvasH_ || targets_.empty() || dirty_))
        screen(canvasW_, canvasH_);
}

bool BuildMode::press(simd_float2 p) {
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
        case Action::Category:
            openCategory(t.value, 0);
            if (t.value == 1)
                selectWallTool(false);
            break;
        case Action::Subsort: subsort_ = t.value, page_ = 0; break;
        case Action::Back: openCategory(0, 0); break;
        case Action::Prev: page_ = page_ ? page_ - 1 : 0; break;
        case Action::Next: ++page_; break;
        case Action::Item: {
            const auto list = items();
            if (size_t(t.value) < list.size()) {
                const Item& it = list[size_t(t.value)];
                if (it.pattern)
                    selectPattern(*it.pattern);
                else if (it.fence)
                    selectFence(*it.fence);
                else if (it.object)
                    selectObject(*it.object);
                else if (it.tool == Tool::Foundation)
                    selectFoundationTool(it.product);
                else if (it.tool == Tool::Pool)
                    selectPoolTool();
                else if (it.stairs)
                    selectStairsTool(*it.stairs);
                else if (it.dormer)
                    selectDormerTool(*it.dormer);
                else if (it.roofPattern)
                    applied(editor_->setRoofPattern(it.roofPattern->guid));
                else if (it.tool == Tool::Roof)
                    selectRoofTool(it.roofType);
                else if (it.tool == Tool::Terrain)
                    selectTerrainTool(vm::BuildEditor::TerrainOp(std::max(it.terrainOp, 0)), it.paint);
                else
                    selectWallTool(it.tool == Tool::Room);
            }
            break;
        }
        case Action::Hand: cancel(), design_->cancel(), tool_ = Tool::None; break;
        case Action::Design: selectDesignTool(); break;
        case Action::AutoRoof:
            if (t.value < 0)
                preserveRoofs_ = !preserveRoofs_, say(preserveRoofs_ ? "auto roof: preserve existing roofs" : "auto roof: replace its roofs");
            else
                autoRoof(t.value);
            break;
        case Action::DayNight:
            if (onDayNight)
                onDayNight();
            break;
        case Action::Undo: undo(); break;
        case Action::Redo: redo(); break;
        case Action::None: break;
        }
        std::printf("opensims2: build panel: %s %d (%zu items)\n",
                    t.action == Action::Item ? "item" : t.action == Action::Category ? "category"
                    : t.action == Action::Subsort ? "subsort" : "button",
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

void BuildMode::hover(simd_float2 p) {
    mouse_ = p;
    if (!active_)
        return;
    layoutIfNeeded();
    std::optional<Item> over;
    for (const Target& t : targets_)
        if (t.action == Action::Item && inside(t.rect, p)) {
            const auto list = items();
            if (size_t(t.value) < list.size())
                over = list[size_t(t.value)];
        }
    const bool changed = over.has_value() != hovered_.has_value() ||
                         (over && (over->pattern != hovered_->pattern || over->object != hovered_->object ||
                                   over->fence != hovered_->fence || over->paint != hovered_->paint ||
                                   over->terrainOp != hovered_->terrainOp || over->roofType != hovered_->roofType ||
                                   over->roofPattern != hovered_->roofPattern ||
                                   over->tool != hovered_->tool));
    if (changed)
        hovered_ = over, dirty_ = true;
}

void BuildMode::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void BuildMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
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

} // namespace os2

namespace os2 {

// Headless steps, in order (levels default to the current one):
// --build-open <category>[,<subsort>] opens a panel, --build-level <n>,
// --build-funds <n> (test aid: sets the family's funds),
// --wall/--room/--delete-wall x0,y0,x1,y1, --fence x0,y0,x1,y1,<fence>,
// --delete-fence x0,y0,x1,y1, --roof x0,y0,x1,y1[,type 1-6] (over the
// walls of the current level), --delete-roof x,y, --roof-pattern <name|guid>,
// --pool x0,y0,x1,y1 / --delete-pool x,y, --foundation x0,y0,x1,y1[,product 0-2: foundation, lattice deck, deck on columns] / --delete-foundation x0,y0,x1,y1
// (tiles, corners inclusive), --terrain raise|lower|flatten|smooth,x,y[,ticks[,size]]
// and --terrain-paint <paint|erase>,x,y[,ticks[,size]] (a brush stroke of
// ticks of 100 ms), --floor x0,y0,x1,y1,<pattern|none>[,rotation],
// --floor-click px,py,<pattern> (one click: the quarters under the point),
// --floor-rotation <1|2|4|8>, --wallpaper x0,y0,x1,y1,<r|l|b>,<pattern|none>,
// --wallpaper-room x,y,<pattern|none>, --wallpaper-flood x0,y0,x1,y1,<r|l>,<pattern|none>
// (Shift on that face), --door <guid|name>@x,y[,dir] places
// a door, window or gate, --build-undo / --build-redo (with any value).
void BuildMode::runSteps(const std::vector<std::string>& args) {
    if (!active_)
        return;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string& a = args[i];
        const std::string& v = args[i + 1];
        std::vector<std::string> f;
        {
            std::string cur;
            for (char c : v + ",")
                if (c == ',')
                    f.push_back(cur), cur.clear();
                else
                    cur += c;
        }
        auto n = [&](size_t k) { return k < f.size() && !f[k].empty() ? std::stoi(f[k]) : 0; };
        auto pattern = [&](const std::string& key, bool floor) -> const BuildPattern* {
            if (key == "none")
                return nullptr;
            const BuildPattern* p = patterns_.find(key, floor);
            if (!p)
                throw std::runtime_error(a + ": no pattern \"" + key + "\"");
            return p;
        };
        vm::BuildEditor& ed = *editor_;
        std::optional<vm::BuildEditor::Result> r;
        if (a == "--build-open")
            openCategory(n(0), n(1));
        else if (a == "--build-funds") // test aid: the family's funds
            vm::addFunds(vm_, n(0) - vm::funds(vm_));
        else if (a == "--build-level")
            setLevel(n(0));
        else if (a == "--wall" && f.size() >= 4)
            r = ed.drawWalls(level_, n(0), n(1), n(2), n(3)), selectWallTool(false);
        else if (a == "--room" && f.size() >= 4)
            r = ed.drawRoom(level_, n(0), n(1), n(2), n(3)), selectWallTool(true);
        else if (a == "--delete-wall" && f.size() >= 4)
            r = ed.deleteWalls(level_, n(0), n(1), n(2), n(3));
        else if (a == "--fence" && f.size() >= 5) {
            const BuildFence* fence = patterns_.findFence(f[4]);
            if (!fence)
                throw std::runtime_error("--fence: no fence \"" + f[4] + "\"");
            selectFence(*fence);
            r = ed.drawFences(level_, n(0), n(1), n(2), n(3), *fence);
        } else if (a == "--delete-fence" && f.size() >= 4)
            r = ed.deleteFences(level_, n(0), n(1), n(2), n(3));
        else if (a == "--roof" && f.size() >= 4)
            selectRoofTool(uint32_t(f.size() > 4 ? n(4) : 5)),
                r = ed.drawRoof(level_, n(0), n(1), n(2), n(3), uint32_t(f.size() > 4 ? n(4) : 5));
        else if (a == "--delete-roof" && f.size() >= 2)
            r = ed.deleteRoof(level_, std::stof(f[0]), std::stof(f[1]));
        else if (a == "--roof-pattern") {
            const BuildRoofPattern* p = patterns_.findRoofPattern(v);
            if (!p)
                throw std::runtime_error("--roof-pattern: no pattern \"" + v + "\"");
            r = ed.setRoofPattern(p->guid);
        } else if (a == "--pool" && f.size() >= 4)
            selectPoolTool(), r = ed.drawPool(n(0), n(1), n(2), n(3));
        else if (a == "--delete-pool" && f.size() >= 2)
            r = ed.deletePool(n(0), n(1));
        else if (a == "--foundation" && f.size() >= 4)
            selectFoundationTool(n(4)), r = ed.drawFoundation(n(0), n(1), n(2), n(3), n(4));
        else if (a == "--delete-foundation" && f.size() >= 4)
            r = ed.deleteFoundation(n(0), n(1), n(2), n(3));
        else if ((a == "--terrain" || a == "--terrain-paint") && f.size() >= 3) {
            using Op = vm::BuildEditor::TerrainOp;
            Op op = Op::Paint;
            const BuildTerrainPaint* paint = nullptr;
            if (a == "--terrain")
                op = f[0] == "lower" ? Op::Lower : f[0] == "flatten" ? Op::Flatten : f[0] == "smooth" ? Op::Smooth : Op::Raise;
            else if (f[0] == "erase")
                op = Op::Erase;
            else if (!(paint = patterns_.findTerrainPaint(f[0])))
                throw std::runtime_error("--terrain-paint: no paint \"" + f[0] + "\"");
            selectTerrainTool(op, paint);
            r = ed.terrainStroke(op, std::stof(f[1]), std::stof(f[2]), f.size() > 3 ? n(3) : 10, f.size() > 4 ? n(4) : 1,
                                 op == Op::Lower ? patterns_.lowerProfile() : patterns_.raiseProfile(),
                                 paint ? paint->texture : "");
            pressSound(Tool::Terrain, op == Op::Erase);
            stopBrushLoop();
            releaseSound(Tool::Terrain, op == Op::Erase, *r);
        } else if (a == "--floor-rotation")
            floorRotation_ = n(0);
        else if (a == "--floor" && f.size() >= 5) {
            const BuildPattern* p = pattern(f[4], true);
            if (p)
                selectPattern(*p);
            r = ed.paintFloor(level_, n(0), n(1), n(2), n(3), p, 0xF, f.size() >= 6 ? n(5) : floorRotation_);
        } else if (a == "--floor-click" && f.size() >= 3) {
            const float px = std::stof(f[0]), py = std::stof(f[1]);
            const BuildPattern* p = pattern(f[2], true);
            if (p)
                selectPattern(*p);
            const int x = int(std::floor(px)), y = int(std::floor(py));
            r = ed.paintFloor(level_, x, y, x, y, p, ed.clickQuarters(level_, px, py), floorRotation_);
        } else if (a == "--wallpaper" && f.size() >= 6) {
            const BuildPattern* p = pattern(f[5], false);
            r = ed.paintWalls(level_, n(0), n(1), n(2), n(3), f[4] == "r" ? 1 : f[4] == "l" ? -1 : 0, p);
            if (p)
                selectPattern(*p);
        } else if (a == "--wallpaper-flood" && f.size() >= 6) {
            const BuildPattern* p = pattern(f[5], false);
            r = ed.paintFlood(level_, n(0), n(1), n(2), n(3), f[4] == "l" ? -1 : 1, p);
            if (p)
                selectPattern(*p);
        } else if (a == "--wallpaper-room" && f.size() >= 3) {
            r = ed.paintRoom(level_, n(0), n(1), pattern(f[2], false));
        } else if (a == "--door") {
            const auto at = v.find('@');
            const CatalogEntry* e = objects_.find(v.substr(0, at));
            if (!e)
                throw std::runtime_error("--door: no build object \"" + v.substr(0, at) + "\"");
            int x = 0, y = 0, dir = 2;
            if (at != std::string::npos)
                std::sscanf(v.c_str() + at + 1, "%d,%d,%d", &x, &y, &dir);
            selectObject(*e);
            if (at != std::string::npos)
                placeObjectAt(x, y, dir);
        } else if (a == "--build-design") { // <id>[,row:option...][,accept|cancel]
            selectDesignTool();
            if (design(vm::ObjectId(n(0)), simd_make_float2(560, 420)))
                for (size_t k = 1; k < f.size(); ++k) {
                    int row = 0, option = 0;
                    if (f[k] == "accept")
                        design_->accept();
                    else if (f[k] == "cancel")
                        design_->cancel();
                    else if (std::sscanf(f[k].c_str(), "%d:%d", &row, &option) == 2)
                        design_->choose(row, size_t(option));
                }
        } else if (a == "--stairs" && f.size() >= 5) { // product,x,y,level,dir[,endx,endy[,whole]]
            const BuildStairs* b = patterns_.stairsById(n(0));
            if (!b)
                throw std::runtime_error("--stairs: no modular stairs " + f[0]);
            selectStairsTool(*b);
            std::optional<std::array<float, 2>> end;
            if (f.size() >= 7)
                end = std::array<float, 2>{std::stof(f[5]), std::stof(f[6])};
            r = ed.placeStairs(*b, {n(1), n(2), n(3)}, n(4), end, f.size() > 7 && f[7] == "whole");
            releaseSound(Tool::Stairs, false, *r);
        } else if (a == "--dormer" && f.size() >= 4) { // product 6-11,x,y,direction[,level]
            if (const BuildRoofProduct* d = patterns_.roofProduct(n(0)))
                selectDormerTool(*d);
            r = ed.placeDormer(n(0), f.size() > 4 ? n(4) : level_, n(1), n(2), n(3));
            releaseSound(Tool::Roof, false, *r);
        } else if (a == "--delete-stairs" && !f.empty()) {
            r = ed.deleteStairs(vm::ObjectId(n(0)));
            releaseSound(Tool::Stairs, true, *r);
        } else if (a == "--auto-roof") { // tool 1-5, 0 removes the roofs; ",keep" preserves
            preserveRoofs_ = f.size() > 1 && f[1] == "keep";
            autoRoof(n(0));
        } else if (a == "--build-day-night") {
            if (onDayNight)
                onDayNight();
        } else if (a == "--build-undo") {
            undo();
        } else if (a == "--build-redo") {
            redo();
        }
        syncHistory();
        if (r) {
            // The step's sounds, as the mouse would make them.
            static const std::map<std::string, std::pair<Tool, bool>> kStepTools{
                {"--wall", {Tool::Wall, false}},        {"--room", {Tool::Room, false}},
                {"--delete-wall", {Tool::Wall, true}},  {"--fence", {Tool::Fence, false}},
                {"--delete-fence", {Tool::Fence, true}}, {"--roof", {Tool::Roof, false}},
                {"--delete-roof", {Tool::Roof, true}},  {"--pool", {Tool::Pool, false}},
                {"--delete-pool", {Tool::Pool, true}},  {"--foundation", {Tool::Foundation, false}},
                {"--delete-foundation", {Tool::Foundation, true}}, {"--floor", {Tool::Floor, false}},
                {"--floor-click", {Tool::Floor, false}}, {"--wallpaper", {Tool::Wallpaper, false}},
                {"--wallpaper-flood", {Tool::Wallpaper, false}}, {"--wallpaper-room", {Tool::Wallpaper, false}}};
            if (auto t = kStepTools.find(a); t != kStepTools.end()) {
                const auto [tool, erase] = t->second;
                pressSound(tool, erase);
                if (r->ok && (tool == Tool::Wall || tool == Tool::Room || tool == Tool::Fence))
                    for (int k = 0; k < std::min(r->changed, 8); ++k)
                        sound(erase ? "ui_build_walldelete_section" : "ui_build_wall_section");
                if (r->ok && tool == Tool::Foundation && !erase)
                    sound("ui_build_deck_section");
                releaseSound(tool, erase, *r);
            }
            std::printf("opensims2: build: %s %s: %s%s (funds §%d)\n", a.c_str(), v.c_str(), r->ok ? "" : "FAILED: ",
                        r->message.c_str(), vm::funds(vm_));
            if (r->ok && r->changed)
                redraw();
        }
    }
    std::fflush(stdout);
    update();
}

std::optional<std::array<float, 4>> BuildMode::controlRect(uint32_t window, uint32_t control) {
    if (!active_)
        return std::nullopt;
    layoutIfNeeded();
    // The game's "Build Dynamic Menu" (0x502BBF02 with the room tool,
    // 0x502BBF03 with the deck tool: button 3) is made in code; here those
    // tools are items of the catalog: the room tool, and the first deck
    // (the lattice one, deckfoundations.txt "deck 1"; inferred).
    if ((window == 0x502BBF02 || window == 0x502BBF03) && control == 3) {
        const bool deck = window == 0x502BBF03;
        const auto list = items();
        for (const Target& t : targets_)
            if (t.action == Action::Item && size_t(t.value) < list.size()) {
                const Item& it = list[size_t(t.value)];
                if (deck ? it.tool == Tool::Foundation && it.product == 1 : it.tool == Tool::Room)
                    return t.rect;
            }
        return std::nullopt;
    }
    return windows_.find(window, control);
}

} // namespace os2
