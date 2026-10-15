// Build mode on the lot (build_mode.hpp): the mouse rays, the tools' press,
// drag and release, their sounds, the terrain brush and the previews that
// follow the tool.

#include "app/build_mode.hpp"
#include "formats/log_once.hpp"
#include "app/build_mode_internal.hpp"

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

// --- The lot -------------------------------------------------------------------

// The level the mouse works on: the tools that edit the ground (pools,
// terrain, foundations: BuildEditor digs, sculpts and builds them on level
// 0 whatever floor is shown) pick on the terrain; the others on the floor
// shown. Picking the floor shown made a pool dug with an upper floor shown
// land where the ray met that floor's plane, ~3 tiles nearer the camera than
// the mouse (live QA pass 11).
int BuildMode::pickLevel() const {
    return tool_ == Tool::Pool || tool_ == Tool::Terrain || tool_ == Tool::Foundation ? 1 : level_;
}

float BuildMode::pickHeight(simd_float2 p) const { return vm::lotPickHeight(lot_, pickLevel(), p.x, p.y, tool_ == Tool::Pool); }

std::optional<simd_float2> BuildMode::groundPoint(simd_float3 eye, simd_float3 dir) const {
    if (!vm_.world)
        return std::nullopt;
    const auto p = vm::pickLotGround(lot_, pickLevel(), {eye.x, eye.y, eye.z}, {dir.x, dir.y, dir.z}, tool_ == Tool::Pool);
    if (!p)
        return std::nullopt;
    return simd_make_float2((*p)[0], (*p)[1]);
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
        holdObjectAt(int(std::floor(pointer_->x)), int(std::floor(pointer_->y)), direction_, true);
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
            placeObjectAt(int(std::floor(p->x)), int(std::floor(p->y)), direction_, true);
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
    const auto& profile = terrainOp_ == vm::BuildEditor::TerrainOp::Lower   ? patterns_.lowerProfile()
                          : terrainOp_ == vm::BuildEditor::TerrainOp::Water ? patterns_.waterProfile()
                                                                            : patterns_.raiseProfile();
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
    } else if (tool_ == Tool::FenceArch && arch_) {
        // The fence edge nearest the release point.
        const float fx = b.x - std::floor(b.x), fy = b.y - std::floor(b.y);
        const int gx = int(std::floor(b.x)), gy = int(std::floor(b.y));
        std::array<int, 4> e{};
        const float dl = fx, dr = 1 - fx, dd = fy, du = 1 - fy, m = std::min({dl, dr, dd, du});
        if (m == dl)
            e = {gx, gy, gx, gy + 1};
        else if (m == dr)
            e = {gx + 1, gy, gx + 1, gy + 1};
        else if (m == dd)
            e = {gx, gy, gx + 1, gy};
        else
            e = {gx, gy + 1, gx + 1, gy + 1};
        done(control_ ? editor_->deleteFenceArch(level_, e[0], e[1], e[2], e[3], arch_)
                      : editor_->placeFenceArch(level_, e[0], e[1], e[2], e[3], *arch_));
    } else if (tool_ == Tool::Railing) {
        // The staircase part under the mouse (types 10-12), the side by
        // where the click is (cTSUserToolStairsRailing::OnAnyMouseEvent).
        vm::ObjectId best = 0;
        float bestD = 0.75f;
        for (const auto& [id, o] : vm_.objects())
            if (o.position.inWorld() && o.definition && o.definition->objectType() - 10u < 3 &&
                (o.position.level == level_ || o.position.level == level_ - 1))
                if (const float d = std::hypot(o.position.x - b.x, o.position.y - b.y); d < bestD)
                    bestD = d, best = id;
        done(best ? editor_->changeRailing(best, vm::railingSide(vm_, best, b.x, b.y), control_)
                  : vm::BuildEditor::Result{false, 0, 0, 0, "no stairs there"});
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
        done(control_ ? editor_->erasePool(x0, y0, x1, y1) : editor_->drawPool(x0, y0, x1, y1));
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
        auto z = [&](float x, float y) { return pickHeight(simd_make_float2(x, y)) + 0.03f; };
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
            // The pool tool paints the squares red where the dig would be refused.
            const bool digOk = tool_ != Tool::Pool || control_ ||
                               editor_->poolDigError(int(std::floor(a.x)), int(std::floor(a.y)), int(std::floor(p.x)),
                                                     int(std::floor(p.y))) == 0; // from the drag's start
            for (int x = x0; x <= x1 && used < 400; ++x)
                for (int y = y0; y <= y1 && used < 400; ++y)
                    put(!control_ && digOk, placeQuad(float(x) + 0.04f, float(y) + 0.04f, z(float(x) + 0.5f, float(y) + 0.5f), 0.92f,
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

} // namespace os2
