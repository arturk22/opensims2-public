#include "app/design_tool.hpp"

#include "app/vm_objects.hpp"
#include "formats/font.hpp"
#include "formats/image.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/txmt.hpp"
#include "formats/ui_resources.hpp"
#include "vm/placement.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace os2 {

namespace {

constexpr uint32_t kDialogScript = 0x3C605F85;
constexpr uint32_t kTwoRows = 0xA1000, kOneRow = 0xA2000, kName = 0xA3000, kRow1 = 0xA4000, kRow2 = 0xA5000,
                   kCostFrame = 0xA6000, kCostText = 0xA6001, kAccept = 0xA6002, kCancel = 0xA6003;
constexpr uint32_t kRowPrev = 0x1004, kRowNext = 0x1005, kRowGrid = 0x1006, kRowDelete = 0x000A;
constexpr int kColumns = 5; // the rows' constants: kCollapsedColumns=5, kCollapsedRows=1
constexpr uint32_t kUiText = 0x7FA7DC7C, kTypeTxmt = 0x49596978, kTypeTxtr = 0x1C4A276C;

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string money(int v) {
    std::string digits = std::to_string(v < 0 ? -v : v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += '.';
        out += digits[i];
    }
    return std::string(v < 0 ? "-" : "") + "§" + out;
}

std::string area(float x0, float y0, float x1, float y1) {
    return "(" + std::to_string(int(x0)) + "," + std::to_string(int(y0)) + "," + std::to_string(int(x1)) + "," +
           std::to_string(int(y1)) + ")";
}

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

} // namespace

DesignTool::DesignTool(vm::VM& vm, VmObjectView& view, ResourceIndex& index, UiResources& res, FontLibrary& fonts)
    : vm_(vm), view_(view), index_(index), res_(res), fonts_(fonts), catalog_(DesignCatalog::load(index)) {
    if (auto s = res_.script(kDialogScript))
        dialog_ = std::move(*s);
}

DesignTool::~DesignTool() = default;

void DesignTool::message(uint32_t table, int index1, const std::string& fallback) {
    std::string m = fallback;
    if (const StringTable* t = res_.table(kUiText, table))
        if (std::string s = t->get(size_t(index1 - 1)); !s.empty())
            m = s;
    if (say)
        say(m);
}

// The tool's price: its catalog product's (cTSUserToolObjectDesign::Init;
// the product is the "Design Tool" entry of Build.package STR# 0x92: price,
// title, description).
int DesignTool::price() {
    if (price_ >= 0)
        return price_;
    price_ = kDefaultPrice;
    if (const StringTable* t = res_.table(packageGroupHash("Build"), 0x92))
        try {
            price_ = std::stoi(t->get(0));
        } catch (const std::exception&) {
        }
    return price_;
}

size_t DesignTool::optionCount(int row) const {
    return open_ && size_t(row) < open_->options.size() ? open_->options[size_t(row)].size() : 0;
}

bool DesignTool::open(vm::ObjectId id, simd_float2 at, const std::string& name) {
    cancel();
    // The object's model (the part clicked, else its group's lead).
    std::string model = view_.cresOf(int16_t(id));
    vm::ObjectId owner = id;
    if (model.empty() || model == "nullModel")
        if (const vm::Object* o = vm_.object(id)) {
            owner = vm::multiTileGroup(*o);
            model = view_.cresOf(int16_t(owner));
        }
    // GetDesignModeInfo: the first two design subsets, in the order the
    // model's shapes list them.
    std::vector<std::string> order;
    std::map<std::string, std::string> spelled; // lower -> the model's spelling
    if (!model.empty())
        try {
            for (const auto& part : loadObjectModel(index_, model).parts) {
                order.push_back(part.mesh.name);
                spelled.emplace(lower(part.mesh.name), part.mesh.name);
            }
        } catch (const std::exception&) {
        }
    Open o;
    o.id = owner, o.model = model, o.name = name, o.at = at;
    for (const auto& s : catalog_.subsets(model, order)) {
        auto opts = catalog_.options(model, s);
        if (opts.empty())
            continue;
        o.subsets.push_back(spelled.count(s) ? spelled[s] : s);
        o.options.push_back(std::move(opts));
    }
    if (o.subsets.empty()) {
        message(0x8D, 16, "This object is not designable.");
        if (playSound)
            playSound("ui_error");
        return false;
    }
    if (vm::funds(vm_) < price()) {
        message(0x8D, 15, "Insufficient Funds!");
        if (playSound)
            playSound("ui_error");
        return false;
    }
    // The current choice of each row: the material drawn now, else the default.
    o.before = view_.designOf(int16_t(owner));
    const auto drawn = view_.materialsOf(int16_t(owner));
    for (size_t r = 0; r < o.subsets.size(); ++r) {
        std::string now;
        for (const auto& [k, v] : drawn)
            if (lower(k) == lower(o.subsets[r]))
                now = lower(v);
        size_t pick = 0;
        for (size_t i = 0; i < o.options[r].size(); ++i)
            if (lower(o.options[r][i]->material) == now)
                pick = i;
        o.chosen[r] = o.original[r] = pick;
        o.page[r] = pick / kColumns;
    }
    open_ = std::move(o);
    if (say)
        say("design: " + model + " (" + std::to_string(open_->subsets.size()) + " rows, " +
            std::to_string(open_->options[0].size()) +
            (open_->options.size() > 1 ? " and " + std::to_string(open_->options[1].size()) : std::string()) +
            " choices)");
    return true;
}

std::map<std::string, std::string> DesignTool::designFor(const Open& o) const {
    std::map<std::string, std::string> d = o.before;
    for (size_t r = 0; r < o.subsets.size(); ++r) {
        // One key per subset, in the model's spelling.
        for (auto it = d.begin(); it != d.end();)
            it = lower(it->first) == lower(o.subsets[r]) ? d.erase(it) : std::next(it);
        d[o.subsets[r]] = o.options[r][o.chosen[r]]->material;
    }
    return d;
}

bool DesignTool::changed() const {
    return open_ && (open_->chosen[0] != open_->original[0] || open_->chosen[1] != open_->original[1]);
}

void DesignTool::apply(vm::VM& vm, VmObjectView& view, vm::ObjectId id, const std::map<std::string, std::string>& design) {
    if (vm::Object* o = vm.object(id))
        o->design = design;
    view.setDesign(int16_t(id), design);
}

bool DesignTool::choose(int row, size_t option) {
    if (!open_ || size_t(row) >= open_->options.size() || option >= open_->options[size_t(row)].size())
        return false;
    open_->chosen[size_t(row)] = option;
    open_->page[size_t(row)] = option / kColumns;
    // The swatch shows on the object at once (the dialog's onCommand).
    view_.setDesign(int16_t(open_->id), designFor(*open_));
    if (say)
        say("design: " + open_->subsets[size_t(row)] + " = " + open_->options[size_t(row)][option]->material);
    return true;
}

bool DesignTool::accept() {
    if (!open_)
        return false;
    if (!changed()) {
        cancel();
        return false;
    }
    const int cost = price();
    if (vm::funds(vm_) < cost) {
        message(0x8D, 15, "Insufficient Funds!");
        if (playSound)
            playSound("ui_error");
        return false;
    }
    Change c;
    c.id = open_->id, c.before = open_->before, c.after = designFor(*open_), c.cost = cost;
    vm::addFunds(vm_, -cost);
    apply(vm_, view_, c.id, c.after);
    if (playSound)
        playSound("ui_object_buy");
    if (say)
        say("design of object " + std::to_string(c.id) + " changed for " + money(cost) + "; funds " +
            money(vm::funds(vm_)));
    open_.reset();
    targets_.clear();
    if (onAccepted)
        onAccepted(c);
    return true;
}

void DesignTool::cancel() {
    if (!open_)
        return;
    view_.setDesign(int16_t(open_->id), open_->before);
    open_.reset();
    targets_.clear();
}

// A swatch: the material's base texture (the game renders a thumbnail of
// the object in that material; inferred stand-in, as Build mode's patterns).
const RgbaImage* DesignTool::swatch(const std::string& material) {
    if (auto it = swatches_.find(material); it != swatches_.end())
        return it->second.get();
    std::unique_ptr<RgbaImage> img;
    try {
        if (auto loc = index_.findByName(material + "_txmt", kTypeTxmt)) {
            const auto bytes = index_.read(*loc); // the Rcol points into it
            Rcol rc(bytes);
            if (const auto* block = rc.find("cMaterialDefinition")) {
                const MaterialDefinition md = MaterialDefinition::parse(block->body);
                std::string tex = baseTextureName(md);
                if (tex.empty() && !md.textureNames.empty())
                    tex = md.textureNames.front() + "_txtr";
                if (!tex.empty())
                    if (auto t = index_.findByName(tex, kTypeTxtr)) {
                        const TextureLevels lv = loadTextureLevels(index_.read(*t), &index_);
                        size_t pick = 0;
                        for (size_t l = 0; l < lv.levels.size(); ++l)
                            if (lv.levels[l].width >= 48)
                                pick = l;
                        if (pick < lv.levels.size()) {
                            const auto& L = lv.levels[pick];
                            img = std::make_unique<RgbaImage>(decodeTexture(lv.format, L.width, L.height, L.data));
                        }
                    }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: design: swatch of %s: %s\n", material.c_str(), e.what());
    }
    return (swatches_[material] = std::move(img)).get();
}

void DesignTool::layout(UiScreen& out, int width, int height) {
    targets_.clear();
    rect_ = {0, 0, 0, 0};
    if (!open_ || dialog_.roots.empty())
        return;
    Open& o = *open_;
    UiElement& root = dialog_.roots.front();
    const bool two = o.subsets.size() > 1;
    // One row: the single-row background, the cost frame under it, the
    // dialog that high (Show).
    const UiElement* bg = root.child(two ? kTwoRows : kOneRow);
    const auto ba = bg ? bg->area() : std::array<int, 4>{0, 0, 373, 294};
    const float w = float(ba[2] - ba[0]), h = float(ba[3] - ba[1]);
    if (UiElement* cf = root.child(kCostFrame)) {
        const auto a = cf->area();
        const float ch = float(a[3] - a[1]);
        cf->set("area", area(float(a[0]), h - ch, float(a[2]), h));
    }
    // By the click, kept on the screen.
    const float x = std::clamp(o.at.x, 4.0f, std::max(4.0f, float(width) - w - 4));
    const float y = std::clamp(o.at.y - h, 4.0f, std::max(4.0f, float(height) - h - 4));
    rect_ = {x, y, x + w, y + h};
    std::map<const UiElement*, std::array<float, 4>> rects;
    UiDrawHooks hk;
    hk.rects = &rects;
    if (const UiElement* e = root.child(two ? kOneRow : kTwoRows))
        hk.hide.insert(e);
    if (!two)
        if (const UiElement* e = root.child(kRow2))
            hk.hide.insert(e);
    for (uint32_t row : {kRow1, kRow2})
        if (const UiElement* r = root.child(row))
            if (const UiElement* d = r->child(kRowDelete)) // custom content: none here
                hk.hide.insert(d);
    if (const UiElement* e = root.child(kName))
        hk.captions[e] = o.name;
    if (const UiElement* cf = root.child(kCostFrame)) {
        if (const UiElement* e = cf->child(kAccept))
            hk.buttonState[e] = changed() ? 1 : 0;
        if (const UiElement* e = cf->child(kCostText)) {
            std::string fmt = "Cost to Update: %s";
            if (const StringTable* t = res_.table(kUiText, 0x93))
                if (std::string s = t->get(20); !s.empty())
                    fmt = s;
            const auto pos = fmt.find("%s");
            hk.captions[e] = pos == std::string::npos ? fmt + " " + money(price())
                                                       : fmt.substr(0, pos) + money(price()) + fmt.substr(pos + 2);
        }
    }
    const auto ra = root.area();
    layoutUiElement(root, x - float(ra[0]), y - float(ra[1]), res_, fonts_, out, hk);
    auto rectOf = [&](const UiElement* e) -> std::optional<std::array<float, 4>> {
        auto it = e ? rects.find(e) : rects.end();
        return it == rects.end() ? std::nullopt : std::optional(it->second);
    };
    // The swatches of each row in its grid (5 a page), the chosen one framed.
    for (size_t r = 0; r < o.subsets.size() && r < 2; ++r) {
        const UiElement* row = root.child(r == 0 ? kRow1 : kRow2);
        if (!row)
            continue;
        const auto& opts = o.options[r];
        const size_t pages = std::max<size_t>(1, (opts.size() + kColumns - 1) / kColumns);
        o.page[r] = std::min(o.page[r], pages - 1);
        if (auto g = rectOf(row->child(kRowGrid)); g || rectOf(row)) {
            std::array<float, 4> grid;
            if (g) {
                grid = *g;
            } else {
                // The grid window is hidden: its area under the row.
                const auto rr = *rectOf(row);
                const auto ga = row->child(kRowGrid) ? row->child(kRowGrid)->area() : std::array<int, 4>{19, 0, 269, 51};
                grid = {rr[0] + float(ga[0]), rr[1] + float(ga[1]), rr[0] + float(ga[2]), rr[1] + float(ga[3])};
            }
            const float cw = (grid[2] - grid[0]) / kColumns, side = std::min(cw - 4, grid[3] - grid[1] - 4);
            for (size_t k = 0; k < size_t(kColumns); ++k) {
                const size_t i = o.page[r] * kColumns + k;
                if (i >= opts.size())
                    break;
                const float sx = grid[0] + float(k) * cw + (cw - side) / 2, sy = grid[1] + (grid[3] - grid[1] - side) / 2;
                if (i == o.chosen[r]) {
                    UiQuad q;
                    q.x0 = sx - 2, q.y0 = sy - 2, q.x1 = sx + side + 2, q.y1 = sy + side + 2;
                    q.color = {1, 1, 0.55f, 0.95f};
                    out.quads.push_back(q);
                }
                UiQuad q;
                q.x0 = sx, q.y0 = sy, q.x1 = sx + side, q.y1 = sy + side;
                if (const RgbaImage* img = swatch(opts[i]->material))
                    q.image = img;
                else
                    q.color = {0.6f, 0.6f, 0.65f, 1};
                out.quads.push_back(q);
                targets_.push_back({{sx, sy, sx + side, sy + side}, Action::Swatch, int(r), i});
            }
        }
        if (auto p = rectOf(row->child(kRowPrev)))
            targets_.push_back({*p, Action::Prev, int(r), 0});
        if (auto n = rectOf(row->child(kRowNext)))
            targets_.push_back({*n, Action::Next, int(r), 0});
    }
    if (const UiElement* cf = root.child(kCostFrame)) {
        if (auto a = rectOf(cf->child(kAccept)))
            targets_.push_back({*a, Action::Accept, 0, 0});
        if (auto c = rectOf(cf->child(kCancel)))
            targets_.push_back({*c, Action::Cancel, 0, 0});
    }
}

bool DesignTool::press(simd_float2 p) {
    if (!open_)
        return false;
    for (const Target& t : std::vector<Target>(targets_)) {
        if (!inside(t.rect, p))
            continue;
        switch (t.action) {
        case Action::Swatch: choose(t.row, t.value); break;
        case Action::Prev:
            if (open_->page[size_t(t.row)] > 0)
                --open_->page[size_t(t.row)];
            break;
        case Action::Next: ++open_->page[size_t(t.row)]; break;
        case Action::Accept: accept(); break;
        case Action::Cancel: cancel(); break;
        }
        return true;
    }
    return inside(rect_, p);
}

} // namespace os2
