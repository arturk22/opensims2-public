// The Dialog primitive's windows (dialog_window.hpp). Numbers marked
// "binary" are from the Mac BV executable (docs/primitives-mac.md "Dialog");
// the rest is inferred.

#include "app/dialog_window.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/font.hpp"
#include "formats/ui_resources.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace os2 {

namespace {

constexpr uint32_t kImageGroup = 0x499DB772;
// cTSWinPictureDialog::Init (binary): the frame (drawn in nine parts, the
// corners a third of the image) and the buttons' strip.
constexpr uint32_t kFrameImage = 0xA9500615, kButtonImage = 0xA9500630;
// cTSObjectDialog's icon (FUN_001d206c, binary): background, frame, alpha mask.
constexpr uint32_t kIconBack = 0xE9000005, kIconFrame = 0xE9000006, kIconMask = 0xE9000007;
// The close box's image 0x11110002 is not in ui.package: the phone books'
// cancel button (an X, 40x40 strip of four) stands in for it (inferred).
constexpr uint32_t kCloseImage = 0xAC76E000;
constexpr const char* kTitleFont = "GenHeader";    // 0x4A809916 (SetTitle)
constexpr const char* kTextFont = "GenDialogBody"; // 0x8D2A495C (DoLayout)
constexpr const char* kButtonFont = "GenButton";   // 0x4A809919 (AddButton)
constexpr size_t kTextEntryMax = 64;  // AddEditBox: vtable 0x84(0x40) (inferred: the box's length)
constexpr size_t kNameMax = 18;       // maxtext of the name script's boxes
constexpr int kRowsList = 0, kRowsInvite = 1; // party lists (region value >> 16)

// Phone book window ids (scripts 0x8C159242-44).
constexpr uint32_t kPhoneOk = 0xAAAA, kPhoneCancel = 0xBBBB;
// Name script ids.
constexpr uint32_t kNameOk = 0xA000, kNameCancel = 0xA001;

UiElement element(const char* clsid, const char* iid, float x0, float y0, float x1, float y1) {
    UiElement e;
    e.tag = "LEGACY";
    e.set("clsid", clsid);
    e.set("iid", iid);
    e.set("area", uiArea(int(std::lround(x0)), int(std::lround(y0)), int(std::lround(x1)), int(std::lround(y1))));
    e.set("winflag_visible", "yes");
    return e;
}

std::string imageRef(uint32_t instance) {
    char b[32];
    std::snprintf(b, sizeof b, "{499db772,%08x}", instance);
    return b;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// Mac key codes.
constexpr int kReturn = 36, kEnter = 76, kEscape = 53, kBackspace = 51, kSpace = 49, kTab = 48;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

bool contains(const std::vector<int16_t>& v, int16_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

} // namespace

DialogWindow::DialogWindow(vm::VM&, UiResources* res, FontLibrary* fonts) : res_(res), fonts_(fonts) {}
DialogWindow::~DialogWindow() = default;

const UiScript* DialogWindow::script(uint32_t id) {
    if (!res_)
        return nullptr;
    auto it = scripts_.find(id);
    if (it == scripts_.end()) {
        std::optional<UiScript> s;
        try {
            s = res_->script(id);
        } catch (const std::exception&) {
        }
        if (s && s->roots.empty())
            s.reset();
        it = scripts_.emplace(id, std::move(s)).first;
    }
    return it->second ? &*it->second : nullptr;
}

float DialogWindow::textWidth(const std::string& style, const std::string& s) const {
    const FontStyle* st = fonts_ ? fonts_->style(style) : nullptr;
    const Font* f = st ? fonts_->font(*st) : nullptr;
    return f ? measureText(*f, decodeUtf8(s), float(st->size)).width : 7.0f * float(decodeUtf8(s).size());
}

float DialogWindow::lineHeight(const std::string& style) const {
    const FontStyle* st = fonts_ ? fonts_->style(style) : nullptr;
    const Font* f = st ? fonts_->font(*st) : nullptr;
    if (!f)
        return 16;
    const TextMetrics m = measureText(*f, U"", float(st->size));
    return std::round(m.ascent + m.descent); // as the UI layout draws a line
}

// Greedy word wrap, as the UI layout's text windows do it.
std::vector<std::string> DialogWindow::wrap(const std::string& style, const std::string& text, float width) const {
    std::vector<std::string> lines;
    std::string para;
    auto flush = [&] {
        std::string line;
        size_t i = 0;
        while (i <= para.size()) {
            const size_t sp = std::min(para.find(' ', i), para.size());
            const std::string word = para.substr(i, sp - i);
            const std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(style, next) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = next;
            }
            i = sp + 1;
        }
        lines.push_back(line);
        para.clear();
    };
    for (char ch : text) {
        if (ch == '\r')
            continue;
        if (ch == '\n')
            flush();
        else
            para += ch;
    }
    flush();
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

bool DialogWindow::hasTextBox(const vm::Dialog& d) {
    return (d.cls.cls == vm::DialogClass::Object && d.type == vm::dialogtype::TextEntry) ||
           (d.cls.cls == vm::DialogClass::NameObject && !d.names.empty());
}

std::string& DialogWindow::editText(vm::Dialog& d) {
    if (d.cls.cls == vm::DialogClass::NameObject && !d.names.empty())
        return d.names[size_t(std::clamp(d.nameFocus, 0, int(d.names.size()) - 1))].text;
    return d.text;
}

bool DialogWindow::okEnabled(const vm::Dialog& d) {
    if (d.cls.cls == vm::DialogClass::NameObject)
        for (const auto& n : d.names)
            if (trim(n.text).empty())
                return false;
    return true;
}

void DialogWindow::answer(vm::Dialog& d, int index) {
    if (index < 0 || index >= int(d.buttons.size()))
        return;
    if (index == 0 && !okEnabled(d))
        return;
    if (index == 0 && d.cls.cls == vm::DialogClass::Clothing && applyOutfit)
        if (const Outfits* o = wardrobe(d)) {
            auto pick = [&](int t) {
                const auto& l = o->lists[size_t(t)];
                const int c = o->current[size_t(t)];
                return c >= 0 && c < int(l.size()) ? l[size_t(c)].part : std::string();
            };
            if (o->tab == 2)
                applyOutfit(d.outfitSim, d.outfitCategory, {}, {}, pick(2));
            else
                applyOutfit(d.outfitSim, d.outfitCategory, pick(0), pick(1), {});
        }
    d.answer = index;
    if (log) {
        std::printf("opensims2: dialog \"%s\": %s\n", d.title.c_str(), d.buttons[size_t(index)].c_str());
        std::fflush(stdout);
    }
}

// The 44x44 icon of cTSObjectDialog (FUN_001d206c): the 40x40 thumbnail
// centred on the background, the frame over it, the mask's alpha.
const RgbaImage* DialogWindow::icon(const vm::Dialog& d) {
    if (auto it = icons_.find(d.id); it != icons_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> out;
    const RgbaImage* thumb = nullptr;
    if (d.iconImage && res_)
        thumb = res_->image(UiResourceRef{kTypeUiImage, kImageGroup, d.iconImage});
    else if ((d.iconObject || d.iconNeighbor) && thumbnail)
        thumb = thumbnail(d.iconObject, d.iconNeighbor);
    const RgbaImage* back = res_ ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, kIconBack}) : nullptr;
    const RgbaImage* frame = res_ ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, kIconFrame}) : nullptr;
    const RgbaImage* mask = res_ ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, kIconMask}) : nullptr;
    if (thumb && thumb->width && back && back->width) {
        out = std::make_unique<RgbaImage>(*back);
        auto blend = [&](const RgbaImage& src, int ox, int oy, int w, int h) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int dx = ox + x, dy = oy + y;
                    if (dx < 0 || dy < 0 || dx >= int(out->width) || dy >= int(out->height))
                        continue;
                    // Scaled to w x h (nearest texel).
                    const uint32_t sx = uint32_t(x) * src.width / uint32_t(w), sy = uint32_t(y) * src.height / uint32_t(h);
                    const uint8_t* s = &src.pixels[(size_t(sy) * src.width + sx) * 4];
                    uint8_t* p = &out->pixels[(size_t(dy) * out->width + size_t(dx)) * 4];
                    const float a = float(s[3]) / 255;
                    for (int k = 0; k < 3; ++k)
                        p[k] = uint8_t(std::lround(float(s[k]) * a + float(p[k]) * (1 - a)));
                    p[3] = uint8_t(std::max<int>(p[3], s[3]));
                }
        };
        const int tw = 40, th = 40;
        blend(*thumb, (int(out->width) - tw) / 2, (int(out->height) - th) / 2, tw, th);
        if (frame && frame->width == out->width && frame->height == out->height)
            blend(*frame, 0, 0, int(frame->width), int(frame->height));
        if (mask && mask->width == out->width && mask->height == out->height)
            for (size_t i = 3; i < out->pixels.size(); i += 4)
                out->pixels[i] = mask->pixels[i];
    }
    return (icons_[d.id] = std::move(out)).get();
}

const RgbaImage* DialogWindow::frame0(uint32_t image) {
    if (auto it = frames_.find(image); it != frames_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> out;
    if (const RgbaImage* im = res_ ? res_->image(UiResourceRef{kTypeUiImage, kImageGroup, image}) : nullptr;
        im && im->width >= 4) {
        out = std::make_unique<RgbaImage>();
        out->width = im->width / 4, out->height = im->height;
        out->pixels.resize(size_t(out->width) * out->height * 4);
        for (uint32_t y = 0; y < out->height; ++y)
            std::copy_n(&im->pixels[size_t(y) * im->width * 4], size_t(out->width) * 4, &out->pixels[size_t(y) * out->width * 4]);
    }
    return (frames_[image] = std::move(out)).get();
}

// --- cTSObjectDialog: cTSWinPictureDialog::DoLayout ---------------------------

DialogWindow::Layout DialogWindow::pictureDialog(const vm::Dialog& d, int width, int height) {
    Layout L;
    const bool hasTitle = !d.title.empty();
    // get_title_area_size: the title text (one line, up to 750 px wide).
    const float tw = hasTitle ? std::min(750.0f, textWidth(kTitleFont, d.title)) : 0;
    const float th = hasTitle ? lineHeight(kTitleFont) : 0;
    float w = 128;
    if (hasTitle)
        w = std::max(w, tw + 52);
    // FUN_0025b1fc: the buttons as wide as their text + 16, at least 82,
    // the ones within 20 px of a wider one made as wide; 12 px apart.
    const bool closeBox = d.continueOthers;
    std::vector<float> bw;
    for (size_t i = 0; i < d.buttons.size(); ++i)
        bw.push_back(closeBox && i == 0 ? 40.0f : textWidth(kButtonFont, d.buttons[i]) + 16);
    {
        float prev = 1e9f;
        for (;;) {
            float widest = 0;
            for (float b : bw)
                if (b > widest && b < prev)
                    widest = b;
            if (widest == 0)
                break;
            float low = widest - 20;
            if (widest < 82)
                widest = 82, low = 0;
            for (size_t i = 0; i < bw.size(); ++i)
                if (!(closeBox && i == 0) && bw[i] < widest && bw[i] > low)
                    bw[i] = widest;
            prev = widest;
        }
    }
    float bTotal = 0;
    for (size_t i = 0; i < bw.size(); ++i)
        bTotal += bw[i] + (i ? 12 : 0);
    const float bh = 34;
    w = std::max(w, bTotal + 52);
    // The text: at least the title's or the buttons' width and its widest
    // word + 8, then widened 32 px at a time while it is taller than the
    // limit (64, growing by 32 up to 500) and not past 700.
    float textW = std::max(128.0f, tw);
    {
        float widestWord = 0;
        std::string word;
        for (char ch : d.message + " ") {
            if (ch == ' ' || ch == '\n' || ch == '\r') {
                widestWord = std::max(widestWord, textWidth(kTextFont, word));
                word.clear();
            } else {
                word += ch;
            }
        }
        textW = std::max({textW, widestWord + 8, bTotal});
    }
    const RgbaImage* img = icon(d);
    const float iw = img ? float(img->width) : 0, ih = img ? float(img->height) : 0;
    if (img)
        textW = std::max(128.0f, textW - iw - 8);
    const float lh = lineHeight(kTextFont);
    std::vector<std::string> lines = wrap(kTextFont, d.message, textW);
    float limit = 64;
    while (lines.size() >= 2 && float(lines.size()) * lh > limit && textW <= 700) {
        textW += 32;
        limit = std::min(limit + 32, 500.0f);
        lines = wrap(kTextFont, d.message, textW);
    }
    if (lines.size() < 2)
        textW = (lines.empty() ? 0 : textWidth(kTextFont, lines[0])) + 1;
    const float textH = float(std::max<size_t>(lines.size(), d.message.empty() ? 0 : 1)) * lh;
    const float tx = img ? iw + 34 : 26;
    const float ty = hasTitle ? th + 26 : 18;
    w = std::max(w, img ? iw + textW + 60 : textW + 52);
    // The edit box between the text and the buttons; the buttons 12 px under
    // the text, not higher than 118 - their height.
    float by = std::max(ty + textH + 12, 118 - bh);
    const bool edit = d.type == vm::dialogtype::TextEntry;
    const float eh = lh + 12;
    std::array<float, 4> editRect{26, by, w - 26, by + eh};
    if (edit)
        by += eh + 8;
    const float H = by + bh + 14;
    w = std::round(w);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - H) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + H;
    L.lines = lines;

    UiElement root = element("GZWinGen", "IGZWinGen", 0, 0, w, H);
    UiElement frame = element("GZWinBMP", "IGZWinBMP", 0, 0, w, H);
    frame.set("image", imageRef(kFrameImage));
    frame.set("edgeimage", "yes");
    root.children.push_back(frame);
    auto text = [&](uint32_t id, const char* font, float x0, float y0, float x1, float y1) {
        UiElement t = element("GZWinText", "IGZWinText", x0, y0, x1, y1);
        t.set("id", hex(id));
        t.set("font", font);
        t.set("align", "lefttop");
        t.set("wrapped", "yes");
        t.set("forecolor", "(5,6,113)");
        t.set("gutters", "(5,5)");
        return t;
    };
    if (hasTitle) {
        root.children.push_back(text(1, kTitleFont, 21 - 5, 13 - 5, 21 + tw + 6, 13 + th + 5));
        L.captions[1] = d.title;
    }
    if (img) {
        UiElement p = element("GZWinBMP", "IGZWinBMP", 26, hasTitle ? th + 28 : 20, 26 + iw, (hasTitle ? th + 28 : 20) + ih);
        p.set("id", hex(3));
        root.children.push_back(p);
        L.images[3] = img;
    }
    root.children.push_back(text(2, kTextFont, tx - 5, ty - 5, tx + textW + 6, ty + textH + 5));
    L.captions[2] = d.message;
    if (edit) {
        UiElement box = element("GZWinFlatRect", "IGZWinFlatRect", editRect[0], editRect[1], editRect[2], editRect[3]);
        box.set("fillcolor", "(204,214,255)");
        for (const char* side : {"colorleft", "colortop", "colorright", "colorbottom"})
            box.set(side, "(5,6,113)");
        root.children.push_back(box);
        UiElement t = text(4, kTextFont, editRect[0], editRect[1], editRect[2], editRect[3]);
        t.set("wrapped", "no");
        t.set("align", "leftcenter");
        root.children.push_back(t);
        L.captions[4] = d.text;
        L.textBox = {L.x0 + editRect[0], L.y0 + editRect[1], L.x0 + editRect[2], L.y0 + editRect[3]};
        L.caretText = d.text;
        L.caretFont = kTextFont;
    }
    float bx = std::round((w - bTotal) / 2);
    for (size_t i = 0; i < d.buttons.size(); ++i) {
        const uint32_t id = 0x10 + uint32_t(i);
        UiElement b = element("GZWinBtn", "IGZWinBtn", bx, by, bx + bw[i], by + bh);
        b.set("id", hex(id));
        if (closeBox && i == 0) {
            b.set("image", imageRef(kCloseImage));
            b.set("showcaption", "no");
        } else {
            b.set("image", imageRef(kButtonImage));
            b.set("showcaption", "yes");
            b.set("font", kButtonFont);
            b.set("colorfontnormal", "(5,6,113)");
            b.set("colorfonthilited", "(5,6,113)");
            b.set("align", "center");
            L.captions[id] = d.buttons[i];
        }
        root.children.push_back(b);
        L.buttonIds.push_back(id);
        L.regions.push_back({{L.x0 + bx, L.y0 + by, L.x0 + bx + bw[i], L.y0 + by + bh},
                             closeBox && i == 0 ? Hit::Close : Hit::Button, int(i)});
        bx += bw[i] + 12;
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcNameObjectDialog -----------------------------------------------

DialogWindow::Layout DialogWindow::nameDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x2D91050A);
    const UiElement* panel = s ? s->roots.front().child(d.namePanel) : nullptr;
    if (!panel && s)
        panel = s->roots.front().child(0xCDC2CAA4);
    if (!panel)
        return plainDialog(d, width, height);
    Layout L;
    const auto a = panel->area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = *panel;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    root.set("winflag_visible", "yes");
    // onAttach: the girl/boy icon of each newborn (0x1A-0x1D / 0x2A-0x2D).
    for (auto& c : root.children) {
        const uint32_t id = c.id();
        for (size_t i = 0; i < d.names.size() && i < 4; ++i) {
            if (id == 0x1A + i)
                c.set("winflag_visible", d.names[i].female ? "yes" : "no");
            if (id == 0x2A + i)
                c.set("winflag_visible", d.names[i].female ? "no" : "yes");
        }
        if (c.clsid() == "GZWinTextEdit" && id >= 1 && id <= 4) {
            const size_t i = id - 1;
            if (i < d.names.size()) {
                L.captions[id] = d.names[i].text;
                const auto r = c.area();
                L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])},
                                     Hit::NameBox, int(i)});
                if (int(i) == d.nameFocus) {
                    L.textBox = L.regions.back().rect;
                    L.caretText = d.names[i].text;
                    L.caretFont = c.get("font", "NeighborhoodButton");
                }
            }
        }
        if (id == kNameOk || id == kNameCancel) {
            const auto r = c.area();
            const int index = id == kNameOk ? 0 : 1;
            L.buttonIds.push_back(id);
            L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])},
                                 Hit::Button, index});
        }
    }
    // The twins' header wraps to two lines in Portuguese: its window reaches
    // down to the text under it (a visual fix; the script's is one line).
    if (d.namePanel == 0xCDC2CAA6)
        if (UiElement* head = root.child(0x8DC2D562)) {
            auto r = head->area();
            head->set("area", uiArea(r[0], r[1], r[2], r[3] + 10));
        }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcPhoneBook* -----------------------------------------------------

DialogWindow::Layout DialogWindow::phoneDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(d.cls.script);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    using C = vm::DialogClass;
    // The window's panel: a GZWinBMP without an image under the frame
    // overlay, filled with its fillcolor (inferred: the overlay's middle is
    // transparent; the main menu's lists sit on the same teal).
    for (auto& c : root.children)
        if (c.clsid() == "GZWinBMP" && !c.attr("image")) {
            c.set("clsid", "GZWinFlatRect");
            c.set("iid", "IGZWinFlatRect");
        }
    // The texts, as each class's PreShow / SetSelectedSim fills them: the
    // dialog's strings, else the script's own (UIText STR# 0xAB).
    const vm::DialogEntry* chosen = nullptr;
    const int16_t chosenId = d.selection.empty() ? int16_t(0) : d.selection[0];
    for (const auto& e : d.entries)
        if (e.id == chosenId)
            chosen = &e;
    auto callFormat = [&](const std::string& own) {
        std::string format = own;
        if (format.empty() && res_)
            if (auto t = res_->string(UiStringRef{0x7FA7DC7C, 0xAB, 15}))
                format = *t;
        return format.empty() ? std::string("Ligar para %s") : format;
    };
    auto sprintfName = [&](std::string format, const std::string& name) {
        if (auto p = format.find("%s"); p != std::string::npos)
            format.replace(p, 2, name);
        return format;
    };
    if (d.cls.cls == C::PhoneServices) {
        if (!d.title.empty())
            L.captions[0x1000] = d.title;
        L.captions[0x2000] = chosen ? sprintfName(callFormat(d.message), chosen->name) : "";
    } else if (d.cls.cls == C::PhoneNeighbors) {
        if (!d.message.empty())
            L.captions[0xA000] = d.message;
        L.captions[0x6000] = chosen ? sprintfName(callFormat(d.title), chosen->name) : "";
        L.captions[0x7000] = chosen ? chosen->detail : "";
    } else {
        if (!d.title.empty())
            L.captions[0xA000] = d.title;
        if (!d.message.empty())
            L.captions[0xB000] = d.message;
        if (!d.caption2.empty())
            L.captions[0xD000] = d.caption2;
        if (UiElement* limit = findWindow(root, 0xC000)) {
            if (d.caption.empty())
                limit->set("winflag_visible", "no");
            else
                L.captions[0xC000] = d.caption;
        }
    }
    // The list boxes' look: the script's constants table 0xCCCC.
    std::map<std::string, std::string> k = uiConstants(*s);
    auto color = [&](const char* name, std::array<float, 4> fallback) {
        int r, g, b;
        if (auto it = k.find(name); it != k.end() && std::sscanf(it->second.c_str(), "%d,%d,%d", &r, &g, &b) == 3)
            return std::array<float, 4>{float(r) / 255, float(g) / 255, float(b) / 255, 1};
        return fallback;
    };
    auto number = [&](const char* name, uint32_t fallback) {
        auto it = k.find(name);
        return it == k.end() ? fallback : uint32_t(std::stoul(it->second, nullptr, 0));
    };
    const float rowH = float(number("kListBoxRowHeight", 20));
    const auto textNormal = color("kListBoxTextColorNormal", {0.02f, 0.02f, 0.44f, 1});
    const auto textSelected = color("kListBoxTextColorSelected", {0, 1, 0, 1});
    const auto rowSelected = color("kListBoxSelectedRowColor", {0.43f, 0.65f, 0.71f, 1});
    const std::string listFont = hex(number("kListBoxFont", 0x8DE3D64F));
    const uint32_t iconFriend = number("kListBoxIconFriend", 0xA9590161), iconEnemy = number("kListBoxIconEnemy", 0xA9590157);
    const uint32_t iconCrush = number("kListBoxIconCrush", 0xA9590166), iconLove = number("kListBoxIconLove", 0xA9590171);

    struct ListRows {
        uint32_t window = 0;
        int list = 0;
        std::vector<const vm::DialogEntry*> rows;
        int16_t focus = 0;
    };
    std::vector<ListRows> lists;
    // A party starts on the first neighbor (PreShow selects row 0 of 0x5000).
    if (d.cls.cls == C::PhoneParty && !partyFocus_.count(d.id))
        partyFocus_[d.id] = kRowsList << 16;
    const int focus = partyFocus_[d.id];
    if (d.cls.cls == C::PhoneParty) {
        ListRows left{0x5000, kRowsList, {}, 0}, right{0x6000, kRowsInvite, {}, 0};
        for (const auto& e : d.entries)
            if (!contains(d.selection, e.id))
                left.rows.push_back(&e);
        for (int16_t id : d.selection)
            for (const auto& e : d.entries)
                if (e.id == id)
                    right.rows.push_back(&e);
        if (focus >= 0) {
            ListRows& l = (focus >> 16) == kRowsInvite ? right : left;
            const size_t row = size_t(focus & 0xFFFF);
            if (row < l.rows.size())
                l.focus = l.rows[row]->id, chosen = l.rows[row];
        }
        lists = {left, right};
    } else {
        ListRows l{d.cls.cls == C::PhoneServices ? 0x4000u : 0x8001u, kRowsList, {}, chosenId};
        for (const auto& e : d.entries)
            l.rows.push_back(&e);
        lists = {l};
    }
    // The chosen row's thumbnail (services 0x3001, neighbors 0x5002, party 0x9002).
    const uint32_t thumbId = d.cls.cls == C::PhoneServices ? 0x3001 : d.cls.cls == C::PhoneNeighbors ? 0x5002 : 0x9002;
    if (chosen && thumbnail) {
        const bool object = d.cls.cls == C::PhoneServices;
        if (const RgbaImage* t = thumbnail(object ? chosen->id : int16_t(0), object ? int16_t(0) : chosen->id))
            L.images[thumbId] = t;
    }
    // Hidden by the class until it fills them: the grid view of the
    // neighbors (0x9000) stays hidden in the list view.
    for (const auto& list : lists) {
        const UiElement* box = findWindow(root, list.window);
        if (!box)
            continue;
        // The rows, drawn after the list box window (its canvas rectangle is
        // found by walking the parents' areas).
        float ox = 0, oy = 0;
        std::function<bool(const UiElement&, float, float)> find = [&](const UiElement& e, float px, float py) {
            const auto r = e.area();
            if (&e == box) {
                ox = px + float(r[0]), oy = py + float(r[1]);
                return true;
            }
            for (const auto& c : e.children)
                if (find(c, px + float(r[0]), py + float(r[1])))
                    return true;
            return false;
        };
        for (const auto& c : root.children)
            if (find(c, 0, 0))
                break;
        const auto r = box->area();
        const float bw = float(r[2] - r[0]), bhh = float(r[3] - r[1]);
        for (size_t i = 0; i < list.rows.size() && float(i + 1) * rowH <= bhh + 0.5f; ++i) {
            const vm::DialogEntry& e = *list.rows[i];
            const float ry = oy + float(i) * rowH;
            const bool on = e.id == list.focus && (d.cls.cls != C::PhoneParty || focus >= 0);
            if (on) {
                UiElement sel = element("GZWinFlatRect", "IGZWinFlatRect", ox, ry, ox + bw, ry + rowH);
                char c[48];
                std::snprintf(c, sizeof c, "(%d,%d,%d)", int(rowSelected[0] * 255), int(rowSelected[1] * 255),
                              int(rowSelected[2] * 255));
                sel.set("fillcolor", c);
                root.children.push_back(sel);
            }
            UiElement t = element("GZWinText", "IGZWinText", ox + 4, ry, ox + bw - 44, ry + rowH);
            const uint32_t id = 0x00F00000u | uint32_t(list.list << 12) | uint32_t(i);
            t.set("id", hex(id));
            t.set("font", listFont);
            t.set("align", "leftcenter");
            const auto& tc = on ? textSelected : textNormal;
            char c[48];
            std::snprintf(c, sizeof c, "(%d,%d,%d)", int(tc[0] * 255), int(tc[1] * 255), int(tc[2] * 255));
            t.set("forecolor", c);
            root.children.push_back(t);
            L.captions[id] = e.name;
            // SetMode: the relationship icons at the row's right (friend or
            // enemy, crush or love; which column is which is inferred).
            auto iconAt = [&](uint32_t image, float x) {
                if (!res_ || !image)
                    return;
                const RgbaImage* im = frame0(image);
                if (!im || !im->width)
                    return;
                UiElement p = element("GZWinBMP", "IGZWinBMP", x, ry + std::round((rowH - float(im->height)) / 2),
                                      x + float(im->width), ry + std::round((rowH - float(im->height)) / 2) + float(im->height));
                const uint32_t pid = 0x00E00000u | uint32_t(list.list << 12) | uint32_t(i << 1) | (x > ox + bw - 22 ? 1u : 0u);
                p.set("id", hex(pid));
                root.children.push_back(p);
                L.images[pid] = im;
            };
            if (d.cls.cls != C::PhoneServices) {
                constexpr int Crush = 1, Love = 2, Friend = 16, BestFriend = 32, Enemy = 128;
                iconAt(e.flags & Enemy ? iconEnemy : e.flags & (Friend | BestFriend) ? iconFriend : 0, ox + bw - 42);
                iconAt(e.flags & Love ? iconLove : e.flags & Crush ? iconCrush : 0, ox + bw - 20);
            }
            L.regions.push_back({{L.x0 + ox, L.y0 + ry, L.x0 + ox + bw, L.y0 + ry + rowH}, Hit::Row,
                                 (list.list << 16) | int(i)});
        }
    }
    auto region = [&](uint32_t id, Hit kind, int value) {
        float ox = 0, oy = 0;
        const UiElement* found = nullptr;
        std::function<void(const UiElement&, float, float)> walk = [&](const UiElement& e, float px, float py) {
            const auto r = e.area();
            if (e.id() == id && !found) {
                found = &e, ox = px + float(r[0]), oy = py + float(r[1]);
                return;
            }
            for (const auto& c : e.children)
                walk(c, px + float(r[0]), py + float(r[1]));
        };
        for (const auto& c : root.children)
            walk(c, 0, 0);
        if (!found)
            return;
        const auto r = found->area();
        L.regions.push_back({{L.x0 + ox, L.y0 + oy, L.x0 + ox + float(r[2] - r[0]), L.y0 + oy + float(r[3] - r[1])}, kind, value});
        if (kind == Hit::Button)
            L.buttonIds.push_back(id);
    };
    region(kPhoneOk, Hit::Button, 0);
    region(kPhoneCancel, Hit::Button, 1);
    if (d.cls.cls == C::PhoneParty) {
        region(0x2000, Hit::Add, 0);
        region(0x1000, Hit::Remove, 0);
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcClothingDialog ------------------------------------------------

DialogWindow::Outfits* DialogWindow::wardrobe(const vm::Dialog& d) {
    auto it = wardrobes_.find(d.id);
    if (it == wardrobes_.end()) {
        std::optional<Outfits> o;
        if (outfits)
            o = outfits(d.outfitSim, d.outfitCategory);
        it = wardrobes_.emplace(d.id, std::move(o)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// Script 0x8C159250: the Sim view (here the chosen part's catalog
// thumbnail, inferred: the game renders the Sim in the outfit), the tabs
// tops / bottoms / whole outfits (0xECC70000-2), previous / next
// (0x8CC731A0/1), OK 0x6CC7318B and cancel 0x6CC73195.
DialogWindow::Layout DialogWindow::wardrobeDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x8C159250);
    if (!s)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    Outfits* o = wardrobe(d);
    auto rect = [&](uint32_t id) -> std::optional<std::array<float, 4>> {
        if (const UiElement* e = root.child(id)) {
            const auto r = e->area();
            return std::array<float, 4>{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        }
        return std::nullopt;
    };
    if (o) {
        const auto& list = o->lists[size_t(o->tab)];
        const int cur = o->current[size_t(o->tab)];
        if (cur >= 0 && cur < int(list.size()) && list[size_t(cur)].thumb)
            if (UiElement* view = root.child(0x2CC7315A)) {
                const auto r = view->area();
                const RgbaImage* t = list[size_t(cur)].thumb;
                const float vw = float(r[2] - r[0]), vh = float(r[3] - r[1]);
                const float k = std::min(vw / float(t->width), vh / float(t->height));
                const float tw = std::round(float(t->width) * k), th = std::round(float(t->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", float(r[0]) + std::round((vw - tw) / 2),
                                      float(r[1]) + std::round((vh - th) / 2), float(r[0]) + std::round((vw - tw) / 2) + tw,
                                      float(r[1]) + std::round((vh - th) / 2) + th);
                p.set("id", hex(0x2CC7315B));
                root.children.insert(root.children.begin() + 2, p);
                L.images[0x2CC7315B] = t;
            }
    }
    for (int t = 0; t < 3; ++t)
        if (auto r = rect(0xECC70000u + uint32_t(t)))
            L.regions.push_back({*r, Hit::Tab, t});
    if (auto r = rect(0x8CC731A0))
        L.regions.push_back({*r, Hit::Prev, 0});
    if (auto r = rect(0x8CC731A1))
        L.regions.push_back({*r, Hit::Next, 0});
    if (auto r = rect(0x6CC7318B)) {
        L.regions.push_back({*r, Hit::Button, 0});
        L.buttonIds.push_back(0x6CC7318B);
    }
    if (auto r = rect(0x6CC73195)) {
        L.regions.push_back({*r, Hit::Button, 1});
        L.buttonIds.push_back(0x6CC73195);
    }
    L.root = std::move(root);
    return L;
}

// --- the other special dialogs: an OK/cancel box (not done yet) -----------

DialogWindow::Layout DialogWindow::plainDialog(const vm::Dialog& d, int width, int height) {
    vm::Dialog copy = d;
    copy.cls = vm::DialogClassInfo{};
    copy.type = vm::dialogtype::Message;
    copy.iconObject = copy.iconNeighbor = 0;
    copy.iconImage = 0;
    if (copy.message.empty())
        copy.message = d.typeName.empty() ? d.cls.name : d.typeName;
    return pictureDialog(copy, width, height);
}

DialogWindow::Layout DialogWindow::layout(const vm::Dialog& d, int width, int height) {
    switch (d.cls.cls) {
    case vm::DialogClass::Object: return pictureDialog(d, width, height);
    case vm::DialogClass::NameObject: return nameDialog(d, width, height);
    case vm::DialogClass::PhoneServices:
    case vm::DialogClass::PhoneNeighbors:
    case vm::DialogClass::PhoneParty: return phoneDialog(d, width, height);
    case vm::DialogClass::Clothing: return wardrobeDialog(d, width, height);
    default: return plainDialog(d, width, height);
    }
}

void DialogWindow::draw(UiScreen& out, const vm::Dialog& d, int width, int height) {
    const Layout L = layout(d, width, height);
    if (!res_ || !fonts_ || L.root.tag.empty())
        return;
    UiDrawHooks hk;
    std::map<uint32_t, const UiElement*> byId;
    std::function<void(const UiElement&)> index = [&](const UiElement& e) {
        if (e.attr("id"))
            byId.emplace(e.id(), &e);
        for (const auto& c : e.children)
            index(c);
    };
    index(L.root);
    for (const auto& [id, text] : L.captions)
        if (auto it = byId.find(id); it != byId.end())
            hk.captions[it->second] = text;
    for (const auto& [id, img] : L.images)
        if (auto it = byId.find(id); it != byId.end()) {
            hk.images[it->second] = img;
            hk.show.insert(it->second);
        }
    // Buttons: hover lights the one under the mouse; OK is off while a name
    // box is empty (EnableOkButton).
    std::vector<size_t> buttonRegions;
    for (size_t r = 0; r < L.regions.size(); ++r)
        if (L.regions[r].kind == Hit::Button || L.regions[r].kind == Hit::Close)
            buttonRegions.push_back(r);
    for (size_t i = 0; i < L.buttonIds.size() && i < buttonRegions.size(); ++i) {
        auto it = byId.find(L.buttonIds[i]);
        if (it == byId.end())
            continue;
        const Region& g = L.regions[buttonRegions[i]];
        int state = int(buttonRegions[i]) == hover_ ? 3 : 1;
        if (g.value == 0 && !okEnabled(d))
            state = 0;
        hk.buttonState[it->second] = state;
    }
    // The wardrobe's tab in use looks pressed.
    if (d.cls.cls == vm::DialogClass::Clothing)
        if (const Outfits* o = wardrobe(d))
            for (int t = 0; t < 3; ++t)
                if (auto it = byId.find(0xECC70000u + uint32_t(t)); it != byId.end())
                    hk.buttonState[it->second] = t == o->tab ? 2 : 1;
    // The caret in the text box, blinking every half second (the scripts'
    // caretperiod=1000).
    if (L.textBox[2] > L.textBox[0]) {
        const bool on = std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                                  1.0) < 0.5;
        const float tx = textWidth(L.caretFont, L.caretText);
        const auto box = L.textBox;
        layoutUiElement(L.root, L.x0, L.y0, *res_, *fonts_, out, hk);
        if (on) {
            UiQuad q;
            q.x0 = std::round(box[0] + 5 + tx + 1), q.x1 = q.x0 + 1;
            q.y0 = box[1] + 4, q.y1 = box[3] - 4;
            q.color = {5 / 255.0f, 6 / 255.0f, 113 / 255.0f, 1};
            out.quads.push_back(q);
        }
        return;
    }
    layoutUiElement(L.root, L.x0, L.y0, *res_, *fonts_, out, hk);
}

void DialogWindow::hover(const vm::Dialog& d, float x, float y, int width, int height) {
    const Layout L = layout(d, width, height);
    int h = -1;
    for (size_t i = 0; i < L.regions.size(); ++i)
        if (insideRect(L.regions[i].rect, x, y))
            h = int(i);
    hover_ = h;
}

bool DialogWindow::click(vm::Dialog& d, float x, float y, int width, int height) {
    const Layout L = layout(d, width, height);
    for (const Region& g : L.regions) {
        if (!insideRect(g.rect, x, y))
            continue;
        switch (g.kind) {
        case Hit::Button:
        case Hit::Close: answer(d, g.value); break;
        case Hit::NameBox: d.nameFocus = g.value; break;
        case Hit::Row: {
            const int list = g.value >> 16, row = g.value & 0xFFFF;
            if (d.cls.cls == vm::DialogClass::PhoneParty) {
                partyFocus_[d.id] = g.value;
            } else if (row < int(d.entries.size())) {
                (void)list;
                d.selection = {d.entries[size_t(row)].id};
            }
            break;
        }
        case Hit::Add:
        case Hit::Remove: {
            // onBtnClickedID 0x2000 / 0x1000: the focused row moves to the
            // other list (a party takes at most temp 5 guests; inferred: the
            // limit is checked here).
            auto it = partyFocus_.find(d.id);
            if (it == partyFocus_.end())
                break;
            const int list = it->second >> 16, row = it->second & 0xFFFF;
            std::vector<int16_t> left;
            for (const auto& e : d.entries)
                if (!contains(d.selection, e.id))
                    left.push_back(e.id);
            if (g.kind == Hit::Add && list == kRowsList && row < int(left.size()) &&
                (d.maxSelection <= 0 || int(d.selection.size()) < d.maxSelection)) {
                d.selection.push_back(left[size_t(row)]);
                left.erase(left.begin() + row);
                if (left.empty())
                    partyFocus_.erase(it);
                else
                    it->second = (kRowsList << 16) | std::min(row, int(left.size()) - 1);
            } else if (g.kind == Hit::Remove && list == kRowsInvite && row < int(d.selection.size())) {
                d.selection.erase(d.selection.begin() + row);
                if (d.selection.empty())
                    partyFocus_.erase(it);
                else
                    it->second = (kRowsInvite << 16) | std::min(row, int(d.selection.size()) - 1);
            }
            break;
        }
        case Hit::Sort: break;
        case Hit::Tab:
        case Hit::Prev:
        case Hit::Next:
            // onBtnClickedID: a tab shows its list; the arrows step through it.
            if (Outfits* o = wardrobe(d)) {
                if (g.kind == Hit::Tab)
                    o->tab = g.value;
                const int n = int(o->lists[size_t(o->tab)].size());
                int& cur = o->current[size_t(o->tab)];
                if (n > 0 && g.kind == Hit::Prev)
                    cur = (cur + n - 1) % n;
                if (n > 0 && g.kind == Hit::Next)
                    cur = (cur + 1) % n;
            }
            break;
        }
        return true;
    }
    return true; // modal
}

bool DialogWindow::key(vm::Dialog& d, int keyCode, const std::string& typed) {
    const bool yesNo = d.cls.cls == vm::DialogClass::Object &&
                       (d.type == vm::dialogtype::YesNo || d.type == vm::dialogtype::YesNoCancel);
    if (keyCode == kReturn || keyCode == kEnter) {
        answer(d, 0);
        return true;
    }
    if (keyCode == kEscape) {
        // SetKey(last button, Esc): the message's and the text entry's OK,
        // no / cancel; the special dialogs' cancel (inferred).
        if (d.cls.cls == vm::DialogClass::Object || d.buttons.size() > 1)
            answer(d, int(d.buttons.size()) - 1);
        return true;
    }
    if (!hasTextBox(d)) {
        if (keyCode == kSpace && d.cls.cls == vm::DialogClass::Object && !yesNo)
            answer(d, 0); // SetKey(button 1, space) outside the yes/no dialogs
        return true;
    }
    if (keyCode == kTab && d.cls.cls == vm::DialogClass::NameObject && !d.names.empty()) {
        d.nameFocus = (d.nameFocus + 1) % int(d.names.size());
        return true;
    }
    std::string& text = editText(d);
    const int box = d.cls.cls == vm::DialogClass::NameObject ? d.nameFocus : -1;
    if (edited_ != d.id || editedBox_ != box) {
        // The default name is selected (SetEditTextControl selects it all):
        // the first key replaces it.
        edited_ = d.id, editedBox_ = box;
        if (d.cls.cls == vm::DialogClass::NameObject)
            text.clear();
    }
    if (keyCode == kBackspace) {
        while (!text.empty() && (uint8_t(text.back()) & 0xC0) == 0x80)
            text.pop_back();
        if (!text.empty())
            text.pop_back();
        return true;
    }
    const size_t max = d.cls.cls == vm::DialogClass::NameObject ? kNameMax : kTextEntryMax;
    for (char c : typed)
        if (uint8_t(c) >= 0x20 && c != 0x7F && text.size() < max)
            text += c;
    return true;
}

} // namespace os2
