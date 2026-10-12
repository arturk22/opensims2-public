#include "hood_mode.hpp"
#include "app/scene_loader.hpp"

#include "formats/hood_lots.hpp"
#include "formats/hood_moves.hpp"
#include "formats/neighborhood.hpp"
#include "render/camera.hpp"

#include <algorithm>
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
    loading::phase("hood: ui");
    // The player's Lots & Houses bin (filled from the game's the first time).
    ui_->bins().setLotBin(
        hood::loadLotBin(hood::playerLotCatalog(view_->folder(), res / "UserData/LotCatalog")));
    loading::phase("hood: bins");
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
        for (int k = 0; k < 4; ++k) {
            const hood::TileRect r = hood::emptyLotRect(size, x, y, k);
            const hood::Placement p = hood::checkPlacement(world, r);
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
    hood::Placement p = hood::checkPlacement(world, out.rect, self);
    if (p.ok)
        p.ok = false, p.why = "its roads are not on its road edges";
    finish(p);
    return out;
}

void HoodMode::updatePlacement(const LotCamera& camera, simd_float2 p, float w, float h) {
    const HoodBins::Tool t = ui_->bins().tool();
    const bool placing = t == HoodBins::Tool::PlaceEmpty || t == HoodBins::Tool::PlaceBin ||
                         (t == HoodBins::Tool::MoveLot && movingLot_);
    pending_ = {};
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
            pending_ = placementAt(tile->x - cw / 2, tile->y - cd / 2);
        }
    view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
}

bool HoodMode::place(const LotPlacement& p) {
    if (!p.any || !p.ok)
        return false;
    const fs::path folder = view_->folder();
    const HoodBins& bins = ui_->bins();
    hood::LotResult r;
    if (bins.tool() == HoodBins::Tool::PlaceEmpty)
        r = hood::placeEmptyLot(folder, view_->placementWorld(), hood::emptyLotSize(bins.emptySize()), p.x, p.y,
                                p.turn, gameDir_);
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
    if (a == HoodUi::Action::Bins)
        carryOut(ui_->binRequest());
    if (a != HoodUi::Action::None)
        return;
    if (ui_->dialogLot())
        return; // modal: clicks outside the dialog do nothing
    const HoodBins::Tool t = ui_->bins().tool();
    if (t == HoodBins::Tool::PlaceEmpty || t == HoodBins::Tool::PlaceBin || (t == HoodBins::Tool::MoveLot && movingLot_)) {
        updatePlacement(camera, p, w, h);
        place(pending_);
        return;
    }
    if (const HoodLot* lot = lotAt(camera, p, w, h))
        lotClicked(*lot);
}

void HoodMode::lotClicked(const HoodLot& lot) {
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

void HoodMode::escape() {
    movingLot_ = 0;
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
        ui_->bins().togglePanel(*p == "lots" ? HoodBins::Panel::Lots : HoodBins::Panel::Families);
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
    for (const char* flag : {"--hood-place-at", "--hood-place-click"})
        if (auto at = value(flag)) {
            const auto comma = at->find(',');
            const int x = std::stoi(at->substr(0, comma)), y = std::stoi(at->substr(comma + 1));
            pending_ = placementAt(x, y);
            std::printf("opensims2: lot placement at (%d, %d): %s, rect (%d, %d)-(%d, %d), turn %d\n", x, y,
                        pending_.ok ? "ok" : pending_.why.c_str(), pending_.rect.x0, pending_.rect.y0, pending_.rect.x1,
                        pending_.rect.y1, pending_.turn);
            std::fflush(stdout);
            view_->showPlacement(pending_.any ? &pending_.rect : nullptr, pending_.ok, pending_.height);
            camera.setPivot(simd_make_float3(float(pending_.rect.x0 + pending_.rect.x1) * 5.0f,
                                             float(pending_.rect.y0 + pending_.rect.y1) * 5.0f, pending_.height));
            camera.snap();
            if (std::string(flag) == "--hood-place-click")
                place(pending_);
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
    if (select)
        ui_->openLot(select);
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
                              "--hood-lot-click", "--hood-place", "--hood-place-at", "--hood-place-click"});
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
    if (l->package.empty() || !fs::exists(l->package))
        throw std::runtime_error("--hood-lot: lot " + std::to_string(lot) + " has no lot file");
    return l->package;
}

} // namespace os2
