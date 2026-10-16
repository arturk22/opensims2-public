#include "lot_light.hpp"

#include "dialog.hpp"
#include "lot.hpp"
#include "sim_motion.hpp"
#include "vm.hpp"

#include "formats/dbpf.hpp"
#include "formats/log_once.hpp"
#include "formats/lot.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"
#include "formats/skinning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2::vm {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kObjd = 0x4F424A44;
constexpr uint16_t kModelBoneNames = 0x90; // STR# "Model - Bones"

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool validDirection(int d) { return d >= 0 && d < 8; }

Quat aboutZ(float radians) { return quatFromEulerXYZ(0, 0, radians); }

bool noModel(const std::string& cres) {
    return cres.empty() || cres == "nullModel" || cres == "noModel" || cres == "null" || cres == "placeholder";
}

} // namespace

bool lampIsPortal(const Object& o) {
    const std::string objd = o.definition ? lower(o.definition->fileName) : std::string();
    return objd.starts_with("window") || objd.starts_with("door");
}

bool lampIsDoorOrArch(const Object& o) { return o.definition && o.definition->objectType() == 8; }

int lampLevel(const VM& m, const Object& o, const std::string& light, int defaultLevel) {
    if (lampIsPortal(o))
        return 100; // the outside's light, not a lamp
    const int level = lightLevel(m, o, light);
    if (level >= 0)
        return level;
    // Not changed since the lot loaded: the lamps keep on/off in semi-attribute 2 (inferred).
    return defaultLevel >= 0 ? defaultLevel : o.semiAttributes.size() > 2 && o.semiAttributes[2] ? 100 : 0;
}

HeadlessLotLighting::HeadlessLotLighting(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                                         const std::filesystem::path& resRoot, VM& machine)
    : index_(index), vm_(machine), lot_(std::make_unique<Lot>(lot)) {
    // As the app loads the lot (app/lot_loading.mm): the stale cObjects
    // out, then the lot model, whose light-map atlas does not depend on
    // the view (formats/lot_model.hpp).
    dropLeftoverCObjects(*lot_, lotPackage, index_);
    const PatternCatalog patterns(index_, resRoot);
    (void)buildLotModel(*lot_, index_, patterns, LotViewOptions{}, &layout_);
    config_ = loadLightingConfig(resRoot / "Lights/Lighting.txt");
    overrides_ = loadLightOverrides(resRoot / "Lights/LotLights.txt");
    // The objects the lot already had are drawn from its cObjects
    // (VmObjectView's constructor and addPlaced); only the ones the VM
    // creates from now on are added.
    for (const auto& [id, o] : vm_.objects()) {
        triedCreate_[id] = true;
        seenDefinition_[id] = o.definition;
    }
    addPlaced(*lot_);
}

HeadlessLotLighting::~HeadlessLotLighting() = default;

void HeadlessLotLighting::install() {
    vm_.floorIllumination = [this](int level, float x, float y) { return floorIllumination(level, x, y); };
}

// app/lot_loading.mm's loadLotObjects and VmObjectView::addPlaced: the
// cObjects with a model, each ID the first's (or one in the world), kept
// when the VM object of that ID has the same OBJD.
void HeadlessLotLighting::addPlaced(const Lot& lot) {
    std::map<std::string, Tgi> definitions; // lowercase OBJD file name -> OBJD
    auto cresOf = [&](const Lot::Object& o) {
        if (index_.findByName(o.model + "_cres", kTypeCres))
            return o.model;
        if (definitions.empty())
            for (const auto& loc : index_.ofType(kObjd))
                definitions.emplace(lower(ObjectDefinition::parse(index_.read(loc)).fileName), loc.entry->tgi);
        std::string name = o.definition.ends_with("_objt") ? o.definition.substr(0, o.definition.size() - 5)
                                                           : o.definition;
        auto it = definitions.find(lower(name));
        std::string cres = it == definitions.end() ? std::string() : objectCresName(index_, it->second);
        return cres.empty() ? o.model : cres;
    };
    struct Placed {
        std::string cres, definition;
        std::array<float, 3> position{};
        std::array<float, 4> rotation{};
        bool contained = false;
        std::vector<Xform> pose;
    };
    std::map<int16_t, Placed> placed;
    std::map<std::string, std::optional<Skeleton>> modelSkeletons; // a model's skeleton (with its shapes)
    for (const auto& o : lot.objects) {
        if (noModel(o.model) || o.person)
            continue;
        const bool contained = o.position[0] == 0 && o.position[1] == 0;
        const std::string cres = cresOf(o);
        if (!index_.findByName(cres + "_cres", kTypeCres))
            continue; // a model that does not load is not drawn
        std::vector<Xform> pose;
        if (!o.nodes.empty()) {
            auto sk = modelSkeletons.find(cres);
            if (sk == modelSkeletons.end()) {
                std::optional<Skeleton> s;
                try {
                    if (auto loc = index_.findByName(cres + "_cres", kTypeCres))
                        s = Skeleton::fromCres(Rcol(index_.read(*loc)), true);
                } catch (const std::exception& err) {
                    logOnce("headless light: skeleton", cres, err);
                }
                sk = modelSkeletons.emplace(cres, std::move(s)).first;
            }
            if (sk->second)
                if (auto p = savedPose(*sk->second, o.nodes))
                    pose = std::move(*p);
        }
        const int16_t id = int16_t(o.instance & 0xFFFF);
        if (!placed.count(id) || (placed[id].contained && !contained))
            placed[id] = {cres, o.definition, o.position, o.rotation, contained, std::move(pose)};
    }
    for (auto& [id, p] : placed) {
        const Object* o = vm_.object(id);
        std::string name = p.definition;
        if (name.ends_with("_objt"))
            name.resize(name.size() - 5);
        if (!o || o->person || !o->definition || o->definition->fileName != name)
            continue;
        std::optional<Xform> saved;
        if (!p.contained)
            saved = Xform{Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]},
                          {p.position[0], p.position[1], p.position[2]}};
        if (!saved && multiTileGroup(*o) != id)
            continue;
        if (!saved && !o->data[objdata::ContainerId])
            continue;
        Drawn d;
        d.cres = p.cres;
        d.saved = saved;
        d.pose = std::move(p.pose);
        d.graphic = o->data[0];
        d.base = {o->position.x, o->position.y, o->position.level, o->data[objdata::Direction],
                  o->data[objdata::ContainerId], o->data[objdata::SlotNumber]};
        drawn_[id] = std::move(d);
    }
}

const Skeleton* HeadlessLotLighting::skeleton(const std::string& cres) {
    auto it = skeletons_.find(cres);
    if (it != skeletons_.end())
        return it->second.get();
    std::unique_ptr<Skeleton> s;
    try {
        if (auto loc = index_.findByName(cres + "_cres", kTypeCres))
            s = std::make_unique<Skeleton>(Skeleton::fromCres(Rcol(index_.read(*loc))));
    } catch (const std::exception& e) {
        logOnce("headless light: skeleton", cres, e);
    }
    return skeletons_.emplace(cres, std::move(s)).first->second.get();
}

const std::vector<ModelLight>& HeadlessLotLighting::modelLights(const std::string& cres) {
    auto it = modelLights_.find(cres);
    if (it != modelLights_.end())
        return it->second;
    return modelLights_[cres] = loadModelLights(index_, cres, overrides_);
}

std::string HeadlessLotLighting::cresFor(int16_t id) {
    if (auto it = drawn_.find(id); it != drawn_.end())
        return it->second.cres;
    if (auto it = cres_.find(id); it != cres_.end())
        return it->second;
    std::string cres;
    if (const Object* o = vm_.object(id); o && o->definition && !o->person) {
        try {
            cres = objectCresName(index_, Tgi{kObjd, o->group, o->definitionInstance, 0});
        } catch (const std::exception& err) {
            logOnce("object CRES", std::to_string(o->group) + ":" + std::to_string(o->definitionInstance), err);
        }
    }
    return cres_[id] = cres;
}

// VmObjectView::slotBone: the containment slot's bone, its SLOT's type 0
// entries in order, each naming an entry of the object's STR# 0x90.
std::string HeadlessLotLighting::slotBone(int16_t container, int slot) {
    const Object* c = vm_.object(container);
    if (!c || !c->definition || slot < 0)
        return {};
    const Slots* slots = vm_.code().slots(c->group, c->definition->slotId());
    const StringTable* names = vm_.code().strings(c->group, kModelBoneNames);
    std::string bone;
    if (slots && names) {
        int k = 0;
        for (const auto& s : slots->slots) {
            if (s.type != 0)
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
    return bone;
}

int16_t HeadlessLotLighting::modelFrame(int16_t id) const {
    if (drawn_.count(id))
        return id;
    const Object* o = vm_.object(id);
    if (!o || o->person)
        return id;
    const ObjectId lead = multiTileGroup(*o);
    return lead != id && vm_.object(lead) ? int16_t(lead) : id;
}

std::vector<Xform> HeadlessLotLighting::localPose(int16_t id, const Skeleton& s) const {
    if (auto d = drawn_.find(id); d != drawn_.end() && d->second.pose.size() == s.bones.size())
        return d->second.pose;
    return s.restPose();
}

// LotSimulation::floorHeight: the level's floor (a pool's water surface).
float HeadlessLotLighting::floorHeight(int level, float x, float y) const {
    const Lot& lot = *lot_;
    const float cx = std::clamp(x, 0.0f, float(lot.width)), cy = std::clamp(y, 0.0f, float(lot.height));
    for (const auto& pool : lot.pools)
        if (level - 1 == pool.level - 1 && x >= pool.x && y >= pool.y && x < pool.x + float(pool.width) &&
            y < pool.y + float(pool.height))
            return lot.elevationAt(pool.level, cx, cy) - pool.depth;
    const int l = std::clamp(level - 1, lot.minLevel, lot.minLevel + lot.levelCount - 1);
    return lot.elevationAt(l, cx, cy);
}

// VmObjectView::compute without the actors (see the header).
std::optional<Xform> HeadlessLotLighting::compute(int16_t id, int depth) {
    if (auto it = frame_.find(id); it != frame_.end())
        return it->second;
    std::optional<Xform> out;
    const Object* o = vm_.object(id);
    if (!o || depth > 8 || o->person)
        return frame_[id] = out;
    const int16_t containerId = o->data[objdata::ContainerId];
    const int dir = o->data[objdata::Direction];
    const auto drawn = drawn_.find(id);
    if (drawn != drawn_.end() && drawn->second.saved) {
        const Baseline& b = drawn->second.base;
        const bool same = containerId ? b.container == containerId && b.slot == o->data[objdata::SlotNumber]
                                      : !b.container && b.x == o->position.x && b.y == o->position.y &&
                                            b.level == o->position.level && b.direction == dir;
        if (same)
            return frame_[id] = drawn->second.saved;
    }
    if (containerId && containerId != id) {
        const Object* c = vm_.object(containerId);
        if (!c || c->person)
            return frame_[id] = out; // in a Sim's hands: only the app's actors know where
        const std::string bone = slotBone(containerId, o->data[objdata::SlotNumber]);
        const Skeleton* own = skeleton(cresFor(id));
        Xform attach;
        if (own && !bone.empty())
            if (const int b = own->find(bone); b >= 0)
                attach = own->world(localPose(id, *own))[size_t(b)];
        // VmObjectView::slotFrame, the container's model (its lead's) at rest or as saved.
        const int16_t frameId = modelFrame(containerId);
        const auto at = compute(frameId, depth + 1);
        if (!at)
            return frame_[id] = out;
        Xform node;
        if (const Skeleton* cs = bone.empty() ? nullptr : skeleton(cresFor(frameId)))
            if (const int b = cs->find(bone); b >= 0)
                node = cs->world(localPose(frameId, *cs))[size_t(b)];
        out = at->then(node).then(attach.inverse());
        return frame_[id] = out;
    }
    if (!o->position.inWorld())
        return frame_[id] = out;
    const float z = o->position.hasHeight() ? o->position.height : floorHeight(o->position.level, o->position.x, o->position.y);
    if (drawn != drawn_.end() && drawn->second.saved) {
        const Drawn& d = drawn->second;
        const Baseline& b = d.base;
        if (b.container == 0 && b.x >= 0) {
            const float bz = floorHeight(b.level, b.x, b.y);
            const float turn = validDirection(dir) && validDirection(b.direction)
                                   ? headingOfDirection(dir) - headingOfDirection(b.direction)
                                   : 0.0f;
            const Xform r{aboutZ(turn), {0, 0, 0}};
            const Vec3 off = r.rotate({d.saved->t[0] - b.x, d.saved->t[1] - b.y, d.saved->t[2] - bz});
            out = Xform{r.q * d.saved->q, {o->position.x + off[0], o->position.y + off[1], z + off[2]}};
            return frame_[id] = out;
        }
    }
    out = Xform{aboutZ(validDirection(dir) ? headingOfDirection(dir) : 0.0f), {o->position.x, o->position.y, z}};
    return frame_[id] = out;
}

// VmObjectView::update's bookkeeping: objects whose definition changed are
// forgotten, the ones the VM created are drawn, and a changed graphic
// (object data 0) is drawn with its model (STR# 0x85).
void HeadlessLotLighting::updateObjects() {
    frame_.clear();
    for (const auto& [id, o] : vm_.objects()) {
        const void* def = o.definition;
        if (auto it = seenDefinition_.find(id); it != seenDefinition_.end() && it->second != def) {
            drawn_.erase(id);
            triedCreate_.erase(id);
            cres_.erase(id);
        }
        seenDefinition_[id] = def;
    }
    for (const auto& [id, o] : vm_.objects()) {
        if (o.person || drawn_.count(id) || triedCreate_.count(id) || multiTileGroup(o) != id ||
            !(o.position.inWorld() || o.data[objdata::ContainerId]))
            continue;
        triedCreate_[id] = true;
        const std::string cres = cresFor(id);
        if (noModel(cres) || !index_.findByName(cres + "_cres", kTypeCres))
            continue;
        Drawn d;
        d.cres = cres;
        d.base.x = -2; // never at a saved place
        d.graphic = o.data[0];
        drawn_[id] = std::move(d);
    }
    for (auto& [id, d] : drawn_) {
        const Object* o = vm_.object(id);
        if (!o || o->person)
            continue;
        const int16_t g = o->data[0];
        if (!d.graphic || *d.graphic == g) {
            d.graphic = g;
            continue;
        }
        d.graphic = g;
        const StringTable* names = g > 0 ? vm_.code().strings(o->group, 0x85) : nullptr;
        const std::string cres = names ? names->get(size_t(g), lang::EnglishUS) : std::string();
        if (cres.empty() || cres == d.cres || !index_.findByName(cres + "_cres", kTypeCres))
            continue;
        d.cres = cres;
        d.pose.clear();
    }
}

void HeadlessLotLighting::update() {
    updatedAt_ = vm_.ticks;
    updateObjects();
    current_ = lightingStateOfHour(vm_.clock.hour);
    // LotLighting::update's lamps: every light of every drawn object's model, at its level.
    lamps_.clear();
    for (const auto& [id, d] : drawn_) {
        const Object* o = vm_.object(id);
        if (!o)
            continue;
        const auto& lights = modelLights(d.cres);
        if (lights.empty())
            continue;
        const auto at = compute(id, 0);
        if (!at)
            continue;
        const bool portal = lampIsPortal(*o), doorOrArch = lampIsDoorOrArch(*o);
        for (const auto& l : lights)
            lamps_.push_back(placedLamp(id, l, *at, lampLevel(vm_, *o, l.name, defaultLevel_), o->position.level - 1,
                                        portal, doorOrArch));
    }
    // LotLighting::lightMap: cached by the state and the lamps' levels.
    std::string key;
    const std::vector<LightMapLight> lights = lightMapLights(lamps_, current_, &key);
    for (const auto& [k, map] : lightMaps_)
        if (k == key) {
            shown_ = map;
            return;
        }
    const LightingState* state = config_.state(current_);
    if (!state) {
        shown_.reset();
        return;
    }
    shown_ = std::make_shared<const LotLightMap>(computeLotLightMap(layout_, *state, lights, LightMapGlobals::from(config_)));
    lightMaps_.push_back({key, shown_});
    if (lightMaps_.size() > 4)
        lightMaps_.erase(lightMaps_.begin());
    if (std::getenv("OS2_DEBUG_LIGHTS"))
        std::fprintf(stderr, "headless light: tick %llu state %s, %zu lamps, light maps made again\n",
                     (unsigned long long)vm_.ticks, current_.c_str(), lamps_.size());
}

std::optional<float> HeadlessLotLighting::floorIllumination(int vmLevel, float x, float y) {
    if (!updatedAt_ || *updatedAt_ != vm_.ticks)
        update();
    return floorIlluminationAt(layout_, nullptr, shown_.get(), 1.0f, vmLevel, x, y);
}

} // namespace os2::vm
