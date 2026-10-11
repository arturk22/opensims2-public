#include "app/lot_effects.hpp"

#include "app/ui_panel_util.hpp"
#include "app/vm_objects.hpp"
#include "formats/ui_image.hpp"
#include "formats/strings.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
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
    : renderer_(renderer) {
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
                                                     const SimLookup& sims, vm::VM* vm) const {
    // The effect's slot (TryChangeEffect): type 0 a containment slot (the
    // object's SLOT type-0 entries), type 2 a bone or model node by its
    // index in the object's STR# 0x90 ("Model - Bones": the stove's
    // slot_stove_0, a Sim's hat_grip for slot 4, where balloons go); type 1
    // is not handled (the object's origin). Inferred from the data.
    std::string node;
    const vm::Object* o = vm ? vm->object(id) : nullptr;
    if (view && o) {
        if (slotType == 0 && slot > 0)
            node = view->containerSlotBone(id, slot);
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
                    if (auto at = placementOf(id, e.slotType, e.slot, view, sims, vm)) {
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
                    }
                    if (std::getenv("OS2_DEBUG_FX"))
                        std::fprintf(stderr, "opensims2: effect %s on %d (slot %d/%d, flags 0x%X, icon '%s') at %.2f s, (%.2f %.2f %.2f)%s\n",
                                     e.name.c_str(), id, e.slotType, e.slot, e.flags, e.icon.c_str(), seconds, where[0], where[1], where[2],
                                     r.handle < 0 ? " not shown" : "");
                    it = vm_.emplace(key, r).first;
                    if (e.oneShot && r.handle >= 0)
                        manual_.push_back({"", id, {float(seconds + kOneShotSeconds), 0, 0}, r.handle});
                }
                it->second.seen = true;
                if (it->second.handle >= 0)
                    if (auto at = placementOf(id, e.slotType, e.slot, view, sims, vm))
                        system_->move(it->second.handle, *at);
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
    quads_.clear();
    system_->collect(quads_);
    layer_->setQuads(quads_);
    if (std::getenv("OS2_DEBUG_FX"))
        std::fprintf(stderr, "opensims2: effects at %.2f s: %zu running, %zu particles\n", seconds,
                     system_->instanceCount(), quads_.size());
}

} // namespace os2
