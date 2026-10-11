#include "app/lot_effects.hpp"

#include "app/ui_panel_util.hpp"
#include "app/vm_objects.hpp"
#include "formats/ui_image.hpp"
#include "formats/strings.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/model.hpp"
#include "formats/sim_actor.hpp"
#include "render/particle_layer.hpp"
#include "render/renderer.hpp"
#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace os2 {

namespace {

// Sims' balloon slot (Effect Stop/Start slot type 2, slot 4 = hat_grip: the
// binary checks the ShowHeadlines option for it). The balloons' own offsets
// are a few centimetres, so they would sit on the head: lifted a little
// (inferred, by eye).
constexpr float kBalloonLift = 0.2f;
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

} // namespace

LotEffects::LotEffects(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir)
    : renderer_(renderer), index_(index) {
    try {
        thumbnails_ = std::make_unique<DbpfPackage>(resDir / "UserData/Thumbnails/ObjectThumbnails.package");
        for (const auto& e : thumbnails_->entries())
            thumbIndex_.emplace(e.tgi.instance, &e);
    } catch (const std::exception&) {
        thumbnails_.reset(); // balloons keep their default icons
    }
    std::optional<std::vector<uint8_t>> data;
    for (const auto& loc : index.ofType(fx::kTypeEffects)) {
        data = index.read(loc);
        break;
    }
    if (!data)
        throw std::runtime_error("no effects resource (Res/Effects/effects.package)");
    library_ = std::make_unique<fx::EffectsLibrary>(fx::EffectsLibrary::parse(*data));
    system_ = std::make_unique<fx::EffectSystem>(*library_);
    layer_ = std::make_unique<ParticleLayer>(renderer.device(), renderer.colorFormat(), renderer.depthFormat(), &index);
    ParticleLayer* layer = layer_.get();
    renderer.setSceneHook([layer](MTL::RenderCommandEncoder* enc, const simd_float4x4& view,
                                  const simd_float4x4& viewProj) { layer->encode(enc, view, viewProj); });
}

LotEffects::~LotEffects() { renderer_.setSceneHook(nullptr); }

std::string LotEffects::thumbnail(vm::VM& vm, uint32_t guid) {
    if (auto it = thumbNames_.find(guid); it != thumbNames_.end())
        return it->second;
    std::string name;
    // As the live UI keys them (app/ui_panel_util.hpp objectThumbnailInstance).
    if (thumbnails_)
        if (auto def = vm.code().definitionByGuid(guid))
            if (const ObjectDefinition* d = vm.code().definition(def->first, def->second))
                if (const StringTable* names = vm.code().strings(def->first, 0x85)) {
                    const std::string model = names->get(d->modelIndex(), lang::EnglishUS);
                    if (auto e = thumbIndex_.find(objectThumbnailInstance(def->first, model)); e != thumbIndex_.end())
                        if (auto img = decodeUiImage(thumbnails_->read(*e->second))) {
                            char buf[32];
                            std::snprintf(buf, sizeof buf, "thumbnail:%08X", guid);
                            name = buf;
                            layer_->setImage(name, *img);
                        }
                }
    thumbNames_[guid] = name;
    return name;
}

bool LotEffects::addManual(const std::string& name, int16_t object, fx::V3 at, int slot) {
    if (!library_->find(name))
        return false;
    manual_.push_back({name, object, at, -1, slot});
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
            if (sim && slotType == 2)
                p.position[2] += kBalloonLift;
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
                        r.handle = system_->start(e.name, *at);
                        if (r.handle >= 0 && !e.icon.empty())
                            system_->setIcon(r.handle, e.icon, e.iconFrame);
                        // An object's thumbnail in the balloon (flag 0x04: the
                        // object, or the GUID in temps 4/5 with 0x400).
                        uint32_t guid = e.iconGuid;
                        if (!guid && e.iconObject)
                            if (const vm::Object* o = vm->object(e.iconObject))
                                guid = o->guid();
                        if (r.handle >= 0 && guid)
                            if (auto t = thumbnail(*vm, guid); !t.empty())
                                system_->setIcon(r.handle, t);
                        if (r.handle >= 0 && iconImage && (e.iconSim || e.iconId || e.wantSim)) {
                            // Portraits are kept by Sim; want icons change, one texture each time.
                            const std::string t = e.iconSim ? "portrait:" + std::to_string(e.iconSim)
                                                            : "icon:" + std::to_string(++iconSerial_);
                            if (e.iconSim && iconNames_.count(t))
                                system_->setIcon(r.handle, t);
                            else if (auto img = iconImage(e)) {
                                if (std::getenv("OS2_DEBUG_FX"))
                                    std::fprintf(stderr, "opensims2: balloon icon %s (%ux%u)\n", t.c_str(), img->width, img->height);
                                layer_->setImage(t, *img);
                                iconNames_.insert(t);
                                system_->setIcon(r.handle, t);
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
        if (m.handle < 0 || !system_->alive(m.handle))
            m.handle = system_->start(m.name, p);
        else
            system_->move(m.handle, p);
        ++it;
    }

    system_->update(dt);
    placeModels();
    quads_.clear();
    system_->collect(quads_);
    layer_->setQuads(quads_);
    if (std::getenv("OS2_DEBUG_FX"))
        std::fprintf(stderr, "opensims2: effects at %.2f s: %zu running, %zu particles\n", seconds,
                     system_->instanceCount(), quads_.size());
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
        if (auto it = modelInstances_.find(key); it != modelInstances_.end())
            inst = it->second;
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
                r.handle = system_->start(name, *at);
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
