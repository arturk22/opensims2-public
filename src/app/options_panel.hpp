#pragma once

// The option windows of the lot's options panel (the puck's "Opções - F5":
// app/live_ui.hpp draws the panel 0x49060001 and its Neighborhood, Quit and
// Save buttons; its Audio 0xA242 and Game 0xA243 buttons open these), as
// ui.package defines them, drawn with the UI layer:
//
//   0x49060F01  "Opções de Áudio": a slider per group (Voices 0xB31, Sound
//               Effects 0xB01, Music 0xB41, Ambient 0xB21) and a mute box
//               per group (0x10 voice, 0x11 effects, 0x12 music, 0x13
//               ambience; the script's "EnableVoice"... buttons, tip "Mudo")
//   0x49060F02  "Opções de Jogo": Edge Scrolling ("radio:edgeScrolling:
//               bool:0:1", 0xA1: Off 1, On 2)
//
// What maps to the app: the four group volumes and mutes (the lot audio's
// mixer groups) and edge scrolling, kept in the user's Options.txt
// (app/user_data.hpp). The other controls do nothing yet. Inferred: a window
// opens with its bottom left on the options panel's (found by the panel's
// "4bdeffaa:bl" anchor over the puck, as live_ui places it): the art's lower
// left tab is as tall as the panel, which stays drawn over it; the sliders
// are drawn as their thumb on a flat track (GZWinScrollbar2 is not drawn by
// the UI layer yet). Plain C++.

#include "app/user_data.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <simd/simd.h>

#include <array>
#include <filesystem>
#include <functional>
#include <map>
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

class OptionsPanel {
public:
    OptionsPanel(ResourceIndex& index, const std::filesystem::path& resDir, GameSettings settings);
    ~OptionsPanel();
    bool ok() const { return ok_; }

    // Opens window 1 (audio) or 2 (game options); the open one closes.
    void toggleWindow(int window);
    bool active() const { return window_ != 0; }

    enum class Action { None, Consumed };
    // A left click (canvas pixels) on the open window.
    Action press(simd_float2 p);
    void setCanvas(int width, int height) { canvasW_ = width, canvasH_ = height, dirty_ = true; }

    const GameSettings& settings() const { return settings_; }
    // Called when an option changes (with the new settings).
    std::function<void(const GameSettings&)> onChanged;
    std::function<void(const std::string&)> playSound;
    // 0 none, 1 audio, 2 game options.
    int window() const { return window_; }
    void closeWindow(); // also when the options panel closes

    UiScreen screen(int width, int height);
    void attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat);
    void encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale);

private:
    enum class Target { Slider, Mute, EdgeOff, EdgeOn, Window };
    struct Hit {
        std::array<float, 4> rect;
        Target target;
        int value = 0;
        const UiElement* element = nullptr;
    };
    void changed();

    std::unique_ptr<UiResources> res_;
    std::unique_ptr<FontLibrary> fonts_;
    UiScript puck_, panel_, audio_, game_;
    bool ok_ = false;
    GameSettings settings_;
    int window_ = 0;
    std::vector<Hit> hits_;
    int canvasW_ = 1280, canvasH_ = 720, laidW_ = 0, laidH_ = 0;
    bool dirty_ = true, uploaded_ = false; // uploaded_: the current layer holds the last screen()
    std::array<std::unique_ptr<UiLayer>, 4> layers_;
    size_t layer_ = 0;
};

} // namespace os2
