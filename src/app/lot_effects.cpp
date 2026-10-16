#include "app/lot_effects.hpp"
#include "formats/log_once.hpp"

#include "app/ui_panel_util.hpp"
#include "app/vm_objects.hpp"
#include "formats/ui_image.hpp"
#include "formats/strings.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/resindex.hpp"
#include "formats/model.hpp"
#include "formats/sim_actor.hpp"
#ifdef _WIN32
#include "render/gpu_renderer.hpp"
#else
#include "render/particle_layer.hpp"
#include "render/renderer.hpp"
#endif
#include "vm/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace os2 {

namespace {

// Sims' balloon slot: Effect Stop/Start slot type 2, slot 4 = hat_grip (the
// binary checks the ShowHeadlines option for it). The balloons rise from it
// along their meta particles' paths (0.15 to 0.35 above it, SnapParticleToPath).
constexpr uint32_t kTemplatePersonGroup = 0x7FEDFE16; // TemplatePerson: Sims' SLOT and bone names
// Fire-and-forget effects (mode 6) are stopped after this long.
constexpr double kOneShotSeconds = 4.0;

fx::Placement fromXform(const Xform& x) {
    fx::Placement p;
    p.position = {x.t[0], x.t[1], x.t[2]};
    const Quat& q = x.q;
    p.rotation = {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w),
                  2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w),
                  2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)};
    return p;
}

// The lot as the particles' world-space colliders see it (cFloorsCollider,
// cWallsCollider; docs/effects-mac.md 4f.3). Levels as os2::Lot numbers
// them (0 the ground).
class LotColliders : public fx::ColliderWorld {
public:
    explicit LotColliders(const Lot& lot) : lot_(lot) {}

    // nTSWorld::LevelBelow (BV 0x1670cc).
    int levelBelow(const fx::V3& p) const override {
        if (lot_.levelCount <= 0)
            return 0;
        const float x = std::clamp(p[0], 0.0f, float(lot_.width)), y = std::clamp(p[1], 0.0f, float(lot_.height));
        for (int l = lot_.minLevel + lot_.levelCount - 1; l > lot_.minLevel; --l)
            if (lot_.elevationAt(l, x, y) < p[2] + 0.01f)
                return l;
        return lot_.minLevel;
    }

    // SampledFloorElevation where the level has a floor at the point's
    // quarter tile (the WorldDB's test, 0xb8). The ground counts everywhere
    // on the lot and the quarter is the point's triangle of the tile
    // (inferred: FUN_0015d2f0 and the test's terrain case not read).
    bool floor(int level, float x, float y, float& z) const override {
        if (!lot_.hasLevel(level) || x < 0 || y < 0 || x > float(lot_.width) || y > float(lot_.height))
            return false;
        z = lot_.elevationAt(level, x, y);
        if (level == 0)
            return true;
        const uint32_t tx = std::min(uint32_t(x), lot_.width - 1), ty = std::min(uint32_t(y), lot_.height - 1);
        const float dx = x - float(tx) - 0.5f, dy = y - float(ty) - 0.5f;
        const int quarter = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? 0 : 2) : (dy > 0 ? 1 : 3);
        return lot_.floor(level, tx, ty)[size_t(quarter)] != 0;
    }

    // The walls of the level the segment starts on (no fences), a story
    // high: the segment stops `radius` before the first one it crosses, on
    // the side it came from (inferred: the WorldDB's wall tests not read).
    bool wall(const fx::V3& a, const fx::V3& b, float radius, fx::V3& at, fx::V3& normal) const override {
        const int level = levelBelow(a);
        float bottom = 0, top = 0;
        const float cx = std::clamp(a[0], 0.0f, float(lot_.width)), cy = std::clamp(a[1], 0.0f, float(lot_.height));
        bottom = lot_.elevationAt(level, cx, cy);
        top = lot_.hasLevel(level + 1) ? lot_.elevationAt(level + 1, cx, cy) : bottom + 3;
        if (a[2] < bottom || a[2] > top)
            return false;
        const float mx = b[0] - a[0], my = b[1] - a[1];
        float best = 2;
        for (const auto& w : lot_.walls) {
            if (w.a.level != level || w.type >= 0x100)
                continue;
            const float wx = w.b.x - w.a.x, wy = w.b.y - w.a.y;
            const float wl = std::hypot(wx, wy);
            if (wl < 1e-6f)
                continue;
            float nx = -wy / wl, ny = wx / wl; // the wall's normal, towards where the segment starts
            float da = (a[0] - w.a.x) * nx + (a[1] - w.a.y) * ny;
            if (da < 0)
                nx = -nx, ny = -ny, da = -da;
            const float vn = mx * nx + my * ny;
            if (!(vn < -1e-6f))
                continue;
            const float t = (radius - da) / vn; // when it comes within the radius
            if (!(t <= 1) || t >= best)
                continue;
            const float tc = std::max(t, 0.0f);
            const float px = a[0] + mx * tc, py = a[1] + my * tc;
            const float along = ((px - w.a.x) * wx + (py - w.a.y) * wy) / (wl * wl);
            if (along < 0 || along > 1)
                continue;
            best = t;
            at = {px, py, a[2] + (b[2] - a[2]) * tc};
            normal = {nx, ny, 0};
        }
        return best <= 1;
    }

private:
    const Lot& lot_;
};

} // namespace

void LotEffects::setLot(const Lot* lot) {
    if (lot == lotSeen_)
        return;
    lotSeen_ = lot;
    world_ = lot ? std::make_unique<LotColliders>(*lot) : nullptr;
    system_->setWorld(world_.get());
}

LotEffects::LotEffects(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir)
    : renderer_(renderer), index_(index) {
    try {
        thumbnails_ = std::make_unique<DbpfPackage>(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
        for (const auto& e : thumbnails_->entries())
            thumbIndex_.emplace(e.tgi.instance, &e);
    } catch (const std::exception& err) {
        os2::logOnce("balloon icons (ObjectThumbnails.package)", "", err);
        thumbnails_.reset(); // balloons keep their default icons
    }
    // Ultimate Collection's Catalog/Bins/*.bundle.package carry small effects
    // resources of their own, indexed before Effects/effects.package.
    std::optional<std::vector<uint8_t>> data;
    for (const auto& loc : index.ofType(fx::kTypeEffects)) {
        bool main = loc.package->path().filename() == "effects.package";
        if (main || !data || loc.entry->rawSize.value_or(loc.entry->size) > data->size())
            data = index.read(loc);
        if (main)
            break;
    }
    if (!data)
        throw std::runtime_error("no effects resource (Res/Effects/effects.package)");
    library_ = std::make_unique<fx::EffectsLibrary>(fx::EffectsLibrary::parse(*data));
    system_ = std::make_unique<fx::EffectSystem>(*library_);
#ifndef _WIN32
    layer_ = std::make_unique<ParticleLayer>(renderer.device(), renderer.colorFormat(), renderer.depthFormat(), &index);
    // The particle groups sort among the blended parts (layer 7 and up), as
    // the game's display list sorts them.
    ParticleLayer* layer = layer_.get();
    renderer.setExternalBlended(
        [layer](const simd_float4x4& view, const simd_float4x4& viewProj, std::vector<Renderer::ExternalItem>& out) {
            std::vector<ParticleLayer::Group> groups;
            layer->prepare(view, viewProj, groups);
            for (const auto& g : groups)
                out.push_back({g.layer, g.depth, g.id});
        },
        [layer](MTL::RenderCommandEncoder* enc, int id) { layer->drawGroup(enc, id); });
#endif
}

#ifdef _WIN32
LotEffects::~LotEffects() { renderer_.setParticles({}, nullptr); }

void LotEffects::setImage(const std::string& name, const RgbaImage& image) { renderer_.setParticleImage(name, image); }
#else
LotEffects::~LotEffects() { renderer_.setExternalBlended(nullptr, nullptr); }

void LotEffects::setImage(const std::string& name, const RgbaImage& image) { layer_->setImage(name, image); }
#endif

// The picture of what a balloon names by GUID (TryChangeEffect flag 0x04 BV
// 0xe44a6c: the object or GUID handed to the thumbnail service, whose
// texture replaces the balloon's "objectthumbnail" particle). The game
// renders one when it has none; here: the object's catalog thumbnail, a
// memory's icon (its UI image 1), a Sim's portrait (a character's GUID);
// "" when none of them exists (inferred: the service not followed).
std::string LotEffects::thumbnail(vm::VM& vm, uint32_t guid) {
    if (auto it = thumbNames_.find(guid); it != thumbNames_.end())
        return it->second;
    std::string name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "thumbnail:%08X", guid);
    std::string what = "no definition";
    if (auto def = vm.code().definitionByGuid(guid))
        if (const ObjectDefinition* d = vm.code().definition(def->first, def->second)) {
            what = d->fileName;
            // As the live UI keys them (app/ui_panel_util.hpp objectThumbnailInstance).
            if (const StringTable* names = vm.code().strings(def->first, 0x85); names && thumbnails_) {
                const std::string model = names->get(d->modelIndex(), lang::EnglishUS);
                if (auto e = thumbIndex_.find(objectThumbnailInstance(def->first, model)); e != thumbIndex_.end())
                    if (auto img = decodeUiImage(thumbnails_->read(*e->second))) {
                        name = buf;
                        setImage(name, *img);
                    }
            }
            // A memory: its icon (as the memories panel draws it).
            if (name.empty() && d->objectType() == 15)
                if (auto bytes = index_.read(Tgi{kTypeUiImage, def->first, 1, 0}))
                    if (auto img = decodeUiImage(*bytes)) {
                        name = buf;
                        setImage(name, *img);
                    }
        }
    // A Sim (its character's GUID): the portrait.
    if (name.empty() && iconImage)
        for (const auto& [nid, n] : vm.neighbors)
            if (n.guid == guid) {
                vm::ActiveEffect e;
                e.iconSim = nid;
                if (auto img = iconImage(e)) {
                    name = buf;
                    setImage(name, *img);
                }
                break;
            }
    if (std::getenv("OS2_DEBUG_FX"))
        std::fprintf(stderr, "opensims2: balloon thumbnail %08X (%s): %s\n", guid, what.c_str(),
                     name.empty() ? "none" : name.c_str());
    thumbNames_[guid] = name;
    return name;
}

bool LotEffects::addManual(const std::string& spec, int16_t object, fx::V3 at, int slot) {
    // "<name>~<state>": in that state (Effect Stop/Start modes 10-13);
    // "<name>#<guid hex>": a balloon showing that GUID's picture.
    std::string name = spec;
    int state = 0;
    uint32_t guid = 0;
    if (const size_t hash = name.rfind('#'); hash != std::string::npos) {
        guid = uint32_t(std::stoul(name.substr(hash + 1), nullptr, 16));
        name.resize(hash);
    }
    if (const size_t tilde = name.rfind('~'); tilde != std::string::npos) {
        state = std::atoi(name.c_str() + tilde + 1);
        name.resize(tilde);
    }
    if (!library_->find(name))
        return false;
    manual_.push_back({name, object, at, -1, slot, state, guid});
    return true;
}

std::optional<fx::Placement> LotEffects::placementOf(int16_t id, int8_t slotType, uint8_t slot, VmObjectView* view,
                                                     const SimLookup& sims, vm::VM* vm, const std::string& named) const {
    // The effect's slot (TryChangeEffect): type 0 a containment slot (the
    // object's SLOT type-0 entries), type 2 a bone or model node by its
    // index in the object's STR# 0x90 ("Model - Bones": the stove's
    // slot_stove_0, a Sim's hat_grip for slot 4, where balloons go); type 1
    // a target slot (the object's SLOT entries of type 1; the binary's
    // GetTargetSlot, only the flowers' bugs use it). Inferred from the data.
    std::string node = named; // a saved effect names its node
    const vm::Object* o = vm ? vm->object(id) : nullptr;
    if (view && o && node.empty()) {
        // Type 0 GetObjectSlot: the containment slots (SLOT type 0), else
        // the effect/target slots (type 4: the PlumbBob's only entry,
        // slot_plumbob_0_fx); type 1 GetTargetSlot: type 4 (the flowers'
        // slot_flowers_0 for their bugs). Inferred: SLOT type 4 = "target"
        // as SimPE names them; the saved effects name those nodes.
        if (slotType == 0) {
            node = view->containerSlotBone(id, slot);
            if (node.empty())
                node = view->typedSlotBone(id, slot, 4);
        } else if (slotType == 1) {
            node = view->typedSlotBone(id, slot, 4);
        }
        else if (slotType == 2) {
            const StringTable* names = vm->code().strings(o->group, 0x90);
            if (!names && o->person)
                names = vm->code().strings(kTemplatePersonGroup, 0x90);
            if (names && slot < names->count(lang::EnglishUS))
                node = names->get(slot, lang::EnglishUS);
            if (auto comma = node.find(','); comma != std::string::npos)
                node.resize(comma);
        }
    }
    if (placementOverride && view)
        if (auto moved = placementOverride(id)) {
            Xform at = *moved;
            if (auto base = view->placement(id); base && !node.empty())
                if (auto n = view->nodePlacement(id, node)) // the node's height over the object, kept
                    for (int k = 0; k < 3; ++k)
                        at.t[size_t(k)] += n->t[size_t(k)] - base->t[size_t(k)];
            return fromXform(at);
        }
    const SimActor* sim = sims ? sims(id) : nullptr;
    fx::Placement p;
    if (sim)
        p = fromXform(sim->placement());
    else if (view) {
        auto x = view->placement(id);
        if (!x)
            return std::nullopt;
        p = fromXform(*x);
    } else {
        return std::nullopt;
    }
    if (!node.empty() && view)
        if (auto n = view->nodePlacement(id, node)) {
            // The node's position; Sims keep their facing (bones turn
            // with the animation), objects take the node's rotation.
            fx::Placement np = fromXform(*n);
            if (sim)
                np.rotation = p.rotation;
            p = np;
        }
    return p;
}

void LotEffects::update(double seconds, vm::VM* vm, VmObjectView* view, const SimLookup& sims) {
    const float dt = last_ < 0 ? 0.0f : float(std::max(0.0, seconds - last_));
    last_ = seconds;

    // The VM's effects: start new ones (by serial), stop the ones it dropped.
    for (auto& [k, r] : vm_)
        r.seen = false;
    if (vm) {
        for (const auto& [id, obj] : vm->objects())
            for (const auto& e : obj.animation.effects) {
                const auto key = std::make_pair(id, e.serial);
                auto it = vm_.find(key);
                if (it == vm_.end()) {
                    Running r;
                    fx::V3 where{-1, -1, -1};
                    if (auto at = placementOf(id, e.slotType, e.slot, view, sims, vm, e.node)) {
                        where = at->position;
                        // Mode 1 starts hard, mode 0 soft (TryChangeEffect ->
                        // PerformEffectAction 2 / 0); an effect the lot saved comes
                        // back as a hard start (inferred: its Read not followed).
                        r.handle = system_->start(e.name, *at, e.hard || !e.node.empty());
                        if (r.handle >= 0 && !e.icon.empty())
                            system_->setIcon(r.handle, e.icon, e.iconFrame);
                        // An object's thumbnail in the balloon (flag 0x04: the
                        // object, or the GUID in temps 4/5 with 0x400).
                        uint32_t guid = e.iconGuid;
                        if (!guid && e.iconObject)
                            if (const vm::Object* o = vm->object(e.iconObject))
                                guid = o->guid();
                        if (r.handle >= 0 && guid) {
                            if (auto t = thumbnail(*vm, guid); !t.empty())
                                system_->setIcon(r.handle, t);
                            else // no picture: no icon, not the particle's own atlas squeezed (inferred)
                                system_->hideIcon(r.handle);
                        }
                        if (r.handle >= 0 && iconImage && (e.iconSim || e.iconId || e.wantSim)) {
                            // Portraits are kept by Sim; want icons change, one texture each time.
                            const std::string t = e.iconSim ? "portrait:" + std::to_string(e.iconSim)
                                                            : "icon:" + std::to_string(++iconSerial_);
                            if (e.iconSim && iconNames_.count(t))
                                system_->setIcon(r.handle, t);
                            else if (auto img = iconImage(e)) {
                                if (std::getenv("OS2_DEBUG_FX"))
                                    std::fprintf(stderr, "opensims2: balloon icon %s (%ux%u)\n", t.c_str(), img->width, img->height);
                                setImage(t, *img);
                                iconNames_.insert(t);
                                system_->setIcon(r.handle, t);
                            } else {
                                system_->hideIcon(r.handle); // (no picture: not the particle's own atlas)
                            }
                        }
                    }
                    if (std::getenv("OS2_DEBUG_FX"))
                        std::fprintf(stderr, "opensims2: effect %s on %d (slot %d/%d, flags 0x%X, icon '%s', sim %d) at %.2f s, (%.2f %.2f %.2f)%s\n",
                                     e.name.c_str(), id, e.slotType, e.slot, e.flags, e.icon.c_str(), e.iconSim, seconds, where[0], where[1], where[2],
                                     r.handle < 0 ? " not shown" : "");
                    it = vm_.emplace(key, r).first;
                    if (e.oneShot && r.handle >= 0)
                        manual_.push_back({"", id, {float(seconds + kOneShotSeconds), 0, 0}, r.handle});
                }
                it->second.seen = true;
                if (it->second.handle >= 0)
                    system_->setState(it->second.handle, e.state);
                if (it->second.handle >= 0)
                    if (auto at = placementOf(id, e.slotType, e.slot, view, sims, vm, e.node)) {
                        system_->move(it->second.handle, *at);
                        if (std::getenv("OS2_DEBUG_FX_MOVE"))
                            std::fprintf(stderr, "opensims2: effect %s on %d moved to (%.2f %.2f %.2f)\n", e.name.c_str(), id,
                                         at->position[0], at->position[1], at->position[2]);
                    }
            }
    }
    for (auto it = vm_.begin(); it != vm_.end();) {
        if (!it->second.seen) {
            if (it->second.handle >= 0)
                system_->stop(it->second.handle, false);
            it = vm_.erase(it);
        } else {
            ++it;
        }
    }

    // Hand-started effects (and the timers of fire-and-forget ones: name "").
    for (auto it = manual_.begin(); it != manual_.end();) {
        Manual& m = *it;
        if (m.name.empty()) { // a one-shot timer: at[0] is when it stops
            if (seconds >= m.at[0]) {
                system_->stop(m.handle, false);
                it = manual_.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        fx::Placement p;
        p.position = m.at;
        if (m.object)
            if (auto at = m.slot >= 0 ? placementOf(m.object, 2, uint8_t(m.slot), view, sims, vm)
                                      : placementOf(m.object, 0, 0, view, sims, vm)) {
                p = *at;
                for (int k = 0; k < 3; ++k)
                    p.position[size_t(k)] += m.at[size_t(k)];
            }
        // (Started soft, as the VM's mode 0: a burst plays, no pre-roll.)
        if (m.handle < 0 || !system_->alive(m.handle)) {
            m.handle = system_->start(m.name, p, false);
            if (m.handle >= 0 && m.thumbnailGuid && vm) {
                if (auto t = thumbnail(*vm, m.thumbnailGuid); !t.empty())
                    system_->setIcon(m.handle, t);
                else
                    system_->hideIcon(m.handle);
            }
        }
        else
            system_->move(m.handle, p);
        system_->setState(m.handle, m.state);
        ++it;
    }

    system_->update(dt);
    placeModels();
    quads_.clear();
    system_->collect(quads_);
    // The lit particles (texture -light) under the lot's environmentCube
    // light, "LM_Environment" (TSCreateLightsFromLightingStateWithAmbient BV
    // 0xd741dc, the lighting state's `environment`: 1 by day, 0.8 at night):
    // StandardParticlesMaterial's colorScalar -applyTextureLightColor stage.
#ifdef _WIN32
    const simd_float3 env = {1.0f, 1.0f, 1.0f}; // the Windows renderer has no environmentLight() yet
#else
    const simd_float3 env = renderer_.environmentLight();
#endif
    for (auto& q : quads_)
        if (q.lit)
            for (int k = 0; k < 3; ++k)
                q.color[size_t(k)] *= env[k];
#ifdef _WIN32
    renderer_.setParticles(quads_, &index_);
#else
    layer_->setQuads(quads_);
#endif
    if (std::getenv("OS2_DEBUG_FX")) {
        std::fprintf(stderr, "opensims2: effects at %.2f s: %zu running, %zu particles (LOD %.2f, environment %.2f)\n",
                     seconds, system_->instanceCount(), quads_.size(), system_->lod(), env[0]);
        if (!quads_.empty())
            std::fprintf(stderr, "opensims2:   first at (%.2f %.2f %.2f) size %.3f rgba (%.2f %.2f %.2f %.2f)\n",
                         quads_[0].position[0], quads_[0].position[1], quads_[0].position[2], quads_[0].width,
                         quads_[0].color[0], quads_[0].color[1], quads_[0].color[2], quads_[0].color[3]);
    }
}

} // namespace os2

namespace os2 {

namespace {

std::string eventField(const std::string& data, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while (at < data.size()) {
        const size_t end = data.find_first_of("|\r\n", at);
        if (data.compare(at, k.size(), k) == 0)
            return data.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return "";
}

std::optional<fx::Placement> bonePlacement(const SimActor& a, const std::string& bone) {
    const int b = bone.empty() ? -1 : a.skin.skeleton.find(bone);
    if (b >= 0 && size_t(b) < a.bones.size())
        return fromXform(a.bones[size_t(b)]);
    return fromXform(a.placement());
}

} // namespace

void LotEffects::placeModels() {
    models_.clear();
    system_->collectModels(models_);
    std::map<std::pair<int, int>, int> seen;
    for (const auto& m : models_) {
        std::string cres = *m.model;
        if (cres.ends_with("_cres"))
            cres.resize(cres.size() - 5);
        auto mit = effectModels_.find(cres);
        if (mit == effectModels_.end()) {
            int id = -1;
            try {
                id = renderer_.addObject(loadObjectModel(index_, cres), index_);
            } catch (const std::exception& e) {
                if (std::getenv("OS2_DEBUG_FX"))
                    std::fprintf(stderr, "opensims2: effect model %s: %s\n", cres.c_str(), e.what());
            }
            mit = effectModels_.emplace(cres, id).first;
        }
        if (mit->second < 0)
            continue;
        const auto key = std::make_pair(m.handle, m.part);
        int inst = -1;
        auto old = modelInstances_.find(key);
        if (old != modelInstances_.end() && instanceModel_[old->second] != mit->second) {
            // The part now shows another model (a fish that died): its old instance goes back.
            simd_float4x4 away = matrix_identity_float4x4;
            away.columns[3] = simd_make_float4(0, 0, -1000, 1);
            renderer_.setInstanceTransform(old->second, away);
            freeModelInstances_.push_back(old->second);
            modelInstances_.erase(old);
            old = modelInstances_.end();
        }
        if (old != modelInstances_.end())
            inst = old->second;
        else {
            for (auto f = freeModelInstances_.begin(); f != freeModelInstances_.end(); ++f)
                if (instanceModel_[*f] == mit->second) {
                    inst = *f;
                    freeModelInstances_.erase(f);
                    break;
                }
            if (inst < 0) {
                inst = renderer_.addInstance(mit->second, matrix_identity_float4x4);
                instanceModel_[inst] = mit->second;
            }
        }
        const auto& r = m.where.rotation;
        const float k = m.where.scale;
        simd_float4x4 t = simd_matrix(simd_make_float4(r[0] * k, r[3] * k, r[6] * k, 0),
                                      simd_make_float4(r[1] * k, r[4] * k, r[7] * k, 0),
                                      simd_make_float4(r[2] * k, r[5] * k, r[8] * k, 0),
                                      simd_make_float4(m.where.position[0], m.where.position[1], m.where.position[2], 1));
        renderer_.setInstanceTransform(inst, t);
        seen[key] = inst;
    }
    for (const auto& [key, inst] : modelInstances_)
        if (!seen.count(key)) {
            simd_float4x4 away = matrix_identity_float4x4;
            away.columns[3] = simd_make_float4(0, 0, -1000, 1);
            renderer_.setInstanceTransform(inst, away);
            freeModelInstances_.push_back(inst);
        }
    modelInstances_ = std::move(seen);
}

void LotEffects::animationEffects(const std::vector<const SimActor*>& actors, double seconds) {
    const double from = animScanned_ < 0 ? seconds : animScanned_;
    animScanned_ = seconds;
    std::set<const SimActor*> present(actors.begin(), actors.end());
    for (const SimActor* a : actors) {
        if (!a)
            continue;
        for (const auto& e : a->player.eventsBetween(from, seconds)) {
            if (e.event->type != 8)
                continue;
            const std::string name = eventField(e.event->data, "effectName");
            const std::string bone = eventField(e.event->data, "boneName");
            const std::string op = eventField(e.event->data, "op");
            if (name.empty())
                continue;
            const bool stopping = op.find("Stop") != std::string::npos || op.find("stop") != std::string::npos;
            const bool hard = op.rfind("hard", 0) == 0;
            for (auto& r : animEffects_)
                if (r.actor == a && r.name == name && r.handle >= 0) {
                    system_->stop(r.handle, hard);
                    r.handle = -1;
                }
            if (stopping)
                continue;
            AnimEffect r;
            r.actor = a;
            r.name = name;
            r.bone = bone;
            r.end = e.at - e.event->ticks / 1000.0 + e.anim->duration / 1000.0;
            if (auto at = bonePlacement(*a, bone))
                r.handle = system_->start(name, *at, hard);
            if (std::getenv("OS2_DEBUG_FX"))
                std::fprintf(stderr, "opensims2: animation effect %s on %s (%s) at %.2f s%s\n", name.c_str(),
                             bone.c_str(), op.c_str(), e.at, r.handle < 0 ? " not found" : "");
            if (r.handle >= 0)
                animEffects_.push_back(std::move(r));
        }
    }
    for (auto it = animEffects_.begin(); it != animEffects_.end();) {
        AnimEffect& r = *it;
        const bool gone = !present.count(r.actor);
        if (r.handle >= 0 && !gone && seconds < r.end)
            if (auto at = bonePlacement(*r.actor, r.bone))
                system_->move(r.handle, *at);
        if (r.handle >= 0 && (gone || seconds >= r.end)) {
            system_->stop(r.handle, gone);
            r.handle = -1;
        }
        if (r.handle < 0)
            it = animEffects_.erase(it);
        else
            ++it;
    }
}

} // namespace os2
