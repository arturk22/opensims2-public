#include "hood_mode.hpp"
#include "app/scene_loader.hpp"

#include "formats/hood_lots.hpp"
#include "formats/hood_moves.hpp"
#include "formats/neighborhood.hpp"
#include "render/camera.hpp"

#include <algorithm>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

namespace os2 {

namespace fs = std::filesystem;

HoodMode::HoodMode(Renderer& renderer, ResourceIndex& index, const fs::path& gameDir, const std::string& query,
                   std::vector<std::string> args)
    : gameDir_(gameDir), query_(query), args_(std::move(args)) {
    const fs::path res = gameDir / "TSData/Res";
    view_ = std::make_unique<HoodView>(renderer, index, res, gameDir / "TSData/Sys", query);
    ui_ = std::make_unique<HoodUi>(*view_, index, res);
    ui_->bins().setDecorCatalog(hood::loadDecorCatalog(index));
    loading::phase("hood: ui");
    // The player's Lots & Houses bin (filled from the game's the first time).
    ui_->bins().setLotBin(
        hood::loadLotBin(hood::playerLotCatalog(view_->folder(), res / "UserData/LotCatalog")));
    loading::phase("hood: bins");
    // The story editor's save (F4 panel): the CTSS of the neighborhood package.
    ui_->saveStory = [this](const std::string& text) {
        const auto r = hood::setDescription(view_->folder(), text, gameDir_);
        for (const auto& l : r.lines)
            std::printf("opensims2: %s\n", l.c_str());
        if (!r.ok)
            std::printf("opensims2: neighborhood story not saved: %s\n", r.error.c_str());
        std::fflush(stdout);
        if (r.ok)
            view_->setDescription(text);
    };
}

std::optional<simd_int2> HoodMode::tileAt(const LotCamera& camera, simd_float2 p, float w, float h) const {
    if (w <= 0 || h <= 0)
        return std::nullopt;
    const simd_float4x4 m = simd_mul(camera.projection(w / h), camera.view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = camera.eye();
    const auto g = view_->groundAt(eye, simd_normalize(q.xyz / q.w - eye));
    if (!g)
        return std::nullopt;
    return simd_make_int2(int(std::floor(g->x / hood::kHoodCellSize)), int(std::floor(g->y / hood::kHoodCellSize)));
}

HoodMode::LotPlacement HoodMode::placementAt(int x, int y) const {
    LotPlacement out;
    const auto& world = view_->placementWorld();
    const HoodBins& bins = ui_->bins();
    const auto& terrain = view_->rawTerrain();
    out.x = x, out.y = y;
    auto finish = [&](const hood::Placement& p) {
        out.any = true;
        out.ok = p.ok;
        out.why = p.why;
        out.height = p.ok ? p.roadHeight
                          : terrain.heightAt((float(out.rect.x0 + out.rect.x1) / 2) * hood::kHoodCellSize,
                                             (float(out.rect.y0 + out.rect.y1) / 2) * hood::kHoodCellSize);
    };
    if (bins.tool() == HoodBins::Tool::PlaceEmpty && bins.emptySize() >= 0) {
        // cTSUserToolPlaceNewLot: the first turn that may go there (the
        // game turns it with the keys).
        const hood::EmptyLotSize size = hood::emptyLotSize(bins.emptySize());
        hood::Placement last;
        if (turn_ >= 0) {
            out.rect = hood::emptyLotRect(size, x, y, turn_);
            out.turn = turn_;
            finish(hood::checkEmptyLot(world, size, x, y, turn_));
            return out;
        }
        for (int k = 0; k < 4; ++k) {
            const hood::TileRect r = hood::emptyLotRect(size, x, y, k);
            const hood::Placement p = hood::checkEmptyLot(world, size, x, y, k);
            if (k == 0 || p.ok)
                out.rect = r, out.turn = k, last = p;
            if (p.ok)
                break;
        }
        finish(last);
        return out;
    }
    const hood::LotInfo* base = nullptr;
    uint32_t self = 0;
    if (bins.tool() == HoodBins::Tool::PlaceBin && bins.binLot() >= 0)
        base = &bins.lotBin()[size_t(bins.binLot())].base;
    else if (bins.tool() == HoodBins::Tool::MoveLot && movingLot_)
        if (const hood::Lot* l = view_->neighborhood().lot(movingLot_))
            base = &l->info, self = l->id;
    if (!base)
        return out;
    if (turn_ >= 0) {
        hood::LotInfo turned = *base;
        turned.frontEdge = uint8_t(turn_), turned.x = uint32_t(std::max(0, x)), turned.y = uint32_t(std::max(0, y));
        out.rect = hood::lotRect(turned);
        out.turn = turn_;
        finish(hood::checkLot(world, *base, self, x, y, turn_));
        return out;
    }
    if (auto f = hood::findFront(world, *base, x, y, self, self ? int(base->frontEdge) : -1)) {
        hood::LotInfo turned = *base;
        turned.frontEdge = uint8_t(f->first), turned.x = uint32_t(x), turned.y = uint32_t(y);
        out.rect = hood::lotRect(turned);
        out.turn = f->first;
        finish(f->second);
        return out;
    }
    hood::LotInfo turned = *base;
    turned.x = uint32_t(std::max(0, x)), turned.y = uint32_t(std::max(0, y));
    out.rect = hood::lotRect(turned);
    out.turn = turned.frontEdge;
    finish(hood::checkLot(world, *base, self, x, y, turned.frontEdge));
    return out;
}

void HoodMode::typeKey(int keyCode, const std::string& typed) {
    if (ui_->storyEditing()) {
        ui_->typeStoryKey(keyCode, typed);
        ui_->refreshLayout();
        return;
    }
    const HoodBins::Request r = ui_->bins().typeKey(keyCode, typed);
    ui_->refreshLayout();
    if (r.kind == HoodBins::Request::Kind::PlaceLot)
        carryOut(r);
}

bool HoodMode::decorating() const { return ui_->bins().tool() == HoodBins::Tool::Decorate; }

const hood::DecorProduct* HoodMode::decorProduct(uint32_t guid) const {
    for (const auto& p : ui_->bins().decorCatalog())
        if (p.guid == guid)
            return &p;
    return nullptr;
}

// The tile centre under the mouse: the ghost of the product there, and whether it may stand.
void HoodMode::updateDecoration(const LotCamera& camera, simd_float2 p, float w, float h) {
    decorSpot_.reset();
    const hood::DecorProduct* product = ui_->bins().tool() == HoodBins::Tool::Decorate ? decorProduct(ui_->bins().decorGuid()) : nullptr;
    if (product)
        if (auto tile = tileAt(camera, p, w, h))
            decorSpot_ = simd_make_float2(float(tile->x) * hood::kHoodCellSize + 5, float(tile->y) * hood::kHoodCellSize + 5);
    if (product && decorSpot_) {
        const auto at = view_->decorationFootprint(*product, decorSpot_->x, decorSpot_->y, decorRotation_);
        view_->showDecorationGhost(product, decorSpot_->x, decorSpot_->y, decorRotation_,
                                   hood::decorationBlocked(view_->placementWorld(), *product, at).empty());
    } else {
        view_->showDecorationGhost(nullptr, 0, 0, 0, false);
    }
}

bool HoodMode::placeDecoration(const hood::DecorProduct& product, float x, float y, float rotation, bool record,
                               std::optional<simd_float2> screen) {
    const auto at = view_->decorationFootprint(product, x, y, rotation);
    const auto r = hood::addDecoration(view_->folder(), view_->placementWorld(), product, at, rotation, gameDir_);
    for (const auto& l : r.lines)
        std::printf("opensims2: %s\n", l.c_str());
    if (!r.ok) {
        // ShowErrorTooltips: the refusal on screen (STR# 0x89), as the game does.
        if (r.code >= hood::DecorInLot && r.code <= hood::DecorNeedsWater && screen)
            ui_->showErrorTooltip(uint32_t(r.code), *screen);
        std::printf("opensims2: decoration not placed: %s%s%s\n", r.error.c_str(),
                    ui_->errorTooltip().empty() ? "" : " - tooltip: ", ui_->errorTooltip().c_str());
        std::fflush(stdout);
        return false;
    }
    std::fflush(stdout);
    view_->addDecoration(product, at, rotation);
    if (record) {
        decorUndo_.push_back({true, product, at, rotation});
        decorRedo_.clear();
    }
    return true;
}

// A click with a decoration tool, on the ground at the mouse.
bool HoodMode::decorClick(const LotCamera& camera, simd_float2 p, float w, float h) {
    const HoodBins::Tool t = ui_->bins().tool();
    if (t != HoodBins::Tool::Decorate && t != HoodBins::Tool::DecorHand && t != HoodBins::Tool::DecorPick)
        return false;
    if (w <= 0 || h <= 0)
        return true;
    if (t == HoodBins::Tool::Decorate) {
        updateDecoration(camera, p, w, h);
        if (const hood::DecorProduct* product = decorProduct(ui_->bins().decorGuid()); product && decorSpot_)
            placeDecoration(*product, decorSpot_->x, decorSpot_->y, decorRotation_, true, p);
        return true;
    }
    const simd_float4x4 m = simd_mul(camera.projection(w / h), camera.view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = camera.eye();
    const auto g = view_->groundAt(eye, simd_normalize(q.xyz / q.w - eye));
    if (!g)
        return true;
    const auto hit = view_->decorationAt(g->x, g->y);
    if (!hit)
        return true;
    const hood::Occupants& o = view_->occupants();
    const uint32_t guid = hit->first ? o.flora[hit->second].guid : o.props[hit->second].guid;
    const float rot = hit->first ? o.flora[hit->second].rotation : o.props[hit->second].rotation;
    const hood::DecorProduct* product = decorProduct(guid);
    if (!product)
        return true;
    if (t == HoodBins::Tool::DecorPick) { // the Eyedropper: that product becomes the tool
        ui_->bins().setDecorSort(product->sort);
        ui_->bins().pickDecoration(guid);
        decorRotation_ = rot;
        ui_->prefetchDecorations();
        return true;
    }
    // The Hand: picks it up (it leaves the neighborhood) to put it down elsewhere.
    const hood::OccupantPosition at = hit->first ? o.flora[hit->second].at : o.props[hit->second].at;
    const auto r = hood::removeDecoration(view_->folder(), view_->placementWorld(), hit->first, hit->second, gameDir_);
    for (const auto& l : r.lines)
        std::printf("opensims2: %s\n", l.c_str());
    if (!r.ok)
        return true;
    decorUndo_.push_back({false, *product, at, rot});
    decorRedo_.clear();
    decorRotation_ = rot;
    view_->removeDecoration(hit->first, hit->second);
    ui_->bins().setDecorSort(product->sort);
    ui_->bins().pickDecoration(guid); // held: put down with the next click
    ui_->prefetchDecorations();
    return true;
}

void HoodMode::undoDecoration(bool redo) {
    auto& from = redo ? decorRedo_ : decorUndo_;
    auto& to = redo ? decorUndo_ : decorRedo_;
    if (from.empty())
        return;
    const DecorOp op = from.back();
    from.pop_back();
    // Undoing an add (or redoing a removal) takes the last record of its list away; the other puts it back.
    const bool remove = redo ? !op.added : op.added;
    bool ok = false;
    if (remove) {
        const hood::Occupants& o = view_->occupants();
        // The record is the one at the same place (the add put it last; a removal's may be anywhere).
        const bool flora = op.product.flora();
        const size_t n = flora ? o.flora.size() : o.props.size();
        for (size_t i = n; i-- > 0 && !ok;) {
            const auto& at = flora ? o.flora[i].at : o.props[i].at;
            const uint32_t guid = flora ? o.flora[i].guid : o.props[i].guid;
            if (guid == op.product.guid && at.position == op.at.position)
                if ((ok = hood::removeDecoration(view_->folder(), view_->placementWorld(), flora, i, gameDir_).ok))
                    view_->removeDecoration(flora, i);
        }
    } else {
        ok = hood::addDecoration(view_->folder(), view_->placementWorld(), op.product, op.at, op.rotation, gameDir_).ok;
        if (ok)
            view_->addDecoration(op.product, op.at, op.rotation);
    }
    std::printf("opensims2: decoration %s %s\n", redo ? "redo" : "undo", ok ? "done" : "failed");
    std::fflush(stdout);
    if (!ok)
        return;
    to.push_back(op);
}

bool HoodMode::placingLot() const {
    const HoodBins::Tool t = ui_->bins().tool();
    return t == HoodBins::Tool::Decorate || t == HoodBins::Tool::PlaceEmpty || t == HoodBins::Tool::PlaceBin || (t == HoodBins::Tool::MoveLot && movingLot_);
}

void HoodMode::turnLot(int direction) {
    if (!placingLot())
        return;
    if (ui_->bins().tool() == HoodBins::Tool::Decorate) { // quarter turns of the decoration
        decorRotation_ = std::fmod(decorRotation_ + (direction > 0 ? 90.0f : 270.0f), 360.0f);
        std::printf("opensims2: decoration turned to %.0f\n", decorRotation_);
        std::fflush(stdout);
        return;
    }
    const int from = turn_ >= 0 ? turn_ : pending_.any ? pending_.turn : 0;
    turn_ = (from + (direction > 0 ? 1 : 3)) & 3;
    std::printf("opensims2: lot turned to %d\n", turn_);
    std::fflush(stdout);
    if (lastTile_)
        pending_ = placementAt(lastTile_->x, lastTile_->y);
    view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
}

void HoodMode::updatePlacement(const LotCamera& camera, simd_float2 p, float w, float h) {
    const HoodBins::Tool t = ui_->bins().tool();
    const bool placing = t == HoodBins::Tool::PlaceEmpty || t == HoodBins::Tool::PlaceBin ||
                         (t == HoodBins::Tool::MoveLot && movingLot_);
    pending_ = {};
    updateDecoration(camera, p, w, h);
    if (placing)
        if (auto tile = tileAt(camera, p, w, h)) {
            // The lot's middle under the mouse.
            int cw = 3, cd = 3;
            if (t == HoodBins::Tool::PlaceEmpty) {
                const auto s = hood::emptyLotSize(ui_->bins().emptySize());
                cw = s.width, cd = s.depth;
            } else if (t == HoodBins::Tool::PlaceBin && ui_->bins().binLot() >= 0) {
                const auto& b = ui_->bins().lotBin()[size_t(ui_->bins().binLot())].base;
                cw = int(b.width), cd = int(b.depth);
            } else if (const hood::Lot* l = view_->neighborhood().lot(movingLot_)) {
                cw = int(l->info.currentWidth()), cd = int(l->info.currentDepth());
            }
            lastTile_ = simd_make_int2(tile->x - cw / 2, tile->y - cd / 2);
            pending_ = placementAt(lastTile_->x, lastTile_->y);
        }
    view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
}

bool HoodMode::place(const LotPlacement& p) {
    if (!p.any || !p.ok)
        return false;
    if (ui_->bins().tool() == HoodBins::Tool::PlaceEmpty) {
        armed_ = p;
        ui_->bins().openPlaceLot();
        return true;
    }
    return placeNamed(p, {}, 0);
}

bool HoodMode::placeNamed(const LotPlacement& p, const std::string& name, uint8_t zone) {
    if (!p.any || !p.ok)
        return false;
    const fs::path folder = view_->folder();
    const HoodBins& bins = ui_->bins();
    hood::LotResult r;
    if (bins.tool() == HoodBins::Tool::PlaceEmpty)
        r = hood::placeEmptyLot(folder, view_->placementWorld(), hood::emptyLotSize(bins.emptySize()), p.x, p.y,
                                p.turn, gameDir_, name, zone, gameDir_ / "TSData/Res/LotTemplates");
    else if (bins.tool() == HoodBins::Tool::PlaceBin)
        r = hood::placeBinLot(folder, view_->placementWorld(), bins.lotBin()[size_t(bins.binLot())], p.x, p.y, p.turn,
                              gameDir_);
    else if (bins.tool() == HoodBins::Tool::MoveLot && movingLot_)
        r = hood::moveLot(folder, view_->placementWorld(), movingLot_, p.x, p.y, p.turn, gameDir_);
    else
        return false;
    for (const auto& l : r.lines)
        std::printf("opensims2: %s\n", l.c_str());
    if (!r.ok) {
        std::printf("opensims2: %s\n", r.error.c_str());
        std::fflush(stdout);
        return false;
    }
    std::fflush(stdout);
    movingLot_ = 0;
    turn_ = -1;
    ui_->bins().cancelTool();
    lotsChanged();
    return true;
}

void HoodMode::lotsChanged() {
    view_->showPlacement(nullptr, false, 0);
    if (onReload)
        onReload();
    else
        refresh();
}

HoodMode::~HoodMode() = default;

std::unique_ptr<LotCamera> HoodMode::makeCamera() const {
    return view_->makeCamera(gameDir_ / "TSData/Res/UserData/Cameras/cameras.txt");
}

const HoodLot* HoodMode::lotAt(const LotCamera& camera, simd_float2 p, float w, float h) const {
    if (w <= 0 || h <= 0)
        return nullptr;
    const simd_float4x4 m = simd_mul(camera.projection(w / h), camera.view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = camera.eye();
    return view_->pick(eye, simd_normalize(q.xyz / q.w - eye));
}

std::optional<simd_float2> HoodMode::groundAt(const LotCamera& camera, simd_float2 p, float w, float h) const {
    if (w <= 0 || h <= 0)
        return std::nullopt;
    const simd_float4x4 m = simd_mul(camera.projection(w / h), camera.view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = camera.eye();
    const auto g = view_->groundAt(eye, simd_normalize(q.xyz / q.w - eye));
    if (!g)
        return std::nullopt;
    return simd_make_float2(g->x, g->y);
}

// RotateLot (BV 0x222b2c): a new front from the drag, the lot where it is.
void HoodMode::turnTo(int front) {
    if (front == turn_)
        return;
    turn_ = front;
    std::printf("opensims2: lot turned to %d by the drag\n", turn_);
    std::fflush(stdout);
    if (lastTile_)
        pending_ = placementAt(lastTile_->x, lastTile_->y);
    view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
}

void HoodMode::mouseDragged(const LotCamera& camera, simd_float2 p, float w, float h) {
    if (!pressGround_ || !placingLot() || decorating())
        return;
    if (auto g = groundAt(camera, p, w, h))
        if (auto f = hood::frontFromDrag(pressGround_->x, pressGround_->y, g->x, g->y))
            turnTo(*f);
}

void HoodMode::mouseUp(const LotCamera&, simd_float2, float, float) {
    if (!pressGround_)
        return;
    pressGround_.reset();
    if (placingLot() && !decorating())
        place(pending_); // invalid: the lot stays in hand
}

void HoodMode::dragTurn(float dx, float dy) {
    if (!pending_.any)
        return;
    lastTile_ = simd_make_int2(pending_.x, pending_.y);
    const float cx = float(pending_.rect.x0 + pending_.rect.x1) * hood::kHoodCellSize / 2;
    const float cy = float(pending_.rect.y0 + pending_.rect.y1) * hood::kHoodCellSize / 2;
    if (auto f = hood::frontFromDrag(cx, cy, cx + dx, cy + dy))
        turnTo(*f);
}

void HoodMode::mouseMoved(const LotCamera& camera, simd_float2 p, float w, float h) {
    ui_->setCanvas(int(w), int(h));
    ui_->hover(p);
    const HoodLot* lot = ui_->covers(p) ? nullptr : lotAt(camera, p, w, h);
    ui_->hoverLot(lot, p);
    view_->highlight(lot);
    updatePlacement(camera, p, w, h);
}

void HoodMode::mouseDown(LotCamera& camera, simd_float2 p, float w, float h) {
    ui_->setCanvas(int(w), int(h));
    const HoodUi::Action a = ui_->press(p);
    if (a == HoodUi::Action::EnterLot && ui_->dialogLot())
        enterLot(*ui_->dialogLot());
    if (a == HoodUi::Action::ZoomIn)
        camera.zoomBy(1);
    else if (a == HoodUi::Action::ZoomOut)
        camera.zoomBy(-1);
    else if (a == HoodUi::Action::RotateLeft)
        camera.rotateBy(-1);
    else if (a == HoodUi::Action::RotateRight)
        camera.rotateBy(1);
    if (a == HoodUi::Action::MainMenu && onMainMenu)
        onMainMenu();
    if (a == HoodUi::Action::Friends && ui_->dialogLot())
        showFriends(*ui_->dialogLot());
    if (a == HoodUi::Action::Bins)
        carryOut(ui_->binRequest());
    if (a != HoodUi::Action::None)
        return;
    if (ui_->dialogLot())
        return; // modal: clicks outside the dialog do nothing
    const HoodBins::Tool t = ui_->bins().tool();
    if (decorClick(camera, p, w, h))
        return;
    if (t == HoodBins::Tool::PlaceEmpty || t == HoodBins::Tool::PlaceBin || (t == HoodBins::Tool::MoveLot && movingLot_)) {
        // DoMouseDown (BV 0x222e20): the ground point kept, the lot put down on release.
        updatePlacement(camera, p, w, h);
        pressGround_ = groundAt(camera, p, w, h);
        if (!pressGround_) // off the terrain: as a click
            place(pending_);
        return;
    }
    if (const HoodLot* lot = lotAt(camera, p, w, h))
        lotClicked(*lot);
}

void HoodMode::lotClicked(const HoodLot& lot) {
    view_->showFriends(nullptr, {});
    std::printf("opensims2: lot %u \"%s\" (%s)\n", lot.id, lot.name.c_str(),
                lot.familyName.empty() ? "no family" : lot.familyName.c_str());
    std::fflush(stdout);
    if (ui_->bins().tool() == HoodBins::Tool::MoveLot) {
        // cTSUserToolMoveLot: the lot picked up, to be put down elsewhere.
        movingLot_ = lot.id;
        std::printf("opensims2: moving lot %u\n", lot.id);
        std::fflush(stdout);
        return;
    }
    if (ui_->bins().tool() != HoodBins::Tool::None) {
        ui_->bins().lotClicked(lot.id); // the tool's question (or why not)
        return;
    }
    ui_->openLot(&lot);
}

void HoodMode::carryOut(const HoodBins::Request& r) {
    using K = HoodBins::Request::Kind;
    const fs::path folder = view_->folder();
    if (r.kind == K::CreateFamily) {
        std::printf("opensims2: create a new family in %s\n", folder.string().c_str());
        std::fflush(stdout);
        if (onCreateFamily)
            onCreateFamily(folder);
        return;
    }
    if (r.kind == K::DecorUndo || r.kind == K::DecorRedo) {
        undoDecoration(r.kind == K::DecorRedo);
        return;
    }
    if (r.kind == K::MainMenu) {
        if (onMainMenu)
            onMainMenu();
        return;
    }
    if (r.kind == K::Quit) {
        if (onQuit)
            onQuit();
        return;
    }
    if (r.kind == K::DeleteBinLot) {
        const auto& bin = ui_->bins().lotBin();
        if (r.lot < bin.size()) {
            const bool ok = hood::deleteBinLot(bin[r.lot], gameDir_);
            std::printf("opensims2: %s %s from the Lots & Houses bin\n", ok ? "deleted" : "could not delete",
                        bin[r.lot].file.filename().string().c_str());
            std::fflush(stdout);
            ui_->bins().setLotBin(hood::loadLotBin(hood::playerLotCatalog(folder, gameDir_ / "TSData/Res/UserData/LotCatalog")));
        }
        return;
    }
    if (r.kind == K::PlaceLot) {
        placeNamed(armed_, ui_->bins().placeLotName(), ui_->bins().placeLotZone());
        return;
    }
    if (r.kind == K::ToBin) {
        const hood::LotResult b = hood::moveLotToBin(
            folder, r.lot, hood::playerLotCatalog(folder, gameDir_ / "TSData/Res/UserData/LotCatalog"), gameDir_);
        for (const auto& l : b.lines)
            std::printf("opensims2: %s\n", l.c_str());
        if (!b.ok) {
            std::printf("opensims2: %s\n", b.error.c_str());
            ui_->bins().showMessage(b.error);
        }
        std::fflush(stdout);
        if (b.ok)
            lotsChanged();
        return;
    }
    hood::MoveResult res;
    if (r.kind == K::MoveIn)
        res = hood::moveIn(folder, r.family, r.lot, gameDir_);
    else if (r.kind == K::MoveOut)
        res = hood::moveOut(folder, r.lot, gameDir_);
    else if (r.kind == K::Bulldoze)
        res = hood::bulldoze(folder, r.lot, gameDir_);
    else
        return;
    for (const auto& l : res.lines)
        std::printf("opensims2: %s\n", l.c_str());
    if (!res.ok) {
        std::printf("opensims2: %s\n", res.error.c_str());
        ui_->bins().showMessage(res.error);
    }
    std::fflush(stdout);
    refresh();
    // ConfirmAndMoveInFamily posts the transition to the lot: the family
    // goes in at once.
    if (res.ok && r.kind == K::MoveIn)
        if (const HoodLot* lot = view_->lot(r.lot))
            enterLot(*lot);
}

void HoodMode::refresh() {
    view_->reload();
    ui_->refresh();
}

void HoodMode::familyCreated(uint32_t family) {
    refresh();
    if (family) {
        if (ui_->bins().panel() != HoodBins::Panel::Families)
            ui_->bins().togglePanel(HoodBins::Panel::Families);
        ui_->bins().selectFamily(family);
    }
}

// "Ver Amigos da Família e Parentes" (the Lot Info Dialog's A003/B003/C003):
// the dialog closes and the lots where the family's friends and relatives live
// are highlighted (blue friends, violet relatives, a lot with both is a
// relative's). INFERRED: the binary's window procedure was not read; friends
// are the SRELs of the family's Sims with the friend or best-friend flag
// (bits 4, 5 of the flags value) and relatives those with the family flag
// (bit 14) or a familial relation.
// "Ver Amigos da Família e Parentes": cTSNeighborhood::ShowFamilyRelationshipsWeb
// (BV 0x4da814) with cTSNeighborManager::GetFamilyFriendsGuids(family, ..., true)
// (BV 0x53a480): every neighbor outside the family with a family of its own
// (person data 61) who, with some member, has relationships both ways and
// either both ways the friend bits (flags bit 4 or 5) or, on either side, a
// family relation (TestFriendsAndFamily, BV 0x537a50); the lots their
// families live on. Friends and relatives are not told apart.
void HoodMode::showFriends(const HoodLot& from) {
    std::vector<const HoodLot*> out;
    const hood::Neighborhood& h = view_->neighborhood();
    const hood::Family* fam = h.family(from.family);
    if (fam) {
        std::map<std::pair<uint16_t, uint16_t>, const hood::Relationship*> rel;
        for (const auto& r : h.relationships)
            rel[{r.from, r.to}] = &r;
        std::set<uint32_t> lots;
        for (const auto& n : h.neighbors) {
            if (n.family == 0 || n.family == from.family ||
                std::find(fam->members.begin(), fam->members.end(), n.id) != fam->members.end())
                continue;
            bool counts = false;
            for (uint16_t member : fam->members) {
                const hood::Neighbor* m = h.neighbor(member);
                if (!m)
                    continue;
                auto ab = rel.find({uint16_t(n.id), uint16_t(m->id)}), ba = rel.find({uint16_t(m->id), uint16_t(n.id)});
                if (ab == rel.end() || ba == rel.end())
                    continue;
                auto friendly = [](const hood::Relationship& r) { return (r.value(hood::relvar::Flags) & 0x30) != 0; };
                if ((friendly(*ab->second) && friendly(*ba->second)) || ab->second->value(hood::relvar::Familial) != 0 ||
                    ba->second->value(hood::relvar::Familial) != 0) {
                    counts = true;
                    break;
                }
            }
            if (!counts)
                continue;
            for (const auto& l : h.lots)
                if (std::find(l.families.begin(), l.families.end(), n.family) != l.families.end())
                    lots.insert(l.id);
        }
        for (const auto& l : view_->lots())
            if (lots.count(l.id))
                out.push_back(&l);
    }
    std::printf("opensims2: friends and relatives of the %s family: %zu lots\n", from.familyName.c_str(), out.size());
    std::fflush(stdout);
    ui_->openLot(nullptr);
    view_->showFriends(&from, out);
}

void HoodMode::escape() {
    view_->showFriends(nullptr, {});
    view_->showDecorationGhost(nullptr, 0, 0, 0, false);
    if (ui_->storyOpen()) {
        ui_->typeStoryKey(53, {}); // Esc drops an edit under way
        ui_->showStory(false);
        return;
    }
    movingLot_ = 0;
    turn_ = -1;
    pressGround_.reset();
    view_->showPlacement(nullptr, false, 0);
    if (ui_->bins().tool() != HoodBins::Tool::None)
        ui_->bins().cancelTool();
    else
        ui_->openLot(nullptr);
}

void HoodMode::enterLot(const HoodLot& lot) {
    std::printf("opensims2: entering lot %u (%s)\n", lot.id, lot.package.string().c_str());
    std::fflush(stdout);
    if (onEnterLot)
        onEnterLot(lot);
    else
        restartWith(hoodLotArguments(args_, query_, lot.id));
}

void HoodMode::scriptedView(LotCamera& camera, float w, float h) {
    auto find = [&](const char* flag) -> const HoodLot* {
        for (size_t i = 1; i + 1 < args_.size(); ++i)
            if (args_[i] == flag) {
                const HoodLot* l = view_->lot(uint32_t(std::stoul(args_[i + 1])));
                if (!l)
                    throw std::runtime_error(std::string(flag) + ": no lot " + args_[i + 1]);
                return l;
            }
        return nullptr;
    };
    ui_->setCanvas(int(w), int(h));
    auto value = [&](const char* flag) -> std::optional<std::string> {
        for (size_t i = 1; i + 1 < args_.size(); ++i)
            if (args_[i] == flag)
                return args_[i + 1];
        return std::nullopt;
    };
    if (auto p = value("--hood-panel"))
        ui_->bins().togglePanel(*p == "lots"      ? HoodBins::Panel::Lots
                                : *p == "options" ? HoodBins::Panel::Options
                                : *p == "decorations" ? HoodBins::Panel::Decorations
                                                  : HoodBins::Panel::Families);
    // --hood-options-window 1-5: the Options panel opens (it is the panel shown) and that window.
    if (auto wnd = value("--hood-options-window")) {
        if (ui_->bins().panel() != HoodBins::Panel::Options)
            ui_->bins().togglePanel(HoodBins::Panel::Options);
        ui_->refreshLayout();
        ui_->screen(int(w), int(h)); // lays the panel out: the window's corner
        ui_->options().toggleWindow(std::stoi(*wnd));
        ui_->refreshLayout();
    }
    // --hood-panel decorations, --hood-decor-sort <sort>, --hood-decor-place guid,tx,ty[,rotation]
    // (a placement at that tile's centre, as a click would), --hood-decor-pick guid (the tool).
    if (auto sort = value("--hood-decor-sort"))
        ui_->bins().setDecorSort(*sort);
    if (auto g = value("--hood-decor-pick"))
        ui_->bins().pickDecoration(uint32_t(std::stoul(*g)));
    if (auto spec = value("--hood-decor-place")) {
        std::vector<float> v;
        for (size_t at = 0; at < spec->size();) {
            const size_t comma = std::min(spec->find(',', at), spec->size());
            v.push_back(std::stof(spec->substr(at, comma - at)));
            at = comma + 1;
        }
        if (v.size() >= 3)
            if (const hood::DecorProduct* product = decorProduct(uint32_t(v[0]))) {
                const float wx = v[1] * hood::kHoodCellSize + 5, wy = v[2] * hood::kHoodCellSize + 5;
                camera.setPivot(simd_make_float3(wx, wy, view_->rawTerrain().heightAt(wx, wy)));
                camera.snap();
                const simd_float4 q = simd_mul(simd_mul(camera.projection(w / h), camera.view()),
                                               simd_make_float4(wx, wy, view_->rawTerrain().heightAt(wx, wy), 1));
                const simd_float2 at = simd_make_float2((q.x / q.w + 1) / 2 * w, (1 - q.y / q.w) / 2 * h);
                placeDecoration(*product, wx, wy, v.size() > 3 ? v[3] : 0, true, at);
            }
    }
    if (value("--hood-story"))
        ui_->showStory(true);
    if (auto text = value("--hood-story-text")) { // the editor, typing the text (\n is a new line); with ",save" Pronto
        ui_->showStory(true, true);
        std::string t = *text;
        const bool save = t.size() > 5 && t.compare(t.size() - 5, 5, ",save") == 0;
        if (save)
            t.resize(t.size() - 5);
        for (size_t i = 0; i < t.size(); ++i)
            if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == 'n')
                ui_->typeStoryKey(36, {}), ++i;
            else
                ui_->typeStoryKey(0, std::string(1, t[i]));
        if (save)
            ui_->showStory(false);
    }
    if (ui_->bins().panel() == HoodBins::Panel::Decorations)
        ui_->prefetchDecorations();
    if (auto f = value("--hood-family"))
        ui_->bins().selectFamily(uint32_t(std::stoul(*f)));
    if (auto t = value("--hood-tool"))
        ui_->bins().setTool(*t == "bulldoze" ? HoodBins::Tool::Bulldoze
                            : *t == "move"   ? HoodBins::Tool::MoveLot
                            : *t == "tobin"  ? HoodBins::Tool::ToBin
                                             : HoodBins::Tool::Evict);
    // --hood-place empty:<0-9>|bin:<n>|move:<lot>, then --hood-place-at x,y
    // (tiles: the footprint highlight there) or --hood-place-click x,y (put
    // it there).
    if (auto spec = value("--hood-place")) {
        const auto colon = spec->find(':');
        const std::string kind = spec->substr(0, colon);
        const int n = colon == std::string::npos ? 0 : std::stoi(spec->substr(colon + 1));
        if (kind == "empty") {
            ui_->bins().togglePanel(HoodBins::Panel::Lots);
            ui_->bins().setLotsPanel(1);
            ui_->bins().pickEmptyLot(n);
        } else if (kind == "bin") {
            ui_->bins().togglePanel(HoodBins::Panel::Lots);
            ui_->bins().setLotsPanel(0);
            ui_->bins().pickBinLot(size_t(n));
        } else if (kind == "move") {
            ui_->bins().togglePanel(HoodBins::Panel::Lots);
            ui_->bins().setTool(HoodBins::Tool::MoveLot);
            movingLot_ = uint32_t(n);
        }
    }
    // --hood-turn <0-3>: the turn the , and . keys would have chosen.
    if (auto t = value("--hood-turn"))
        turn_ = std::stoi(*t) & 3;
    for (const char* flag : {"--hood-place-at", "--hood-place-click"})
        if (auto at = value(flag)) {
            const auto comma = at->find(',');
            const int x = std::stoi(at->substr(0, comma)), y = std::stoi(at->substr(comma + 1));
            pending_ = placementAt(x, y);
            std::printf("opensims2: lot placement at (%d, %d): %s, rect (%d, %d)-(%d, %d), turn %d\n", x, y,
                        pending_.ok ? "ok" : pending_.why.c_str(), pending_.rect.x0, pending_.rect.y0, pending_.rect.x1,
                        pending_.rect.y1, pending_.turn);
            std::fflush(stdout);
            // --hood-drag dx,dy: the button held at the lot's middle and dragged (RotateLot).
            if (auto d = value("--hood-drag")) {
                const auto c = d->find(',');
                dragTurn(std::stof(d->substr(0, c)), std::stof(d->substr(c + 1)));
                std::printf("opensims2: after the drag: %s, rect (%d, %d)-(%d, %d), turn %d\n",
                            pending_.ok ? "ok" : pending_.why.c_str(), pending_.rect.x0, pending_.rect.y0,
                            pending_.rect.x1, pending_.rect.y1, pending_.turn);
                std::fflush(stdout);
            }
            view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
            camera.setPivot(simd_make_float3(float(pending_.rect.x0 + pending_.rect.x1) * 5.0f,
                                             float(pending_.rect.y0 + pending_.rect.y1) * 5.0f, pending_.height));
            camera.snap();
            if (std::string(flag) == "--hood-place-click") {
                place(pending_);
                // --hood-lot-name <name> [--hood-lot-zone community]: the
                // place-lot dialog filled in and accepted.
                if (auto n = value("--hood-lot-name"); n && ui_->bins().textEntry()) {
                    ui_->bins().setPlaceLotName(*n);
                    HoodBins::Request req = ui_->bins().typeKey(36, {});
                    if (req.kind == HoodBins::Request::Kind::PlaceLot)
                        placeNamed(armed_, ui_->bins().placeLotName(),
                                   value("--hood-lot-zone").value_or("") == "community" ? 1 : ui_->bins().placeLotZone());
                }
            }
        }
    if (const HoodLot* l = find("--hood-lot-click")) {
        camera.setPivot(view_->lotCentre(*l));
        camera.snap();
        lotClicked(*l);
    }
    const HoodLot* hover = find("--hood-hover");
    const HoodLot* select = find("--hood-select");
    const HoodLot* target = select ? select : hover;
    if (!target)
        return;
    camera.setPivot(view_->lotCentre(*target));
    camera.snap();
    if (hover) {
        // The mouse over the middle of the lot.
        const simd_float3 c = view_->lotCentre(*hover);
        const simd_float4 q = simd_mul(simd_mul(camera.projection(w / h), camera.view()), simd_make_float4(c, 1));
        const simd_float2 p = simd_make_float2((q.x / q.w + 1) / 2 * w, (1 - q.y / q.w) / 2 * h);
        ui_->hoverLot(lotAt(camera, p, w, h), p);
        view_->highlight(hover);
    }
    if (select) {
        ui_->openLot(select);
        if (auto i = value("--hood-info"))
            ui_->showInfo(true, size_t(std::stoul(*i)));
        if (value("--hood-package"))
            ui_->showPackage(true);
        if (value("--hood-friends"))
            showFriends(*select);
    }
}

namespace {

// Without the flags (and their values) in `drop`.
std::vector<std::string> without(const std::vector<std::string>& args, std::initializer_list<const char*> drop,
                                 std::initializer_list<const char*> dropBare = {}) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        bool skip = false;
        for (const char* d : drop)
            if (i > 0 && args[i] == d) {
                skip = true, ++i;
                break;
            }
        for (const char* d : dropBare)
            if (i > 0 && args[i] == d)
                skip = true;
        if (!skip)
            out.push_back(args[i]);
    }
    return out;
}

bool has(const std::vector<std::string>& args, const char* flag) {
    return std::find(args.begin() + (args.empty() ? 0 : 1), args.end(), flag) != args.end();
}

} // namespace

std::vector<std::string> hoodLotArguments(const std::vector<std::string>& args, const std::string& hood, uint32_t lot) {
    auto out = without(args, {"--hood", "--hood-lot", "--hood-hover", "--hood-select", "--screenshot", "--pivot",
                              "--zoom", "--orientation", "--lot", "--hood-panel", "--hood-family", "--hood-tool",
                              "--hood-lot-click", "--hood-place", "--hood-place-at", "--hood-place-click", "--hood-turn", "--hood-drag", "--hood-lot-name", "--hood-lot-zone", "--hood-info", "--hood-options-window", "--hood-package", "--hood-friends", "--hood-story", "--hood-story-text", "--hood-decor-sort", "--hood-decor-pick", "--hood-decor-place"});
    out.insert(out.end(), {"--hood", hood, "--hood-lot", std::to_string(lot)});
    if (!has(out, "--vm"))
        out.push_back("--vm");
    if (!has(out, "--ui"))
        out.insert(out.end(), {"--ui", "live"});
    return out;
}

std::vector<std::string> hoodArguments(const std::vector<std::string>& args, const std::string& hood) {
    auto out = without(args, {"--hood", "--hood-lot", "--hood-hover", "--hood-select", "--screenshot", "--pivot",
                              "--zoom", "--orientation", "--lot", "--ui"},
                       {"--vm"});
    out.insert(out.end(), {"--hood", hood});
    return out;
}

void restartWith(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::fflush(stdout);
    execv(argv[0], argv.data());
    std::perror("opensims2: restart");
    std::exit(1);
}

fs::path hoodLotPackage(const fs::path& resDir, const std::string& hood, uint32_t lot) {
    const fs::path dir = neighborhoodFolder(resDir, hood);
    const auto h = hood::Neighborhood::load(dir, false);
    const hood::Lot* l = h.lot(lot);
    if (!l)
        throw std::runtime_error("--hood-lot: neighborhood " + h.prefix + " has no lot " + std::to_string(lot));
    if (!l->package.empty() && fs::exists(l->package))
        return l->package;
    // A new lot (LoadLot steps 0xB-0x11): its file from the lot template.
    // Lots placed before placeEmptyLot wrote it (playtest 33: N001 lot 83).
    const auto r = hood::makeLotFile(dir, lot, resDir / "LotTemplates", resDir.parent_path().parent_path());
    for (const auto& line : r.lines)
        std::printf("opensims2: %s\n", line.c_str());
    std::fflush(stdout);
    if (!r.ok)
        throw std::runtime_error("--hood-lot: lot " + std::to_string(lot) + " has no lot file: " + r.error);
    const auto again = hood::Neighborhood::load(dir, false);
    const hood::Lot* made = again.lot(lot);
    if (!made || made->package.empty() || !fs::exists(made->package))
        throw std::runtime_error("--hood-lot: lot " + std::to_string(lot) + " has no lot file");
    return made->package;
}

} // namespace os2
