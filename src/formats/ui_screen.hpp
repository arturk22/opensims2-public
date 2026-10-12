#pragma once

#include "image.hpp"
#include "ui_script.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace os2 {

class FontLibrary;
class UiResources;

// A UI script laid out for drawing: textured or flat quads in canvas pixels
// (origin top-left, y down), back to front.
struct UiQuad {
    const RgbaImage* image = nullptr; // null: flat colour
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1; // normalized texture coordinates
    std::array<float, 4> color{1, 1, 1, 1}; // multiplies the texture
};

struct UiScreen {
    int width = 0, height = 0; // canvas
    std::vector<UiQuad> quads;
    std::vector<std::unique_ptr<RgbaImage>> textImages; // rendered captions (quads point at them)
    size_t windows = 0, drawn = 0, texts = 0, missingImages = 0;
    std::vector<std::string> warnings;
};

struct UiLayoutOptions {
    int canvasWidth = 1024, canvasHeight = 768;
    // Windows to show although the script hides them (ids). A shown window
    // larger than its parent is centred on the canvas instead (backgrounds).
    std::set<uint32_t> show;
};

// Per-window changes for code-driven screens (the live-mode panel): what the
// game's window procedures do to the windows of a script, keyed by element.
struct UiDrawHooks {
    std::set<const UiElement*> hide, show;               // override winflag_visible
    std::map<const UiElement*, const RgbaImage*> images; // in place of the image attribute (whole image)
    std::map<const UiElement*, int> buttonState;         // strip frame of a button: 0 disabled, 1 normal, 2 pressed, 3 hover
    std::map<const UiElement*, std::string> captions;    // in place of caption/captionres
    std::map<const UiElement*, std::array<float, 4>> colors; // multiplies the window's quads
    // Extra drawing after the window itself and before its children, given
    // the window's canvas rectangle.
    std::map<const UiElement*, std::function<void(UiScreen&, float x, float y, float w, float h)>> after;
    // Out: canvas rectangle (x0, y0, x1, y1) of every visible window laid out.
    std::map<const UiElement*, std::array<float, 4>>* rects = nullptr;
};

// A window's caption: its captionres in the game's language when there is
// one, else the caption attribute (the English one in the shipped scripts).
std::string uiCaption(const UiElement& e, UiResources& res);

// Lays out one window and its children with the window's area relative to
// (parentX, parentY), appending to `out` (whose canvas size is kept). Classes
// known only by GUID draw as their interface says (iid=IGZWinBtn: a button,
// IGZWinBMP: an image; the rest only their children).
void layoutUiElement(const UiElement& e, float parentX, float parentY, UiResources& res, FontLibrary& fonts,
                     UiScreen& out, const UiDrawHooks& hooks = {});

// Lays out every root window centred on the canvas: images (GZWinBMP, and
// GZWinGen by blttype: edge = nine-slice of the image in thirds, tiled,
// normal = stretched; imagerect picks a sub-rectangle), buttons (the image is
// a strip of four states: disabled, normal, pressed, hover; the normal one is
// drawn), flat rectangles (fillcolor unless style=nofill, outline colours),
// animations (class 0x4d9ccdb1: the first frame of a horizontal strip as wide
// as the window) and text (GZWinText/GZWinTextEdit and buttons with
// showcaption=yes: captionres in Brazilian Portuguese, else caption; font
// style from FontStyle.ini, forecolor, align, wrapped, gutters).
UiScreen layoutUi(const UiScript& script, UiResources& res, FontLibrary& fonts, const UiLayoutOptions& opt = {});

// Draws the quads on the CPU (nearest texel, alpha blended over `clear`): a
// reference for the renderer's UI layer and for s2pkg.
RgbaImage composeUi(const UiScreen& screen, std::array<uint8_t, 4> clear = {0, 0, 0, 255});

} // namespace os2
