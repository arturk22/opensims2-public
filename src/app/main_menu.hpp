#pragma once

// The game's main menu, which is also its neighborhood chooser, as ui.package
// defines it, drawn with the UI layer:
//
//   0x49001017  "Main menu" (root 0xCDA360CE, 702x562, centred): the
//               blurred neighborhood background 0x0DA36C7D (hidden in the
//               script; drawn here over the whole canvas), "Escolha uma
//               Vizinhança para Jogar", the thumbnail strip 0x1006
//               ("neighborhood chooser thumbs") with Previous 0x1004 / Next
//               0x1005, "Aprenda a Jogar" 0xA2, and at the bottom either
//               Quit 0xA5 (group 0x8DC06B6A "on initial load") or Close
//               0xA3 + Quit 0xA4 (group 0x3000 "from neighborhood"); the
//               shade 0x100A darkens it under the dialogs below
//   0x49001021  "NH Preview" (0xA001): a neighborhood's name 0x2003,
//               picture 0x2001 (its <P>_Neighborhood.png), description
//               0x2002, Play 0xB2, Close 0xB3 (Delete 0xB1 is hidden: the
//               shipped neighborhoods are not deleted here)
//   0x49001022  "Learn to play" (0xA002, cTSWinProcNeighborhoodLearn): the
//               episodes in two list boxes, "O Básico" 0x4000 (group 1) and
//               "Além do Básico" 0x5000 (group 2), the selected one's
//               thumbnail 0x3001 and description 0x3002, Play Tutorial 0xC2
//               (on only with an episode selected; the first is selected
//               when it opens), Close 0xC3 (app/tutorial.hpp)
//
// Inferred (the window procedure, cTSWinProcNeighborhoodMainMenu, is not
// decoded): three thumbnails per page in the strip, each the picture over the
// neighborhood's name; the dialogs over the menu at the root's origin; the
// background scaled to cover the canvas. Plain C++ over the formats layer.

#include "app/tutorial.hpp"
#include "formats/image.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
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

struct MainMenuHood {
    std::string prefix;            // "N001": what Play opens
    std::string name, description; // the neighborhood package's CTSS
    std::shared_ptr<RgbaImage> picture; // <P>_Neighborhood.png (null: none)
};

// The neighborhoods of `dirs` (each a neighborhood folder), read for the menu.
std::vector<MainMenuHood> readMainMenuHoods(const std::vector<std::filesystem::path>& dirs);

class MainMenu {
public:
    // `fromNeighborhood`: opened from a neighborhood (Close goes back to it).
    MainMenu(ResourceIndex& index, const std::filesystem::path& resDir, std::vector<MainMenuHood> hoods,
             bool fromNeighborhood = false);
    ~MainMenu();
    bool ok() const { return ok_; }

    enum class Action { None, Consumed, Play, Tutorial, Close, Quit };
    // A left click (canvas pixels). After Play, chosen() names the neighborhood.
    Action press(simd_float2 p);
    void hover(simd_float2 p);
    const std::string& chosen() const { return chosen_; }
    // The tutorial episodes of "Aprenda a Jogar"; after Tutorial,
    // chosenEpisode() is the one to play.
    void setEpisodes(std::vector<TutorialEpisode> episodes);
    const TutorialEpisode* chosenEpisode() const;
    // Opens "Aprenda a Jogar" (as its button does) and selects episode `i`.
    void openLearn(int episode = 0);
    // An episode's row in its list (after a layout), for scripts.
    std::array<float, 4> episodeRect(int i);
    // Opens the preview of hood `i` (as clicking its thumbnail; -1 closes).
    void preview(int i);
    int previewed() const { return preview_; }
    // The thumbnail of hood `i` on the canvas (after a layout), for scripts.
    std::array<float, 4> thumbnailRect(int i);

    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }
    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

    // UI sounds by name (button click sounds), if set.
    void setSoundPlayer(std::function<void(const std::string&)> play) { playSound_ = std::move(play); }

    static constexpr int kPerPage = 3;

private:
    enum class Target { Prev, Next, Thumb, LearnToPlay, Quit, Close, PreviewPlay, PreviewClose, TutorialPlay,
                        TutorialClose, Episode };
    struct Hit {
        std::array<float, 4> rect;
        Target target;
        int value = 0;
        const UiElement* element = nullptr;
    };
    void layoutIfNeeded();

    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    UiScript menu_, preview_script_, learn_;
    bool ok_ = false, fromHood_ = false;
    std::vector<MainMenuHood> hoods_;
    std::vector<std::unique_ptr<RgbaImage>> scaled_; // thumbnails made for the current screen
    int page_ = 0, preview_ = -1;
    bool learnOpen_ = false;
    std::vector<TutorialEpisode> episodes_;
    int episode_ = -1;                               // selected
    std::vector<std::array<float, 4>> episodeRects_; // by episode (empty: not shown)
    std::string chosen_;
    std::vector<Hit> hits_;
    std::vector<std::array<float, 4>> thumbs_; // by hood index (empty rect: not on the page)
    std::function<void(const std::string&)> playSound_;
    int canvasW_ = 1280, canvasH_ = 720, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true, uploaded_ = false; // uploaded_: the current layer holds the last screen()
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
