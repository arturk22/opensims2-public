#include "app/build_mode.hpp"

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
#include <cstdio>
#include <stdexcept>

namespace os2 {

namespace {

constexpr uint32_t kPanelScript = 0x49060003, kPuckScript = 0x4905F85B, kDescriptionScript = 0x49063004;
constexpr uint32_t kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C;
// Build panel windows (0x49060003, docs/buy-build-mac.md §7).
constexpr uint32_t kSortArea = 0x000000A8, kCategories = 0xA000, kCatalogArea = 0xAD502BBF, kCollapsed = 0x1000,
                   kExpanded = 0x2000, kPrev = 0x1004, kNext = 0x1005, kInfoText = 0x4000;
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
    editor_->resetHistory([this](vm::ObjectId part) { view_.forget(part); });
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
    dragFrom_.reset();
    dirty_ = true;
}

void BuildMode::holdObjectAt(int x, int y, int direction) {
    direction_ = direction;
    if (!held_)
        return;
    last_ = vm::checkPlacement(vm_, held_, {x, y, level_}, direction_);
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
        say("cannot place " + object_->name + ": " + last_.reason);
        return false;
    }
    const vm::ObjectId id = held_;
    std::string why;
    if (!vm::buy(vm_, id, last_, object_->price, &why)) {
        say("not bought: " + why);
        return false;
    }
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
    }
    syncAfterObject();
    say("bought " + object_->name + " as object " + std::to_string(id) + " for " + money(object_->price) + "; funds " +
        money(vm::funds(vm_)));
    selectObject(*object_); // another of the same (doors and windows keep buying)
    return true;
}

void BuildMode::syncAfterObject() {
    editor_->syncWorld();
    if (onGeometryChanged)
        onGeometryChanged();
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

bool BuildMode::undo() {
    cancel();
    const bool ok = editor_->undo();
    if (ok && onGeometryChanged)
        onGeometryChanged();
    say(ok ? "undone; funds " + money(vm::funds(vm_)) : "nothing to undo");
    return ok;
}

bool BuildMode::redo() {
    cancel();
    const bool ok = editor_->redo();
    if (ok && onGeometryChanged)
        onGeometryChanged();
    say(ok ? "redone; funds " + money(vm::funds(vm_)) : "nothing to redo");
    return ok;
}

void BuildMode::applied(const vm::BuildEditor::Result& r) {
    // A refusal shows the game's text for its code (UIText STR# 0x89).
    std::string text = r.message;
    if (!r.ok && r.error >= 2)
        if (const StringTable* t = res_->table(0x7FA7DC7C, 0x89))
            if (std::string g = t->get(size_t(vm::placementerror::textIndex(r.error))); !g.empty())
                text = g + " (" + r.message + ")";
    say((r.ok ? "" : "cannot: ") + text + "; funds " + money(vm::funds(vm_)));
    if (r.ok && r.changed && onGeometryChanged)
        onGeometryChanged();
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
    if (tool_ == Tool::Wallpaper) {
        if (auto f = faceAt(*p); f && pattern_) {
            const auto& [s, side] = *f;
            if (shift) {
                // The room on that side: the tile a quarter tile off the wall.
                const float mx = (float(s[0]) + float(s[2])) / 2, my = (float(s[1]) + float(s[3])) / 2;
                float nx = float(s[3] - s[1]), ny = -float(s[2] - s[0]);
                const float len = std::hypot(nx, ny);
                nx = nx / len * float(side), ny = ny / len * float(side);
                applied(editor_->paintRoom(level_, int(std::floor(mx + nx * 0.3f)), int(std::floor(my + ny * 0.3f)),
                                           control ? nullptr : pattern_));
            } else {
                applied(editor_->paintWalls(level_, s[0], s[1], s[2], s[3], side, control ? nullptr : pattern_));
            }
        }
        return true;
    }
    dragFrom_ = p;
    return true;
}

void BuildMode::mouseDrag(simd_float3 eye, simd_float3 dir) {
    if (!active_)
        return;
    if (auto p = groundPoint(eye, dir))
        pointer_ = p;
    dirty_ = true;
}

bool BuildMode::mouseUp(simd_float3 eye, simd_float3 dir) {
    if (!active_ || !dragFrom_)
        return false;
    if (auto p = groundPoint(eye, dir))
        pointer_ = p;
    const simd_float2 a = *dragFrom_, b = pointer_.value_or(a);
    dragFrom_.reset();
    if (tool_ == Tool::Fence && fence_) {
        const auto r = snapRun(a, b);
        if (r[0] == r[2] && r[1] == r[3])
            return true;
        applied(control_ ? editor_->deleteFences(level_, r[0], r[1], r[2], r[3])
                         : editor_->drawFences(level_, r[0], r[1], r[2], r[3], *fence_));
    } else if (tool_ == Tool::Wall || tool_ == Tool::Room) {
        if (tool_ == Tool::Room || shift_) {
            const int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y)), x1 = int(std::lround(b.x)),
                      y1 = int(std::lround(b.y));
            if (control_)
                for (const auto& s : std::array<std::array<int, 4>, 4>{
                         {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
                    applied(editor_->deleteWalls(level_, s[0], s[1], s[2], s[3]));
            else
                applied(editor_->drawRoom(level_, x0, y0, x1, y1));
        } else {
            const auto r = snapRun(a, b);
            if (r[0] == r[2] && r[1] == r[3])
                return true;
            applied(control_ ? editor_->deleteWalls(level_, r[0], r[1], r[2], r[3])
                             : editor_->drawWalls(level_, r[0], r[1], r[2], r[3]));
        }
    } else if (tool_ == Tool::Floor) {
        // A click without a drag floors the quarters under the mouse
        // (FUN_003000cc: the half on its side of a diagonal wall).
        const bool click = int(std::floor(a.x)) == int(std::floor(b.x)) && int(std::floor(a.y)) == int(std::floor(b.y));
        const int quarters = click ? editor_->clickQuarters(level_, b.x, b.y) : 0xF;
        applied(editor_->paintFloor(level_, int(std::floor(a.x)), int(std::floor(a.y)), int(std::floor(b.x)),
                                    int(std::floor(b.y)), control_ ? nullptr : pattern_, quarters, floorRotation_));
    }
    return true;
}

void BuildMode::update() {
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
        if (tool_ == Tool::Wall || tool_ == Tool::Room || tool_ == Tool::Fence) {
            std::vector<std::array<int, 4>> runs;
            if (dragFrom_) {
                const simd_float2 a = *dragFrom_;
                if (tool_ == Tool::Room || (shift_ && tool_ == Tool::Wall)) {
                    const int x0 = int(std::lround(a.x)), y0 = int(std::lround(a.y)), x1 = int(std::lround(p.x)),
                              y1 = int(std::lround(p.y));
                    runs = {{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}};
                } else {
                    runs = {snapRun(a, p)};
                }
            }
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
        } else if (tool_ == Tool::Floor) {
            const simd_float2 a = dragFrom_.value_or(p);
            const int x0 = int(std::floor(std::min(a.x, p.x))), x1 = int(std::floor(std::max(a.x, p.x)));
            const int y0 = int(std::floor(std::min(a.y, p.y))), y1 = int(std::floor(std::max(a.y, p.y)));
            for (int x = x0; x <= x1 && used < 400; ++x)
                for (int y = y0; y <= y1 && used < 400; ++y)
                    put(!control_, placeQuad(float(x) + 0.04f, float(y) + 0.04f, z(float(x) + 0.5f, float(y) + 0.5f), 0.92f,
                                             0.92f, 0, false));
        } else if (tool_ == Tool::Wallpaper) {
            if (auto f = faceAt(p)) {
                const auto& [s, side] = *f;
                const float ang = std::atan2(float(s[3] - s[1]), float(s[2] - s[0]));
                const float len = std::hypot(float(s[2] - s[0]), float(s[3] - s[1]));
                // A strip on the face's side (right of a -> b is -90°).
                const float off = side > 0 ? -0.3f : 0.0f;
                const float c = std::cos(ang), sn = std::sin(ang);
                put(true, placeQuad(float(s[0]) - sn * off, float(s[1]) + c * off, z(float(s[0]), float(s[1])), len, 0.3f,
                                    ang, false));
            }
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
    case 3: objectItems(8, doorSubsort(subsort_)); break;
    case 4: patternItems(true, kFloorSubsorts, std::size(kFloorSubsorts)); break;
    case 5: patternItems(false, kWallSubsorts, std::size(kWallSubsorts)); break;
    case 10:
        if (subsort_ == 6)
            objectItems(8, 8); // gates
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
    const bool showCatalog = category_ != 0;
    // The catalog follows the open sort panel (as in Buy mode; the
    // script's place for it, x 484, is where the category buttons end).
    if (sortArea && open) {
        const int right = sortArea->area()[0] + open->area()[2] - 1;
        if (UiElement* c = root.child(kCatalogArea)) {
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
    for (uint32_t id : {kBgNoSubsort, kBgCollections, kCollectionsBtn, kCollectionsWin, kAutoRoof})
        if (const UiElement* e = root.child(id))
            hk.hide.insert(e);
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
    if (const UiElement* e = root.child(kUndo))
        hk.buttonState[e] = editor_->canUndo() ? 1 : 0;
    if (const UiElement* e = root.child(kRedo))
        hk.buttonState[e] = editor_->canRedo() ? 1 : 0;
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
                                  (it.fence && it.fence == fence_) ||
                                  (!it.pattern && !it.object && !it.fence && it.tool == tool_);
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
        const std::string shown = message_.substr(0, message_.find(';'));
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
                else
                    selectWallTool(it.tool == Tool::Room);
            }
            break;
        }
        case Action::Hand: cancel(), tool_ = Tool::None; break;
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
                                   over->fence != hovered_->fence ||
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
// --delete-fence x0,y0,x1,y1, --floor x0,y0,x1,y1,<pattern|none>[,rotation],
// --floor-click px,py,<pattern> (one click: the quarters under the point),
// --floor-rotation <1|2|4|8>, --wallpaper x0,y0,x1,y1,<r|l|b>,<pattern|none>,
// --wallpaper-room x,y,<pattern|none>, --door <guid|name>@x,y[,dir] places
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
        else if (a == "--floor-rotation")
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
        } else if (a == "--build-undo") {
            undo();
        } else if (a == "--build-redo") {
            redo();
        }
        if (r) {
            std::printf("opensims2: build: %s %s: %s%s (funds §%d)\n", a.c_str(), v.c_str(), r->ok ? "" : "FAILED: ",
                        r->message.c_str(), vm::funds(vm_));
            if (r->ok && r->changed && onGeometryChanged)
                onGeometryChanged();
        }
    }
    std::fflush(stdout);
    update();
}

} // namespace os2
