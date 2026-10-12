#include "cas_mode.hpp"
#include "app/scene_loader.hpp"

#include "app/cas_studio.hpp"
#include "app/ui_panel_util.hpp"
#include "formats/cas_thumbnails.hpp"
#include "formats/dbpf.hpp"
#include "formats/property_set.hpp"
#include "formats/font.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "formats/ui_resources.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#include "vm/family_write.hpp"
#include "vm/genetics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>
#include <unistd.h>

namespace os2 {

namespace fs = std::filesystem;

namespace {

// The family photo: the members' head shots side by side (two rows from
// four Sims on) over a soft blue backdrop, 512 x 512 like the shipped ones.
RgbaImage composeFamilyPhoto(const std::vector<const RgbaImage*>& heads) {
    constexpr uint32_t kSize = 512;
    RgbaImage out;
    out.width = out.height = kSize;
    out.pixels.resize(size_t(kSize) * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        const float t = float(y) / float(kSize - 1);
        for (uint32_t x = 0; x < kSize; ++x) {
            uint8_t* px = &out.pixels[(size_t(y) * kSize + x) * 4];
            px[0] = uint8_t(150 - 50 * t), px[1] = uint8_t(180 - 50 * t), px[2] = uint8_t(225 - 40 * t), px[3] = 255;
        }
    }
    const size_t n = heads.size();
    if (!n)
        return out;
    const size_t cols = n <= 3 ? n : (n + 1) / 2, rows = n <= 3 ? 1 : 2;
    const float stride = std::min(float(kSize) / float(cols), float(kSize) / float(rows));
    // One row: the shots overlap a little, so the family fills the picture.
    const float cell = rows == 1 ? std::min(float(kSize), stride * 1.3f) : stride;
    for (size_t i = 0; i < n; ++i) {
        const RgbaImage& h = *heads[i];
        if (!h.width || !h.height)
            continue;
        const size_t inRow = (i / cols + 1 == rows) ? n - (rows - 1) * cols : cols;
        const float x0 = (float(kSize) - float(inRow) * stride) / 2 + float(i % cols) * stride - (cell - stride) / 2;
        const float y0 = (float(kSize) - float(rows) * stride) / 2 + float(i / cols) * stride - (cell - stride) / 2 +
                          (rows == 1 ? 0.12f * cell : 0.0f);
        for (int dy = 0; dy < int(cell); ++dy)
            for (int dx = 0; dx < int(cell); ++dx) {
                const int ox = int(x0) + dx, oy = int(y0) + dy;
                if (ox < 0 || oy < 0 || ox >= int(kSize) || oy >= int(kSize))
                    continue;
                const float sx = (float(dx) + 0.5f) * float(h.width) / cell - 0.5f;
                const float sy = (float(dy) + 0.5f) * float(h.height) / cell - 0.5f;
                const int ix = std::clamp(int(std::floor(sx)), 0, int(h.width) - 1);
                const int iy = std::clamp(int(std::floor(sy)), 0, int(h.height) - 1);
                const int jx = std::min(ix + 1, int(h.width) - 1), jy = std::min(iy + 1, int(h.height) - 1);
                const float fx = std::clamp(sx - float(ix), 0.0f, 1.0f), fy = std::clamp(sy - float(iy), 0.0f, 1.0f);
                auto at = [&](int x, int y, int c) { return float(h.pixels[(size_t(y) * h.width + size_t(x)) * 4 + size_t(c)]); };
                uint8_t* px = &out.pixels[(size_t(oy) * kSize + size_t(ox)) * 4];
                const float a = (at(ix, iy, 3) * (1 - fx) + at(jx, iy, 3) * fx) * (1 - fy) +
                                (at(ix, jy, 3) * (1 - fx) + at(jx, jy, 3) * fx) * fy;
                for (int c = 0; c < 3; ++c) {
                    const float v = (at(ix, iy, c) * (1 - fx) + at(jx, iy, c) * fx) * (1 - fy) +
                                    (at(ix, jy, c) * (1 - fx) + at(jx, jy, c) * fx) * fy;
                    px[c] = uint8_t(std::lround(float(px[c]) * (1 - a / 255) + v * a / 255));
                }
            }
    }
    return out;
}

constexpr uint32_t kFrame = 0xCB980E50;
constexpr uint32_t kSimBin = 0xCB980E86, kMakeBaby = 0xCB980E87, kFamilyTree = 0x2C054EF0, kTieDialog = 0x2C054EF3,
                   kTreeButton = 0xCCB70001;
constexpr uint32_t kStepScripts[6] = {0xCB980E81, 0xCB980E88, 0xCB980E82, 0xCB980E85, 0xCB980E84, 0xCB980E83};
constexpr uint32_t kSubPanelAnchor = 0x2D050914;
constexpr uint32_t kPanelAnchor = 0x0000AAAA;

// CB980E50 windows.
constexpr uint32_t kControls = 0x8C6B1CA2, kStepButton0 = 0x8C6B0000, kDone = 0xEC6B2109, kCancel = 0xED012A1C,
                   kUndo = 0x8D24D87A, kRedo = 0x8D24D88D, kFirstNameText = 0x2CB7B3D6, kHousehold = 0x0C327D9F,
                   kFamilyNameGen = 0x6C6AE0BD, kRotationWindow = 0x4D097B6D, kPuck = 0x6CB7EA1A,
                   kAccept = 0x122, kExit = 0x105, kZoomIn = 0x2D486AC2, kZoomOut = 0x2D486ACD,
                   kTurnRight = 0x2CAFB772, kTurnLeft = 0x4CAFB7AA, kSimChooser = 0x4BDEFFAB;

// The face panels of CB980E82, in the order of faceTab_.
constexpr uint32_t kFaceTabs[7] = {0x4CB80000, 0x4CB80006, 0x4CB80001, 0x4CB80002, 0x4CB80003, 0x4CB80004, 0x4CB80005};
// The archetype thumbnails' region suffix per face panel (GetCASArchetypeThumbnailContext).
const char* const kRegionThumbs[7] = {"", "", "brow", "eyes", "nose", "mouth", "jaw"};
constexpr uint32_t kFacePanels[7] = {0x6CB8A784, 0xED2A53C1, 0x8CB8A7A5, 0xACB8A7BB, 0x2CB8A844, 0x4CB8A856, 0x4CB8A868};
// CB980E85's tabs in the order of cosmeticTab_ and the overlays component's titles.
constexpr uint32_t kCosmeticTabs[6] = {0x6C6E0002, 0x6C6E0003, 0x6C6E0001, 0x6C6E0000, 0x6C6E0004, 0x6C6E0005};
constexpr uint32_t kCosmeticTitles[6] = {0, 0x0DE60003, 0x0DE60001, 0x0DE60000, 0x0DE60004, 0x0DE60005};
// CB980E84's category tabs (0x02000000 + k, title 0x0DE6000k) and categories.
constexpr uint32_t kClothesTabK[6] = {0, 5, 6, 4, 3, 8};
constexpr uint32_t kClothesCategory[6] = {cas::category::Everyday, cas::category::Formal, cas::category::Underwear,
                                          cas::category::Pajamas,  cas::category::Swimwear, cas::category::Athletic};
constexpr uint32_t kClothesParts[3] = {cas::part::Top, cas::part::Bottom, cas::part::Body};
// CB980E83's trait bars, in Personality order (outgoing, nice, playful, neat, active).
constexpr uint32_t kTraitBars[5] = {0x4BDEFF00, 0x4BDEFF01, 0x4BDEFF02, 0x4BDEFF03, 0x4BDEFF04};

constexpr std::array<float, 4> kCellColor{0.80f, 0.84f, 0.98f, 1}, kCellSelected{1.0f, 0.95f, 0.55f, 1},
    kCellText{0.02f, 0.03f, 0.40f, 1}, kThumbBg{0.16f, 0.22f, 0.45f, 1}, kThumbSelectedBg{0.30f, 0.40f, 0.75f, 1};

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "k...=value" constants of the hidden text windows under `e`.
void readConstants(const UiElement& e, std::map<std::string, std::string>& out) {
    const std::string caption = e.get("caption");
    if (caption.size() > 1 && caption[0] == 'k')
        if (auto eq = caption.find('='); eq != std::string::npos)
            out.emplace(caption.substr(0, eq), caption.substr(eq + 1));
    for (const auto& c : e.children)
        readConstants(c, out);
}

int constantInt(const std::map<std::string, std::string>& c, const std::string& key, int fallback) {
    auto it = c.find(key);
    if (it == c.end())
        return fallback;
    try {
        return std::stoi(it->second, nullptr, 0);
    } catch (const std::logic_error&) {
        return fallback;
    }
}

// The readable part of a catalog name ("amhairshort_brown" -> "short brown").
std::string label(const std::string& name) {
    std::string n = name;
    for (const char* p : {"uuface_", "uuface-"})
        if (lower(n).starts_with(p))
            n = n.substr(std::string(p).size());
    if (n.size() > 2 && std::islower(static_cast<unsigned char>(n[0])) && std::islower(static_cast<unsigned char>(n[1])))
        for (const char* kind : {"hair", "body", "top", "bottom", "arch"})
            if (lower(n).substr(2).starts_with(kind)) {
                n = n.substr(2 + std::string(kind).size());
                break;
            }
    std::replace(n.begin(), n.end(), '_', ' ');
    return n;
}

} // namespace

CasMode::CasMode(Renderer& renderer, ResourceIndex& index, const fs::path& gameDir, std::vector<std::string> args)
    : renderer_(renderer), index_(index), gameDir_(gameDir), args_(std::move(args)) {
    const fs::path res = gameDir_ / "TSData/Res";
    catalog_ = std::make_unique<cas::CasCatalog>(cas::CasCatalog::load(index_));
    baker_ = std::make_unique<cas::FaceBaker>(index_, *catalog_);
    loading::phase("cas: catalog");
    res_ = std::make_unique<UiResources>(index_, res);
    fonts_ = std::make_unique<FontLibrary>(res / "UI/Fonts");
    thumbs_ = std::make_unique<cas::CasThumbnails>(res / "UserData/Thumbnails/CASThumbnails.package");
    for (uint32_t id : {kFrame, kStepScripts[0], kStepScripts[1], kStepScripts[2], kStepScripts[3], kStepScripts[4],
                        kStepScripts[5], kSimBin, kMakeBaby, kFamilyTree, kTieDialog})
        if (auto s = res_->script(id); s && !s->roots.empty())
            scripts_.emplace(id, std::move(*s));
    loading::phase("cas: ui");
    tempDir_ = fs::temp_directory_path() / ("opensims2-cas-" + std::to_string(getpid()));
    fs::create_directories(tempDir_);
    renderer_.hideTestScene();
    // The studio lot (app/cas_studio.hpp) unless --cas-no-studio.
    if (std::find(args_.begin(), args_.end(), "--cas-no-studio") == args_.end()) {
        studio_ = std::make_unique<CasStudio>(renderer_, index_, res);
        // Its walls cut away towards the camera, which looks at the podium from its front.
        if (!studio_->load(simd_make_float2(0, -1)))
            studio_.reset();
    }
    loading::phase("cas: studio");
    familyName_ = "";
    newSim();
}

CasMode::~CasMode() {
    std::error_code ec;
    fs::remove_all(tempDir_, ec);
}

std::unique_ptr<LotCamera> CasMode::makeCamera() {
    CameraDef def;
    def.name = "CAS";
    def.controller = "CreateASim";
    def.distances = {4.8f, 2.6f, 1.45f};
    def.fov = {30.0f};
    def.verticalFov = true; // our framing of the studio (the game's CAS camera is not decoded)
    def.pitchMin = {6.0f, 4.0f, 2.0f};
    def.pitchMax = {6.0f, 4.0f, 2.0f};
    def.nearClip = {0.1f};
    def.farClip = {200.0f};
    def.yaws = {90.0f};
    if (studio_) {
        const simd_float2 f = studio_->podium().facing;
        def.yaws = {std::atan2(f.y, f.x) * 180.0f / 3.14159265f};
    }
    def.zoomSmoothing = 7;
    def.spinSmoothing = 8;
    def.params["pivotheightabovefloor"] = 0;
    def.params["defaultminchange"] = 0.01f;
    auto cam = std::make_unique<LotCamera>(def);
    cam->setBounds(simd_make_float2(-100, -100), simd_make_float2(100, 100));
    camera_ = cam.get();
    zoom_ = 0;
    applyCamera(false);
    return cam;
}

simd_float3 CasMode::spot() const {
    if (!studio_)
        return simd_make_float3(0, 0, 0);
    if (editing_ >= 0 || studio_->familyMarks().empty())
        return studio_->podium().position;
    // The family: the middle of the marks its members take.
    simd_float3 c{0, 0, 0};
    const size_t n = std::clamp<size_t>(family_.size(), 1, studio_->familyMarks().size());
    for (size_t i = 0; i < n; ++i)
        c += studio_->familyMarks()[familyMark(i)].position;
    return c / float(n);
}

size_t CasMode::familyMark(size_t member) const {
    // Up to four in the back row (marks 0-3) from the middle out, then the
    // front row (4-7); inferred order.
    static const size_t order[8] = {2, 1, 3, 0, 5, 6, 4, 7};
    const size_t n = studio_ ? studio_->familyMarks().size() : 0;
    if (!n)
        return 0;
    return order[member % 8] < n ? order[member % 8] : member % n;
}

simd_float2 CasMode::facing() const {
    if (!studio_)
        return simd_make_float2(0, 1);
    if (editing_ >= 0 || studio_->familyMarks().empty())
        return studio_->podium().facing;
    // The family looks at its camera ("camera_family_0").
    if (auto it = studio_->cameras().find("camera_family_0"); it != studio_->cameras().end()) {
        const simd_float3 d = it->second - spot();
        if (simd_length(d.xy) > 0.01f)
            return simd_normalize(d.xy);
    }
    return studio_->familyMarks().front().facing;
}

float CasMode::simHeight() const {
    switch (edit_.spec.age) {
    case cas::age::Baby: return 0.45f;
    case cas::age::Toddler: return 0.75f;
    case cas::age::Child: return 1.15f;
    case cas::age::Teen: return 1.6f;
    default: return 1.75f;
    }
}

void CasMode::applyCamera(bool smooth) {
    if (!camera_)
        return;
    const float h = editing_ >= 0 ? simHeight() : 1.75f;
    const float z = zoom_ == 0 ? h * 0.5f : zoom_ == 1 ? h * 0.78f : h * 0.95f;
    // The Sim stands in the middle of the room the step panels leave on the
    // left (the frame is laid out at 1024 wide from the left edge; the panels
    // start at x 583): the pivot moves right of it (screen right is -X) by
    // that point's offset from the screen centre, at the zoom's distance
    // (vertical field of view 30 degrees).
    float side = 0;
    if (editing_ >= 0) {
        const float dist = zoom_ == 0 ? 4.8f : zoom_ == 1 ? 2.6f : 1.45f;
        const float aspect = float(std::max(canvasW_, 1)) / float(std::max(canvasH_, 1));
        const float halfW = dist * std::tan(15.0f * 3.14159265f / 180.0f) * aspect;
        const float ndc = 2.0f * 300.0f / float(std::max(canvasW_, 1)) - 1.0f;
        side = ndc * halfW;
    }
    // Screen right, with the camera on the side the Sims face; the camera
    // turns to that side.
    const simd_float2 f = facing();
    {
        const float yaw = std::atan2(f.y, f.x) * 180.0f / 3.14159265f;
        float d = std::fmod(yaw - camera_->yaw(), 360.0f);
        if (d > 180)
            d -= 360;
        if (d < -180)
            d += 360;
        if (std::fabs(d) > 0.01f)
            camera_->freeRotate(d);
    }
    const simd_float3 right = simd_make_float3(-f.y, f.x, 0);
    const simd_float3 p = spot() + right * -side + simd_make_float3(0, 0, z);
    if (smooth)
        camera_->moveTo(p);
    else {
        camera_->setPivot(p);
        camera_->snap();
    }
}

void CasMode::newSim() {
    edit_ = Member{};
    bioOpen_ = false;
    edit_.spec.gender = family_.size() % 2 ? cas::gender::Male : cas::gender::Female;
    edit_.spec.lastName = familyName_;
    edit_.spec.firstName.clear();
    // The base head of the age and gender ("..archbase_base") to start from.
    for (const SimPart* a : catalog_->archetypesFor(edit_.spec.age, edit_.spec.gender))
        if (lower(a->name()).find("archbase") != std::string::npos)
            edit_.spec.face.archetype = cas::family(*a);
    cas::completeSpec(*catalog_, edit_.spec);
    editing_ = int(family_.size());
    undo_.clear();
    redo_.clear();
    step_ = 0;
    heading_ = 0;
    rebuild();
}

void CasMode::randomize(uint32_t seed) {
    std::mt19937 rng(seed ? seed : std::random_device{}());
    cas::CharacterSpec& s = edit_.spec;
    const auto arch = catalog_->archetypesFor(s.age, s.gender);
    if (!arch.empty())
        s.face.archetype = cas::family(*arch[rng() % arch.size()]);
    s.face.modifiers.clear();
    s.face.regions.clear();
    s.skinTone = int(rng() % 4) + 1;
    s.hairTone = int(rng() % 4) + 1;
    const auto eyes = catalog_->overlaysFor(cas::overlay::Eyes, s.age, s.gender);
    if (!eyes.empty())
        s.eyes = cas::family(*eyes[rng() % std::min<size_t>(eyes.size(), 5)]); // not the alien eyes (inferred)
    s.overlays.clear();
    s.outfits.clear();
    cas::completeSpec(*catalog_, s, uint32_t(rng() | 1));
    // Personality: 25 points at random, then the sign.
    edit_.personality = cas::Personality{0, 0, 0, 0, 0};
    for (int i = 0; i < cas::kPersonalityPoints; ++i) {
        int t = int(rng() % 5);
        while (edit_.personality[t] >= 10)
            t = (t + 1) % 5;
        ++edit_.personality[t];
    }
    edit_.zodiac = cas::zodiacSign(edit_.personality);
    changed();
}

void CasMode::pushUndo() {
    undo_.push_back(edit_);
    if (undo_.size() > 50)
        undo_.erase(undo_.begin());
    redo_.clear();
}

void CasMode::undo() {
    if (undo_.empty())
        return;
    redo_.push_back(edit_);
    edit_ = undo_.back();
    undo_.pop_back();
    rebuild();
}

void CasMode::redo() {
    if (redo_.empty())
        return;
    undo_.push_back(edit_);
    edit_ = redo_.back();
    redo_.pop_back();
    rebuild();
}

namespace {
// The idle that loops for a Sim of the skeleton's age (auskel, cuskel, puskel,
// buskel): the first one of the age that the game has. Nobody stands in the
// rest pose (a T-pose) in the studio.
const Animation* findIdle(SimActor& actor, ResourceIndex& index, const std::string& sk, bool male) {
    std::vector<const char*> names;
    if (sk.starts_with("c"))
        names = {male ? "c-UIpose-M0" : "c-UIpose-F0", "c-idle-neutral-armsSide-wtC-breatheLoop50", "c-idle-neutral-armsSide-wtC-breatheLoop"};
    else if (sk.starts_with("p"))
        names = {male ? "p-UIpose-m0" : "p-UIpose-F0", "p-idle-neutral-breatheLoop-CAS", "p-idle-standing-loopBreathe", "p-idle-neutral-lookAround"};
    else if (sk.starts_with("b"))
        names = {"b-floor-idle-loopBreathe", "b-idle-carryBaby-breathe"};
    else
        names = {male ? "a-UIpose-M0" : "a-UIpose-F0", "a-idle-neutral-armsSide-wtC-breatheLoop"};
    for (const char* n : names)
        try {
            return &actor.animation(index, n);
        } catch (const std::exception&) {
        }
    return nullptr;
}
} // namespace

void CasMode::changed() {
    dirty_ = true;
    rebuild();
    ensureArchetypeShots();
}

void CasMode::ensureArchetypeShots() {
    if (editing_ < 0 || step_ != 1 || !thumbs_ || !catalog_)
        return;
    for (const SimPart* a : catalog_->archetypesFor(edit_.spec.age, edit_.spec.gender)) {
        const std::string fam = cas::family(*a);
        if (thumbs_->image(cas::archetypeThumbnailId(edit_.spec.age, edit_.spec.gender, fam, "")))
            continue;
        const std::string key = cas::casFileName(edit_.spec.age, edit_.spec.gender) + "|" + fam + "|" +
                                std::to_string(edit_.spec.skinTone);
        if (archetypeShots_.count(key))
            continue;
        Member m = edit_;
        m.spec.face.archetype = fam;
        m.spec.face.structure.reset();
        m.spec.face.regions.clear();
        m.spec.guid = 0; // its own resource names: the baked face of the Sim shown must not be reused
        for (auto& [category, parts] : m.spec.outfits) // bald, like the cached thumbnails
            for (auto it = parts.begin(); it != parts.end();)
                it = lower(it->second).find("hair") != std::string::npos ? parts.erase(it) : std::next(it);
        archetypeShots_[key] = portrait(m, 160, 0.62f);
    }
    dirty_ = true;
}

void CasMode::rebuild() {
    dirty_ = true;
    // The Sims shown: the one edited, or the whole family side by side.
    std::vector<const Member*> members;
    if (editing_ >= 0)
        members.push_back(&edit_);
    else
        for (const auto& m : family_)
            members.push_back(&m);
    const float spacing = 0.8f;
    std::vector<Shown> next;
    for (size_t i = 0; i < members.size(); ++i) {
        const Member& m = *members[i];
        const fs::path file = tempDir_ / ("sim" + std::to_string(rebuilds_++) + ".package");
        try {
            cas::CharacterSpec spec = m.spec;
            if (spec.firstName.empty())
                spec.firstName = "Sim";
            const auto r = cas::writeCharacter(index_, *catalog_, *baker_, spec, "CAS_Preview", file, false);
            for (const auto& w : r.warnings)
                warnings_.push_back(w);
            const uint32_t category =
                editing_ >= 0 && step_ == 4 ? kClothesCategory[std::clamp(clothesCategory_, 0, 5)] : 1;
            auto actor = std::make_unique<SimActor>(loadCharacterSim(index_, catalog_->parts, file, category));
            for (const auto& w : actor->skin.model.warnings)
                warnings_.push_back(w);
            const std::string sk = lower(actor->skin.skeleton.name);
            if (std::getenv("OS2_CAS_DEBUG"))
                std::fprintf(stderr, "cas: skeleton '%s'\n", actor->skin.skeleton.name.c_str());
            if (const auto* idle = findIdle(*actor, index_, sk, spec.gender == cas::gender::Male))
                actor->cue(-0.3 * double(i), *idle);
            // The first member on the left of the screen (screen right is -X).
            const simd_float2 f = facing();
            const simd_float3 right = simd_make_float3(-f.y, f.x, 0);
            simd_float3 at = spot() + right * (members.size() > 1 ? (float(i) - float(members.size() - 1) / 2) * spacing : 0.0f);
            float heading = baseHeading() + (editing_ >= 0 ? heading_ : 0);
            if (studio_ && editing_ < 0 && !studio_->familyMarks().empty()) {
                // The family area's marks, each with its own turn.
                const auto& mark = studio_->familyMarks()[familyMark(i)];
                at = mark.position;
                heading = std::atan2(-mark.facing.x, mark.facing.y);
            }
            actor->position = {at.x, at.y, at.z};
            actor->heading = heading; // the skeleton faces +Y
            actor->update(lastUpdate_);
            Shown s;
            actor->model = renderer_.addObject(actor->skin.model, index_, true);
            if (i < shown_.size() && shown_[i].instance >= 0) {
                s.instance = shown_[i].instance;
                renderer_.setInstanceModel(s.instance, actor->model);
            } else {
                s.instance = renderer_.addInstance(actor->model, matrix_identity_float4x4);
            }
            s.actor = std::move(actor);
            next.push_back(std::move(s));
        } catch (const std::exception& e) {
            warnings_.push_back(std::string("rebuild: ") + e.what());
        }
        std::error_code ec;
        fs::remove(file, ec);
    }
    // Instances no longer used show nothing (an empty model).
    for (size_t i = next.size(); i < shown_.size(); ++i)
        if (shown_[i].instance >= 0) {
            ObjectModel empty;
            renderer_.setInstanceModel(shown_[i].instance, renderer_.addObject(empty, index_, true));
            Shown s;
            s.instance = shown_[i].instance;
            next.push_back(std::move(s));
        }
    shown_ = std::move(next);
    if (studio_) {
        studio_->applyLights(spot(), editing_ < 0);
        for (const auto& sh : shown_)
            if (sh.instance >= 0)
                renderer_.setInstanceLightSet(sh.instance, 1);
    }
    applyCamera(true);
}

void CasMode::update(double seconds) {
    const double dt = std::clamp(seconds - lastUpdate_, 0.0, 0.25);
    lastUpdate_ = seconds;
    if (turning_ && editing_ >= 0)
        heading_ += float(turning_) * float(dt) * 2.5f;
    for (auto& s : shown_) {
        if (!s.actor)
            continue;
        if (editing_ >= 0)
            s.actor->heading = baseHeading() + heading_;
        s.actor->update(seconds);
        renderer_.updateObject(s.actor->model, s.actor->posed);
    }
}

void CasMode::setStep(int step) {
    if (editing_ < 0)
        return;
    const int before = step_;
    step_ = std::clamp(step, 0, 5);
    bioOpen_ = false;
    if (focus_ == Focus::Biography)
        focus_ = Focus::None;
    // Face steps look at the face.
    zoom_ = (step_ == 1 || step_ == 2 || step_ == 3) ? 2 : 0;
    if (camera_) {
        camera_->reset();
        camera_->zoomBy(zoom_);
    }
    applyCamera(true);
    dirty_ = true;
    if ((before == 4) != (step_ == 4) || step_ == 4)
        rebuild(); // the clothes step shows the category being chosen
    ensureArchetypeShots();
}

std::shared_ptr<RgbaImage> CasMode::portrait(const Member& m, uint32_t size, float closeness) {
    const fs::path file = tempDir_ / ("portrait" + std::to_string(rebuilds_++) + ".package");
    std::shared_ptr<RgbaImage> out;
    try {
        cas::CharacterSpec spec = m.spec;
        if (spec.firstName.empty())
            spec.firstName = "Sim";
        cas::writeCharacter(index_, *catalog_, *baker_, spec, "CAS_Portrait", file, false);
        SimActor actor(loadCharacterSim(index_, catalog_->parts, file, 1));
        const std::string sk = lower(actor.skin.skeleton.name);
        if (const auto* idle = findIdle(actor, index_, sk, spec.gender == cas::gender::Male))
            actor.cue(0, *idle);
        // Alone, far from the studio, facing +Y.
        const simd_float3 at = simd_make_float3(1000, 1000, 0);
        actor.position = {at.x, at.y, at.z};
        actor.heading = 0;
        actor.update(0.4);
        const int model = renderer_.addObject(actor.skin.model, index_, true);
        if (portraitInstance_ < 0)
            portraitInstance_ = renderer_.addInstance(model, matrix_identity_float4x4);
        else
            renderer_.setInstanceModel(portraitInstance_, model);
        renderer_.updateObject(model, actor.posed);
        renderer_.setInstanceLightSet(portraitInstance_, studio_ ? 1 : 0);
        const float h = [&] {
            switch (spec.age) {
            case cas::age::Baby: return 0.25f;
            case cas::age::Toddler: return 0.40f;
            case cas::age::Child: return 1.17f;
            case cas::age::Teen: return 1.5f;
            default: return 1.64f;
            }
        }();
        const float dist = (spec.age == cas::age::Toddler || spec.age == cas::age::Baby ? 0.8f : 1.0f) * closeness;
        const float aim = h + (1 - closeness) * 0.3f; // closer shots look at the head, not the neck
        const simd_float3 e = at + simd_make_float3(0, dist, aim + 0.03f), target = at + simd_make_float3(0, 0, aim);
        const simd_float3 f = simd_normalize(target - e);
        const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
        const simd_float3 u = simd_cross(sv, f);
        FrameView fv;
        fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                              simd_make_float4(sv.z, u.z, -f.z, 0),
                              simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
        const float n = 0.05f, fa = 50, ys = 1.0f / std::tan(15.0f * 3.14159265f / 180.0f), zs = fa / (n - fa);
        fv.projection = simd_matrix(simd_make_float4(ys, 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                    simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
        capturing_ = true;
        out = std::make_shared<RgbaImage>(renderer_.capture(size, size, fv, simd_make_float4(0, 0, 0, 0)));
        capturing_ = false;
        ObjectModel empty;
        renderer_.setInstanceModel(portraitInstance_, renderer_.addObject(empty, index_, true));
    } catch (const std::exception& e) {
        capturing_ = false;
        warnings_.push_back(std::string("portrait: ") + e.what());
    }
    std::error_code ec;
    fs::remove(file, ec);
    return out;
}

void CasMode::setTie(int a, int b, int choice) {
    if (a < 0 || b < 0 || a == b || size_t(std::max(a, b)) >= family_.size())
        return;
    Member& A = family_[size_t(a)];
    Member& B = family_[size_t(b)];
    if (!A.id)
        A.id = nextId_++;
    if (!B.id)
        B.id = nextId_++;
    A.ties.erase(B.id);
    B.ties.erase(A.id);
    auto parentSlot = [&](const Member& child) {
        for (const auto& [o, t] : child.ties)
            if (t == 0)
                return 1u; // the other parent
        return 0u;
    };
    switch (choice) {
    case 1: A.ties[B.id] = 3, B.ties[A.id] = 3; break; // siblings
    case 2: A.ties[B.id] = 2, B.ties[A.id] = 2; break; // spouses
    case 3: A.ties[B.id] = parentSlot(A), B.ties[A.id] = 4; break; // A a child of B
    case 4: B.ties[A.id] = parentSlot(B), A.ties[B.id] = 4; break; // A a parent of B
    default: break;                                                  // roommates
    }
}

void CasMode::makeChild() {
    if (mabParent_[0] < 0 || mabParent_[1] < 0 || size_t(std::max(mabParent_[0], mabParent_[1])) >= family_.size())
        return;
    const Member& a = family_[size_t(mabParent_[0])];
    const Member& b = family_[size_t(mabParent_[1])];
    std::mt19937 rng(std::random_device{}());
    auto random = [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; };
    auto dnaOf = [&](const Member& m) {
        return genetics::dnaFromLook(cas::toneFamily(m.spec.hairTone), cas::toneFamily(m.spec.skinTone), m.spec.eyes,
                                     m.spec.gender == cas::gender::Female, random);
    };
    const genetics::Dna da = dnaOf(a), db = dnaOf(b);
    genetics::Dna dna = genetics::determineGenetics(da, db, genetics::baseGameGeneticValues(), random,
                                                    genetics::baseGameSkinTones());
    const genetics::DescendantFace df = genetics::determineDescendantFace(da, db, random);
    Member child;
    cas::CharacterSpec& s = child.spec;
    s.age = mabAge_;
    s.gender = mabGender_ ? mabGender_
                          : (genetics::babyIsFemale(false, false, random) ? cas::gender::Female : cas::gender::Male);
    if (int t = cas::toneNumber(dna.expressed(genetics::gene::Hair)))
        s.hairTone = std::clamp(t, 1, 4);
    if (int t = cas::toneNumber(dna.expressed(genetics::gene::Skin)))
        s.skinTone = std::clamp(t, 1, 4);
    if (!dna.expressed(genetics::gene::Eyes).empty())
        s.eyes = dna.expressed(genetics::gene::Eyes);
    // The face: the parents' structures mixed region by region.
    const cas::FaceStructure fa = baker_->structureOf(a.spec.face, a.spec.gender);
    const cas::FaceStructure fb = baker_->structureOf(b.spec.face, b.spec.gender);
    s.face.structure = std::make_shared<cas::FaceStructure>(baker_->descendant(fa, fb, df.regions));
    s.lastName = familyName_;
    cas::completeSpec(*catalog_, s, uint32_t(rng() | 1));
    child.personality = cas::Personality{0, 0, 0, 0, 0};
    for (int i = 0; i < cas::kPersonalityPoints; ++i) {
        int t = int(rng() % 5);
        while (child.personality[t] >= 10)
            t = (t + 1) % 5;
        ++child.personality[t];
    }
    child.zodiac = cas::zodiacSign(child.personality);
    child.parents = {a.id, b.id};
    child.ties[a.id] = 0; // my parent is
    child.ties[b.id] = 1; // the other parent
    child.portrait = portrait(child, 256);
    mabChild_ = std::move(child);
    std::string regions;
    for (const auto& [r, p] : df.regions)
        regions += r + (p ? "<" + (b.spec.firstName.empty() ? std::string("2") : b.spec.firstName) + " "
                          : "<" + (a.spec.firstName.empty() ? std::string("1") : a.spec.firstName) + " ");
    status_ = "Filho: " + regions;
    dirty_ = true;
}

void CasMode::loadBin() {
    if (binLoaded_)
        return;
    binLoaded_ = true;
    binFile_ = gameDir_ / "TSData/Res/Catalog/Bins/globalcatbin.bundle.package";
    try {
        DbpfPackage pkg(binFile_);
        constexpr uint32_t kBinx = 0x0C560F39, kIdr = 0xAC506764, kAged = 0xAC598EAC;
        std::map<std::pair<uint32_t, uint32_t>, const DbpfEntry*> idrs, ageds;
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type == kIdr)
                idrs[{e.tgi.group, e.tgi.instance}] = &e;
            else if (e.tgi.type == kAged)
                ageds[{e.tgi.group, e.tgi.instance}] = &e;
        }
        for (const auto& e : pkg.entries()) {
            if (e.tgi.type != kBinx)
                continue;
            const auto props = readPropertySet(pkg.read(e));
            auto num = [&](const char* k) -> uint32_t {
                auto it = props.find(k);
                return it == props.end() ? 0 : uint32_t(std::strtoul(it->second.c_str(), nullptr, 0));
            };
            const DbpfEntry* idr = idrs.count({e.tgi.group, e.tgi.instance}) ? idrs[{e.tgi.group, e.tgi.instance}] : nullptr;
            if (!idr)
                continue;
            const auto refs = parseReferenceList(pkg.read(*idr));
            const uint32_t oi = num("objectidx");
            if (oi >= refs.size() || refs[oi].type != kAged)
                continue;
            const Tgi aged = refs[oi];
            if (auto ai = ageds.find({aged.group, aged.instance}); ai != ageds.end()) {
                {
                    const auto ap = readPropertySet(pkg.read(*ai->second));
                    BinSim b;
                    b.group = aged.group;
                    b.age = ap.count("age") ? uint32_t(std::strtoul(ap.at("age").c_str(), nullptr, 0)) : 0;
                    b.gender = ap.count("gender") ? uint32_t(std::strtoul(ap.at("gender").c_str(), nullptr, 0)) : 0;
                    b.sort = int(num("sortindex"));
                    b.thumbnail = cas::outfitDataThumbnailId(aged, false);
                    bin_.push_back(b);
                }
            }
        }
        std::stable_sort(bin_.begin(), bin_.end(), [](const BinSim& a, const BinSim& b) { return a.sort < b.sort; });
    } catch (const std::exception& e) {
        warnings_.push_back(std::string("sim bin: ") + e.what());
    }
}

bool CasMode::openCharacter(const fs::path& file, uint32_t group) {
    try {
        DbpfPackage pkg(file);
        Member m;
        m.spec = cas::readCharacterSpec(pkg, group, *catalog_);
        cas::completeSpec(*catalog_, m.spec);
        edit_ = std::move(m);
        if (!edit_.spec.lastName.empty() && familyName_.empty())
            familyName_ = edit_.spec.lastName;
        editing_ = int(family_.size());
        undo_.clear();
        redo_.clear();
        step_ = 0;
        status_ = edit_.spec.firstName.empty() ? (group != 0xFFFFFFFF ? std::string("Sim do estoque") : "Editando " + file.stem().string())
                                               : "Editando " + edit_.spec.firstName;
        rebuild();
        return true;
    } catch (const std::exception& e) {
        status_ = "Não abriu " + file.filename().string() + ": " + e.what();
        dirty_ = true;
        return false;
    }
}

void CasMode::finishSim() {
    if (editing_ < 0)
        return;
    if (edit_.spec.firstName.empty())
        edit_.spec.firstName = edit_.spec.gender == cas::gender::Female ? "Simone" : "Simão";
    if (!edit_.zodiacChosen)
        edit_.zodiac = cas::zodiacSign(edit_.personality);
    if (edit_.spec.age <= cas::age::Child || edit_.spec.age == cas::age::Baby)
        edit_.aspiration = cas::aspiration::GrowUp;
    else if (!edit_.aspiration || edit_.aspiration == cas::aspiration::GrowUp)
        edit_.aspiration = cas::suggestedAspiration(edit_.zodiac); // none chosen: the sign's (inferred)
    if (!edit_.id)
        edit_.id = nextId_++;
    if (!edit_.thumbnail)
        edit_.portrait = portrait(edit_);
    if (size_t(editing_) < family_.size())
        family_[size_t(editing_)] = edit_;
    else
        family_.push_back(edit_);
    editing_ = -1;
    zoom_ = 0;
    if (camera_)
        camera_->reset();
    status_ = edit_.spec.firstName + " entrou na família";
    rebuild();
}

void CasMode::acceptFamily() {
    if (editing_ >= 0)
        finishSim();
    if (family_.empty()) {
        status_ = "A família precisa de pelo menos um Sim";
        dirty_ = true;
        return;
    }
    // ValidateHouseHold: a family name, and an adult or elder when there are children.
    bool grown = false;
    for (const auto& m : family_)
        grown |= m.spec.age == cas::age::Teen || m.spec.age == cas::age::Adult || m.spec.age == cas::age::Elder;
    if (!grown) {
        status_ = "Adicione um adulto ou idoso à família"; // STR# 0x8C #10
        dirty_ = true;
        return;
    }
    if (familyName_.empty())
        familyName_ = family_.front().spec.lastName.empty() ? "Família" : family_.front().spec.lastName;
    auto arg = [&](const std::string& key) -> std::optional<std::string> {
        for (size_t i = 1; i + 1 < args_.size(); ++i)
            if (args_[i] == key)
                return args_[i + 1];
        return std::nullopt;
    };
    const fs::path out = arg("--cas-out").value_or(vm::defaultUserNeighborhoods().string());
    try {
        if (auto hood = arg("--cas-hood")) {
            fs::path src = *hood;
            if (!fs::exists(src))
                src = gameDir_ / "TSData/Res/UserData/Neighborhoods" / *hood;
            // The user's own copy of the neighborhood is written in place;
            // the shipped one (under the game data) into a copy under --cas-out.
            std::error_code ec;
            const fs::path canonSrc = fs::weakly_canonical(src, ec), canonGame = fs::weakly_canonical(gameDir_, ec);
            const std::string rel = canonSrc.lexically_relative(canonGame).string();
            const bool shipped = !rel.empty() && rel.rfind("..", 0) != 0;
            const fs::path target = shipped ? out / src.filename() : src;
            std::vector<vm::NewSim> sims;
            std::vector<std::shared_ptr<RgbaImage>> heads;
            std::mt19937 rng(std::random_device{}());
            for (auto& m : family_) {
                vm::NewSim s;
                s.firstName = m.spec.firstName;
                s.lastName = m.spec.lastName.empty() ? familyName_ : m.spec.lastName;
                s.ageBit = m.spec.age;
                s.genderBit = m.spec.gender;
                s.fitness = m.spec.fitness;
                s.personality = m.personality;
                s.aspiration = m.aspiration;
                s.zodiac = m.zodiac;
                const auto dna = genetics::dnaFromLook(cas::toneFamily(m.spec.hairTone), cas::toneFamily(m.spec.skinTone),
                                                       m.spec.eyes, m.spec.gender == cas::gender::Female,
                                                       [&](uint32_t n) { return n ? uint32_t(rng() % n) : 0u; });
                s.dna = dna.toProperties();
                for (const auto& [otherId, tie] : m.ties)
                    for (size_t k = 0; k < family_.size(); ++k)
                        if (family_[k].id == otherId)
                            s.ties.push_back({k, tie});
                s.clothing = cas::familyBinClothing(*catalog_, m.spec); // the family's clothing bin
                cas::CharacterSpec spec = m.spec;
                spec.lastName = s.lastName;
                // The head shot the family panels and the lot's Sim picker show.
                std::shared_ptr<RgbaImage> head = m.portrait ? m.portrait : portrait(m);
                if (head) {
                    spec.portraitPng = encodePng(*head);
                    heads.push_back(head);
                }
                s.writeCharacter = [this, spec](const std::string& objectName, const fs::path& file) {
                    return cas::writeCharacter(index_, *catalog_, *baker_, spec, objectName, file).guid;
                };
                sims.push_back(std::move(s));
            }
            const auto r = vm::writeNewFamily(src, target, familyName_, "", std::move(sims), 1, !shipped);
            // The photo the family bin and the lot panel show.
            if (!heads.empty()) {
                std::vector<const RgbaImage*> ptrs;
                for (const auto& h : heads)
                    ptrs.push_back(h.get());
                try {
                    vm::writeFamilyThumbnail(r.neighborhood, r.family, encodePng(composeFamilyPhoto(ptrs)));
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: cas: family photo: %s\n", e.what());
                }
            }
            status_ = "Família " + familyName_ + " (" + std::to_string(r.family) + ") salva em " + r.neighborhood.string();
            std::printf("opensims2: cas: family %u \"%s\" written to %s:", r.family, familyName_.c_str(),
                        r.neighborhood.string().c_str());
            for (const auto& c : r.characters)
                std::printf(" %s", c.filename().string().c_str());
            std::printf("\n");
            std::fflush(stdout);
            if (onFamilySaved)
                onFamilySaved(r.family);
        } else {
            // No neighborhood: the character files alone.
            fs::create_directories(out / "CAS");
            int n = 0;
            for (const auto& m : family_) {
                char stem[64];
                std::snprintf(stem, sizeof stem, "CAS_User%05d", n++);
                const fs::path file = out / "CAS" / (std::string(stem) + ".package");
                cas::writeCharacter(index_, *catalog_, *baker_, m.spec, stem, file);
                std::printf("opensims2: cas: wrote %s\n", file.string().c_str());
            }
            status_ = "Sims salvos em " + (out / "CAS").string();
        }
    } catch (const std::exception& e) {
        status_ = std::string("Erro ao salvar: ") + e.what();
        std::fprintf(stderr, "opensims2: cas: %s\n", e.what());
    }
    std::fflush(stdout);
    dirty_ = true;
}

void CasMode::attach(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat) {
    for (auto& l : layers_)
        l = std::make_unique<UiLayer>(device, colorFormat, depthFormat);
}

void CasMode::setCanvas(int width, int height) {
    if (width != canvasW_ || height != canvasH_) {
        canvasW_ = width, canvasH_ = height, dirty_ = true;
        applyCamera(false); // the Sim's place depends on the width
    }
}

void CasMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0 || capturing_)
        return;
    const int cw = int(std::lround(float(width) / std::max(scale, 0.01f)));
    const int ch = int(std::lround(float(height) / std::max(scale, 0.01f)));
    setCanvas(cw, ch);
    if (dirty_ || laidW_ != cw || laidH_ != ch) {
        layer_ = (layer_ + 1) % layers_.size();
        const UiScreen sc = screen(cw, ch);
        layers_[layer_]->setScreen(sc);
    }
    layers_[layer_]->encode(enc, width, height);
}

void CasMode::mouseDown(simd_float2 p) {
    if (dirty_ || laidW_ != canvasW_ || laidH_ != canvasH_) {
        screen(canvasW_, canvasH_);
        dirty_ = true; // the layout only found the targets: encode still has to upload it
    }
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it)
        if (inside(it->rect, p)) {
            auto action = it->action; // the action may lay out again
            if (playSound && !it->sound.empty())
                playSound(it->sound);
            action(p);
            dirty_ = true;
            return;
        }
    for (const auto& r : solid_)
        if (inside(r, p))
            return;
    focus_ = Focus::None;
    if (inside(rotationRect_, p) && editing_ >= 0) {
        dragging_ = true;
        dragFrom_ = p;
    }
}

void CasMode::mouseDragged(simd_float2 p) {
    if (!dragging_)
        return;
    heading_ += (p.x - dragFrom_.x) * 0.01f;
    dragFrom_ = p;
}

void CasMode::mouseUp(simd_float2) { dragging_ = false; }

void CasMode::keyDown(unsigned short keyCode, const std::string& chars, bool control) {
    constexpr unsigned short kZ = 6, kY = 16, kComma = 43, kPeriod = 47, kBackspace = 51, kReturn = 36;
    if (focus_ != Focus::None) {
        std::string& text = focus_ == Focus::FirstName ? edit_.spec.firstName
                            : focus_ == Focus::Biography ? edit_.spec.description
                                                         : familyName_;
        const size_t limit = focus_ == Focus::Biography ? 512 : 32; // inferred: the bio's limit was not read
        if (keyCode == kBackspace) {
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
                text.pop_back(); // UTF-8 continuation bytes
            if (!text.empty())
                text.pop_back();
        } else if (keyCode == kReturn) {
            focus_ = Focus::None;
        } else {
            for (char c : chars)
                if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F && text.size() < limit)
                    text += c;
        }
        if (focus_ == Focus::FamilyName)
            for (auto& m : family_)
                m.spec.lastName = familyName_;
        dirty_ = true;
        return;
    }
    if (control && keyCode == kZ)
        undo();
    else if (control && keyCode == kY)
        redo();
    else if (keyCode == kComma)
        turning_ = -1; // the CAS keyboard file: , and . turn the Sim while held
    else if (keyCode == kPeriod)
        turning_ = 1;
}

void CasMode::keyUp(unsigned short keyCode) {
    if (keyCode == 43 || keyCode == 47)
        turning_ = 0;
}

std::string CasMode::title() const { return "opensims2 - Criar um Sim"; }

std::vector<std::string> CasMode::report() const {
    std::vector<std::string> out;
    out.push_back("cas: catalog " + std::to_string(catalog_->archetypes.size()) + " archetypes, " +
                  std::to_string(catalog_->modifiers.size()) + " modifiers, " + std::to_string(catalog_->parts.size()) +
                  " parts; family of " + std::to_string(family_.size()) + (editing_ >= 0 ? ", editing step " + std::to_string(step_ + 1) : ""));
    if (editing_ >= 0) {
        const auto& s = edit_.spec;
        std::string o = "cas: " + (s.firstName.empty() ? std::string("(no name)") : s.firstName) + " " + cas::ageName(s.age) +
                        (s.gender == cas::gender::Female ? " female" : " male") + ", skin s" + std::to_string(s.skinTone) +
                        ", hair tone " + std::to_string(s.hairTone) + ", outfit:";
        if (auto it = s.outfits.find(cas::category::Everyday); it != s.outfits.end())
            for (const auto& [bit, name] : it->second)
                o += " " + name;
        out.push_back(o);
    }
    for (size_t i = 0; i < warnings_.size() && i < 8; ++i)
        out.push_back("cas: warning: " + warnings_[i]);
    if (!status_.empty())
        out.push_back("cas: " + status_);
    return out;
}

void CasMode::setGender(uint32_t g, bool undo) {
    if (edit_.spec.gender == g)
        return;
    if (undo)
        pushUndo();
    edit_.spec.gender = g;
    edit_.spec.outfits.clear();
    edit_.spec.overlays.clear();
    edit_.spec.face.modifiers.clear();
    edit_.spec.face.regions.clear();
    for (const SimPart* a : catalog_->archetypesFor(edit_.spec.age, g))
        if (lower(a->name()).find("archbase") != std::string::npos)
            edit_.spec.face.archetype = cas::family(*a);
    cas::completeSpec(*catalog_, edit_.spec);
    changed();
}

void CasMode::runScripted(const std::vector<std::string>& args) {
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        const std::string& a = args[i];
        const std::string& v = args[i + 1];
        if (a == "--cas-random")
            randomize(uint32_t(std::stoul(v)));
        else if (a == "--cas-name") {
            const auto comma = v.find(',');
            edit_.spec.firstName = v.substr(0, comma);
            if (comma != std::string::npos) {
                familyName_ = v.substr(comma + 1);
                edit_.spec.lastName = familyName_;
            }
            dirty_ = true;
        } else if (a == "--cas-click") { // x,y: a mouse press (the click sound plays)
            float x = 0, y = 0;
            if (std::sscanf(v.c_str(), "%f,%f", &x, &y) == 2)
                mouseDown({x, y});
        } else if (a == "--cas-step")
            setStep(std::stoi(v) - 1);
        else if (a == "--cas-tab") {
            if (step_ == 2)
                faceTab_ = std::clamp(std::stoi(v), 0, 6);
            else if (step_ == 3)
                cosmeticTab_ = std::clamp(std::stoi(v), 0, 5);
            dirty_ = true;
        } else if (a == "--cas-category") {
            clothesCategory_ = std::clamp(std::stoi(v), 0, 5);
            rebuild();
        } else if (a == "--cas-part") {
            clothesPart_ = std::clamp(std::stoi(v), 0, 2);
            dirty_ = true;
        } else if (a == "--cas-gender") { // m/male or f/female, as the gender buttons
            const bool male = v == "m" || v == "male" || v == "M" || v == "Male";
            setGender(male ? cas::gender::Male : cas::gender::Female, false);
        } else if (a == "--cas-age") {
            edit_.spec.age = v == "toddler" ? cas::age::Toddler : v == "child" ? cas::age::Child : v == "teen" ? cas::age::Teen
                             : v == "elder" ? cas::age::Elder : v == "baby" ? cas::age::Baby : cas::age::Adult;
            edit_.spec.outfits.clear();
            edit_.spec.overlays.clear();
            for (const SimPart* ar : catalog_->archetypesFor(edit_.spec.age, edit_.spec.gender))
                if (lower(ar->name()).find("archbase") != std::string::npos)
                    edit_.spec.face.archetype = cas::family(*ar);
            cas::completeSpec(*catalog_, edit_.spec);
            changed();
        } else if (a == "--cas-skin") { // skin tone 1-4 (Light .. Dark), as the tone buttons
            edit_.spec.skinTone = std::clamp(std::stoi(v), 1, 4);
            edit_.spec.outfits.erase(cas::category::Naked);
            cas::completeSpec(*catalog_, edit_.spec);
            changed();
        } else if (a == "--cas-bio") { // the biography window opened and its text typed
            setStep(0);
            bioOpen_ = true;
            edit_.spec.description = v;
            dirty_ = true;
        } else if (a == "--cas-done")
            finishSim();
        else if (a == "--cas-new")
            newSim();
        else if (a == "--cas-accept")
            acceptFamily();
        else if (a == "--cas-edit")
            openCharacter(v);
        else if (a == "--cas-tie") { // a,b,choice (0 roomie, 1 sibling, 2 spouse, 3 child of, 4 parent of)
            int x = -1, y = -1, c = 0;
            if (std::sscanf(v.c_str(), "%d,%d,%d", &x, &y, &c) == 3)
                setTie(x, y, c);
            treeOpen_ = true;
            dirty_ = true;
        }
        else if (a == "--cas-baby") { // Make a Child of the family's first two grown-ups, at an age
            mabAge_ = v == "child" ? cas::age::Child : v == "teen" ? cas::age::Teen : cas::age::Toddler;
            mabParent_[0] = mabParent_[1] = -1;
            for (size_t k = 0; k < family_.size(); ++k) {
                if (family_[k].spec.age != cas::age::Adult && family_[k].spec.age != cas::age::Elder)
                    continue;
                if (mabParent_[0] < 0)
                    mabParent_[0] = int(k);
                else if (mabParent_[1] < 0)
                    mabParent_[1] = int(k);
            }
            mabOpen_ = mabParent_[1] >= 0;
            if (mabOpen_)
                makeChild();
            dirty_ = true;
        }
        else if (a == "--cas-turn")
            heading_ = std::stof(v) * 3.14159265f / 180;
        else if (a == "--click") {
            float x, y;
            if (std::sscanf(v.c_str(), "%f,%f", &x, &y) == 2) {
                mouseDown(simd_make_float2(x, y));
                mouseUp(simd_make_float2(x, y));
            }
        } else if (a == "--cas-type") {
            for (char c : v)
                keyDown(0xFFFF, std::string(1, c), false);
        }
    }
}

UiScreen CasMode::screen(int width, int height) {
    UiScreen out;
    out.width = width, out.height = height;
    laidW_ = width, laidH_ = height;
    targets_.clear();
    solid_.clear();
    keep_.clear();
    dirty_ = false;
    auto frameIt = scripts_.find(kFrame);
    if (frameIt == scripts_.end())
        return out;
    const UiElement& frame = frameIt->second.roots.front();
    std::map<const UiElement*, std::array<float, 4>> rects;
    const float ox = 0, oy = float(height - 768); // the frame is laid out for 1024x768, anchored bottom left

    const FontStyle* style = fonts_->style("CASText");
    if (!style)
        style = fonts_->style("GenBodyMedium");
    const Font* font = style ? fonts_->font(*style) : nullptr;
    const float ppem = 11.0f;
    auto text = [&](float x, float y, float maxW, const std::string& s, std::array<float, 4> c) {
        if (!font || s.empty())
            return;
        std::u32string u = decodeUtf8(s);
        while (u.size() > 1 && measureText(*font, u, ppem).width > maxW)
            u.pop_back();
        const TextMetrics m = measureText(*font, u, ppem);
        auto img = std::make_unique<RgbaImage>();
        img->width = uint32_t(std::ceil(m.width)) + 2;
        img->height = uint32_t(std::ceil(m.ascent + m.descent)) + 2;
        img->pixels.assign(size_t(img->width) * img->height * 4, 0);
        const uint8_t rgba[4] = {uint8_t(c[0] * 255), uint8_t(c[1] * 255), uint8_t(c[2] * 255), 255};
        for (size_t i = 0; i < img->pixels.size(); i += 4)
            std::copy_n(rgba, 3, &img->pixels[i]);
        drawText(*img, *font, u, 1, 1 + std::round(m.ascent), ppem, rgba);
        UiQuad q;
        q.image = img.get();
        q.x0 = std::round(x), q.y0 = std::round(y);
        q.x1 = q.x0 + float(img->width), q.y1 = q.y0 + float(img->height);
        out.quads.push_back(q);
        out.textImages.push_back(std::move(img));
    };
    auto rect = [&](float x0, float y0, float x1, float y1, std::array<float, 4> c) {
        UiQuad q;
        q.x0 = x0, q.y0 = y0, q.x1 = x1, q.y1 = y1;
        q.color = c;
        out.quads.push_back(q);
    };
    auto target = [&](const UiElement* e, std::function<void(simd_float2)> f) {
        if (!e)
            return;
        if (auto r = rects.find(e); r != rects.end())
            targets_.push_back({r->second, std::move(f), uiButtonSound(*e)});
    };
    auto find = [](const UiElement& root, uint32_t id) { return findWindow(root, id); };

    // --- The frame.
    UiDrawHooks fh;
    fh.rects = &rects;
    const UiElement* controls = find(frame, kControls);
    const UiElement* chooser = find(frame, kSimChooser);
    if (controls)
        (editing_ >= 0 ? fh.show : fh.hide).insert(controls);
    if (chooser)
        fh.hide.insert(chooser);
    if (const UiElement* t = find(frame, kFirstNameText))
        fh.captions[t] = edit_.spec.firstName.empty() ? t->get("caption") : edit_.spec.firstName;
    if (const UiElement* gen = find(frame, kFamilyNameGen))
        if (const UiElement* t = gen->child(1); t && !familyName_.empty())
            fh.captions[t] = familyName_ + (focus_ == Focus::FamilyName ? "|" : "");
    if (controls)
        for (int k = 0; k < 6; ++k)
            if (const UiElement* b = find(*controls, kStepButton0 + uint32_t(k)); b && k == step_)
                fh.buttonState[b] = 2;
    layoutUiElement(frame, ox, oy, *res_, *fonts_, out, fh);
    if (const UiElement* rw = find(frame, kRotationWindow); rw)
        if (auto r = rects.find(rw); r != rects.end())
            rotationRect_ = r->second;
    for (uint32_t id : {kHousehold, kPuck})
        if (const UiElement* e = find(frame, id))
            if (auto r = rects.find(e); r != rects.end())
                solid_.push_back(r->second);
    if (controls) {
        for (int k = 0; k < 6; ++k)
            target(find(*controls, kStepButton0 + uint32_t(k)), [this, k](simd_float2) { setStep(k); });
        target(find(*controls, kDone), [this](simd_float2) { finishSim(); });
        target(find(*controls, kCancel), [this](simd_float2) {
            editing_ = -1;
            rebuild();
        });
        target(find(*controls, kUndo), [this](simd_float2) { undo(); });
        target(find(*controls, kRedo), [this](simd_float2) { redo(); });
        if (const UiElement* t = find(*controls, kFirstNameText))
            target(t, [this](simd_float2) { focus_ = Focus::FirstName; });
    }
    target(find(frame, kAccept), [this](simd_float2) { acceptFamily(); });
    target(find(frame, kTreeButton), [this](simd_float2) {
        if (editing_ >= 0)
            finishSim();
        binOpen_ = mabOpen_ = false;
        treeOpen_ = !treeOpen_;
        treeFirst_ = treeSecond_ = treeChoice_ = -1;
    });
    target(find(frame, kExit), [this](simd_float2) {
        if (onExit)
            onExit();
        else
            status_ = "Sair do CAS";
    });
    target(find(frame, kZoomIn), [this](simd_float2) {
        zoom_ = std::min(zoom_ + 1, 2);
        if (camera_)
            camera_->zoomBy(1);
        applyCamera(true);
    });
    target(find(frame, kZoomOut), [this](simd_float2) {
        zoom_ = std::max(zoom_ - 1, 0);
        if (camera_)
            camera_->zoomBy(-1);
        applyCamera(true);
    });
    target(find(frame, kTurnRight), [this](simd_float2) { heading_ -= 0.4f; });
    target(find(frame, kTurnLeft), [this](simd_float2) { heading_ += 0.4f; });
    if (const UiElement* gen = find(frame, kFamilyNameGen))
        target(gen, [this](simd_float2) { focus_ = Focus::FamilyName; });

    // The family view: the members over the household bar (the head
    // thumbnail when there is one, the name), and the ways to add one:
    // create a Sim, take one from the Sim bin, make a child (STR# 0x80).
    if (editing_ < 0) {
        float x = ox + 180;
        const float y = oy + 650;
        for (size_t i = 0; i < family_.size(); ++i) {
            const std::string n = family_[i].spec.firstName;
            const RgbaImage* t = family_[i].thumbnail && thumbs_ ? thumbs_->image(family_[i].thumbnail) : family_[i].portrait.get();
            if (t) {
                rect(x - 4, y - 62, x + 56, y - 2, kThumbBg);
                UiQuad q;
                q.image = t;
                q.x0 = x - 4, q.y0 = y - 62, q.x1 = x + 56, q.y1 = y - 2;
                out.quads.push_back(q);
            }
            rect(x - 4, y - 2, x + 90, y + 18, kCellColor);
            text(x, y, 86, n, kCellText);
            const int idx = int(i);
            targets_.push_back({{x - 4, y - 62, x + 90, y + 18}, [this, idx](simd_float2) {
                                    edit_ = family_[size_t(idx)];
                                    editing_ = idx;
                                    step_ = 0;
                                    binOpen_ = false;
                                    rebuild();
                                }});
            x += 98;
        }
        if (family_.size() < 8) {
            const std::pair<const char*, std::function<void()>> adds[] = {
                {"+ Criar um Sim", [this] { binOpen_ = false, newSim(); }},
                {"Estoque de Sims", [this] {
                     loadBin();
                     binOpen_ = !binOpen_;
                     binSel_ = -1;
                 }},
                {"Fazer um Filho", [this] {
                     binOpen_ = false;
                     mabOpen_ = true;
                     mabChild_.reset();
                     // The first two grown-ups as parents.
                     mabParent_[0] = mabParent_[1] = -1;
                     for (size_t i = 0; i < family_.size(); ++i) {
                         const uint32_t a = family_[i].spec.age;
                         if (a != cas::age::Adult && a != cas::age::Elder)
                             continue;
                         if (mabParent_[0] < 0)
                             mabParent_[0] = int(i);
                         else if (mabParent_[1] < 0)
                             mabParent_[1] = int(i);
                     }
                     if (mabParent_[1] < 0) {
                         mabOpen_ = false;
                         status_ = "Fazer um Filho precisa de dois adultos ou idosos na família";
                     }
                 }},
            };
            float bx = x, by = y - 48; // stacked where the next member would go
            for (const auto& [label, act] : adds) {
                rect(bx - 4, by - 2, bx + 116, by + 18, kCellSelected);
                text(bx, by, 112, label, kCellText);
                auto a = act;
                targets_.push_back({{bx - 4, by - 2, bx + 116, by + 18}, [a](simd_float2) { a(); }});
                by += 24;
            }
        }
        // The Sim bin, hanging from the frame's "sim bin anchor" (2d016e9f:tl).
        if (binOpen_)
            if (auto it = scripts_.find(kSimBin); it != scripts_.end()) {
                const UiElement& panel = it->second.roots.front();
                float ax = 0, ay = 0;
                if (const UiElement* pan = panel.child(kPanelAnchor)) {
                    const auto a = pan->area();
                    ax = float(a[0]), ay = float(a[1]);
                }
                float anchorX = ox + 421, anchorY = oy + 152;
                std::function<bool(const UiElement&, float, float)> locateBin = [&](const UiElement& e, float px, float py) {
                    const auto a = e.area();
                    const float ex = px + float(a[0]), ey = py + float(a[1]);
                    if (e.id() == 0x2D016E9F) {
                        anchorX = ex, anchorY = ey;
                        return true;
                    }
                    for (const auto& c : e.children)
                        if (locateBin(c, ex, ey))
                            return true;
                    return false;
                };
                locateBin(frame, ox, oy);
                const auto pa = panel.area();
                const float px = anchorX - ax - float(pa[0]), py = anchorY - ay - float(pa[1]);
                UiDrawHooks bh;
                bh.rects = &rects;
                for (int k = 0; k < 2; ++k)
                    if (const UiElement* b = find(panel, 0x4C6C0000 + uint32_t(k)); b && binGender_ == (k ? cas::gender::Male : cas::gender::Female))
                        bh.buttonState[b] = 2;
                const uint32_t ageBits[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
                for (int k = 0; k < 5; ++k)
                    if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)); b && binAge_ == ageBits[k])
                        bh.buttonState[b] = 2;
                for (uint32_t id : {0x4C6CA47Du, 0xCC6CA4E0u}) // the gender and age filters
                    if (const UiElement* e = find(panel, id))
                        bh.show.insert(e);
                if (const UiElement* g = find(panel, 0x0CFD6EC6))
                    bh.hide.insert(g); // custom collections
                layoutUiElement(panel, px, py, *res_, *fonts_, out, bh);
                if (auto r = rects.find(&panel); r != rects.end())
                    solid_.push_back(r->second);
                std::vector<int> shownSims;
                for (size_t i = 0; i < bin_.size(); ++i)
                    if ((!binAge_ || (bin_[i].age & binAge_)) && (!binGender_ || (bin_[i].gender & binGender_)))
                        shownSims.push_back(int(i));
                std::map<std::string, std::string> c;
                readConstants(panel, c);
                const int cols = constantInt(c, "kCASBinGridNumColumns", 4), rows = constantInt(c, "kCASBinGridNumRows", 2);
                if (const UiElement* grid = find(panel, 0x4011))
                    if (auto r = rects.find(grid); r != rects.end()) {
                        const auto gr = r->second;
                        const int perPage = cols * rows;
                        const int pages = std::max(1, (int(shownSims.size()) + perPage - 1) / perPage);
                        int& page = pages_["simbin"];
                        page = std::clamp(page, 0, pages - 1);
                        const float cw = (gr[2] - gr[0]) / float(cols), ch = (gr[3] - gr[1]) / float(rows);
                        for (int i = 0; i < perPage; ++i) {
                            const size_t k = size_t(page * perPage + i);
                            if (k >= shownSims.size())
                                break;
                            const int sim = shownSims[k];
                            const float x0 = gr[0] + cw * float(i % cols) + 4, y0 = gr[1] + ch * float(i / cols) + 4;
                            const float x1 = x0 + cw - 8, y1 = y0 + ch - 8;
                            if (sim == binSel_)
                                rect(x0 - 3, y0 - 3, x1 + 3, y1 + 3, kCellSelected);
                            rect(x0, y0, x1, y1, sim == binSel_ ? kThumbSelectedBg : kThumbBg);
                            if (const RgbaImage* t = thumbs_ ? thumbs_->image(bin_[size_t(sim)].thumbnail) : nullptr) {
                                const float side = std::min(x1 - x0, y1 - y0);
                                UiQuad q;
                                q.image = t;
                                q.x0 = (x0 + x1 - side) / 2, q.y0 = (y0 + y1 - side) / 2;
                                q.x1 = q.x0 + side, q.y1 = q.y0 + side;
                                out.quads.push_back(q);
                            }
                            targets_.push_back({{x0, y0, x1, y1}, [this, sim](simd_float2) { binSel_ = sim; }});
                        }
                        target(find(panel, 0x4008), [this](simd_float2) { pages_["simbin"] = std::max(0, pages_["simbin"] - 1); });
                        target(find(panel, 0x4009), [this, pages](simd_float2) {
                            pages_["simbin"] = std::min(pages - 1, pages_["simbin"] + 1);
                        });
                    }
                for (int k = 0; k < 2; ++k)
                    target(find(panel, 0x4C6C0000 + uint32_t(k)), [this, k](simd_float2) {
                        const uint32_t g = k ? cas::gender::Male : cas::gender::Female;
                        binGender_ = binGender_ == g ? 0 : g;
                        pages_["simbin"] = 0;
                    });
                for (int k = 0; k < 5; ++k) {
                    const uint32_t a = ageBits[k];
                    target(find(panel, 0xCC6C0200 + uint32_t(k)), [this, a](simd_float2) {
                        binAge_ = binAge_ == a ? 0 : a;
                        pages_["simbin"] = 0;
                    });
                }
                target(find(panel, 0x2C6CA64E), [this](simd_float2) { binOpen_ = false; }); // cancel
                target(find(panel, 0x4C6CA66D), [this](simd_float2) { // accept: edit the Sim chosen
                    if (binSel_ < 0 || size_t(binSel_) >= bin_.size())
                        return;
                    const BinSim b = bin_[size_t(binSel_)];
                    binOpen_ = false;
                    if (openCharacter(binFile_, b.group)) {
                        edit_.thumbnail = b.thumbnail;
                        edit_.spec.lastName = familyName_;
                    }
                });
            }
    }
    // The family tree (2C054EF0), centred: the members, picked one then
    // another, then the tie in the popup (2C054EF3).
    if (editing_ < 0 && treeOpen_)
        if (auto it = scripts_.find(kFamilyTree); it != scripts_.end()) {
            const UiElement& panel = it->second.roots.front();
            const auto pa = panel.area();
            const float px = ox + float(1024 - (pa[2] - pa[0])) / 2 - float(pa[0]);
            const float py = oy + float(768 - (pa[3] - pa[1])) / 2 - float(pa[1]);
            targets_.clear(); // modal
            UiDrawHooks th;
            th.rects = &rects;
            layoutUiElement(panel, px, py, *res_, *fonts_, out, th);
            static const char* kTieNames[5] = {"pai/mãe", "pai/mãe", "cônjuge", "irmão(ã)", "filho(a)"};
            if (const UiElement* area = find(panel, 0x2000))
                if (auto r = rects.find(area); r != rects.end()) {
                    const auto ar = r->second;
                    const float tw = std::min(72.0f, (ar[2] - ar[0]) / float(std::max<size_t>(family_.size(), 1)) - 8);
                    for (size_t i = 0; i < family_.size(); ++i) {
                        const float x0 = ar[0] + 8 + float(i) * (tw + 8), y0 = ar[1] + 8;
                        const bool sel = int(i) == treeFirst_ || int(i) == treeSecond_;
                        if (sel)
                            rect(x0 - 3, y0 - 3, x0 + tw + 3, y0 + tw + 3, kCellSelected);
                        rect(x0, y0, x0 + tw, y0 + tw, kThumbBg);
                        const Member& m = family_[i];
                        const RgbaImage* img = m.thumbnail && thumbs_ ? thumbs_->image(m.thumbnail) : m.portrait.get();
                        if (img) {
                            UiQuad q;
                            q.image = img;
                            q.x0 = x0, q.y0 = y0, q.x1 = x0 + tw, q.y1 = y0 + tw;
                            out.quads.push_back(q);
                        }
                        text(x0, y0 + tw + 4, tw + 6, m.spec.firstName, kCellText);
                        // Its ties, one line each.
                        float ly = y0 + tw + 20;
                        for (const auto& [otherId, tie] : m.ties)
                            for (const auto& o : family_)
                                if (o.id == otherId && tie < 5) {
                                    text(x0, ly, tw + 6, std::string(kTieNames[tie]) + ": " + o.spec.firstName, kCellText);
                                    ly += 14;
                                }
                        const int idx = int(i);
                        targets_.push_back({{x0, y0, x0 + tw, y0 + tw}, [this, idx](simd_float2) {
                                                if (treeFirst_ < 0 || treeFirst_ == idx) {
                                                    treeFirst_ = treeFirst_ == idx ? -1 : idx;
                                                    treeSecond_ = -1;
                                                } else {
                                                    treeSecond_ = idx;
                                                    treeChoice_ = -1;
                                                }
                                            }});
                    }
                }
            target(find(panel, 0xCC33DE0B), [this](simd_float2) { treeOpen_ = false; }); // accept
            target(find(panel, 0xCC33DE1F), [this](simd_float2) { treeOpen_ = false; }); // cancel
            // The popup: "Make <first> a ... of <second>".
            if (treeFirst_ >= 0 && treeSecond_ >= 0)
                if (auto dt = scripts_.find(kTieDialog); dt != scripts_.end()) {
                    targets_.clear();
                    const UiElement& dlg = dt->second.roots.front();
                    const auto da = dlg.area();
                    const float dx = ox + float(1024 - (da[2] - da[0])) / 2 - float(da[0]);
                    const float dy = oy + float(768 - (da[3] - da[1])) / 2 - float(da[1]) + 60;
                    UiDrawHooks dh;
                    dh.rects = &rects;
                    const std::string first = family_[size_t(treeFirst_)].spec.firstName;
                    const std::string second = family_[size_t(treeSecond_)].spec.firstName;
                    auto fill = [](std::string t, const std::string& name) {
                        if (auto p = t.find("%s"); p != std::string::npos)
                            t.replace(p, 2, name);
                        return t;
                    };
                    auto resText = [&](const UiElement* e, const std::string& fallback) {
                        if (e)
                            if (auto ref = uiStringRef(e->get("captionres")))
                                if (auto str = res_->string(*ref))
                                    return *str;
                        return fallback;
                    };
                    if (const UiElement* t = find(dlg, 0xECDDA449))
                        dh.captions[t] = fill(resText(t, "Make %s a"), first);
                    layoutUiElement(dlg, dx, dy, *res_, *fonts_, out, dh);
                    // The five ties share one place (0x2000-0x2004): the one chosen
                    // shows there, a click goes to the next.
                    if (treeChoice_ < 0)
                        treeChoice_ = 0;
                    if (const UiElement* b = find(dlg, 0x2000 + uint32_t(treeChoice_))) {
                        const auto ba = b->area();
                        const float lx = dx + float(ba[0]), ly = dy + float(ba[1]);
                        const float lw = float(ba[2] - ba[0]), lh = float(ba[3] - ba[1]);
                        rect(lx, ly, lx + lw, ly + lh, kCellSelected);
                        text(lx + 18, ly + 1, lw - 36, fill(resText(b, b->get("caption")), second), kCellText);
                        text(lx + 4, ly + 1, 12, "<", kCellText);
                        text(lx + lw - 12, ly + 1, 12, ">", kCellText);
                        targets_.push_back({{lx, ly, lx + lw / 2, ly + lh}, [this](simd_float2) { treeChoice_ = (treeChoice_ + 4) % 5; }});
                        targets_.push_back({{lx + lw / 2, ly, lx + lw, ly + lh}, [this](simd_float2) { treeChoice_ = (treeChoice_ + 1) % 5; }});
                    }
                    target(find(dlg, 0x1000), [this](simd_float2) { // accept
                        if (treeChoice_ >= 0)
                            setTie(treeFirst_, treeSecond_, treeChoice_);
                        treeFirst_ = treeSecond_ = treeChoice_ = -1;
                    });
                    target(find(dlg, 0x1001), [this](simd_float2) { treeFirst_ = treeSecond_ = treeChoice_ = -1; });
                }
        }
    // Make a Child (CB980E87): the whole screen, from the "MAB anchor"
    // (0d016e8a:bl); the parents' and the child's views show head shots.
    if (editing_ < 0 && mabOpen_)
        if (auto it = scripts_.find(kMakeBaby); it != scripts_.end()) {
            const UiElement& panel = it->second.roots.front();
            float ax = 0, ay = 768;
            if (const UiElement* pan = panel.child(kPanelAnchor)) {
                const auto a = pan->area();
                ax = float(a[0]), ay = float(a[1]);
            }
            const auto pa = panel.area();
            const float px = ox + 0 - ax - float(pa[0]), py = oy + 768 - ay - float(pa[1]);
            UiDrawHooks mh;
            mh.rects = &rects;
            const uint32_t ageBits[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
            for (int k = 0; k < 5; ++k)
                if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)); b && mabAge_ == ageBits[k])
                    mh.buttonState[b] = 2;
            for (int k = 0; k < 2; ++k)
                if (const UiElement* b = find(panel, 0x4C6C0000 + uint32_t(k));
                    b && mabGender_ == (k ? cas::gender::Male : cas::gender::Female))
                    mh.buttonState[b] = 2;
            // The adult and elder age buttons do nothing for a child: hidden.
            for (int k = 3; k < 5; ++k)
                if (const UiElement* b = find(panel, 0xCC6C0200 + uint32_t(k)))
                    mh.hide.insert(b);
            out.quads.clear(); // the panel covers the frame
            out.textImages.clear();
            targets_.clear();
            layoutUiElement(panel, px, py, *res_, *fonts_, out, mh);
            auto image = [&](const UiElement* w, const RgbaImage* img) {
                if (!w || !img)
                    return;
                auto r = rects.find(w);
                if (r == rects.end())
                    return;
                const auto rr = r->second;
                const float side = std::min(rr[2] - rr[0], rr[3] - rr[1]);
                UiQuad q;
                q.image = img;
                q.x0 = (rr[0] + rr[2] - side) / 2, q.y0 = (rr[1] + rr[3] - side) / 2;
                q.x1 = q.x0 + side, q.y1 = q.y0 + side;
                out.quads.push_back(q);
            };
            auto shot = [&](int idx) -> const RgbaImage* {
                if (idx < 0 || size_t(idx) >= family_.size())
                    return nullptr;
                const Member& m = family_[size_t(idx)];
                if (m.thumbnail && thumbs_)
                    if (const RgbaImage* t = thumbs_->image(m.thumbnail))
                        return t;
                return m.portrait.get();
            };
            image(find(panel, 0xCCB911B8), shot(mabParent_[0]));
            image(find(panel, 0xCCB911A2), shot(mabParent_[1]));
            if (mabChild_)
                image(find(panel, 0xACB91188), mabChild_->portrait.get());
            // The parent grids: the family's adults and elders.
            std::vector<int> grown;
            for (size_t i = 0; i < family_.size(); ++i)
                if (family_[i].spec.age == cas::age::Adult || family_[i].spec.age == cas::age::Elder)
                    grown.push_back(int(i));
            for (int side = 0; side < 2; ++side)
                if (const UiElement* grid = find(panel, side ? 0x4CD98491 : 0x4CD98490))
                    if (auto r = rects.find(grid); r != rects.end()) {
                        const auto gr = r->second;
                        const float cw = (gr[2] - gr[0]) / 2, ch = (gr[3] - gr[1]) / 3;
                        for (size_t k = 0; k < grown.size() && k < 6; ++k) {
                            const int idx = grown[k];
                            const float x0 = gr[0] + cw * float(k % 2) + 2, y0 = gr[1] + ch * float(k / 2) + 2;
                            const float x1 = x0 + cw - 4, y1 = y0 + ch - 4;
                            const bool sel = mabParent_[side] == idx;
                            if (sel)
                                rect(x0 - 2, y0 - 2, x1 + 2, y1 + 2, kCellSelected);
                            rect(x0, y0, x1, y1, kThumbBg);
                            if (const RgbaImage* t = shot(idx)) {
                                UiQuad q;
                                q.image = t;
                                const float sd = std::min(x1 - x0, y1 - y0);
                                q.x0 = (x0 + x1 - sd) / 2, q.y0 = (y0 + y1 - sd) / 2, q.x1 = q.x0 + sd, q.y1 = q.y0 + sd;
                                out.quads.push_back(q);
                            } else {
                                text(x0 + 2, y0 + 2, x1 - x0 - 4, family_[size_t(idx)].spec.firstName, kCellText);
                            }
                            targets_.push_back({{x0, y0, x1, y1}, [this, side, idx](simd_float2) {
                                                    if (mabParent_[1 - side] == idx)
                                                        std::swap(mabParent_[0], mabParent_[1]);
                                                    else
                                                        mabParent_[side] = idx;
                                                    mabChild_.reset();
                                                }});
                        }
                    }
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = ageBits[k];
                target(find(panel, 0xCC6C0200 + uint32_t(k)), [this, a](simd_float2) { mabAge_ = a; });
            }
            for (int k = 0; k < 2; ++k)
                target(find(panel, 0x4C6C0000 + uint32_t(k)), [this, k](simd_float2) {
                    const uint32_t g = k ? cas::gender::Male : cas::gender::Female;
                    mabGender_ = mabGender_ == g ? 0 : g;
                });
            target(find(panel, 0x4C76BB8F), [this](simd_float2) { makeChild(); });
            target(find(panel, 0x2C6CA64E), [this](simd_float2) {
                mabOpen_ = false;
                mabChild_.reset();
            });
            target(find(panel, 0x4C6CA66D), [this](simd_float2) {
                if (!mabChild_ || family_.size() >= 8)
                    return;
                Member c = std::move(*mabChild_);
                mabChild_.reset();
                mabOpen_ = false;
                c.id = nextId_++;
                if (c.spec.firstName.empty())
                    c.spec.firstName = c.spec.gender == cas::gender::Female ? "Filha" : "Filho";
                c.aspiration = cas::aspiration::GrowUp;
                if (!c.portrait)
                    c.portrait = portrait(c);
                status_ = c.spec.firstName + " entrou na família";
                for (int pid : c.parents)
                    for (auto& m : family_)
                        if (m.id == pid)
                            m.ties[c.id] = 4; // my child is
                family_.push_back(std::move(c));
                rebuild();
            });
            if (!status_.empty()) {
                rect(ox + 200, oy + 8, ox + 820, oy + 30, {0.1f, 0.1f, 0.3f, 0.75f});
                text(ox + 206, oy + 11, 600, status_, {1, 1, 1, 1});
            }
            return out;
        }
    if (!status_.empty()) {
        rect(ox + 200, oy + 8, ox + 820, oy + 30, {0.1f, 0.1f, 0.3f, 0.75f});
        text(ox + 206, oy + 11, 600, status_, {1, 1, 1, 1});
    }
    if (editing_ < 0)
        return out;

    // --- The step panel, hanging from the sub panel anchor.
    auto scriptIt = scripts_.find(kStepScripts[step_]);
    const UiElement* anchor = find(frame, kSubPanelAnchor);
    if (scriptIt == scripts_.end() || !anchor)
        return out;
    const UiElement& panel = scriptIt->second.roots.front();
    const auto pa = panel.area();
    float ax = 0, ay = float(pa[3] - pa[1]); // bottom left by default
    if (const UiElement* pan = panel.child(kPanelAnchor)) {
        const auto a = pan->area();
        ax = float(a[0]), ay = float(a[1]);
    }
    // The anchor is hidden (not laid out): its place from the areas on the way down.
    std::function<bool(const UiElement&, float, float, float&, float&)> locate =
        [&](const UiElement& e, float px, float py, float& outX, float& outY) {
            const auto a = e.area();
            const float x = px + float(a[0]), y = py + float(a[1]);
            if (e.id() == kSubPanelAnchor) {
                outX = x, outY = y;
                return true;
            }
            for (const auto& c : e.children)
                if (locate(c, x, y, outX, outY))
                    return true;
            return false;
        };
    float anchorX = 0, anchorY = 0;
    locate(frame, ox, oy, anchorX, anchorY);
    const float panelX = anchorX - ax, panelY = anchorY - ay;
    UiDrawHooks hk;
    hk.rects = &rects;
    cas::CharacterSpec& s = edit_.spec;
    auto pressed = [&](const UiElement* e, bool on) {
        if (e && on)
            hk.buttonState[e] = 2;
    };
    // Grids drawn after layout: (grid window, items).
    struct Item {
        std::string text;
        bool selected = false;
        const RgbaImage* thumb = nullptr; // from CASThumbnails.package
        std::function<void()> pick;
        float slider = NAN; // modifiers: the weight
        std::function<void(float)> slide;
    };
    struct Grid {
        const UiElement* window;
        std::string key;
        int cols, rows;
        std::vector<Item> items;
        const UiElement* prev = nullptr;
        const UiElement* next = nullptr;
    };
    std::vector<Grid> grids;
    auto addGrid = [&](const UiElement* component, uint32_t gridId, const std::string& key, int cols, int rows,
                       std::vector<Item> items, uint32_t prevId = 0x4008, uint32_t nextId = 0x4009) {
        if (!component)
            return;
        const UiElement* g = find(*component, gridId);
        if (!g)
            return;
        grids.push_back({g, key, cols, rows, std::move(items), find(*component, prevId), find(*component, nextId)});
    };
    auto thumb = [&](uint32_t id) { return thumbs_ ? thumbs_->image(id) : nullptr; };
    // A texture overlay: the cached thumbnail, else its texture over the face skin.
    auto overlayThumb = [&](const SimPart& o) -> const RgbaImage* {
        if (const RgbaImage* t = thumb(cas::entryThumbnailId(o, s.age, s.gender)))
            return t;
        const std::string face = cas::faceSkinTexture(index_, cas::agePrefix(s.age, s.gender, true), s.skinTone);
        const std::string tex = cas::overlayTexture(index_, o);
        const std::string key = face + "|" + tex;
        auto it = previews_.find(key);
        if (it == previews_.end()) {
            auto img = cas::overlayPreview(index_, face, tex, o.number("subtype"));
            it = previews_.emplace(key, img ? std::make_unique<RgbaImage>(std::move(*img)) : nullptr).first;
        }
        return it->second.get();
    };
    auto overlayItems = [&](uint32_t subtype, int bin, bool removable) {
        std::vector<Item> items;
        const std::string hair = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
        std::vector<std::string> seen;
        int current = -1;
        for (size_t i = 0; i < s.overlays.size(); ++i)
            if (const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, s.overlays[i]))
                if (o->number("subtype") == subtype && (bin < 0 || int(o->number("bin")) == bin))
                    current = int(i);
        if (removable)
            items.push_back({"(nenhum)", current < 0, nullptr, [this, subtype, bin]() {
                                 pushUndo();
                                 auto& ov = edit_.spec.overlays;
                                 ov.erase(std::remove_if(ov.begin(), ov.end(),
                                                         [&](const std::string& f) {
                                                             const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, f);
                                                             return o && o->number("subtype") == subtype &&
                                                                    (bin < 0 || int(o->number("bin")) == bin);
                                                         }),
                                          ov.end());
                                 changed();
                             }});
        for (const SimPart* o : catalog_->overlaysFor(subtype, s.age, s.gender)) {
            if (bin >= 0 && int(o->number("bin")) != bin)
                continue;
            const std::string fam = cas::family(*o);
            if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                continue;
            // Brows and facial hair: one entry per style (the colour follows the hair).
            if (subtype == cas::overlay::Brows || subtype == cas::overlay::FacialHair) {
                if (lower(o->property("hairtone")) != hair)
                    continue;
            }
            seen.push_back(fam);
            const bool sel = current >= 0 && s.overlays[size_t(current)] == fam;
            items.push_back({label(o->name()), sel, overlayThumb(*o), [this, fam, subtype, bin]() {
                                 pushUndo();
                                 auto& ov = edit_.spec.overlays;
                                 ov.erase(std::remove_if(ov.begin(), ov.end(),
                                                         [&](const std::string& f) {
                                                             const SimPart* x = catalog_->byFamily(catalog_->textureOverlays, f);
                                                             return x && x->number("subtype") == subtype &&
                                                                    (bin < 0 || int(x->number("bin")) == bin);
                                                         }),
                                          ov.end());
                                 ov.push_back(fam);
                                 changed();
                             }});
        }
        return items;
    };
    // Mesh overlays (glasses: subtype 5 bin 15; beards with a mesh: subtype 0
    // bin 2), one per family (a beard in the hair's tone), without the
    // CAS-hidden ones (flag 0x01).
    auto meshOverlayItems = [&](uint32_t subtype, uint32_t bin, bool removable) {
        std::vector<Item> items;
        const std::string hair = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
        auto same = [this, subtype, bin](const std::string& f) {
            const SimPart* o = catalog_->byFamily(catalog_->meshOverlays, f);
            return o && o->number("subtype") == subtype && o->number("bin") == bin;
        };
        bool any = false;
        for (const auto& f : s.meshOverlays)
            any |= same(f);
        if (removable)
            items.push_back({"(nenhum)", !any, nullptr, [this, same]() {
                                 pushUndo();
                                 auto& mo = edit_.spec.meshOverlays;
                                 mo.erase(std::remove_if(mo.begin(), mo.end(), same), mo.end());
                                 changed();
                             }});
        std::vector<std::string> seen;
        for (const SimPart* o : catalog_->meshOverlaysFor(s.age, s.gender)) {
            if (o->number("subtype") != subtype || o->number("bin") != bin || (o->number("flags") & 1))
                continue;
            const std::string fam = cas::family(*o);
            const std::string ht = lower(o->property("hairtone"));
            if (!ht.empty() && ht != "00000000-0000-0000-0000-000000000000" && ht != hair)
                continue;
            if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                continue;
            seen.push_back(fam);
            const bool sel = std::find(s.meshOverlays.begin(), s.meshOverlays.end(), fam) != s.meshOverlays.end();
            items.push_back({label(o->name()), sel, thumb(cas::entryThumbnailId(*o, s.age, s.gender)), [this, fam, same]() {
                                 pushUndo();
                                 auto& mo = edit_.spec.meshOverlays;
                                 mo.erase(std::remove_if(mo.begin(), mo.end(), same), mo.end());
                                 mo.push_back(fam);
                                 changed();
                             }});
        }
        return items;
    };
    auto archetypeItems = [&](const std::string& maskList, const std::string& region) {
        std::vector<Item> items;
        for (const SimPart* a : catalog_->archetypesFor(s.age, s.gender)) {
            const std::string fam = cas::family(*a);
            bool sel;
            if (maskList.empty())
                sel = s.face.archetype == fam;
            else {
                auto it = s.face.regions.find(maskList);
                sel = it != s.face.regions.end() && it->second == fam;
            }
            const RgbaImage* shot = thumb(cas::archetypeThumbnailId(s.age, s.gender, fam, region));
            if (!shot)
                if (auto it = archetypeShots_.find(cas::casFileName(s.age, s.gender) + "|" + fam + "|" + std::to_string(s.skinTone));
                    it != archetypeShots_.end())
                    shot = it->second.get();
            items.push_back({label(a->name().substr(0, a->name().find('_'))), sel, shot, [this, fam, maskList]() {
                                 pushUndo();
                                 if (maskList.empty()) {
                                     edit_.spec.face.archetype = fam;
                                     edit_.spec.face.structure.reset(); // a saved face gives way to the head
                                     edit_.spec.face.regions.clear();
                                 } else {
                                     edit_.spec.face.regions[maskList] = fam;
                                 }
                                 changed();
                             }});
        }
        return items;
    };
    auto modifierItems = [&](int group) {
        std::vector<Item> items;
        for (const SimPart* m : catalog_->modifiersFor(s.age, s.gender)) {
            if (int(m->number("groupindex")) != group)
                continue;
            const std::string fam = cas::family(*m);
            auto it = s.face.modifiers.find(fam);
            Item item;
            item.text = m->name();
            item.slider = it == s.face.modifiers.end() ? 0.0f : it->second;
            const float lo = cas::floatProperty(*m, "negativelimit", -1.5f), hi = cas::floatProperty(*m, "positivelimit", 1.5f);
            item.slide = [this, fam, lo, hi](float t) {
                pushUndo();
                const float w = lo + (hi - lo) * std::clamp(t, 0.0f, 1.0f);
                edit_.spec.face.modifiers[fam] = std::fabs(w) < 0.05f ? 0.0f : w;
                changed();
            };
            item.slider = (item.slider - lo) / (hi - lo);
            items.push_back(std::move(item));
        }
        return items;
    };

    switch (step_) {
    case 0: { // name, gender, age, skin, body
        if (const UiElement* gen = find(panel, 0x8C6B1EB9))
            if (const UiElement* t = gen->child(1))
                hk.captions[t] = s.firstName.empty() ? t->get("caption") : s.firstName + (focus_ == Focus::FirstName ? "|" : "");
        if (const UiElement* bio = find(panel, 0x4CD0FDD9)) {
            (bioOpen_ ? hk.show : hk.hide).insert(bio);
            if (const UiElement* t = bio->child(1); t && (!s.description.empty() || focus_ == Focus::Biography))
                hk.captions[t] = s.description + (focus_ == Focus::Biography ? "|" : "");
        }
        pressed(find(panel, 0x8CB7C11F), bioOpen_);
        pressed(find(panel, 0x401), s.gender == cas::gender::Female);
        pressed(find(panel, 0x400), s.gender == cas::gender::Male);
        const uint32_t ages[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
        for (int k = 0; k < 5; ++k)
            pressed(find(panel, 0x200 + uint32_t(k)), s.age == ages[k]);
        for (int k = 0; k < 4; ++k)
            pressed(find(panel, 0x2CB70000 + uint32_t(k)), s.skinTone == k + 1);
        pressed(find(panel, 0x2C720000), s.fitness != 2);
        pressed(find(panel, 0x2C720001), s.fitness == 2);
        break;
    }
    case 1: // the head bin
        if (const UiElement* g = find(panel, 0x0CFD6EC6))
            hk.hide.insert(g);
        addGrid(&panel, 0x4011, "heads", 3, 3, archetypeItems("", ""));
        break;
    case 2: { // hair and face
        for (int k = 0; k < 7; ++k) {
            if (const UiElement* p = find(panel, kFacePanels[k]))
                (k == faceTab_ ? hk.show : hk.hide).insert(p);
            pressed(find(panel, kFaceTabs[k]), k == faceTab_);
        }
        const UiElement* fp = find(panel, kFacePanels[faceTab_]);
        if (!fp)
            break;
        if (faceTab_ == 0) { // hair
            const UiElement* hc = find(*fp, 0x2CED6A8D);
            std::vector<Item> items;
            const std::string tone = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
            const auto& everyday = s.outfits[cas::category::Everyday];
            const std::string current = everyday.count(cas::part::Hair) ? lower(everyday.at(cas::part::Hair)) : "";
            for (const SimPart* h : catalog_->hairStyles(s.age, s.gender, tone)) {
                const std::string name = h->name();
                items.push_back({label(name), lower(name) == current, thumb(cas::entryThumbnailId(*h, s.age, s.gender)), [this, name]() {
                                     pushUndo();
                                     for (auto& [cat, parts] : edit_.spec.outfits)
                                         if (parts.count(cas::part::Hair) || cat == cas::category::Everyday)
                                             parts[cas::part::Hair] = name;
                                     changed();
                                 }});
            }
            if (hc) {
                std::map<std::string, std::string> c;
                readConstants(*hc, c);
                addGrid(hc, 0x8CB8C2B9, "hair", constantInt(c, "kHairGridNumColumns", 3), constantInt(c, "kHairGridNumRows", 3),
                        std::move(items), 0x8CB8C2C3, 0xACB8C2CF);
                for (int k = 0; k < 4; ++k)
                    pressed(find(*hc, 0x8800 + uint32_t(k)), s.hairTone == k + 1);
            }
        } else {
            // Archetype (region) bin and modifiers, the eyes' colours.
            std::vector<const UiElement*> archComps, modComps;
            std::function<void(const UiElement&)> walk = [&](const UiElement& e) {
                if (e.id() == 0xACB8A9AE)
                    archComps.push_back(&e);
                if (e.id() == 0x4CA50588)
                    modComps.push_back(&e);
                for (const auto& c : e.children)
                    walk(c);
            };
            walk(*fp);
            for (const UiElement* ac : archComps) {
                std::map<std::string, std::string> c;
                readConstants(*ac, c);
                const std::string mask = c.count("kCAFArchetypeRegionMask") ? c["kCAFArchetypeRegionMask"] : "";
                addGrid(ac, 0x4011, "arch" + std::to_string(faceTab_), constantInt(c, "kCAFArchetypesGridNumColumns", 3),
                        constantInt(c, "kCAFArchetypesGridNumRows", 2), archetypeItems(mask, kRegionThumbs[faceTab_]));
            }
            for (const UiElement* mc : modComps) {
                std::map<std::string, std::string> c;
                readConstants(*mc, c);
                addGrid(mc, 0x4011, "mod" + std::to_string(faceTab_), constantInt(c, "kCAFModifiersGridNumColumns", 2),
                        constantInt(c, "kCAFModifiersGridNumRows", 3),
                        modifierItems(constantInt(c, "kCAFModifiersValidGroup", 0)));
            }
            if (faceTab_ == 3)
                if (const UiElement* ec = find(*fp, 0xACB8A9AF)) {
                    std::vector<Item> items;
                    for (const SimPart* o : catalog_->overlaysFor(cas::overlay::Eyes, s.age, s.gender)) {
                        const std::string fam = cas::family(*o);
                        items.push_back({label(o->name()).substr(4), s.eyes == fam, overlayThumb(*o), [this, fam]() {
                                             pushUndo();
                                             edit_.spec.eyes = fam;
                                             changed();
                                         }});
                    }
                    addGrid(ec, 0x4011, "eyes", 5, 1, std::move(items));
                }
        }
        break;
    }
    case 3: { // makeup, costume, glasses, brows, stubble, facial hair
        for (int k = 0; k < 6; ++k)
            pressed(find(panel, kCosmeticTabs[k]), k == cosmeticTab_);
        const UiElement* makeup = find(panel, 0x6C6E1002);
        const UiElement* overlays = find(panel, 0x6CA52066);
        if (makeup)
            (cosmeticTab_ == 0 ? hk.show : hk.hide).insert(makeup);
        if (overlays)
            (cosmeticTab_ != 0 ? hk.show : hk.hide).insert(overlays);
        if (cosmeticTab_ == 0 && makeup) {
            const std::pair<uint32_t, std::pair<uint32_t, int>> comps[] = {
                {0x6CA52068, {cas::overlay::Blush, -1}}, {0xAD474C78, {cas::overlay::EyeShadow, 6}},
                {0x6CA52069, {cas::overlay::EyeShadow, 7}}, {0x6CA52067, {cas::overlay::Lipstick, -1}}};
            for (const auto& [id, kind] : comps)
                addGrid(find(*makeup, id), 0x4011, "makeup" + std::to_string(id), 5, 1, overlayItems(kind.first, kind.second, true));
        } else if (overlays) {
            for (int k = 0; k < 6; ++k)
                if (kCosmeticTitles[k])
                    if (const UiElement* t = find(*overlays, kCosmeticTitles[k]))
                        (k == cosmeticTab_ ? hk.show : hk.hide).insert(t);
            std::vector<Item> items;
            switch (cosmeticTab_) {
            case 1: items = overlayItems(cas::overlay::Costume, -1, true); break;
            case 2: items = meshOverlayItems(5, 15, true); break; // glasses
            case 3: items = overlayItems(cas::overlay::Brows, -1, false); break;
            case 4: items = overlayItems(cas::overlay::FacialHair, 1, true); break;
            case 5: { // facial hair: the textures, then the beards with a mesh
                items = overlayItems(cas::overlay::FacialHair, 2, true);
                auto mesh = meshOverlayItems(0, 2, true);
                if (!items.empty() && !mesh.empty()) {
                    // One "(none)" for both kinds.
                    items[0].selected = items[0].selected && mesh[0].selected;
                    auto a = items[0].pick, b = mesh[0].pick;
                    items[0].pick = [a, b]() {
                        a();
                        b();
                    };
                    mesh.erase(mesh.begin());
                }
                // A texture beard and a mesh one exclude each other.
                for (size_t i = 1; i < items.size(); ++i) {
                    auto pick = items[i].pick;
                    items[i].pick = [this, pick]() {
                        auto& mo = edit_.spec.meshOverlays;
                        mo.erase(std::remove_if(mo.begin(), mo.end(), [this](const std::string& f) {
                                     const SimPart* o = catalog_->byFamily(catalog_->meshOverlays, f);
                                     return o && o->number("subtype") == 0;
                                 }),
                                 mo.end());
                        pick();
                    };
                }
                for (auto& m : mesh) {
                    auto pick = m.pick;
                    m.pick = [this, pick]() {
                        auto& ov = edit_.spec.overlays;
                        ov.erase(std::remove_if(ov.begin(), ov.end(), [this](const std::string& f) {
                                     const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, f);
                                     return o && o->number("subtype") == cas::overlay::FacialHair && o->number("bin") == 2;
                                 }),
                                 ov.end());
                        pick();
                    };
                }
                items.insert(items.end(), std::make_move_iterator(mesh.begin()), std::make_move_iterator(mesh.end()));
                break;
            }
            default: break;
            }
            addGrid(overlays, 0x4011, "cosmetic" + std::to_string(cosmeticTab_), 3, 3, std::move(items));
        }
        break;
    }
    case 4: { // clothes
        for (int k = 0; k < 6; ++k) {
            pressed(find(panel, 0x02000000 + kClothesTabK[k]), k == clothesCategory_);
            if (const UiElement* t = find(panel, 0x0DE60000 + kClothesTabK[k]))
                (k == clothesCategory_ ? hk.show : hk.hide).insert(t);
        }
        for (int k = 0; k < 3; ++k)
            pressed(find(panel, 0x01000000 + uint32_t(k)), k == clothesPart_);
        if (const UiElement* g = find(panel, 0x0CFD6EC6))
            hk.hide.insert(g);
        const uint32_t cat = kClothesCategory[clothesCategory_];
        const uint32_t partBit = kClothesParts[clothesPart_];
        std::vector<Item> items;
        const auto& current = s.outfits[cat];
        for (const SimPart* p : catalog_->outfitParts(partBit, s.age, s.gender, cat, s.fitness == 2)) {
            const std::string name = p->name();
            const bool sel = current.count(partBit) && lower(current.at(partBit)) == lower(name);
            items.push_back({label(name), sel, thumb(cas::entryThumbnailId(*p, s.age, s.gender)), [this, name, cat, partBit]() {
                                 pushUndo();
                                 auto& o = edit_.spec.outfits[cat];
                                 if (partBit == cas::part::Body) {
                                     o.erase(cas::part::Top);
                                     o.erase(cas::part::Bottom);
                                 } else {
                                     o.erase(cas::part::Body);
                                     // A top needs a bottom and the other way round.
                                     const uint32_t other = partBit == cas::part::Top ? cas::part::Bottom : cas::part::Top;
                                     if (!o.count(other)) {
                                         const auto list = catalog_->outfitParts(other, edit_.spec.age, edit_.spec.gender, cat);
                                         if (!list.empty())
                                             o[other] = list.front()->name();
                                     }
                                 }
                                 o[partBit] = name;
                                 changed();
                             }});
        }
        if (const UiElement* holder = find(panel, 0x0BD73DA0); holder) {
            Grid g{holder, "clothes" + std::to_string(cat) + "_" + std::to_string(partBit), 3, 3, std::move(items),
                   find(panel, 0x121), find(panel, 0x120)};
            grids.push_back(std::move(g));
        }
        break;
    }
    case 5: { // personality
        const bool young = s.age == cas::age::Toddler || s.age == cas::age::Child || s.age == cas::age::Baby;
        if (const UiElement* e = find(panel, 0x4DA3D18B))
            (young ? hk.show : hk.hide).insert(e);
        if (const UiElement* e = find(panel, 0x2DA3D170))
            (young ? hk.hide : hk.show).insert(e);
        for (int k = 1; k <= 7; ++k)
            pressed(find(panel, 0x0CDC0000 + uint32_t(k)), edit_.aspiration == (1u << (k - 1)));
        const int sign = edit_.zodiacChosen ? edit_.zodiac : cas::zodiacSign(edit_.personality);
        for (int k = 1; k <= 12; ++k) {
            if (const UiElement* bg = find(panel, 0x4CDC0000 + uint32_t(k)))
                (k == sign ? hk.show : hk.hide).insert(bg);
            pressed(find(panel, 0xACDC0000 + uint32_t(k)), k == sign);
        }
        if (const UiElement* t = find(panel, 0x2BE35D53)) {
            auto name = res_->string({packageGroupHash("CAS"), 0x8A, uint32_t(sign - 1)});
            hk.captions[t] = name.value_or(cas::zodiacName(sign));
        }
        for (int k = 0; k < 5; ++k)
            if (const UiElement* bar = find(panel, kTraitBars[k]))
                if (const UiElement* full = bar->child(3))
                    if (auto ref = uiResourceRef(full->get("image")))
                        if (const RgbaImage* img = res_->image(*ref)) {
                            keep_.push_back(std::make_unique<RgbaImage>(segmentFill(*img, float(edit_.personality[k] * 100), 1000, 10)));
                            hk.images[full] = keep_.back().get();
                        }
        if (const UiElement* total = find(panel, 0x4BDEFF05))
            if (const UiElement* full = total->child(3))
                if (auto ref = uiResourceRef(full->get("image")))
                    if (const RgbaImage* img = res_->image(*ref)) {
                        // Points left, from the bottom up.
                        const int left = cas::kPersonalityPoints - edit_.personality.total();
                        RgbaImage cut = *img;
                        const uint32_t keepRows = uint32_t(std::lround(float(img->height) * float(left) / 25.0f));
                        for (uint32_t y = 0; y + keepRows < img->height; ++y)
                            for (uint32_t x = 0; x < img->width; ++x)
                                cut.pixels[(size_t(y) * img->width + x) * 4 + 3] = 0;
                        keep_.push_back(std::make_unique<RgbaImage>(std::move(cut)));
                        hk.images[full] = keep_.back().get();
                    }
        break;
    }
    default: break;
    }
    const size_t panelQuads = out.quads.size();
    layoutUiElement(panel, panelX - float(pa[0]), panelY - float(pa[1]), *res_, *fonts_, out, hk);
    if (step_ == 3 && cosmeticTab_ != 0)
        if (const UiElement* oc = find(panel, 0x6CA52066))
            if (auto r = rects.find(oc); r != rects.end())
                if (const RgbaImage* bg = res_->image(UiResourceRef{kTypeUiImage, 0x499DB772, 0xCA2B0160})) {
                    // The overlays component draws no background of its own in the
                    // script (its code does): the makeup panel's, behind it.
                    UiQuad q;
                    q.image = bg;
                    q.x0 = r->second[0] - 56, q.x1 = q.x0 + 436;
                    q.y0 = r->second[1], q.y1 = r->second[3] + 9;
                    out.quads.insert(out.quads.begin() + std::ptrdiff_t(panelQuads), q);
                }
    if (auto r = rects.find(&panel); r != rects.end())
        solid_.push_back(r->second);

    // Targets of the panel's controls.
    switch (step_) {
    case 0: {
        target(find(panel, 0x401), [this](simd_float2) { setGender(cas::gender::Female, true); });
        target(find(panel, 0x400), [this](simd_float2) { setGender(cas::gender::Male, true); });
        const uint32_t ages[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
        for (int k = 0; k < 5; ++k) {
            const uint32_t a = ages[k];
            target(find(panel, 0x200 + uint32_t(k)), [this, a](simd_float2) {
                if (edit_.spec.age == a)
                    return;
                pushUndo();
                edit_.spec.age = a;
                edit_.spec.outfits.clear();
                // Brows and the like stay; the archetype family carries the face across ages.
                if (!baker_->archetypeFor(edit_.spec.face.archetype, a, edit_.spec.gender))
                    for (const SimPart* ar : catalog_->archetypesFor(a, edit_.spec.gender))
                        if (lower(ar->name()).find("archbase") != std::string::npos)
                            edit_.spec.face.archetype = cas::family(*ar);
                cas::completeSpec(*catalog_, edit_.spec);
                changed();
            });
        }
        for (int k = 0; k < 4; ++k)
            target(find(panel, 0x2CB70000 + uint32_t(k)), [this, k](simd_float2) {
                pushUndo();
                edit_.spec.skinTone = k + 1;
                edit_.spec.outfits.erase(cas::category::Naked);
                cas::completeSpec(*catalog_, edit_.spec);
                changed();
            });
        target(find(panel, 0x2C720000), [this](simd_float2) {
            pushUndo();
            edit_.spec.fitness = 0;
            changed();
        });
        target(find(panel, 0x2C720001), [this](simd_float2) {
            pushUndo();
            edit_.spec.fitness = 2;
            // The fat versions of the outfits.
            for (auto& [cat, parts] : edit_.spec.outfits)
                for (auto& [bit, name] : parts) {
                    if (bit == cas::part::Hair)
                        continue;
                    const auto fat = catalog_->outfitParts(bit, edit_.spec.age, edit_.spec.gender, cat, true, false);
                    for (const SimPart* p : fat)
                        if (lower(p->name()).starts_with(lower(name.substr(0, name.find('_')))))
                            name = p->name();
                }
            changed();
        });
        target(find(panel, 0xAC3405DF), [this](simd_float2) { randomize(0); });
        target(find(panel, 0x8CB7C11F), [this](simd_float2) {
            bioOpen_ = !bioOpen_;
            focus_ = bioOpen_ ? Focus::Biography : Focus::None;
            dirty_ = true;
        });
        if (bioOpen_)
            if (const UiElement* bio = find(panel, 0x4CD0FDD9))
                if (const UiElement* t = bio->child(1))
                    target(t, [this](simd_float2) { focus_ = Focus::Biography; });
        if (const UiElement* gen = find(panel, 0x8C6B1EB9))
            target(gen, [this](simd_float2) { focus_ = Focus::FirstName; });
        break;
    }
    case 2:
        for (int k = 0; k < 7; ++k)
            target(find(panel, kFaceTabs[k]), [this, k](simd_float2) { faceTab_ = k; });
        if (faceTab_ == 0)
            if (const UiElement* fp = find(panel, kFacePanels[0]))
                for (int k = 0; k < 4; ++k)
                    target(find(*fp, 0x8800 + uint32_t(k)), [this, k](simd_float2) {
                        pushUndo();
                        edit_.spec.hairTone = k + 1;
                        const std::string tone = cas::toneFamily(k + 1);
                        for (auto& [cat, parts] : edit_.spec.outfits)
                            if (auto it = parts.find(cas::part::Hair); it != parts.end())
                                if (const SimPart* h = catalog_->byName(catalog_->parts, it->second))
                                    it->second = catalog_->hairInTone(*h, tone)->name();
                        changed();
                    });
        break;
    case 3:
        for (int k = 0; k < 6; ++k)
            target(find(panel, kCosmeticTabs[k]), [this, k](simd_float2) { cosmeticTab_ = k; });
        break;
    case 4:
        for (int k = 0; k < 6; ++k)
            target(find(panel, 0x02000000 + kClothesTabK[k]), [this, k](simd_float2) {
                clothesCategory_ = k;
                if (k != 0 && clothesPart_ != 2)
                    clothesPart_ = 2; // only everyday wear has separates (inferred)
                rebuild();
            });
        for (int k = 0; k < 3; ++k)
            target(find(panel, 0x01000000 + uint32_t(k)), [this, k](simd_float2) { clothesPart_ = k; });
        break;
    case 5:
        for (int k = 1; k <= 7; ++k)
            target(find(panel, 0x0CDC0000 + uint32_t(k)), [this, k](simd_float2) {
                const bool young = edit_.spec.age == cas::age::Toddler || edit_.spec.age == cas::age::Child;
                if (young != (k == 7))
                    return; // HandleButtonAspirations: teens and up choose; the young grow up
                edit_.aspiration = 1u << (k - 1);
            });
        for (int k = 1; k <= 12; ++k)
            target(find(panel, 0xACDC0000 + uint32_t(k)), [this, k](simd_float2) {
                pushUndo();
                edit_.zodiac = k;
                edit_.zodiacChosen = true;
                edit_.personality = cas::zodiacPersonality(k); // SetZodiacSign
            });
        for (int k = 0; k < 5; ++k)
            if (const UiElement* bar = find(panel, kTraitBars[k]))
                if (const UiElement* full = bar->child(3))
                    if (auto r = rects.find(full); r != rects.end()) {
                        const auto rr = r->second;
                        targets_.push_back({{rr[0] - 8, rr[1] - 2, rr[2] + 4, rr[3] + 2}, [this, k, rr](simd_float2 p) {
                                                pushUndo();
                                                const int v = int(std::ceil((p.x - rr[0]) / (rr[2] - rr[0]) * 10.0f));
                                                static const char* const kPoint[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
                                                if (playSound && v >= 1 && v <= 10) // ui_cas_personality<n> for the point clicked
                                                    playSound(std::string("ui_cas_personality") + kPoint[v - 1]);
                                                cas::setTrait(edit_.personality, k, std::clamp(v, 0, 10));
                                                edit_.zodiacChosen = false;
                                                edit_.zodiac = cas::zodiacSign(edit_.personality);
                                            }});
                    }
        break;
    default: break;
    }

    // The grids: a page of cells, the selected one highlighted.
    for (auto& g : grids) {
        auto r = rects.find(g.window);
        if (r == rects.end())
            continue;
        const auto gr = r->second;
        const int perPage = std::max(1, g.cols * g.rows);
        const int pages = std::max(1, (int(g.items.size()) + perPage - 1) / perPage);
        int& page = pages_[g.key];
        page = std::clamp(page, 0, pages - 1);
        const float cw = (gr[2] - gr[0]) / float(g.cols), ch = (gr[3] - gr[1]) / float(g.rows);
        for (int i = 0; i < perPage; ++i) {
            const size_t idx = size_t(page * perPage + i);
            if (idx >= g.items.size())
                break;
            Item& it = g.items[idx];
            const float x0 = gr[0] + cw * float(i % g.cols) + 3, y0 = gr[1] + ch * float(i / g.cols) + 3;
            const float x1 = x0 + cw - 6, y1 = y0 + ch - 6;
            if (it.thumb && std::isnan(it.slider)) {
                // The thumbnail, square in the cell; the selected one framed.
                const float side = std::min(x1 - x0, y1 - y0);
                const float tx0 = (x0 + x1 - side) / 2, ty0 = (y0 + y1 - side) / 2;
                if (it.selected)
                    rect(tx0 - 2, ty0 - 2, tx0 + side + 2, ty0 + side + 2, kCellSelected);
                rect(tx0, ty0, tx0 + side, ty0 + side, it.selected ? kThumbSelectedBg : kThumbBg);
                UiQuad q;
                q.image = it.thumb;
                q.x0 = tx0, q.y0 = ty0, q.x1 = tx0 + side, q.y1 = ty0 + side;
                out.quads.push_back(q);
                auto pick = it.pick;
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }, "ui_secondary_button"});
                continue;
            }
            rect(x0, y0, x1, y1, it.selected ? kCellSelected : kCellColor);
            if (!std::isnan(it.slider)) {
                text(x0 + 4, y0 + 4, x1 - x0 - 8, it.text, kCellText);
                const float by0 = y1 - 18, by1 = y1 - 10, bx0 = x0 + 8, bx1 = x1 - 8;
                rect(bx0, by0, bx1, by1, {0.25f, 0.30f, 0.60f, 1});
                const float mid = (bx0 + bx1) / 2, kx = bx0 + (bx1 - bx0) * it.slider;
                rect(std::min(mid, kx), by0, std::max(mid, kx), by1, {0.45f, 0.85f, 1.0f, 1});
                rect(kx - 3, by0 - 4, kx + 3, by1 + 4, {1, 1, 1, 1});
                auto slide = it.slide;
                targets_.push_back({{bx0 - 6, by0 - 8, bx1 + 6, by1 + 8},
                                    [slide, bx0, bx1](simd_float2 p) { slide((p.x - bx0) / (bx1 - bx0)); }});
            } else {
                // The name, over two lines when it does not fit.
                std::string t = it.text;
                const float maxW = x1 - x0 - 6;
                std::string line2;
                if (font && measureText(*font, decodeUtf8(t), ppem).width > maxW)
                    if (auto sp = t.rfind(' ', t.size() / 2 + 4); sp != std::string::npos && sp > 0)
                        line2 = t.substr(sp + 1), t = t.substr(0, sp);
                const float ty = (y0 + y1) / 2 - (line2.empty() ? 7.0f : 14.0f);
                text(x0 + 3, ty, maxW, t, kCellText);
                if (!line2.empty())
                    text(x0 + 3, ty + 14, maxW, line2, kCellText);
                auto pick = it.pick;
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }, "ui_secondary_button"});
            }
        }
        const std::string key = g.key;
        if (g.prev)
            target(g.prev, [this, key](simd_float2) { pages_[key] = std::max(0, pages_[key] - 1); });
        if (g.next)
            target(g.next, [this, key, pages](simd_float2) { pages_[key] = std::min(pages - 1, pages_[key] + 1); });
    }
    return out;
}

} // namespace os2
