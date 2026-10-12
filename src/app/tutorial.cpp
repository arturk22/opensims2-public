// The tutorial in the app: tutorial.hpp, docs/tutorial-mac.md.

#include "app/tutorial.hpp"

#include "app/ui_panel_util.hpp"
#include "app/user_data.hpp"
#include "formats/dbpf.hpp"
#include "formats/font.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"
#include "formats/ui_resources.hpp"
#include "render/ui_layer.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace os2 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTypeStrTable = 0x53545223;
constexpr uint32_t kProgressScript = 0x2CDB7CBD;
constexpr int16_t kTutorialStateGlobal = 26;
// The progress window (0x2CDB7CBD).
constexpr uint32_t kBackground = 0x4D90EFFC, kTitle = 0xA9000001, kPicture = 0xA9000003, kBody = 0xA9000004,
                   kBack = 0xA9000005, kNext = 0xA9000006, kStep = 0xA9000007, kScrollbar = 0xA9000008,
                   kTextBox = 0xA9000009, kTextInner = 0xA900000A, kBeside = 0xA900000B;
// The arrow (SetTutorialHighlight): three images by the corner it points from.
constexpr uint32_t kArrowDownRight = 0x8CD80000, kArrowLeftDown = 0x8CD80001, kArrowLeftUp = 0x8CD80002;

std::string lowerOf(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string unquote(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

// Text/<name>.package, matched without case.
fs::path textPackage(const fs::path& resDir, const std::string& name) {
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(resDir / "Text", ec))
        if (lowerOf(f.path().stem().string()) == lowerOf(name))
            return f.path();
    return {};
}

} // namespace

std::vector<TutorialEpisode> loadTutorialEpisodes(const fs::path& resDir, const fs::path& userHood) {
    std::vector<TutorialEpisode> out;
    std::ifstream in(resDir / "Tutorial/EpisodeDefinitions.txt");
    if (!in)
        return out;
    struct Text {
        std::string package;
        uint32_t table = 0;
        int title = 1, description = 2;
    };
    std::vector<Text> texts;
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos)
            line = line.substr(0, hash);
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key))
            continue;
        std::string rest;
        std::getline(ls, rest);
        rest = unquote(rest.substr(std::min(rest.find_first_not_of(" \t"), rest.size())));
        const std::string k = lowerOf(key);
        if (k == "episode") {
            out.push_back({});
            out.back().number = std::atoi(rest.c_str());
            texts.push_back({});
            continue;
        }
        if (out.empty())
            continue;
        TutorialEpisode& e = out.back();
        std::istringstream rs(rest);
        if (k == "textresource") {
            rs >> texts.back().package >> texts.back().table;
        } else if (k == "titletextindex") {
            rs >> texts.back().title;
        } else if (k == "descriptiontextindex") {
            rs >> texts.back().description;
        } else if (k == "thumbnailresource") {
            std::string g, i;
            rs >> g >> i;
            e.thumbnail = {kTypeUiImage, uint32_t(std::stoul(g, nullptr, 0)), uint32_t(std::stoul(i, nullptr, 0))};
        } else if (k == "neighborhoodprefix") {
            e.neighborhood = rest;
        } else if (k == "lotname") {
            // "*14": the lot with that number (inferred from the data: the
            // five lots of the Tutorial neighborhood).
            e.lot = uint32_t(std::atoi(rest.c_str() + (rest.starts_with("*") ? 1 : 0)));
        } else if (k == "episodegroup") {
            e.group = std::atoi(rest.c_str());
        }
    }
    for (size_t i = 0; i < out.size(); ++i) {
        const Text& t = texts[i];
        try {
            const fs::path pkgPath = textPackage(resDir, t.package);
            if (!pkgPath.empty()) {
                DbpfPackage pkg(pkgPath);
                if (const auto* entry = pkg.find(Tgi{kTypeStrTable, 0xFFFFFFFF, t.table, 0})) {
                    const StringTable table = StringTable::parse(pkg.read(*entry));
                    if (t.title > 0)
                        out[i].title = table.get(size_t(t.title - 1));
                    if (t.description > 0)
                        out[i].description = table.get(size_t(t.description - 1));
                }
            }
        } catch (const std::exception&) {
        }
        if (out[i].title.empty())
            out[i].title = "Tutorial " + std::to_string(out[i].number);
        // The state: global 26 of the lot's save (RefreshEpisodeInfo).
        const fs::path lot = userHood / "Lots" / (out[i].neighborhood + "_Lot" + std::to_string(out[i].lot) + ".package");
        std::error_code ec;
        if (!userHood.empty() && fs::exists(lot, ec))
            try {
                DbpfPackage pkg(lot);
                const LotObjects saved = LotObjects::load(pkg);
                if (saved.globals.size() > size_t(kTutorialStateGlobal))
                    out[i].state = saved.globals[size_t(kTutorialStateGlobal)];
            } catch (const std::exception&) {
            }
    }
    return out;
}

void resetTutorialNeighborhood(const fs::path& shipped, const fs::path& userCopy) {
    std::error_code ec;
    fs::remove_all(userCopy, ec);
    cloneTree(shipped, userCopy);
}

// --- The overlay -------------------------------------------------------------

TutorialOverlay::TutorialOverlay(vm::VM& vm, ResourceIndex& index, const fs::path& resDir)
    : vm_(vm), res_(std::make_unique<UiResources>(index, resDir)),
      fonts_(std::make_unique<FontLibrary>(resDir / "UI/Fonts")) {
    if (auto s = res_->script(kProgressScript); s && !s->roots.empty())
        script_ = std::move(*s), ok_ = true;
}

TutorialOverlay::~TutorialOverlay() = default;

bool TutorialOverlay::panelVisible() const {
    return ok_ && vm_.tutorial.progress.visible;
}

void TutorialOverlay::update(double seconds) {
    // Paused (Buy, Build, the pause button): the controllers alone go on, at
    // the game's tick rate (vm::tickTutorialControllers).
    if (vm_.globals[vm::global::Paused] && now_ > 0) {
        pausedTime_ += std::clamp(seconds - now_, 0.0, 0.25);
        const double tick = 0.042;
        for (int n = 0; pausedTime_ >= tick && n < 8; ++n, pausedTime_ -= tick)
            vm::tickTutorialControllers(vm_);
    } else
        pausedTime_ = 0;
    now_ = seconds;
    ui_.update(vm_);
    for (const auto& k : ui_.takePresses()) {
        std::printf("opensims2: tutorial: press 0x%08X/0x%08X\n", k.first, k.second);
        if (pressControl)
            pressControl(k.first, k.second);
    }
    if (ui_.highlight() != arrowFor_) {
        arrowFor_ = ui_.highlight(), arrowSince_ = now_;
        std::printf("opensims2: tutorial: arrow on 0x%08X/0x%08X\n", arrowFor_.first, arrowFor_.second);
        if (arrowFor_.second && locate)
            if (auto r = locate(arrowFor_.first, arrowFor_.second))
                std::printf("opensims2: tutorial: arrow target at (%.0f, %.0f)-(%.0f, %.0f)\n", (*r)[0], (*r)[1], (*r)[2], (*r)[3]);
    }
    // Where the panel and its buttons are (for scripted clicks).
    const vm::TutorialProgress& pr = vm_.tutorial.progress;
    if (pr.serial != printedSerial_) {
        printedSerial_ = pr.serial;
        screen(canvasW_, canvasH_);
        if (pr.visible)
            std::printf("opensims2: tutorial: step %d of %d \"%s\", Next at (%.0f, %.0f)-(%.0f, %.0f)\n", pr.step, pr.total,
                        pr.message.substr(0, pr.message.find_first_of("\r\n")).c_str(), nextRect_[0], nextRect_[1],
                        nextRect_[2], nextRect_[3]);
        else
            std::printf("opensims2: tutorial: panel hidden\n");
    }
    std::fflush(stdout);
}

void TutorialOverlay::setCanvas(int width, int height) {
    canvasW_ = width, canvasH_ = height;
}

bool TutorialOverlay::press(simd_float2 p) {
    if (!panelVisible())
        return false;
    if (builtW_ != canvasW_ || builtH_ != canvasH_ || builtSerial_ != vm_.tutorial.progress.serial)
        screen(canvasW_, canvasH_);
    if (!insideRect(panelRect_, p.x, p.y))
        return false;
    auto button = [&](const std::array<float, 4>& r, uint32_t id, const char* name) {
        if (r[2] <= r[0] || !insideRect(r, p.x, p.y))
            return false;
        const bool ok = ui_.click(vm_, vm::kProgressWindow, id);
        std::printf("opensims2: tutorial: %s %s\n", name, ok ? "pressed" : "is off");
        std::fflush(stdout);
        return true;
    };
    if (!button(nextRect_, kNext, "Next"))
        button(backRect_, kBack, "Back");
    return true;
}

bool TutorialOverlay::allows(simd_float2 p) {
    ui_.update(vm_);
    if (!locate)
        return true;
    for (const auto& k : ui_.blocked())
        if (auto r = locate(k.first, k.second); r && insideRect(*r, p.x, p.y))
            return false;
    return true;
}

void TutorialOverlay::clicked(simd_float2 p) {
    ui_.update(vm_);
    if (!locate)
        return;
    for (const auto& k : ui_.withTrees())
        if (auto r = locate(k.first, k.second); r && insideRect(*r, p.x, p.y)) {
            std::printf("opensims2: tutorial: click tree of 0x%08X/0x%08X\n", k.first, k.second);
            ui_.click(vm_, k.first, k.second);
        }
    std::fflush(stdout);
}

void TutorialOverlay::buildPanel(UiScreen& out, int width, int /*height*/) {
    panelRect_ = nextRect_ = backRect_ = {0, 0, 0, 0};
    const vm::TutorialProgress& pr = vm_.tutorial.progress;
    if (!ok_ || !pr.visible)
        return;
    UiElement root = script_.roots.front();
    auto find = [&](uint32_t id) { return findWindow(root, id); };
    // The text: next to the picture while it lasts, then under it
    // (0xA900000B, 0xA9000004), wrapped at their widths.
    const RgbaImage* picture = pr.image ? res_->image({kTypeUiImage, kUiImageGroup, pr.image}) : nullptr;
    const FontStyle* st = fonts_->style("NeighborhoodBody");
    const Font* font = st ? fonts_->font(*st) : nullptr;
    const float ppem = st ? float(st->size) : 11;
    float lineH = 14;
    if (font) {
        const TextMetrics m = measureText(*font, U"", ppem);
        lineH = std::round(m.ascent + m.descent);
    }
    auto wrap = [&](const std::u32string& para, float width) {
        std::vector<std::u32string> lines;
        std::u32string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t j = para.find(U' ', i);
            if (j == std::u32string::npos)
                j = para.size();
            const std::u32string word = para.substr(i, j - i);
            const std::u32string cand = line.empty() ? word : line + U' ' + word;
            if (!line.empty() && font && measureText(*font, cand, ppem).width > width)
                lines.push_back(line), line = word;
            else
                line = cand;
            i = j + 1;
        }
        lines.push_back(line);
        return lines;
    };
    std::vector<std::u32string> paras;
    {
        std::u32string cur;
        const std::u32string all = decodeUtf8(pr.message);
        for (size_t i = 0; i < all.size(); ++i) {
            const char32_t c = all[i];
            if (c == U'\r' || c == U'\n') {
                paras.push_back(cur), cur.clear();
                if (c == U'\r' && i + 1 < all.size() && all[i + 1] == U'\n')
                    ++i;
            } else
                cur += c;
        }
        paras.push_back(cur);
    }
    UiElement* inner = find(kTextInner);
    UiElement* besideWin = find(kBeside);
    UiElement* bodyWin = find(kBody);
    std::string beside, body;
    auto toUtf8 = [](const std::u32string& s) {
        std::string o;
        for (char32_t c : s) {
            if (c < 0x80)
                o += char(c);
            else if (c < 0x800)
                o += char(0xC0 | (c >> 6)), o += char(0x80 | (c & 0x3F));
            else
                o += char(0xE0 | (c >> 12)), o += char(0x80 | ((c >> 6) & 0x3F)), o += char(0x80 | (c & 0x3F));
        }
        return o;
    };
    const float bodyW = 260 - 6;
    float textBottom = 0;
    if (picture && besideWin && bodyWin) {
        const auto ba = besideWin->area();
        const float bw = float(ba[2] - ba[0]) - 6;
        const int fit = std::max(1, int((float(ba[3] - ba[1]) - 6) / lineH));
        int used = 0;
        bool under = false;
        int bodyLines = 0;
        for (const auto& para : paras) {
            if (!under) {
                const auto lines = wrap(para, bw);
                size_t k = 0;
                for (; k < lines.size() && used < fit; ++k, ++used)
                    beside += (used ? "\n" : "") + toUtf8(lines[k]);
                if (used >= fit)
                    under = true;
                if (k < lines.size()) { // the rest of the paragraph, wrapped at the body's width
                    std::u32string rest;
                    for (; k < lines.size(); ++k)
                        rest += (rest.empty() ? U"" : U" ") + lines[k];
                    for (const auto& l : wrap(rest, bodyW))
                        body += (bodyLines ? "\n" : "") + toUtf8(l), ++bodyLines;
                }
            } else {
                for (const auto& l : wrap(para, bodyW))
                    body += (bodyLines ? "\n" : "") + toUtf8(l), ++bodyLines;
            }
        }
        // The body starts under the picture (or the text beside it).
        const auto pa = find(kPicture)->area();
        const float top = std::max(float(pa[3]), float(ba[1]) + float(used) * lineH + 6) + 4;
        bodyWin->set("area", uiArea(0, int(top), 260, int(top + float(bodyLines) * lineH + 8)));
        besideWin->set("area", uiArea(ba[0], ba[1], ba[2], int(float(ba[1]) + float(used) * lineH + 8)));
        textBottom = bodyLines ? top + float(bodyLines) * lineH + 8 : top - 4;
    } else if (bodyWin) {
        int n = 0;
        for (const auto& para : paras)
            for (const auto& l : wrap(para, bodyW))
                body += (n ? "\n" : "") + toUtf8(l), ++n;
        bodyWin->set("area", uiArea(0, 2, 260, int(2 + float(n) * lineH + 8)));
        textBottom = 2 + float(n) * lineH + 8;
    }
    // The window grows with the text (the game scrolls a 77 px box; inferred
    // here: grown so that the whole step reads at once).
    const int extra = std::max(0, int(std::ceil(textBottom)) - 77);
    auto grow = [&](uint32_t id) {
        if (UiElement* e = find(id)) {
            const auto a = e->area();
            e->set("area", uiArea(a[0], a[1], a[2], a[3] + extra));
        }
    };
    auto move = [&](uint32_t id) {
        if (UiElement* e = find(id)) {
            const auto a = e->area();
            e->set("area", uiArea(a[0], a[1] + extra, a[2], a[3] + extra));
        }
    };
    grow(kTextBox);
    grow(kBackground);
    move(kStep);
    if (inner) {
        const auto a = inner->area();
        inner->set("area", uiArea(a[0], a[1], a[2], a[3] + extra));
    }
    {
        const auto a = root.area();
        root.set("area", uiArea(a[0], a[1], a[2], a[3] + extra));
    }
    UiDrawHooks hk;
    std::map<const UiElement*, std::array<float, 4>> rects;
    hk.rects = &rects;
    if (const UiElement* e = find(kTitle))
        hk.captions[e] = pr.title;
    if (besideWin)
        hk.captions[besideWin] = beside;
    if (bodyWin)
        hk.captions[bodyWin] = body;
    if (const UiElement* e = find(kPicture)) {
        if (picture)
            hk.images[e] = picture;
        else
            hk.hide.insert(e);
    }
    if (const UiElement* e = find(kScrollbar))
        hk.hide.insert(e);
    if (const UiElement* e = find(kStep)) {
        // "Passo %d de %d" when there is more than one step.
        std::string fmt = "Passo %d de %d";
        if (const auto* r = e->attr("captionres"))
            if (auto ref = uiStringRef(*r))
                if (auto s = res_->string(*ref))
                    fmt = *s;
        if (pr.total > 1) {
            char buf[128];
            std::snprintf(buf, sizeof buf, fmt.c_str(), pr.step, pr.total);
            hk.captions[e] = buf;
        } else
            hk.captions[e] = "";
    }
    const UiElement* backWin = find(kBack);
    const UiElement* nextWin = find(kNext);
    // Modal: no Back (cTSWinProcTutorialProgressDialog::Show).
    const bool backShown = backWin && !pr.modal && ui_.visible(vm::kProgressWindow, kBack);
    if (backWin && !backShown)
        hk.hide.insert(backWin);
    if (nextWin && !ui_.visible(vm::kProgressWindow, kNext))
        hk.hide.insert(nextWin);
    if (backWin)
        hk.buttonState[backWin] = ui_.enabled(vm::kProgressWindow, kBack) ? 1 : 0;
    if (nextWin)
        hk.buttonState[nextWin] = ui_.enabled(vm::kProgressWindow, kNext) ? 1 : 0;
    const auto ra = root.area();
    const float w = float(ra[2] - ra[0]), h = float(ra[3] - ra[1]);
    // Top right, 6 px from the side and 8 from the top (Show).
    const float x = float(width) - w - 6, y = 8;
    layoutUiElement(root, x - float(ra[0]), y - float(ra[1]), *res_, *fonts_, out, hk);
    panelRect_ = {x, y, x + w, y + h};
    // The rects are of the copy's windows; find them by id.
    for (const auto& [e, r] : rects) {
        const uint32_t id = e->attr("id") ? e->id() : 0;
        if (id == kNext && ui_.visible(vm::kProgressWindow, kNext))
            nextRect_ = r;
        else if (id == kBack && backShown)
            backRect_ = r;
    }
}

void TutorialOverlay::buildArrow(UiScreen& out) {
    // The controls turned off: shaded (their own windows keep drawing them).
    if (locate)
        for (const auto& k : ui_.blocked())
            if (k.first != vm::kProgressWindow)
                if (auto r = locate(k.first, k.second)) {
                    UiQuad q;
                    q.x0 = (*r)[0], q.y0 = (*r)[1], q.x1 = (*r)[2], q.y1 = (*r)[3];
                    q.color = {0.08f, 0.09f, 0.18f, 0.55f};
                    out.quads.push_back(q);
                }
    const auto hl = ui_.highlight();
    if (!hl.second)
        return;
    std::optional<std::array<float, 4>> r;
    if (hl.first == vm::kProgressWindow)
        r = hl.second == kNext ? std::optional(nextRect_) : hl.second == kBack ? std::optional(backRect_) : std::nullopt;
    else if (locate)
        r = locate(hl.first, hl.second);
    if (!r || (*r)[2] <= (*r)[0])
        return;
    // SetTutorialHighlight: the image and direction by the screen corner,
    // the arrow at the control's corner opposite its direction, base
    // distance min(w, h) / 2 + 18; AnimateTutorialHighlight: a bounce of
    // (1 - f^2) * 25 px, f = 2t/1300 - 1, every 1.3 s.
    const float cw = float(canvasW_);
    const bool nearRight = (*r)[2] > cw - 64, nearTop = (*r)[1] < 64;
    uint32_t image;
    float dx, dy;
    if (nearRight && nearTop)
        image = kArrowLeftUp, dx = -1, dy = 1;
    else if (nearRight)
        image = kArrowLeftDown, dx = -1, dy = -1;
    else if (!nearTop)
        image = kArrowDownRight, dx = 1, dy = -1;
    else
        image = kArrowLeftUp, dx = 1, dy = 1;
    const RgbaImage* img = res_->image({kTypeUiImage, kUiImageGroup, image});
    if (!img || !img->width)
        return;
    const float cornerX = dx > 0 ? (*r)[2] : (*r)[0];
    const float cornerY = dy > 0 ? (*r)[3] : (*r)[1];
    const float base = std::min((*r)[2] - (*r)[0], (*r)[3] - (*r)[1]) / 2 + 18;
    const double t = std::fmod((now_ - arrowSince_) * 1000.0, 1300.0);
    const float f = float(2 * t / 1300 - 1);
    const float bounce = (1 - f * f) * 25;
    const float ax = cornerX + (base + bounce) * dx - float(img->width) / 2;
    const float ay = cornerY + (base + bounce) * dy - float(img->height) / 2;
    UiQuad q;
    q.image = img;
    q.x0 = std::round(ax), q.y0 = std::round(ay);
    q.x1 = q.x0 + float(img->width), q.y1 = q.y0 + float(img->height);
    out.quads.push_back(q);
}

UiScreen TutorialOverlay::screen(int width, int height) {
    canvasW_ = width, canvasH_ = height;
    const vm::TutorialProgress& pr = vm_.tutorial.progress;
    uint64_t state = 0;
    for (uint32_t id : {kBack, kNext})
        state = state * 4 + (ui_.enabled(vm::kProgressWindow, id) ? 1 : 0) + (ui_.visible(vm::kProgressWindow, id) ? 2 : 0);
    if (builtSerial_ != pr.serial || builtW_ != width || builtH_ != height || builtState_ != state) {
        panel_ = UiScreen{};
        panel_.width = width, panel_.height = height;
        buildPanel(panel_, width, height);
        builtSerial_ = pr.serial, builtW_ = width, builtH_ = height, builtState_ = state;
    }
    UiScreen out;
    out.width = width, out.height = height;
    out.quads = panel_.quads; // panel_ keeps the text images
    buildArrow(out);
    return out;
}

void TutorialOverlay::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void TutorialOverlay::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    layer_ = (layer_ + 1) % layers_.size();
    layers_[layer_]->setScreen(screen(cw, ch));
    layers_[layer_]->encode(enc, width, height);
}

} // namespace os2
