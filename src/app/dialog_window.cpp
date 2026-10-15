// The Dialog primitive's windows (dialog_window.hpp). Numbers marked
// "binary" are from the Mac BV executable (docs/primitives-mac.md "Dialog");
// the rest is inferred.

#include "app/dialog_window.hpp"
#include "app/dialog_window_internal.hpp"
#include "formats/log_once.hpp"

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

// Name script ids.
constexpr uint32_t kNameOk = 0xA000, kNameCancel = 0xA001;

std::string imageRef(uint32_t instance) {
    char b[32];
    std::snprintf(b, sizeof b, "{499db772,%08x}", instance);
    return b;
}

// Mac key codes.
constexpr int kReturn = 36, kEnter = 76, kEscape = 53, kBackspace = 51, kSpace = 49, kTab = 48;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

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
        } catch (const std::exception& err) {
            os2::logOnce("dialog script", std::to_string(id), err);
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

// `text` cut to `width` with "..." at its end when it is wider.
std::string DialogWindow::fitText(const std::string& style, const std::string& text, float width) const {
    if (textWidth(style, text) <= width)
        return text;
    std::string u = text;
    while (!u.empty()) {
        while (!u.empty() && (uint8_t(u.back()) & 0xC0) == 0x80) // a whole UTF-8 character off
            u.pop_back();
        if (!u.empty())
            u.pop_back();
        while (!u.empty() && u.back() == ' ')
            u.pop_back();
        const std::string cut = u + "...";
        if (textWidth(style, cut) <= width)
            return cut;
    }
    return "...";
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
    if (index == 0 && d.cls.cls == vm::DialogClass::ClothingPurchase && cartCompletesOutfits) {
        // SaveCartStandard: a cart whose tops and bottoms add no complete
        // outfit for an age and gender they are for asks first (UIText
        // STR# 0xCA #7); the answer comes back through confirmCart.
        Shop& s = shop(d);
        if (!s.confirmed && !cartCompletesOutfits(d.outfitSim, d.outfitCategory, d.cart)) {
            s.asking = true;
            return;
        }
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
    if (d.cls.cls == vm::DialogClass::ClothingPurchase && shop(d).asking)
        draw(out, cartQuestion(d), width, height);
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
    if (d.cls.cls == vm::DialogClass::ClothingPurchase && shop(d).asking) {
        const vm::Dialog q = cartQuestion(d);
        for (const Region& g : layout(q, width, height).regions)
            if (g.kind == Hit::Button && insideRect(g.rect, x, y))
                confirmCart(d, g.value == 0);
        return true;
    }
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
                const int per = g.value == VanityHair || g.value == VanityHairCustom ? 12
                                : g.value <= VanityLipstick                          ? 4
                                : g.value <= VanityCostume                           ? 15
                                                                                     : 12;
                v.first[g.value] = std::max(0, v.first[g.value] + (g.kind == Hit::Next ? per : -per));
                break;
            }
            case Hit::Choice:
                if (g.value == 0x310) { // "Custom": the custom tones' hairs
                    v.custom = true;
                    v.first.erase(VanityHairCustom);
                    break;
                }
                v.custom = false;
                [[fallthrough]];
            case Hit::Row: {
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
            } else {
                (void)list;
                const auto rows = phoneRows(d, kRowsList);
                if (row < int(rows.size()))
                    d.selection = {rows[size_t(row)]->id};
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
            for (const vm::DialogEntry* e : phoneRows(d, kRowsList))
                left.push_back(e->id);
            if (g.kind == Hit::Add && list == kRowsList && row < int(left.size()) &&
                (d.maxSelection <= 0 || int(d.selection.size()) < d.maxSelection)) {
                d.selection.push_back(left[size_t(row)]);
                left.erase(left.begin() + row);
                if (left.empty())
                    partyFocus_.erase(it);
                else
                    it->second = (kRowsList << 16) | std::min(row, int(left.size()) - 1);
            } else if (g.kind == Hit::Remove && list == kRowsInvite && row < int(d.selection.size())) {
                const auto guests = phoneRows(d, kRowsInvite);
                d.selection.erase(std::find(d.selection.begin(), d.selection.end(), guests[size_t(row)]->id));
                if (d.selection.empty())
                    partyFocus_.erase(it);
                else
                    it->second = (kRowsInvite << 16) | std::min(row, int(d.selection.size()) - 1);
            }
            break;
        }
        case Hit::Sort: {
            // onBtnClickedID: the neighbors' 0x1000 by name, 0x2000 by
            // relationship, 0x3000 slides, 0x4000 text (SetMode); the party's
            // 0x3000 / 0x4000 sort the neighbors, 0x7000 / 0x8000 the guests.
            Phone& p = phones_[d.id];
            if (d.cls.cls == vm::DialogClass::PhoneNeighbors) {
                const bool slidesNow = p.mode > 3;
                if (g.value == 0x1000)
                    p.mode = slidesNow ? 5 : 2;
                else if (g.value == 0x2000)
                    p.mode = slidesNow ? 4 : 1;
                else if (g.value == 0x3000 && !slidesNow)
                    p.mode += 3;
                else if (g.value == 0x4000 && slidesNow)
                    p.mode -= 3;
            } else if (g.value == 0x3000 || g.value == 0x4000)
                p.leftSort = g.value == 0x3000 ? 1 : 2;
            else
                p.rightSort = g.value == 0x7000 ? 1 : 2;
            break;
        }
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
            if (d.cls.cls == vm::DialogClass::PhoneNeighbors) {
                int& first = phones_[d.id].first;
                first = std::max(0, first + (g.kind == Hit::Next ? 28 : -28)); // a page of slides
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
    if (d.cls.cls == vm::DialogClass::ClothingPurchase && shop(d).asking) {
        // The yes/no's keys: Return yes, Esc no (cTSObjectDialog::SetupDialog).
        if (keyCode == kReturn || keyCode == kEnter || keyCode == kEscape)
            confirmCart(d, keyCode != kEscape);
        return true;
    }
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
