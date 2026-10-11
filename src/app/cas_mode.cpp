#include "cas_mode.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/cas_thumbnails.hpp"
#include "formats/dbpf.hpp"
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

constexpr uint32_t kFrame = 0xCB980E50;
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
    res_ = std::make_unique<UiResources>(index_, res);
    fonts_ = std::make_unique<FontLibrary>(res / "UI/Fonts");
    thumbs_ = std::make_unique<cas::CasThumbnails>(res / "UserData/Thumbnails/CASThumbnails.package");
    for (uint32_t id : {kFrame, kStepScripts[0], kStepScripts[1], kStepScripts[2], kStepScripts[3], kStepScripts[4],
                        kStepScripts[5]})
        if (auto s = res_->script(id); s && !s->roots.empty())
            scripts_.emplace(id, std::move(*s));
    tempDir_ = fs::temp_directory_path() / ("opensims2-cas-" + std::to_string(getpid()));
    fs::create_directories(tempDir_);
    renderer_.hideTestScene();
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
    def.pitchMin = {6.0f, 4.0f, 2.0f};
    def.pitchMax = {6.0f, 4.0f, 2.0f};
    def.nearClip = {0.1f};
    def.farClip = {200.0f};
    def.yaws = {90.0f};
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
    // The Sim stands left of the panels: the pivot to its right (screen right is -X).
    const float side = editing_ >= 0 ? (zoom_ == 0 ? -0.55f : zoom_ == 1 ? -0.32f : -0.16f) : 0.0f;
    const simd_float3 p = simd_make_float3(side, 0, z);
    if (smooth)
        camera_->moveTo(p);
    else {
        camera_->setPivot(p);
        camera_->snap();
    }
}

void CasMode::newSim() {
    edit_ = Member{};
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

void CasMode::changed() {
    dirty_ = true;
    rebuild();
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
            std::string anim = "a-idle-neutral-armsSide-wtC-breatheLoop";
            if (sk.starts_with("c"))
                anim = "c-idle-neutral-armsSide-wtC-breatheLoop50";
            else if (sk.starts_with("p"))
                anim = "p-idle-neutral-lookAround";
            else if (sk.starts_with("b"))
                anim.clear();
            if (!anim.empty())
                try {
                    actor->cue(-0.3 * double(i), actor->animation(index_, anim));
                } catch (const std::exception& e) {
                    warnings_.push_back(anim + ": " + e.what());
                }
            // The first member on the left of the screen (screen right is -X).
            actor->position = {members.size() > 1 ? -(float(i) - float(members.size() - 1) / 2) * spacing : 0.0f, 0, 0};
            // Facing the camera (it looks along -Y from the +Y side; the skeleton faces +Y).
            actor->heading = editing_ >= 0 ? heading_ : 0;
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
            s.actor->heading = heading_;
        s.actor->update(seconds);
        renderer_.updateObject(s.actor->model, s.actor->posed);
    }
}

void CasMode::setStep(int step) {
    if (editing_ < 0)
        return;
    const int before = step_;
    step_ = std::clamp(step, 0, 5);
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
        status_ = "Editando " + (edit_.spec.firstName.empty() ? file.stem().string() : edit_.spec.firstName);
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
            const fs::path target = out / src.filename();
            std::vector<vm::NewSim> sims;
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
                cas::CharacterSpec spec = m.spec;
                spec.lastName = s.lastName;
                s.writeCharacter = [this, spec](const std::string& objectName, const fs::path& file) {
                    return cas::writeCharacter(index_, *catalog_, *baker_, spec, objectName, file).guid;
                };
                sims.push_back(std::move(s));
            }
            const auto r = vm::writeNewFamily(src, target, familyName_, "", std::move(sims));
            status_ = "Família " + familyName_ + " (" + std::to_string(r.family) + ") salva em " + r.neighborhood.string();
            std::printf("opensims2: cas: family %u \"%s\" written to %s:", r.family, familyName_.c_str(),
                        r.neighborhood.string().c_str());
            for (const auto& c : r.characters)
                std::printf(" %s", c.filename().string().c_str());
            std::printf("\n");
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
    if (width != canvasW_ || height != canvasH_)
        canvasW_ = width, canvasH_ = height, dirty_ = true;
}

void CasMode::encode(MTL::RenderCommandEncoder* enc, uint32_t width, uint32_t height, float scale) {
    if (!layers_[0] || width == 0 || height == 0)
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
    if (dirty_ || laidW_ != canvasW_ || laidH_ != canvasH_)
        screen(canvasW_, canvasH_);
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it)
        if (inside(it->rect, p)) {
            auto action = it->action; // the action may lay out again
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
        std::string& text = focus_ == Focus::FirstName ? edit_.spec.firstName : familyName_;
        if (keyCode == kBackspace) {
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
                text.pop_back(); // UTF-8 continuation bytes
            if (!text.empty())
                text.pop_back();
        } else if (keyCode == kReturn) {
            focus_ = Focus::None;
        } else {
            for (char c : chars)
                if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F && text.size() < 32)
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
        } else if (a == "--cas-age" || a == "--cas-gender") {
            if (a == "--cas-age")
                edit_.spec.age = v == "toddler" ? cas::age::Toddler : v == "child" ? cas::age::Child : v == "teen" ? cas::age::Teen
                                 : v == "elder" ? cas::age::Elder : v == "baby" ? cas::age::Baby : cas::age::Adult;
            else
                edit_.spec.gender = v == "m" ? cas::gender::Male : cas::gender::Female;
            edit_.spec.outfits.clear();
            edit_.spec.overlays.clear();
            for (const SimPart* ar : catalog_->archetypesFor(edit_.spec.age, edit_.spec.gender))
                if (lower(ar->name()).find("archbase") != std::string::npos)
                    edit_.spec.face.archetype = cas::family(*ar);
            cas::completeSpec(*catalog_, edit_.spec);
            changed();
        } else if (a == "--cas-done")
            finishSim();
        else if (a == "--cas-new")
            newSim();
        else if (a == "--cas-accept")
            acceptFamily();
        else if (a == "--cas-edit")
            openCharacter(v);
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
            targets_.push_back({r->second, std::move(f)});
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
    target(find(frame, kExit), [this](simd_float2) {
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

    // The family view: the members' names over the household bar, and a
    // button to make another Sim.
    if (editing_ < 0) {
        float x = ox + 180, y = oy + 650;
        for (size_t i = 0; i < family_.size(); ++i) {
            const std::string n = family_[i].spec.firstName;
            rect(x - 4, y - 2, x + 120, y + 18, kCellColor);
            text(x, y, 116, n, kCellText);
            const int idx = int(i);
            targets_.push_back({{x - 4, y - 2, x + 120, y + 18}, [this, idx](simd_float2) {
                                    edit_ = family_[size_t(idx)];
                                    editing_ = idx;
                                    step_ = 0;
                                    rebuild();
                                }});
            x += 130;
        }
        if (family_.size() < 8) {
            rect(x - 4, y - 2, x + 120, y + 18, kCellSelected);
            text(x, y, 116, "+ Criar um Sim", kCellText);
            targets_.push_back({{x - 4, y - 2, x + 120, y + 18}, [this](simd_float2) { newSim(); }});
        }
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
        const std::string face = cas::agePrefix(s.age, s.gender, true) + "face-s" + std::to_string(s.skinTone);
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
            items.push_back({label(a->name().substr(0, a->name().find('_'))), sel,
                             thumb(cas::archetypeThumbnailId(s.age, s.gender, fam, region)), [this, fam, maskList]() {
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
            case 2: items.push_back({"(óculos: overlays de malha, ainda não)", false, nullptr, [] {}}); break;
            case 3: items = overlayItems(cas::overlay::Brows, -1, false); break;
            case 4: items = overlayItems(cas::overlay::FacialHair, 1, true); break;
            case 5: items = overlayItems(cas::overlay::FacialHair, 2, true); break;
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
    layoutUiElement(panel, panelX - float(pa[0]), panelY - float(pa[1]), *res_, *fonts_, out, hk);
    if (auto r = rects.find(&panel); r != rects.end())
        solid_.push_back(r->second);

    // Targets of the panel's controls.
    switch (step_) {
    case 0: {
        auto setGender = [this](uint32_t g) {
            if (edit_.spec.gender == g)
                return;
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
        };
        target(find(panel, 0x401), [setGender](simd_float2) { setGender(cas::gender::Female); });
        target(find(panel, 0x400), [setGender](simd_float2) { setGender(cas::gender::Male); });
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
        target(find(panel, 0x8CB7C11F), [this](simd_float2) { setStep(1); });
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
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }});
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
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }});
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
