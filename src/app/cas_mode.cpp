#include "cas_mode.hpp"
#include "app/cas_mode_internal.hpp"
#include "formats/log_once.hpp"
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
#include "formats/graphics_options.hpp"
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

bool inside(const std::array<float, 4>& r, simd_float2 p) {
    return p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
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
    try {
        casSpecs_ = loadCameraConfig(res / "UserData/Cameras/CASCamera.txt");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: CASCamera.txt: %s\n", e.what());
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
    def.verticalFov = true; // our orbit's framing, only without the studio (the CreateASim camera otherwise)
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
        const simd_float3 d = it->second.position - spot();
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

bool CasMode::applyCasCamera(bool smooth) {
    // The camera spec and studio locator for the view (inferred pairing by
    // name, docs/camera-ui-mac.md 1.6): the family view CAS_Family at
    // camera_family_0; the face steps (hair, face, cosmetics)
    // CAS_Face<age> at camera_face_<age>; the others CAS_Clothes<age> at
    // camera_clothes_<age> (babies as toddlers, elders as adults).
    if (!studio_ || casSpecs_.empty())
        return false;
    std::string spec = "CAS_Family", mark = "camera_family_0";
    if (editing_ >= 0) {
        const auto a = edit_.spec.age;
        const char* age = a == cas::age::Baby || a == cas::age::Toddler ? "Toddler"
                          : a == cas::age::Child                        ? "Child"
                          : a == cas::age::Teen                         ? "Teen"
                                                                        : "Adult";
        const bool face = step_ == 1 || step_ == 2 || step_ == 3;
        spec = std::string(face ? "CAS_Face" : "CAS_Clothes") + age;
        mark = std::string(face ? "camera_face_" : "camera_clothes_") + lower(age);
    }
    const CameraDef* def = findCamera(casSpecs_, spec);
    const auto it = studio_->cameras().find(mark);
    if (!def || it == studio_->cameras().end())
        return false;
    const std::string shot = spec + "@" + mark;
    if (shot != casShot_ || !smooth) {
        casCam_.setActive(CasCameraSpec::from(*def), it->second.position, it->second.orientation, !smooth);
        casShot_ = shot;
    }
    const CasCamera::Pose p = casCam_.pose();
    camera_->setFirstPerson(LotCamera::FirstPerson{p.eye, p.forward, p.up, p.fov, p.nearClip, p.farClip});
    return true;
}

void CasMode::applyCamera(bool smooth) {
    if (!camera_)
        return;
    if (applyCasCamera(smooth))
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
    // HandleButtonGlobalRandomize (docs/cas-mac.md, round n10).
    cas::randomizeLook(*catalog_, edit_.spec, rng);
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

void CasMode::setFitnessLevel(int level, bool undo) {
    if (undo)
        pushUndo();
    // cTSUISimEditor::SetFitnessLevel (Mac BV 0x1b5b24): fitness 2 at 502 and
    // over, else 0, on the outfits; no part changes (the game has no fat
    // outfits), the body takes the fattop/fatbot morphs.
    edit_.spec.fitness = level >= 502 ? 2 : 0;
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
        } catch (const std::exception&) { // (the next one is tried)
        }
    os2::logOnce("CAS idle", sk, "none of the age's idles loads");
    return nullptr;
}
} // namespace

void CasMode::changed() {
    dirty_ = true;
    rebuild();
    ensureArchetypeShots();
}

void CasMode::ensureArchetypeShots() {
    // (Step 2 lists the heads; step 3's brow, eyes, nose, mouth and jaw
    // panels list them again, with the same made shot for the alien.)
    if (editing_ < 0 || (step_ != 1 && step_ != 2) || !thumbs_ || !catalog_)
        return;
    for (const SimPart* a : catalog_->archetypesFor(edit_.spec.age, edit_.spec.gender, cas::flag::Hidden)) {
        const std::string fam = cas::family(*a);
        if (thumbs_->image(cas::archetypeThumbnailId(edit_.spec.age, edit_.spec.gender, fam, "")))
            continue;
        // In the shipped thumbnails' skin (they all show one medium tone,
        // s2 by eye; inferred), so the made one does not stand out of the grid.
        const std::string key = cas::casFileName(edit_.spec.age, edit_.spec.gender) + "|" + fam + "|" +
                                std::to_string(kThumbnailSkinTone);
        if (archetypeShots_.count(key))
            continue;
        Member m = edit_;
        m.spec.skinTone = kThumbnailSkinTone;
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
            actor->bodyMorphs = outfitFitnessMorphs(spec.fitness); // the fat body (Mac BV 0x8141c4)
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
            os2::logOnce("CAS: rebuild", "", e);
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
    if (camera_ && casCam_.active() && camera_->firstPerson()) {
        casCam_.update(dt * 1000.0);
        const CasCamera::Pose p = casCam_.pose();
        camera_->setFirstPerson(LotCamera::FirstPerson{p.eye, p.forward, p.up, p.fov, p.nearClip, p.farClip});
    }
    std::vector<Renderer::ShadowCaster> casters;
    for (auto& s : shown_) {
        if (!s.actor)
            continue;
        if (editing_ >= 0)
            s.actor->heading = baseHeading() + heading_;
        s.actor->update(seconds);
        renderer_.updateObject(s.actor->model, s.actor->posed);
        // The Sims' shadows in the studio (a room: straight down, the
        // floor under the feet multiplied; docs/lighting-mac.md 9).
        if (studio_ && s.instance >= 0 && graphicsOptions().simShadows) {
            const Xform at = s.actor->placement();
            const Vec3 feet = at.apply({0, 0, 0}), side = at.apply({1, 0, 0});
            Renderer::ShadowCaster c;
            c.instance = s.instance;
            c.feet = simd_make_float3(feet[0], feet[1], feet[2]);
            c.heading = std::atan2(side[1] - feet[1], side[0] - feet[0]);
            casters.push_back(c);
        }
    }
    renderer_.setShadowCasters(std::move(casters));
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
        actor.bodyMorphs = outfitFitnessMorphs(spec.fitness);
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
        os2::logOnce("CAS: portrait", "", e);
        capturing_ = false;
        warnings_.push_back(std::string("portrait: ") + e.what());
    }
    std::error_code ec;
    fs::remove(file, ec);
    return out;
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
        else if (a == "--cas-fitness") // fat or normal, as the two body buttons
            setFitnessLevel(v == "fat" ? 1000 : 500, false);
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
    // Scripted views (screenshots) see where the camera arrives.
    if (camera_ && casCam_.active() && camera_->firstPerson()) {
        casCam_.snap();
        const CasCamera::Pose p = casCam_.pose();
        camera_->setFirstPerson(LotCamera::FirstPerson{p.eye, p.forward, p.up, p.fov, p.nearClip, p.farClip});
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
        fh.captions[t] = edit_.spec.firstName.empty() ? uiCaption(*t, *res_) : edit_.spec.firstName;
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
        if (camera_ && camera_->firstPerson()) {
            // A click: ten keyboardzoomspeed steps (half a second held; inferred).
            for (int i = 0; i < 10; ++i)
                casCam_.zoomStep(1);
            return;
        }
        zoom_ = std::min(zoom_ + 1, 2);
        if (camera_)
            camera_->zoomBy(1);
        applyCamera(true);
    });
    target(find(frame, kZoomOut), [this](simd_float2) {
        if (camera_ && camera_->firstPerson()) {
            for (int i = 0; i < 10; ++i)
                casCam_.zoomStep(-1);
            return;
        }
        zoom_ = std::max(zoom_ - 1, 0);
        if (camera_)
            camera_->zoomBy(-1);
        applyCamera(true);
    });
    target(find(frame, kTurnRight), [this](simd_float2) { heading_ -= 0.4f; });
    target(find(frame, kTurnLeft), [this](simd_float2) { heading_ += 0.4f; });
    if (const UiElement* gen = find(frame, kFamilyNameGen))
        target(gen, [this](simd_float2) { focus_ = Focus::FamilyName; });

    // The family view and its panels (cas_mode_family.cpp).
    LayoutContext L{out, rects, frame, ox, oy, font, ppem, text, rect, target};
    if (familyScreen(L))
        return out; // the Make a Child panel covers the screen
    if (!status_.empty()) {
        rect(ox + 200, oy + 8, ox + 820, oy + 30, {0.1f, 0.1f, 0.3f, 0.75f});
        text(ox + 206, oy + 11, 600, status_, {1, 1, 1, 1});
    }
    if (editing_ < 0)
        return out;

    // The step panel (cas_mode_panels.cpp).
    stepScreen(L);
    return out;
}

} // namespace os2
