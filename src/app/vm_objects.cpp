#include "vm_objects.hpp"

#include "lot_vm.hpp"

#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"
#include "formats/skeleton.hpp"
#include "render/renderer.hpp"
#include "vm/sim_motion.hpp"
#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kTypeObjd = 0x4F424A44;
constexpr uint16_t kModelBoneNames = 0x90; // STR# "Model - Bones" / "Model Bone Names"

simd_float4x4 matrixOf(const Xform& x) {
    simd_float4x4 m = simd_matrix4x4(simd_quaternion(x.q.x, x.q.y, x.q.z, x.q.w));
    m.columns[3] = simd_make_float4(x.t[0], x.t[1], x.t[2], 1);
    return m;
}

simd_float4x4 hiddenMatrix() {
    simd_float4x4 m = matrix_identity_float4x4;
    m.columns[3] = simd_make_float4(0, 0, -1000, 1);
    return m;
}

Quat aboutZ(float radians) { return quatFromEulerXYZ(0, 0, radians); }

bool validDirection(int d) { return d >= 0 && d < 8; }

} // namespace

VmObjectView::VmObjectView(Renderer& renderer, ResourceIndex& index, LotSimulation& simulation)
    : renderer_(renderer), index_(index), sim_(simulation) {
    // Objects the lot already had are drawn from its cObjects (addPlaced);
    // only the ones the VM creates from now on are added by update().
    for (const auto& [id, o] : simulation.machine().objects()) {
        triedCreate_[id] = true;
        seenDefinition_[id] = o.definition;
    }
}

VmObjectView::~VmObjectView() = default;

bool VmObjectView::addPlaced(int16_t id, const std::string& definition, const std::string& cres, int instance,
                             std::optional<Xform> saved, const std::map<std::string, std::string>& design) {
    const vm::Object* o = sim_.machine().object(id);
    std::string name = definition;
    if (name.ends_with("_objt"))
        name.resize(name.size() - 5);
    if (!o || o->person || !o->definition || o->definition->fileName != name || drawn_.count(id))
        return false;
    // The parts of a multi-tile object other than its lead are saved at
    // (0, 0) with the lead's model, which draws the whole object.
    if (!saved && vm::multiTileGroup(*o) != id)
        return false;
    // Saved at (0, 0) and in no slot: out of the world (inventories,
    // controllers), whatever position the VM's state gives it.
    if (!saved && !o->data[vm::objdata::ContainerId])
        return false;
    Drawn d;
    d.cres = cres;
    d.instance = instance;
    d.saved = saved;
    d.design = design;
    d.base = {o->position.x, o->position.y, o->position.level, o->data[vm::objdata::Direction],
              o->data[vm::objdata::ContainerId], o->data[vm::objdata::SlotNumber]};
    drawn_[id] = std::move(d);
    return true;
}

void VmObjectView::addSim(int16_t id, SimActor* actor) { sims_[id] = actor; }

void VmObjectView::setHidden(int16_t id, bool hidden) {
    if (hidden)
        suppressed_.insert(id);
    else
        suppressed_.erase(id);
}

int16_t VmObjectView::outermostContainer(int16_t id) const {
    const vm::VM& m = sim_.machine();
    const vm::Object* o = m.objects().count(id) ? &m.objects().at(id) : nullptr;
    for (int depth = 0; o && depth < 8; ++depth) {
        const int16_t c = o->data[vm::objdata::ContainerId];
        if (!c || c == o->id || !m.objects().count(c))
            return o->id;
        o = &m.objects().at(c);
    }
    return o ? o->id : 0;
}

void VmObjectView::setActor(int16_t id, SimActor* actor) {
    auto it = drawn_.find(id);
    if (it == drawn_.end())
        return;
    it->second.actor = actor;
    if (it->second.instance >= 0)
        renderer_.setInstanceTransform(it->second.instance, hiddenMatrix());
}

void VmObjectView::setDesign(int16_t id, const std::map<std::string, std::string>& design) {
    auto it = drawn_.find(id);
    if (it == drawn_.end())
        return;
    Drawn& d = it->second;
    d.design = design;
    if (d.actor) {
        if (onActorMaterials)
            onActorMaterials(id);
        return;
    }
    if (d.instance < 0)
        return;
    const int m = model(d.cres, materialsOf(id));
    if (m >= 0)
        renderer_.setInstanceModel(d.instance, m);
}

std::map<std::string, std::string> VmObjectView::designOf(int16_t id) const {
    auto it = drawn_.find(id);
    return it == drawn_.end() ? std::map<std::string, std::string>{} : it->second.design;
}

std::string VmObjectView::cresOf(int16_t id) const {
    auto it = drawn_.find(id);
    return it == drawn_.end() ? std::string() : it->second.cres;
}

const Skeleton* VmObjectView::skeleton(const std::string& cres) {
    auto it = skeletons_.find(cres);
    if (it != skeletons_.end())
        return it->second.get();
    std::unique_ptr<Skeleton> s;
    try {
        if (auto loc = index_.findByName(cres + "_cres", kTypeCres)) {
            const auto bytes = index_.read(*loc);
            s = std::make_unique<Skeleton>(Skeleton::fromCres(Rcol(bytes)));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s: %s\n", cres.c_str(), e.what());
    }
    return skeletons_.emplace(cres, std::move(s)).first->second.get();
}

std::string VmObjectView::cresFor(int16_t id) {
    if (auto it = drawn_.find(id); it != drawn_.end())
        return it->second.cres;
    if (auto it = cres_.find(id); it != cres_.end())
        return it->second;
    std::string cres;
    if (const vm::Object* o = sim_.machine().object(id); o && o->definition && !o->person) {
        try {
            cres = objectCresName(index_, Tgi{kTypeObjd, o->group, o->definitionInstance, 0});
        } catch (const std::exception&) {
        }
    }
    return cres_[id] = cres;
}

// The bone of containment slot `slot` of `container`: its SLOT's type 0
// entries in order, each naming an entry of the object's STR# 0x90.
std::string VmObjectView::slotBone(int16_t container, int slot, int type) {
    vm::VM& m = sim_.machine();
    const vm::Object* c = m.object(container);
    if (!c || !c->definition || slot < 0)
        return {};
    const Slots* slots = m.code().slots(c->group, c->definition->slotId());
    const StringTable* names = m.code().strings(c->group, kModelBoneNames);
    std::string bone;
    if (slots && names) {
        int k = 0;
        for (const auto& s : slots->slots) {
            if (s.type != type)
                continue;
            if (k++ != slot)
                continue;
            const uint32_t i = s.ts2Extra[0];
            if (i < names->count(lang::EnglishUS))
                bone = names->get(i, lang::EnglishUS);
            break;
        }
    }
    if (const size_t comma = bone.find(','); comma != std::string::npos)
        bone.resize(comma);
    // Sims without a SLOT of their own still hold things in their hands.
    if (bone.empty() && c->person && slot < 2 && type == 0)
        bone = slot == 0 ? "r_hand_grip" : "l_hand_grip";
    return bone;
}

std::optional<Xform> VmObjectView::placement(int16_t id) {
    if (auto it = frame_.find(id); it != frame_.end())
        return it->second;
    return compute(id, 0);
}

bool VmObjectView::shown(int16_t id) const {
    const auto it = drawn_.find(id);
    if (it == drawn_.end())
        return true;
    if (it->second.actor)
        return it->second.actor->transform && it->second.actor->transform->t[2] > -100;
    return !it->second.hidden;
}

std::optional<Xform> VmObjectView::seatPlacement(int16_t sim) {
    vm::VM& m = sim_.machine();
    const vm::Object* o = m.object(sim);
    if (!o)
        return std::nullopt;
    const int16_t containerId = o->data[vm::objdata::ContainerId];
    const vm::Object* c = containerId && containerId != sim ? m.object(containerId) : nullptr;
    if (!c || c->person)
        return std::nullopt;
    const auto at = compute(containerId, 1);
    if (!at)
        return std::nullopt;
    const std::string bone = slotBone(containerId, o->data[vm::objdata::SlotNumber]);
    // A container the VM animates (a chair pulled out or scooted in) moves
    // its slot with it: the slot bone as posed.
    if (auto d = drawn_.find(containerId); d != drawn_.end() && d->second.actor && !bone.empty()) {
        const SimActor& a = *d->second.actor;
        const int b = a.skin.skeleton.find(bone);
        if (b >= 0 && size_t(b) < a.bones.size() && a.transform && a.transform->t[2] > -100)
            return a.bones[size_t(b)];
    }
    Xform node;
    if (const Skeleton* cs = bone.empty() ? nullptr : skeleton(cresFor(containerId)))
        if (const int b = cs->find(bone); b >= 0)
            node = cs->world(cs->restPose())[size_t(b)];
    if (std::getenv("OS2_DEBUG_SEAT")) {
        const Xform w = at->then(node);
        std::fprintf(stderr, "seat: Sim %d in %d slot %d bone %s local t(%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f) -> t(%.2f %.2f %.2f)\n",
                     sim, containerId, o->data[vm::objdata::SlotNumber], bone.c_str(), node.t[0], node.t[1], node.t[2],
                     node.q.x, node.q.y, node.q.z, node.q.w, w.t[0], w.t[1], w.t[2]);
    }
    return at->then(node);
}

std::optional<Xform> VmObjectView::nodePlacement(int16_t id, const std::string& node) {
    if (node.empty())
        return std::nullopt;
    if (auto it = sims_.find(id); it != sims_.end() && it->second) {
        const SimActor& a = *it->second;
        const int b = a.skin.skeleton.find(node);
        if (b < 0 || size_t(b) >= a.bones.size())
            return std::nullopt;
        return a.bones[size_t(b)];
    }
    const auto at = placement(id);
    const Skeleton* s = at ? skeleton(cresFor(id)) : nullptr;
    const int b = s ? s->find(node) : -1;
    if (b < 0)
        return std::nullopt;
    return at->then(s->world(s->restPose())[size_t(b)]);
}

std::optional<Xform> VmObjectView::compute(int16_t id, int depth) {
    if (auto it = frame_.find(id); it != frame_.end())
        return it->second;
    std::optional<Xform> out;
    vm::VM& m = sim_.machine();
    const vm::Object* o = m.object(id);
    if (!o || depth > 8)
        return frame_[id] = out;
    if (o->person) {
        // A Sim: its actor's placement (where the renderer draws it).
        if (auto it = sims_.find(id); it != sims_.end() && it->second)
            out = it->second->placement();
        return frame_[id] = out;
    }
    const int16_t containerId = o->data[vm::objdata::ContainerId];
    const int dir = o->data[vm::objdata::Direction];
    const auto drawn = drawn_.find(id);
    // As long as the VM leaves an object where the lot saved it, it is drawn
    // where its cObject says (the VM's positions are tile centres; some
    // saved objects are out of the VM's world, multi-tile parts sit in their
    // lead's slot, ...).
    if (drawn != drawn_.end() && drawn->second.saved) {
        // (Contained objects move with their container each tick: only the
        // slot counts for them.)
        const Baseline& b = drawn->second.base;
        const bool same = containerId ? b.container == containerId && b.slot == o->data[vm::objdata::SlotNumber]
                                      : !b.container && b.x == o->position.x && b.y == o->position.y &&
                                            b.level == o->position.level && b.direction == dir;
        if (same)
            return frame_[id] = drawn->second.saved;
    }
    if (containerId && containerId != id) {
        const int16_t slot = o->data[vm::objdata::SlotNumber];
        const vm::Object* c = m.object(containerId);
        if (!c)
            return frame_[id] = out;
        const std::string bone = slotBone(containerId, slot);
        const Skeleton* own = skeleton(cresFor(id));
        Xform attach; // the object's node that goes to the bone (its origin by default)
        if (own && !bone.empty()) {
            const int b = own->find(bone);
            if (b >= 0)
                attach = own->world(own->restPose())[size_t(b)];
        }
        if (c->person) {
            auto it = sims_.find(containerId);
            if (it == sims_.end() || !it->second)
                return frame_[id] = out;
            const SimActor& a = *it->second;
            const int b = bone.empty() ? -1 : a.skin.skeleton.find(bone);
            if (b < 0 || size_t(b) >= a.bones.size())
                out = a.placement(); // no bone: at the Sim's feet
            else
                out = a.bones[size_t(b)].then(attach.inverse());
            if (std::getenv("OS2_DEBUG_SLOT") && b >= 0) {
                const Xform& w = a.bones[size_t(b)];
                std::fprintf(stderr, "slot: object %d in Sim %d bone %s q(%.2f %.2f %.2f %.2f) t(%.2f %.2f %.2f)\n", id,
                             containerId, bone.c_str(), w.q.x, w.q.y, w.q.z, w.q.w, w.t[0], w.t[1], w.t[2]);
            }
        } else {
            const auto at = compute(containerId, depth + 1);
            if (!at)
                return frame_[id] = out;
            Xform node;
            if (const Skeleton* cs = bone.empty() ? nullptr : skeleton(cresFor(containerId)))
                if (const int b = cs->find(bone); b >= 0)
                    node = cs->world(cs->restPose())[size_t(b)];
            out = at->then(node).then(attach.inverse());
        }
        return frame_[id] = out;
    }
    if (!o->position.inWorld())
        return frame_[id] = out;
    const float z = sim_.floorHeight(o->position.level, o->position.x, o->position.y);
    if (drawn != drawn_.end() && drawn->second.saved) {
        const Drawn& d = drawn->second;
        const Baseline& b = d.base;
        if (b.container == 0 && b.x >= 0) {
            // Moved from where it was saved: the same offset from its VM
            // position (multi-tile objects, wall objects), turned by the change of direction.
            const float bz = sim_.floorHeight(b.level, b.x, b.y);
            const float turn = validDirection(dir) && validDirection(b.direction)
                                   ? vm::headingOfDirection(dir) - vm::headingOfDirection(b.direction)
                                   : 0.0f;
            const Xform r{aboutZ(turn), {0, 0, 0}};
            const Vec3 off = r.rotate({d.saved->t[0] - b.x, d.saved->t[1] - b.y, d.saved->t[2] - bz});
            out = Xform{r.q * d.saved->q, {o->position.x + off[0], o->position.y + off[1], z + off[2]}};
            return frame_[id] = out;
        }
    }
    out = Xform{aboutZ(validDirection(dir) ? vm::headingOfDirection(dir) : 0.0f), {o->position.x, o->position.y, z}};
    return frame_[id] = out;
}

void VmObjectView::place(Drawn& d, const std::optional<Xform>& at) {
    if (d.actor) {
        if (at)
            d.actor->transform = *at;
        else
            d.actor->transform = Xform{{}, {0, 0, -1000}};
        return;
    }
    if (d.instance < 0)
        return;
    if (!at) {
        if (!d.hidden)
            renderer_.setInstanceTransform(d.instance, hiddenMatrix());
        d.hidden = true;
        return;
    }
    renderer_.setInstanceTransform(d.instance, matrixOf(*at));
    d.hidden = false;
}

int VmObjectView::model(const std::string& cres, const std::map<std::string, std::string>& materials) {
    std::string key = cres;
    for (const auto& [subset, material] : materials)
        key += "|" + subset + "=" + material;
    auto it = models_.find(key);
    if (it != models_.end())
        return it->second;
    int id = -1;
    try {
        ObjectModel m = loadObjectModel(index_, cres, materials);
        if (adjustModel)
            adjustModel(m, materials);
        auto& names = subsets_[cres];
        names.clear();
        for (const auto& p : m.parts)
            names.push_back(p.mesh.name);
        id = renderer_.addObject(m, index_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s: %s\n", cres.c_str(), e.what());
    }
    return models_[key] = id;
}

// Change Material names a mesh group (a subset of the model's meshes; ""
// for all of them) and a material; the cObject's design choice stays under
// it. The instance switches to a model loaded with those materials.
std::map<std::string, std::string> VmObjectView::materialsOf(int16_t id) {
    auto it = drawn_.find(id);
    if (it == drawn_.end())
        return {};
    Drawn& d = it->second;
    if (!subsets_.count(d.cres))
        model(d.cres, d.design); // learns the subset names
    std::map<std::string, std::string> materials = d.design;
    for (const auto& [group, material] : d.vmMaterials) {
        if (group.empty()) {
            for (const auto& subset : subsets_[d.cres])
                materials[subset] = material;
        } else {
            materials[group] = material;
        }
    }
    return materials;
}

void VmObjectView::updateMaterials(int16_t id, Drawn& d) {
    const vm::Object* o = sim_.machine().object(id);
    if (!o || o->materials == d.vmMaterials)
        return;
    d.vmMaterials = o->materials;
    if (std::getenv("OS2_DEBUG_VM"))
        std::fprintf(stderr, "opensims2: object %d (%s) redrawn with the VM's materials\n", id, d.cres.c_str());
    if (d.actor) {
        if (onActorMaterials)
            onActorMaterials(id);
        return;
    }
    if (d.instance < 0)
        return;
    const int m = model(d.cres, materialsOf(id));
    if (m >= 0)
        renderer_.setInstanceModel(d.instance, m);
}

// An object the VM created (or one the lot did not draw) that has a model:
// a static instance of it, placed from now on.
void VmObjectView::addCreated(int16_t id) {
    triedCreate_[id] = true;
    const std::string cres = cresFor(id);
    // "placeholder" (a yellow box with a red "+Y" arrow) is the model of
    // controllers (Controller - Look At, Wants - Controller): never seen in
    // the game (inferred; the VM moves the Look At controller onto the lot).
    if (cres.empty() || cres == "nullModel" || cres == "noModel" || cres == "placeholder")
        return;
    const int m = model(cres, {});
    if (m < 0)
        return;
    Drawn d;
    d.cres = cres;
    d.instance = renderer_.addInstance(m, hiddenMatrix());
    d.hidden = true;
    d.base.x = -2; // never at a saved place
    drawn_[id] = std::move(d);
    if (std::getenv("OS2_DEBUG_VM"))
        std::fprintf(stderr, "opensims2: drawing created object %d (%s)\n", id, cres.c_str());
}

void VmObjectView::forget(int16_t id) {
    if (auto it = drawn_.find(id); it != drawn_.end()) {
        if (it->second.instance >= 0)
            renderer_.setInstanceTransform(it->second.instance, hiddenMatrix());
        drawn_.erase(it);
    }
    triedCreate_.erase(id);
    cres_.erase(id);
}

void VmObjectView::update() {
    frame_.clear();
    vm::VM& m = sim_.machine();
    for (const auto& [id, o] : m.objects()) {
        const void* def = o.definition;
        if (auto it = seenDefinition_.find(id); it != seenDefinition_.end() && it->second != def) {
            if (auto d = drawn_.find(id); d == drawn_.end() || !d->second.actor)
                forget(id);
        }
        seenDefinition_[id] = def;
    }
    for (const auto& [id, o] : m.objects())
        if (!o.person && !drawn_.count(id) && !triedCreate_.count(id) && vm::multiTileGroup(o) == id &&
            (o.position.inWorld() || o.data[vm::objdata::ContainerId]))
            addCreated(id);
    static const bool debug = std::getenv("OS2_DEBUG_PLACE") != nullptr;
    for (auto& [id, d] : drawn_) {
        const auto at = compute(id, 0);
        if (debug) {
            const bool moved = at && (!d.saved || std::fabs(at->t[0] - d.saved->t[0]) + std::fabs(at->t[1] - d.saved->t[1]) +
                                                          std::fabs(at->t[2] - d.saved->t[2]) > 0.01f);
            const vm::Object* o = m.object(id);
            const std::string mats = [&] {
                std::string out;
                if (o)
                    for (const auto& [g, mat] : o->materials)
                        out += " " + g + "=" + mat;
                return out;
            }();
            if (mats != d.debugMaterials)
                std::fprintf(stderr, "opensims2: tick %llu object %d materials%s\n", (unsigned long long)m.ticks, id,
                             mats.c_str());
            d.debugMaterials = mats;
            if (moved != d.debugMoved || (!at) != d.hidden)
                std::fprintf(stderr, "opensims2: tick %llu object %d (%s) %s (%.2f, %.2f, %.2f)\n",
                             (unsigned long long)m.ticks, id, d.cres.c_str(),
                             !at ? "hidden" : moved ? "placed by the VM at" : "back at its saved place",
                             at ? at->t[0] : 0, at ? at->t[1] : 0, at ? at->t[2] : 0);
            d.debugMoved = moved;
        }
        updateMaterials(id, d);
        place(d, suppressed_.count(id) || (at && visible && !visible(id, *at)) ? std::nullopt : at);
    }
}

} // namespace os2
