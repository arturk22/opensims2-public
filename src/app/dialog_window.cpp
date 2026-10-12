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

namespace {
// cTSWinProcFillDialog::CalculateCostOfUnits: 50 units a cost step,
// rounded up, plus the delivery.
int fillCost(const vm::Dialog& d, int units) { return (units * d.fillCost + 49) / 50 + d.fillDelivery; }
std::string simoleons(int v) { return "\xC2\xA7" + std::to_string(v); }
} // namespace

bool DialogWindow::okEnabled(const vm::Dialog& d) {
    // UpdateAmounts: OK only once something is bought.
    if (d.cls.cls == vm::DialogClass::Fill)
        return d.fillValue > d.fillUnits;
    // cTSWinProcSetAspirationDialog::Validate: an aspiration picked.
    if (d.cls.cls == vm::DialogClass::SetAspiration)
        return d.aspiration != 0 && d.aspiration != 0x40 && d.aspiration != 0x100;
    // The rack: something in the cart that the family can pay (inferred:
    // SyncUI's EnableOkButton).
    if (d.cls.cls == vm::DialogClass::ClothingPurchase)
        return !d.cart.empty() && int(d.cart.size()) * d.pricePerItem <= d.fillFunds;
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
    if (index == 0 && d.cls.cls == vm::DialogClass::ClothingTryOn && applyOutfit) {
        // OnOk: the try-on outfit (0x400) as chosen saved in the character file.
        const Shop& s = shop(d);
        if (!s.chosen[2].empty())
            applyOutfit(d.outfitSim, 0x400, {}, {}, s.chosen[2]);
        else
            applyOutfit(d.outfitSim, 0x400, s.chosen[0], s.chosen[1], {});
    }
    if (d.cls.cls == vm::DialogClass::Vanity && vanityDone)
        vanityDone(d.outfitSim, index == 0);
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
        box.set("outline", "yes");
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

// --- cTSWinProcSetAspirationDialog ------------------------------------------

// Script 0xCC16A49E: "%s is no longer a child..." (0x2D9B8E78 for a girl,
// 0x2D9B8E7E for a boy), the Sim's head (0x2D9C8C9E), the five
// aspirations (0x0CDC000n = bit 1 << (n - 1)) and Accept 0xA000. The
// slide with the aspiration's description (0x2000) is not shown yet.
DialogWindow::Layout DialogWindow::aspirationDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(d.type == 0x22 ? 0xCAFE0045 : 0xCC16A49E);
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
    for (auto& c : root.children) {
        const uint32_t id = c.id();
        if (id == 0x2000)
            c.set("winflag_visible", "no");
        if (id == 0x2D9B8E78 || id == 0x2D9B8E7E) {
            const bool mine = (id == 0x2D9B8E78) == d.female;
            c.set("winflag_visible", mine ? "yes" : "no");
            if (mine) {
                std::string text = c.get("caption");
                if (res_)
                    if (auto ref = uiStringRef(c.get("captionres")))
                        if (auto t = res_->string(*ref))
                            text = *t;
                if (auto p = text.find("%s"); p != std::string::npos)
                    text.replace(p, 2, d.text); // the Sim's first name
                L.captions[id] = text;
            }
        }
        if ((id & 0xFFFF0000u) == 0x0CDC0000u) {
            const auto r = c.area();
            L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])}, Hit::Choice,
                                 1 << ((id & 0xFFFF) - 1)});
        }
        if (id == 0x2D9C8C9E && thumbnail)
            if (const RgbaImage* head = thumbnail(0, d.aspirationNeighbor))
                if (UiElement* t = c.child(1)) {
                    // The head inside the round frame (the window's image's alpha).
                    const auto ref = uiResourceRef(t->get("image"));
                    const RgbaImage* mask = ref && res_ ? res_->image(*ref) : nullptr;
                    auto& slot = icons_[d.id ^ 0x80000000u];
                    if (!slot && mask && mask->width) {
                        slot = std::make_unique<RgbaImage>(*mask);
                        for (uint32_t y = 0; y < slot->height; ++y)
                            for (uint32_t x = 0; x < slot->width; ++x) {
                                const uint32_t sx = x * head->width / slot->width, sy = y * head->height / slot->height;
                                const uint8_t* src = &head->pixels[(size_t(sy) * head->width + sx) * 4];
                                uint8_t* p = &slot->pixels[(size_t(y) * slot->width + x) * 4];
                                p[0] = src[0], p[1] = src[1], p[2] = src[2];
                                p[3] = uint8_t(int(p[3]) * int(src[3]) / 255);
                            }
                    }
                    t->set("id", hex(0x2D9C8C01));
                    L.images[0x2D9C8C01] = slot ? slot.get() : head;
                }
        if (id == 0xA000) {
            const auto r = c.area();
            L.regions.push_back({{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])}, Hit::Button, 0});
            L.buttonIds.push_back(0xA000);
        }
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcFillDialog ----------------------------------------------------

// Script 0x2CB242F8 ("Comprar Comida"): the fill bar (0xCCB5FD79: up 2,
// down 1, the bar 3) from the units the Sim has to the most it takes, the
// food cost (0x4DADF755), the delivery (0x4CB613E7) and the total
// (0x4CB613F8), OK 0xACB50208 and cancel 0xACB50200. The bar's step per
// click (10 units) and dragging on it are inferred.
DialogWindow::Layout DialogWindow::fillDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x2CB242F8);
    if (!s || d.type != 14)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = s->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    const int added = std::max(0, d.fillValue - d.fillUnits);
    const int total = fillCost(d, added);
    L.captions[0x4DADF755] = simoleons(total - d.fillDelivery);
    L.captions[0x4CB613E7] = simoleons(d.fillDelivery);
    L.captions[0x4CB613F8] = simoleons(total);
    L.captions[0x0CB613B4] = std::to_string(added);
    std::array<float, 4> fillRect{0, 0, 0, 0};
    for (auto& c : root.children) {
        const auto r = c.area();
        const std::array<float, 4> rr{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        if (c.id() == 0xCCB5FD79) {
            for (auto& k : c.children) {
                const auto q = k.area();
                const std::array<float, 4> kr{rr[0] + float(q[0]), rr[1] + float(q[1]), rr[0] + float(q[2]), rr[1] + float(q[3])};
                if (k.id() == 2)
                    L.regions.push_back({kr, Hit::Next, 0});
                if (k.id() == 1)
                    L.regions.push_back({kr, Hit::Prev, 0});
                if (k.id() == 3) {
                    // The bar filled from the bottom to the value, in the
                    // dark blue of the "gradient" strip 0x8E900055 (how the
                    // control cuts that strip is not known: a flat fill).
                    const float full = float(q[3] - q[1]);
                    const float f = d.fillMax > 0 ? std::clamp(float(d.fillValue) / float(d.fillMax), 0.0f, 1.0f) : 0;
                    fillRect = {float(q[0]) + 3, float(q[3]) - std::round(full * f), float(q[2]) - 3, float(q[3])};
                    L.regions.push_back({kr, Hit::Bar, 0});
                }
            }
        }
        if (c.id() == 0xCCB5FD79 && fillRect[3] > fillRect[1]) {
            UiElement bar = element("GZWinFlatRect", "IGZWinFlatRect", fillRect[0], fillRect[1], fillRect[2], fillRect[3]);
            bar.set("fillcolor", "(5,6,113)");
            c.children.push_back(bar);
        }
        if (c.id() == 0xACB50208) {
            L.regions.push_back({rr, Hit::Button, 0});
            L.buttonIds.push_back(0xACB50208);
        }
        if (c.id() == 0xACB50200) {
            L.regions.push_back({rr, Hit::Button, 1});
            L.buttonIds.push_back(0xACB50200);
        }
    }
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcSelectItemsDialog ---------------------------------------------

namespace {
// What the racks' picks cost.
int pickedCost(const vm::Dialog& d) {
    int sum = 0;
    for (const auto& e : d.entries)
        if (contains(d.selection, e.id))
            sum += e.price;
    return sum;
}
constexpr float kItem = 64, kItemMargin = 5; // the grid's item (inferred size; margins from the script)
} // namespace

// Script 0x2CB242F6 ("Fazer Seleção"): one row of the items' thumbnails in
// 0x1006 (kRowCount=1, kItemMarginX/Y=5), previous / next (0x1004/5), OK
// 0xA000, cancel 0xA001. An item the family cannot pay is greyed out when
// picking several (UpdateFunds).
DialogWindow::Layout DialogWindow::selectItemsDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* s = script(0x2CB242F6);
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
    int& first = pages_[d.id];
    const int spent = pickedCost(d);
    std::vector<UiElement> extra;
    for (auto& c : root.children) {
        const auto r = c.area();
        const std::array<float, 4> rr{L.x0 + float(r[0]), L.y0 + float(r[1]), L.x0 + float(r[2]), L.y0 + float(r[3])};
        const uint32_t id = c.id();
        if (id == 0x1006) {
            const float gw = float(r[2] - r[0]), gh = float(r[3] - r[1]);
            const int per = std::max(1, int((gw + kItemMargin) / (kItem + kItemMargin)));
            first = std::clamp(first, 0, std::max(0, int(d.entries.size()) - per));
            const float used = float(per) * kItem + float(per - 1) * kItemMargin;
            for (int i = 0; i < per && first + i < int(d.entries.size()); ++i) {
                const vm::DialogEntry& e = d.entries[size_t(first + i)];
                const float x = float(r[0]) + std::round((gw - used) / 2) + float(i) * (kItem + kItemMargin);
                const float y = float(r[1]) + std::round((gh - kItem) / 2);
                const bool picked = contains(d.selection, e.id);
                const bool affordable = !d.multiSelect || picked ||
                                        e.price <= (d.obeyPrice ? d.fillFunds - spent : d.fillFunds);
                if (picked) {
                    // A green frame (the lists' selected colour; inferred look).
                    for (int k = 0; k < 2; ++k) {
                        UiElement sel = element("GZWinFlatRect", "IGZWinFlatRect", x - 2 + float(k), y - 2 + float(k),
                                                x + kItem + 2 - float(k), y + kItem + 2 - float(k));
                        sel.set("style", "nofill");
                        sel.set("outline", "yes");
                        for (const char* side : {"colorleft", "colortop", "colorright", "colorbottom"})
                            sel.set(side, "(0,255,0)");
                        extra.push_back(sel);
                    }
                }
                UiElement p = element("GZWinBMP", "IGZWinBMP", x, y, x + kItem, y + kItem);
                const uint32_t pid = 0x00D00000u | uint32_t(i);
                p.set("id", hex(pid));
                extra.push_back(p);
                if (thumbnail)
                    if (const RgbaImage* t = thumbnail(e.id, 0))
                        L.images[pid] = t;
                if (!affordable)
                    L.colors[pid] = {1, 1, 1, 0.35f};
                if (affordable)
                    L.regions.push_back({{L.x0 + x, L.y0 + y, L.x0 + x + kItem, L.y0 + y + kItem}, Hit::Row, first + i});
            }
        }
        if (id == 0x1004)
            L.regions.push_back({rr, Hit::Prev, 0});
        if (id == 0x1005)
            L.regions.push_back({rr, Hit::Next, 0});
        if (id == 0xA000 || id == 0xA001) {
            L.regions.push_back({rr, Hit::Button, id == 0xA000 ? 0 : 1});
            L.buttonIds.push_back(id);
        }
    }
    // The items go over the grid window, under the frame (index 5 is the frame's image).
    root.children.insert(root.children.begin() + std::min<size_t>(5, root.children.size()), extra.begin(), extra.end());
    L.root = std::move(root);
    return L;
}

// --- cTSWinProcClothingPurchaseDialog / ClothingTryOnDialog / VanityDialog ---

namespace {
// Tabs' part bits (0xECC70000-2: tops, bottoms, whole outfits).
constexpr uint32_t kShopPart[3] = {0x04, 0x10, 0x08};
// The thumbnail scripts and grids of the clothing bin (the scripts'
// constants: kClothingBinTopsThumbnail 0x8C159252 3 x 2, bottoms 0x8C159253
// and whole outfits 0x8C159254 4 x 2; the cart kShoppingCartThumbnail
// 0x8C159252 3 x 2).
constexpr uint32_t kShopCell[3] = {0x8C159252, 0x8C159253, 0x8C159254};
constexpr int kShopCols[3] = {3, 4, 4}, kShopRows[3] = {2, 2, 2};
// The rack's age buttons 0x200-0x204 (toddler .. elder) and gender buttons
// 0x400 (female) / 0x401 (male).
constexpr uint32_t kShopAge[5] = {0x01, 0x02, 0x04, 0x08, 0x10};

// A window anywhere under `e` by id, and its canvas rectangle given e's
// top-left corner.
UiElement* findWin(UiElement& e, uint32_t id, float ox, float oy, std::array<float, 4>* rect) {
    for (auto& c : e.children) {
        const auto a = c.area();
        if (c.id() == id) {
            if (rect)
                *rect = {ox + float(a[0]), oy + float(a[1]), ox + float(a[2]), oy + float(a[3])};
            return &c;
        }
        if (UiElement* f = findWin(c, id, ox + float(a[0]), oy + float(a[1]), rect))
            return f;
    }
    return nullptr;
}

std::string captionOf(UiResources* res, const UiElement& e) {
    std::string text = e.get("caption");
    if (res)
        if (auto ref = uiStringRef(e.get("captionres")))
            if (auto t = res->string(*ref))
                text = *t;
    return text;
}

std::string printS(std::string fmt, const std::string& value) {
    if (auto p = fmt.find("%s"); p != std::string::npos)
        fmt.replace(p, 2, value);
    return fmt;
}

// Vanity tabs (the buttons of script 0x8C159280 in their order there) and
// what each shows (HandleTabSelect: hair 0x2CEDF326, makeup 0x2CEDF327,
// costume makeup and glasses 0x2CEDF328, eyebrows / stubble / beards the
// facial hair panel 0xAD4F1B1C with its colour row 0x2D4F258C hidden).
constexpr uint32_t kVanityTabs[7] = {0x4CEDF125, 0x4CEDF126, 0x4CEDF128, 0x4CEDF127, 0x4CEDF130, 0x4CEDF131, 0x4CEDF129};
constexpr uint32_t kVanityPanels[7] = {0x2CEDF326, 0x2CEDF327, 0x2CEDF328, 0x2CEDF328, 0xAD4F1B1C, 0xAD4F1B1C, 0xAD4F1B1C};
struct VanityGrid {
    uint32_t component; // the CAS component window holding the grid
    int group;
    uint32_t grid, prev, next, cell;
    int cols, rows;
};
} // namespace

std::array<float, 4> DialogWindow::addCell(Layout& L, UiElement& parent, std::array<float, 2> parentAbs, uint32_t cellScript,
                                           uint32_t serial, float x, float y, const RgbaImage* thumb, int state, bool mark7,
                                           bool mark8) {
    const UiScript* s = script(cellScript);
    UiElement cell;
    float w = 50, h = 50;
    if (s && !s->roots.empty()) {
        cell = s->roots.front();
        const auto a = cell.area();
        w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    } else {
        cell = element("GZWinFlatRect", "IGZWinFlatRect", 0, 0, w, h);
    }
    cell.set("area", uiArea(int(x), int(y), int(x + w), int(y + h)));
    cell.set("id", hex(0x00E00000u | (serial << 8)));
    const uint32_t base = 0x00E00000u | (serial << 8);
    UiElement* pic = nullptr;
    for (auto& c : cell.children) {
        const uint32_t id = c.id();
        if (id == 7)
            c.set("winflag_visible", mark7 ? "yes" : "no");
        if (id == 8)
            c.set("winflag_visible", mark8 ? "yes" : "no");
        if (id == 3) // "backup image": a cell without a picture (the "none" ones)
            c.set("winflag_visible", thumb ? "no" : "yes");
        if (id == 6)
            pic = &c;
        if (id == 5)
            L.states[base | 5] = state;
        c.set("id", hex(base | id));
    }
    if (pic && thumb && thumb->width && thumb->height) {
        // The picture fitted into window 6 (the game draws the part's own
        // rendered thumbnail there; the CASThumbnails ones keep their shape).
        const auto a = pic->area();
        const float pw = float(a[2] - a[0]), ph = float(a[3] - a[1]);
        const float k = std::min(pw / float(thumb->width), ph / float(thumb->height));
        const float tw = std::round(float(thumb->width) * k), th = std::round(float(thumb->height) * k);
        const float px = float(a[0]) + std::round((pw - tw) / 2), py = float(a[1]) + std::round((ph - th) / 2);
        pic->set("area", uiArea(int(px), int(py), int(px + tw), int(py + th)));
        pic->set("image", "");
        L.images[base | 6] = thumb;
    } else if (pic) {
        pic->set("winflag_visible", "no");
    }
    parent.children.push_back(std::move(cell));
    return {parentAbs[0] + x, parentAbs[1] + y, parentAbs[0] + x + w, parentAbs[1] + y + h};
}

DialogWindow::Shop& DialogWindow::shop(const vm::Dialog& d) {
    Shop& s = shops_[d.id];
    if (!s.ready) {
        s.ready = true;
        // The everyday categories open on tops, the others on whole outfits
        // (SetParams' 0x601 test).
        s.tab = d.outfitSeparates ? 0 : 2;
        if (simAgeGender)
            std::tie(s.age, s.gender) = simAgeGender(d.outfitSim);
        if (!s.age)
            s.age = 0x08;
        if (!s.gender)
            s.gender = 0x01;
        // The try-on starts from what the Sim wears in the category (its
        // SetParams copies those parts into the try-on outfit).
        if (d.cls.cls == vm::DialogClass::ClothingTryOn)
            for (int t = 0; t < 3; ++t)
                for (const ShopItem& i : shopList(d, s, t))
                    if (i.worn)
                        s.chosen[size_t(t)] = i.part;
    }
    return s;
}

const std::vector<DialogWindow::ShopItem>& DialogWindow::shopList(const vm::Dialog& d, Shop& s, int tab) {
    const auto key = std::make_tuple(tab, s.age, s.gender);
    auto it = s.lists.find(key);
    if (it == s.lists.end()) {
        std::vector<ShopItem> l;
        if (shopItems)
            l = shopItems(d.outfitSim, d.outfitCategory, kShopPart[tab], s.age, s.gender);
        it = s.lists.emplace(key, std::move(l)).first;
    }
    return it->second;
}

// Scripts 0x8C159251 ("Comprar Roupa") and 0x8C159255 ("Experimentar"):
// the rack window 0xECCDF759 with the tabs (0xECC70000-2), the grid
// 0x1006 and its arrows 0x1004/5; the rack also has the age (0x200-0x204)
// and gender (0x400/1) buttons, "add to cart" 0xECCDF663, the cart window
// 0x2CCDF792 (grid 0x1006, "remove" 0x6CCDF7D7) behind the rack / cart tabs
// 0x8CCD0000/1, the count (0xCCCDF42D) and total (0xACCDF819) and the Sim
// view 0x2CC7315A (here the picked part's picture, else the Sim's head:
// the game renders the Sim wearing it). OK 0x6CC7318B, cancel 0x6CC73195.
DialogWindow::Layout DialogWindow::shopDialog(const vm::Dialog& d, int width, int height) {
    const bool rack = d.cls.cls == vm::DialogClass::ClothingPurchase;
    const UiScript* sc = script(rack ? 0x8C159251 : 0x8C159255);
    if (!sc)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = sc->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    // A picture window without a picture and not transparent (the try-on's
    // backdrop) is painted in its fill colour (inferred from its attributes).
    for (auto& c : root.children)
        if (c.clsid() == "GZWinBMP" && !c.attr("image") && c.get("transparentbkg") == "no") {
            c.set("clsid", "GZWinFlatRect");
            c.set("iid", "IGZWinFlatRect");
        }
    Shop& s = shop(d);
    const bool cartView = rack && s.view == 1;
    std::array<float, 4> r{};
    auto absOf = [&](const std::array<float, 4>& q) { return std::array<float, 4>{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}; };
    auto region = [&](UiElement& under, uint32_t id, Hit kind, int value) {
        std::array<float, 4> q{};
        if (findWin(under, id, 0, 0, &q))
            L.regions.push_back({absOf(q), kind, value});
    };
    UiElement* rackWin = findWin(root, 0xECCDF759, 0, 0, &r);
    const std::array<float, 4> rackRect = r;
    UiElement* cartWin = rack ? findWin(root, 0x2CCDF792, 0, 0, &r) : nullptr;
    const std::array<float, 4> cartRect = r;
    if (rackWin)
        rackWin->set("winflag_visible", cartView ? "no" : "yes");
    if (cartWin)
        cartWin->set("winflag_visible", cartView ? "yes" : "no");
    const auto& list = shopList(d, s, s.tab);
    auto inCart = [&](const ShopItem& i) {
        return std::any_of(s.cart.begin(), s.cart.end(), [&](const ShopItem& c) { return c.key == i.key; });
    };
    uint32_t serial = 0;
    // The grid of the rack (or of the try-on).
    if (rackWin && !cartView) {
        std::array<float, 4> g{};
        if (UiElement* grid = findWin(*rackWin, 0x1006, 0, 0, &g)) {
            const int cols = kShopCols[s.tab], rows = kShopRows[s.tab], per = cols * rows;
            int& first = s.first[size_t(s.tab)];
            first = std::clamp(first, 0, std::max(0, (int(list.size()) - 1) / per * per));
            const UiScript* cs = script(kShopCell[s.tab]);
            float cw = 76, ch = 111;
            if (cs && !cs->roots.empty()) {
                const auto ca = cs->roots.front().area();
                cw = float(ca[2] - ca[0]), ch = float(ca[3] - ca[1]);
            }
            const float gw = g[2] - g[0], gh = g[3] - g[1];
            const float ox = std::round((gw - cw * float(cols)) / 2), oy = std::round((gh - ch * float(rows)) / 2);
            for (int k = 0; k < per && first + k < int(list.size()); ++k) {
                const ShopItem& item = list[size_t(first + k)];
                const bool picked = rack ? s.pick == first + k : s.chosen[size_t(s.tab)] == item.part;
                const auto cell = addCell(L, *grid, {L.x0 + rackRect[0] + g[0], L.y0 + rackRect[1] + g[1]}, kShopCell[s.tab], serial++,
                                          ox + float(k % cols) * cw, oy + float(k / cols) * ch, item.thumb, picked ? 2 : 1,
                                          rack && inCart(item), item.owned);
                L.regions.push_back({cell, Hit::Row, first + k});
            }
        }
        std::array<float, 4> q{};
        for (uint32_t id : {0x1004u, 0x1005u})
            if (findWin(*rackWin, id, rackRect[0], rackRect[1], &q))
                L.regions.push_back({absOf(q), id == 0x1004 ? Hit::Prev : Hit::Next, 0});
        for (int t = 0; t < 3; ++t)
            if (findWin(*rackWin, 0xECC70000u + uint32_t(t), rackRect[0], rackRect[1], &q)) {
                // SetOutfitTabButtonStates: tops and bottoms only for the everyday categories.
                const bool on = t == 2 || d.outfitSeparates;
                L.states[0xECC70000u + uint32_t(t)] = !on ? 0 : t == s.tab ? 2 : 1;
                if (on)
                    L.regions.push_back({absOf(q), Hit::Tab, t});
            }
        if (rack) {
            for (int k = 0; k < 5; ++k)
                if (findWin(*rackWin, 0x200u + uint32_t(k), rackRect[0], rackRect[1], &q)) {
                    L.states[0x200u + uint32_t(k)] = s.age == kShopAge[k] ? 2 : 1;
                    L.regions.push_back({absOf(q), Hit::Choice, 0x100 | int(kShopAge[k])});
                }
            for (int k = 0; k < 2; ++k)
                if (findWin(*rackWin, 0x400u + uint32_t(k), rackRect[0], rackRect[1], &q)) {
                    L.states[0x400u + uint32_t(k)] = s.gender == uint32_t(k + 1) ? 2 : 1;
                    L.regions.push_back({absOf(q), Hit::Choice, 0x200 | (k + 1)});
                }
            if (findWin(*rackWin, 0xECCDF663, rackRect[0], rackRect[1], &q)) {
                const bool can = s.pick >= 0 && s.pick < int(list.size()) && !inCart(list[size_t(s.pick)]);
                L.states[0xECCDF663] = can ? 1 : 0;
                if (can)
                    L.regions.push_back({absOf(q), Hit::Add, 0});
            }
        }
    }
    // The cart.
    if (cartWin && cartView) {
        std::array<float, 4> g{};
        if (UiElement* grid = findWin(*cartWin, 0x1006, 0, 0, &g)) {
            const int cols = 3, rows = 2, per = cols * rows; // kShoppingCartNumColumns / NumRows
            s.cartFirst = std::clamp(s.cartFirst, 0, std::max(0, (int(s.cart.size()) - 1) / per * per));
            for (int k = 0; k < per && s.cartFirst + k < int(s.cart.size()); ++k) {
                const ShopItem& item = s.cart[size_t(s.cartFirst + k)];
                const auto cell = addCell(L, *grid, {L.x0 + cartRect[0] + g[0], L.y0 + cartRect[1] + g[1]}, 0x8C159252, serial++,
                                          float(k % cols) * 101, float(k / cols) * 110, item.thumb,
                                          s.cartPick == s.cartFirst + k ? 2 : 1, false, item.owned);
                L.regions.push_back({cell, Hit::Row, 0x10000 | (s.cartFirst + k)});
            }
        }
        std::array<float, 4> q{};
        for (uint32_t id : {0x1004u, 0x1005u})
            if (findWin(*cartWin, id, cartRect[0], cartRect[1], &q))
                L.regions.push_back({absOf(q), id == 0x1004 ? Hit::Prev : Hit::Next, 1});
        if (findWin(*cartWin, 0x6CCDF7D7, cartRect[0], cartRect[1], &q)) {
            const bool can = s.cartPick >= 0 && s.cartPick < int(s.cart.size());
            L.states[0x6CCDF7D7] = can ? 1 : 0;
            if (can)
                L.regions.push_back({absOf(q), Hit::Remove, 0});
        }
    }
    if (rack) {
        for (int v = 0; v < 2; ++v) {
            region(root, 0x8CCD0000u + uint32_t(v), Hit::Choice, 0x400 | v);
            L.states[0x8CCD0000u + uint32_t(v)] = s.view == v ? 2 : 1;
        }
        if (UiElement* e = findWin(root, 0xCCCDF42D, 0, 0, nullptr))
            L.captions[0xCCCDF42D] = printS(captionOf(res_, *e), std::to_string(s.cart.size()));
        if (UiElement* e = findWin(root, 0xACCDF819, 0, 0, nullptr))
            L.captions[0xACCDF819] =
                printS(captionOf(res_, *e), simoleons(int(s.cart.size()) * d.pricePerItem));
        // The Sim view: the part picked (rack or cart), else the Sim.
        const RgbaImage* view = nullptr;
        if (cartView && s.cartPick >= 0 && s.cartPick < int(s.cart.size()))
            view = s.cart[size_t(s.cartPick)].thumb;
        else if (!cartView && s.pick >= 0 && s.pick < int(list.size()))
            view = list[size_t(s.pick)].thumb;
        if (!view && thumbnail)
            view = thumbnail(d.outfitSim, 0);
        if (view && view->width && view->height)
            if (UiElement* v = findWin(root, 0x2CC7315A, 0, 0, &r)) {
                const float vw = r[2] - r[0], vh = r[3] - r[1];
                const float k = std::min(vw / float(view->width), vh / float(view->height));
                const float tw = std::round(float(view->width) * k), th = std::round(float(view->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", std::round((vw - tw) / 2), std::round((vh - th) / 2),
                                      std::round((vw - tw) / 2) + tw, std::round((vh - th) / 2) + th);
                p.set("id", hex(0x2CC7315B));
                v->children.push_back(p);
                L.images[0x2CC7315B] = view;
            }
    }
    region(root, 0x6CC7318B, Hit::Button, 0);
    L.buttonIds.push_back(0x6CC7318B);
    region(root, 0x6CC73195, Hit::Button, 1);
    L.buttonIds.push_back(0x6CC73195);
    L.root = std::move(root);
    return L;
}

std::vector<DialogWindow::VanityItem>& DialogWindow::vanityList(const vm::Dialog& d, int group) {
    Vanity& v = vanities_[d.id];
    auto it = v.items.find(group);
    if (it == v.items.end())
        it = v.items.emplace(group, vanityItems ? vanityItems(d.outfitSim, group) : std::vector<VanityItem>{}).first;
    return it->second;
}

// Script 0x8C159280 ("Mudar Aparência"): the tabs (hair, makeup, glasses,
// costume makeup, eyebrows, stubble, beards), each showing its panel and
// CAS components' grids (the components' constants: hair 3 x 4 of
// 0x8C159283 with the colour buttons 0x8800-0x8803, the four makeup rows
// 4 x 1 of 0xCB980E93, the overlays 3 x 5 and the facial hair 3 x 4 of
// 0x8C159283), the Sim view 0xACB91188 (here the Sim's head), OK 0xA000
// and cancel 0xA001.
DialogWindow::Layout DialogWindow::vanityDialog(const vm::Dialog& d, int width, int height) {
    const UiScript* sc = script(0x8C159280);
    if (!sc)
        return plainDialog(d, width, height);
    Layout L;
    const UiElement& top = sc->roots.front();
    const auto a = top.area();
    const float w = float(a[2] - a[0]), h = float(a[3] - a[1]);
    L.x0 = std::round((float(width) - w) / 2), L.y0 = std::round((float(height) - h) / 2);
    L.x1 = L.x0 + w, L.y1 = L.y0 + h;
    UiElement root = top;
    root.set("area", uiArea(0, 0, int(w), int(h)));
    Vanity& v = vanities_[d.id];
    auto absOf = [&](const std::array<float, 4>& q) { return std::array<float, 4>{L.x0 + q[0], L.y0 + q[1], L.x0 + q[2], L.y0 + q[3]}; };
    std::array<float, 4> r{};
    for (int t = 0; t < 7; ++t) {
        if (findWin(root, kVanityTabs[t], 0, 0, &r))
            L.regions.push_back({absOf(r), Hit::Tab, t});
        L.states[kVanityTabs[t]] = t == v.tab ? 2 : 1;
    }
    for (uint32_t p : {0x2CEDF326u, 0x2CEDF327u, 0x2CEDF328u, 0xAD4F1B1Cu})
        if (UiElement* e = findWin(root, p, 0, 0, nullptr))
            e->set("winflag_visible", p == kVanityPanels[v.tab] ? "yes" : "no");
    if (UiElement* e = findWin(root, 0x2D4F258C, 0, 0, nullptr))
        e->set("winflag_visible", "no");
    std::vector<VanityGrid> grids;
    switch (v.tab) {
    case 0: grids = {{0x2CED6A8D, VanityHair, 0x8CB8C2B9, 0x8CB8C2C3, 0xACB8C2CF, 0x8C159283, 3, 4}}; break;
    case 1:
        grids = {{0x6CA52068, VanityBlush, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0xAD474C78, VanityEyeliner, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0x6CA52069, VanityEyeShadow, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1},
                 {0x6CA52067, VanityLipstick, 0x4011, 0x4008, 0x4009, 0xCB980E93, 4, 1}};
        break;
    case 2: grids = {{0x6CA52066, VanityGlasses, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 5}}; break;
    case 3: grids = {{0x6CA52066, VanityCostume, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 5}}; break;
    case 4: grids = {{0x8D4F1FEB, VanityBrows, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    case 5: grids = {{0x8D4F1FEB, VanityStubble, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    case 6: grids = {{0x8D4F1FEB, VanityBeards, 0x4011, 0x4008, 0x4009, 0x8C159283, 3, 4}}; break;
    default: break;
    }
    UiElement* panel = findWin(root, kVanityPanels[v.tab], 0, 0, &r);
    const std::array<float, 4> panelRect = r;
    uint32_t serial = 0;
    for (const VanityGrid& g : grids) {
        if (!panel)
            break;
        std::array<float, 4> cr{};
        UiElement* comp = findWin(*panel, g.component, panelRect[0], panelRect[1], &cr);
        if (!comp)
            continue;
        std::array<float, 4> gr{};
        UiElement* grid = findWin(*comp, g.grid, cr[0], cr[1], &gr);
        auto& items = vanityList(d, g.group);
        const int per = g.cols * g.rows;
        int& first = v.first[g.group];
        first = std::clamp(first, 0, std::max(0, (int(items.size()) - 1) / per * per));
        if (grid) {
            const UiScript* cs = script(g.cell);
            float cw = 50, ch = 50;
            if (cs && !cs->roots.empty()) {
                const auto ca = cs->roots.front().area();
                cw = float(ca[2] - ca[0]), ch = float(ca[3] - ca[1]);
            }
            const float gw = gr[2] - gr[0], gh = gr[3] - gr[1];
            const float ox = std::round((gw - cw * float(g.cols)) / 2), oy = std::round((gh - ch * float(g.rows)) / 2);
            for (int k = 0; k < per && first + k < int(items.size()); ++k) {
                const VanityItem& it = items[size_t(first + k)];
                const auto cell = addCell(L, *grid, {L.x0 + gr[0], L.y0 + gr[1]}, g.cell, serial++, ox + float(k % g.cols) * cw,
                                          oy + float(k / g.cols) * ch, it.thumb, it.selected ? 2 : 1, false, false);
                L.regions.push_back({cell, Hit::Row, (g.group << 16) | (first + k)});
            }
        }
        std::array<float, 4> q{};
        if (findWin(*comp, g.prev, cr[0], cr[1], &q))
            L.regions.push_back({absOf(q), Hit::Prev, g.group});
        if (findWin(*comp, g.next, cr[0], cr[1], &q))
            L.regions.push_back({absOf(q), Hit::Next, g.group});
        if (g.group == VanityHair) {
            // The colour buttons (black, brown, blond, red; grey is hidden).
            auto& tones = vanityList(d, VanityHairColor);
            for (int k = 0; k < 4; ++k)
                if (findWin(*comp, 0x8800u + uint32_t(k), cr[0], cr[1], &q)) {
                    L.regions.push_back({absOf(q), Hit::Choice, 0x300 | k});
                    // Several windows share these ids: the state goes by the one found.
                    if (UiElement* b = findWin(*comp, 0x8800u + uint32_t(k), 0, 0, nullptr)) {
                        const uint32_t id = 0x00F00000u | uint32_t(k);
                        b->set("id", hex(id));
                        L.states[id] = k < int(tones.size()) && tones[size_t(k)].selected ? 2 : 1;
                    }
                }
        }
    }
    // The Sim view: the Sim's head (the game renders the Sim with the edit).
    if (thumbnail)
        if (const RgbaImage* head = thumbnail(d.outfitSim, 0); head && head->width && head->height)
            if (UiElement* view = findWin(root, 0xACB91188, 0, 0, &r)) {
                const float vw = r[2] - r[0], vh = r[3] - r[1];
                const float k = std::min(vw / float(head->width), vh / float(head->height));
                const float tw = std::round(float(head->width) * k), th = std::round(float(head->height) * k);
                UiElement p = element("GZWinBMP", "IGZWinBMP", std::round((vw - tw) / 2), std::round((vh - th) / 2),
                                      std::round((vw - tw) / 2) + tw, std::round((vh - th) / 2) + th);
                p.set("id", hex(0xACB91189));
                view->children.push_back(p);
                L.images[0xACB91189] = head;
            }
    if (findWin(root, 0xA000, 0, 0, &r)) {
        L.regions.push_back({absOf(r), Hit::Button, 0});
        L.buttonIds.push_back(0xA000);
    }
    if (findWin(root, 0xA001, 0, 0, &r)) {
        L.regions.push_back({absOf(r), Hit::Button, 1});
        L.buttonIds.push_back(0xA001);
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
    case vm::DialogClass::SetAspiration: return aspirationDialog(d, width, height);
    case vm::DialogClass::Fill: return fillDialog(d, width, height);
    case vm::DialogClass::SelectItems: return selectItemsDialog(d, width, height);
    case vm::DialogClass::ClothingPurchase:
    case vm::DialogClass::ClothingTryOn: return shopDialog(d, width, height);
    case vm::DialogClass::Vanity: return vanityDialog(d, width, height);
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
    for (const auto& [id, c] : L.colors)
        if (auto it = byId.find(id); it != byId.end())
            hk.colors[it->second] = c;
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
    for (const auto& [id, st] : L.states)
        if (auto it = byId.find(id); it != byId.end())
            hk.buttonState[it->second] = st;
    // The aspiration picked looks pressed.
    if (d.cls.cls == vm::DialogClass::SetAspiration)
        for (const auto& [id, e] : byId)
            if ((id & 0xFFFF0000u) == 0x0CDC0000u)
                hk.buttonState[e] = (1 << ((id & 0xFFFF) - 1)) == d.aspiration ? 2 : 1;
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
        if ((d.cls.cls == vm::DialogClass::ClothingPurchase || d.cls.cls == vm::DialogClass::ClothingTryOn) &&
            g.kind != Hit::Button) {
            Shop& s = shop(d);
            const auto& list = shopList(d, s, s.tab);
            switch (g.kind) {
            case Hit::Tab: s.tab = g.value, s.pick = -1; break;
            case Hit::Prev:
            case Hit::Next: {
                // A page of the grid at a time (clamped by the layout).
                const int per = g.value ? 6 : kShopCols[s.tab] * kShopRows[s.tab];
                int& first = g.value ? s.cartFirst : s.first[size_t(s.tab)];
                first = std::max(0, first + (g.kind == Hit::Next ? per : -per));
                break;
            }
            case Hit::Row:
                if (g.value >> 16)
                    s.cartPick = g.value & 0xFFFF;
                else if (d.cls.cls == vm::DialogClass::ClothingPurchase)
                    s.pick = g.value;
                else if (g.value < int(list.size())) {
                    // ApplyClothingChoice: a whole outfit takes off the top
                    // and bottom, and either of those the whole outfit.
                    s.chosen[size_t(s.tab)] = list[size_t(g.value)].part;
                    if (s.tab == 2)
                        s.chosen[0].clear(), s.chosen[1].clear();
                    else if (!s.chosen[2].empty()) {
                        s.chosen[2].clear();
                        for (int t = 0; t < 2; ++t)
                            if (s.chosen[size_t(t)].empty())
                                for (const ShopItem& i : shopList(d, s, t))
                                    if (i.worn || s.chosen[size_t(t)].empty())
                                        s.chosen[size_t(t)] = i.part;
                    }
                }
                break;
            case Hit::Add: // OnAddToCart
                if (s.pick >= 0 && s.pick < int(list.size())) {
                    s.cart.push_back(list[size_t(s.pick)]);
                    d.cart.push_back(list[size_t(s.pick)].key);
                }
                break;
            case Hit::Remove: // OnRemoveFromCart
                if (s.cartPick >= 0 && s.cartPick < int(s.cart.size())) {
                    const Tgi key = s.cart[size_t(s.cartPick)].key;
                    s.cart.erase(s.cart.begin() + s.cartPick);
                    if (auto it = std::find(d.cart.begin(), d.cart.end(), key); it != d.cart.end())
                        d.cart.erase(it);
                    s.cartPick = -1;
                }
                break;
            case Hit::Choice: // OnSpeciesAgeGender, the rack / cart tabs
                if ((g.value & 0xF00) == 0x100)
                    s.age = uint32_t(g.value & 0xFF), s.pick = -1;
                else if ((g.value & 0xF00) == 0x200)
                    s.gender = uint32_t(g.value & 0xFF), s.pick = -1;
                else if ((g.value & 0xF00) == 0x400)
                    s.view = g.value & 1;
                break;
            default: break;
            }
            return true;
        }
        if (d.cls.cls == vm::DialogClass::Vanity && g.kind != Hit::Button) {
            Vanity& v = vanities_[d.id];
            switch (g.kind) {
            case Hit::Tab: v.tab = g.value; break;
            case Hit::Prev:
            case Hit::Next: {
                const int per = g.value == VanityHair ? 12 : g.value <= VanityLipstick ? 4 : g.value <= VanityCostume ? 15 : 12;
                v.first[g.value] = std::max(0, v.first[g.value] + (g.kind == Hit::Next ? per : -per));
                break;
            }
            case Hit::Row:
            case Hit::Choice: {
                const int group = g.kind == Hit::Choice ? int(VanityHairColor) : g.value >> 16;
                const int index = g.kind == Hit::Choice ? (g.value & 0xFF) : (g.value & 0xFFFF);
                if (vanityPick)
                    vanityPick(d.outfitSim, group, index);
                v.items.clear(); // a pick can change other groups (the hair colour, beards)
                break;
            }
            default: break;
            }
            return true;
        }
        switch (g.kind) {
        case Hit::Button:
        case Hit::Close: answer(d, g.value); break;
        case Hit::NameBox: d.nameFocus = g.value; break;
        case Hit::Row: {
            const int list = g.value >> 16, row = g.value & 0xFFFF;
            if (d.cls.cls == vm::DialogClass::SelectItems) {
                // A pick toggles; one at a time unless several are allowed.
                if (row < int(d.entries.size())) {
                    const int16_t id = d.entries[size_t(row)].id;
                    if (contains(d.selection, id))
                        d.selection.erase(std::find(d.selection.begin(), d.selection.end(), id));
                    else if (d.multiSelect)
                        d.selection.push_back(id);
                    else
                        d.selection = {id};
                }
            } else if (d.cls.cls == vm::DialogClass::PhoneParty) {
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
        case Hit::Choice: d.aspiration = int16_t(g.value); break;
        case Hit::Bar: {
            // A click on the bar sets the units by its height (inferred).
            const float f = std::clamp((g.rect[3] - y) / std::max(1.0f, g.rect[3] - g.rect[1]), 0.0f, 1.0f);
            d.fillValue = std::max(d.fillUnits, int(std::round(f * float(d.fillMax))));
            while (d.fillValue > d.fillUnits && fillCost(d, d.fillValue - d.fillUnits) > d.fillFunds)
                --d.fillValue; // onCommand: not past the family's funds
            break;
        }
        case Hit::Tab:
        case Hit::Prev:
        case Hit::Next:
            if (d.cls.cls == vm::DialogClass::SelectItems) {
                pages_[d.id] += g.kind == Hit::Next ? 1 : -1; // clamped by the layout
                break;
            }
            if (d.cls.cls == vm::DialogClass::Fill) {
                d.fillValue = std::clamp(d.fillValue + (g.kind == Hit::Next ? 10 : -10), d.fillUnits,
                                         std::max(d.fillUnits, d.fillMax));
                while (d.fillValue > d.fillUnits && fillCost(d, d.fillValue - d.fillUnits) > d.fillFunds)
                    --d.fillValue; // onCommand: not past the family's funds
                break;
            }
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
