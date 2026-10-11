#include "hood_mode.hpp"

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
    const HoodLot* lot = lotAt(camera, p, w, h);
    ui_->hoverLot(lot, p);
    view_->highlight(lot);
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
    if (const HoodLot* lot = lotAt(camera, p, w, h))
        lotClicked(*lot);
}

void HoodMode::lotClicked(const HoodLot& lot) {
    std::printf("opensims2: lot %u \"%s\" (%s)\n", lot.id, lot.name.c_str(),
                lot.familyName.empty() ? "no family" : lot.familyName.c_str());
    std::fflush(stdout);
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
        ui_->bins().setTool(*t == "bulldoze" ? HoodBins::Tool::Bulldoze : HoodBins::Tool::Evict);
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
                              "--hood-lot-click"});
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
