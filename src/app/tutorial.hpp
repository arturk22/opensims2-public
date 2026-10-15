#pragma once

// The tutorial in the app (docs/tutorial-mac.md): the episodes "Aprenda a
// Jogar" lists, and over a tutorial lot the tutorial manager's UI: the
// progress window the controller's Dialog 0x16/0x19 shows (ui.package
// 0x2CDB7CBD, top right), the bouncing arrow on the highlighted control,
// the controls it turns off, and the click trees (vm/tutorial.hpp). Plain
// C++ over the formats layer and the UI layer.

#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"
#include "vm/tutorial.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderCommandEncoder;
} // namespace MTL

namespace os2 {

class FontLibrary;
class ResourceIndex;
class UiLayer;
class UiResources;

// One episode of TSData/Res/Tutorial/EpisodeDefinitions.txt
// (cTSTutorialParser): its texts from Text/<TextResource>.package STR#
// <stringset> (title, description; one-based indices), its thumbnail (a UI
// image), the lot it plays ("*14": lot 14 of the neighborhood), its list
// (1 "O Básico", 2 "Além do Básico"), in the file's order.
struct TutorialEpisode {
    int number = 0;
    std::string title, description;
    UiResourceRef thumbnail;
    std::string neighborhood = "Tutorial";
    uint32_t lot = 0;
    int group = 1;
    // Global 26 "Tutorial State" of the lot's save in the user's copy
    // (RefreshEpisodeInfo): 2 = completed.
    int state = 0;
};

// The episodes, `userHood` (the user's copy of the Tutorial neighborhood,
// may be absent) giving their state.
std::vector<TutorialEpisode> loadTutorialEpisodes(const std::filesystem::path& resDir,
                                                  const std::filesystem::path& userHood);

// cTSGameStateController::ResetTutorialNeighborhood: the user's copy of the
// Tutorial neighborhood replaced by a fresh one from the game's data (every
// episode starts as shipped). `shipped` is the game's folder of it.
void resetTutorialNeighborhood(const std::filesystem::path& shipped, const std::filesystem::path& userCopy);

class TutorialOverlay {
public:
    TutorialOverlay(vm::VM& vm, ResourceIndex& index, const std::filesystem::path& resDir);
    ~TutorialOverlay();

    // The other panels' controls by the game's window ids (live panel, Buy,
    // Build): their rectangle on the canvas, if drawn.
    std::function<std::optional<std::array<float, 4>>(uint32_t window, uint32_t control)> locate;
    // UI Effect "press": the host clicks the control.
    std::function<void(uint32_t window, uint32_t control)> pressControl;

    // Takes the VM's new UI Effects and progress window (call every frame
    // and after each scripted step); `seconds` animates the arrow.
    void update(double seconds);
    // A click on the progress window: its buttons, or nothing (consumed).
    bool press(simd_float2 p);
    // Whether a click at p may reach the control under it (false: turned
    // off by the tutorial). clicked: the click trees of the controls at p.
    bool allows(simd_float2 p);
    void clicked(simd_float2 p);
    // Headless play along (--tutorial-auto): clicks the highlighted control
    // if a click on it runs a tree (through pressControl, as the mouse),
    // else the panel's Next if it is on; false if neither.
    bool autoClick();
    // Whether the progress window is up (and where).
    bool panelVisible() const;
    std::array<float, 4> panelRect() const { return panelRect_; }
    std::array<float, 4> nextRect() const { return nextRect_; }
    vm::TutorialUi& ui() { return ui_; }

    void setCanvas(int width, int height);
    UiScreen screen(int width, int height); // the panel, the arrow and the shading
#ifndef _WIN32
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);
#endif

private:
    uint32_t waitingSaid_ = 0; // the step autoClick last said it waits on
    void buildPanel(UiScreen& out, int width, int height);
    void buildArrow(UiScreen& out);

    vm::VM& vm_;
    vm::TutorialUi ui_;
    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    UiScript script_;
    bool ok_ = false;
    double now_ = 0, arrowSince_ = 0, pausedTime_ = 0;
    vm::TutorialUi::Key arrowFor_{0, 0};
    std::array<float, 4> panelRect_{}, nextRect_{}, backRect_{};
    uint32_t builtSerial_ = ~0u, printedSerial_ = 0;
    uint64_t builtState_ = 0;
    int canvasW_ = 1280, canvasH_ = 720, builtW_ = 0, builtH_ = 0;
    UiScreen panel_; // the progress window as last laid out
#ifndef _WIN32
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
#endif
    size_t layer_ = 0;
};

} // namespace os2
