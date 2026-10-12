#include "lot_vm.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot_arrivals.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_character.hpp"
#include "vm/animation.hpp"
#include "vm/autonomy.hpp"
#include "vm/hood_bridge.hpp"
#include "vm/lot_neighborhood.hpp"
#include "vm/lot.hpp"
#include "vm/lot_resume.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {
constexpr int16_t kCurrentOutfit = 8; // person data "current outfit" (STR# 0xC8)
}

LotSimulation::LotSimulation(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                             const std::filesystem::path& lotFile, bool fromInit)
    : index_(index), lot_(lot), code_(std::make_unique<vm::CodeLibrary>(&index)),
      vm_(std::make_unique<vm::VM>(*code_)) {
    vm::VM& m = *vm_;
    m.initCreatedObjects = true;
    m.instructionsPerTurn = 20000; // as lot-run
    // The game's tick: 42 ms of its (speed-scaled) clock (docs/ui-live-mac.md §1.2).
    m.clock.ticksPerSecond = kTicksPerSecond;
    m.walkTilesPerTick = float(2.0 / kTicksPerSecond);
    m.setClockGlobals();
    LotObjects saved = LotObjects::load(lotPackage);
    // A family that just moved in: its Sims join the lot (formats/lot_arrivals.hpp).
    const LotArrivals arrivals = addArrivingSims(saved, lot, lotFile, &index_, false);
    // The neighborhood first, as lot-run (lot_neighborhood.hpp).
    const bool hood = vm::loadLotNeighborhood(m, lotFile);
    if (hood && arrivals.family && arrivals.charge) {
        // The move-in charge (LoadLot step 0x1B), into the budget globals.
        const auto f = int16_t(arrivals.family);
        vm::setFamilyFunds(m, f, int32_t(int64_t(vm::familyFunds(m, f)) - arrivals.charge));
        std::fprintf(stderr, "opensims2: family %d moved in: charged %lld\n", f, (long long)arrivals.charge);
    }
    const vm::LotLoad load = vm::loadLot(m, lot, saved, vm::charactersDirOf(lotFile));
    if (hood)
        vm::bindLotSims(m);
    const vm::ResumeResult r = vm::startSavedLot(m, load, saved, fromInit);
    // The family's first day (SetupFamilyFirstDay): the "reset lot"
    // broadcast, whose "Sub - Generate Family" brings the family's Sims.
    if (hood && arrivals.firstDay) {
        const vm::ResetLotResult rl = vm::runResetLot(m, arrivals.arriving);
        for (size_t i = 0; i < rl.sims.size(); ++i)
            if (!rl.simFiles[i].empty())
                arrived_.emplace_back(rl.sims[i], rl.simFiles[i]);
        std::fprintf(stderr, "opensims2: first day: %zu \"reset lot\" trees, %zu Sims arrived\n", rl.trees,
                     rl.sims.size());
        for (const auto& e : rl.errors)
            std::fprintf(stderr, "opensims2: reset lot: %s\n", e.c_str());
    }
    // The lot is ready (cTSGameStateController, message 0x4C8E7905): "CT -
    // Lot Loaded" plays the move-in scene when a family has just moved in.
    if (std::string err; !vm::runLotLoaded(m, &err) && !err.empty())
        std::fprintf(stderr, "opensims2: CT - Lot Loaded: %s\n", err.c_str());
    char buf[256];
    if (fromInit)
        std::snprintf(buf, sizeof buf, "%zu of %zu saved objects in the VM, from their Inits, %zu Mains", load.created,
                      load.objects.size(), r.freshMains);
    else
        std::snprintf(buf, sizeof buf,
                      "%zu of %zu saved objects in the VM, %zu resumed from their saved stacks, %zu fresh Mains, %02d:%02d",
                      load.created, load.objects.size(), r.stacks, r.freshMains, m.clock.hour, m.clock.minute);
    summary_ = buf;
    // OS2_AUTONOMY_LOG=1: what each Find Best Interaction picks (s2pkg autonomy has more).
    if (std::getenv("OS2_AUTONOMY_LOG"))
        vm::autonomyObserver() = [this](const vm::AutonomyDecision& d) {
            std::fprintf(stderr, "autonomy: tick %llu Sim %d: %s\n", (unsigned long long)vm_->ticks, d.sim,
                         d.found ? ("\"" + d.best.front().name + "\" on object " + std::to_string(d.best.front().object)).c_str()
                                 : "nothing");
        };
}

float LotSimulation::floorHeight(int level, float x, float y) const {
    // VM levels count from 1 = the lot's level 0.
    const int l = std::clamp(level - 1, lot_.minLevel, lot_.minLevel + lot_.levelCount - 1);
    return lot_.elevationAt(l, std::clamp(x, 0.0f, float(lot_.width)), std::clamp(y, 0.0f, float(lot_.height)));
}

void LotSimulation::updateGeometry(const Lot& lot) {
    lot_.minLevel = lot.minLevel, lot_.levelCount = lot.levelCount;
    lot_.elevations = lot.elevations;
    lot_.floors = lot.floors;
    lot_.rooms = lot.rooms;
    lot_.walls = lot.walls;
    lot_.pools = lot.pools;
}

void LotSimulation::attach(SimActor& actor, int16_t id, const Character& character, const std::string& idle) {
    vm::Object* o = vm_->object(id);
    if (!o)
        return;
    if (o->person) {
        // Person data 58 "Person Age" and 65 "Gender" from the character
        // file (the lot loader does not read them from the neighbourhood yet).
        int16_t age = vm::lifestage::Adult;
        if (character.age & 1)
            age = vm::lifestage::Toddler;
        else if (character.age & 2)
            age = vm::lifestage::Child;
        else if (character.age & 4)
            age = vm::lifestage::Teen;
        else if (character.age & 16)
            age = vm::lifestage::Elder;
        else if (character.age & 32)
            age = vm::lifestage::Baby;
        if (!o->person->data[vm::persondata::PersonAge])
            o->person->data[vm::persondata::PersonAge] = age;
        o->person->data[65] = character.gender & 1 ? 1 : 0;
    }
    Bound b;
    b.actor = &actor;
    b.id = id;
    b.idle = idle;
    b.motion.sample(*vm_, *o);
    b.zOffset = actor.position[2] - floorHeight(o->position.level, o->position.x, o->position.y);
    if (std::fabs(b.zOffset) > 0.25f)
        std::fprintf(stderr, "opensims2: Sim %d stands %.2f over its floor\n", id, b.zOffset);
    b.playing = idle;
    b.outfit = o->person ? o->person->data[kCurrentOutfit] : 0;
    b.age = o->person ? o->person->data[vm::persondata::PersonAge] : 0;
    b.look = std::make_shared<HeadLook>(actor);
    actor.adjust = [look = b.look](std::vector<Xform>& local, double now) { look->apply(local, now); };
    bound_.push_back(std::move(b));
}

void LotSimulation::walk(int16_t sim, int16_t target, std::optional<uint16_t> animation) {
    vm::startScriptedWalk(*vm_, sim, target, animation);
}

// Overlays the VM plays on a Sim (Animate Overlay, Reach/Put's grasp and
// carry animations: "a2o-newspaper-carry" holds the arm up; the idle
// system's idles: breathing, finger twitches, stretches, looking around),
// each over the bones it keys, above the base animation. Each start of one
// (vm::Overlay::id) is a layer of its own: it is cued when the VM starts it,
// plays once unless the VM loops or holds it, and fades out when the VM
// takes it off (its end, or Animate Stop: the idle trees stop the idles
// before an interaction's animation, the carry poses go with what was
// held). Layers are ordered by the animation's header priority (carry poses
// 5 over idles and gestures 2-3), then by start, as AnimPlayer orders
// masked overlays. The carry poses also play while Animate Sim plays: the
// trees stop them themselves before an animation that needs the arm
// (Animate Stop mode 4, 237 uses: "Eat - Anim - Bite", "Interaction -
// Drink", "Sub - Put Cup Down", ...), and the newspaper's "Read" never
// does: its animations reach the paper by the arms' IK, which AnimPlayer
// solves over the carry pose.
void LotSimulation::playOverlays(Bound& b, const vm::Object& o, double at) {
    constexpr int kOverlayPriority = 100;
    constexpr float kFade = 0.2f;
    constexpr double kRestartGap = 0.25; // seconds
    // Gone from the VM: fade out.
    for (auto it = b.overlays.begin(); it != b.overlays.end();) {
        const bool kept = std::any_of(o.animation.overlays.begin(), o.animation.overlays.end(),
                                      [&](const vm::Overlay& x) { return x.id == it->first; });
        if (kept) {
            ++it;
            continue;
        }
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "tick %llu sim %d overlay off: %s\n", (unsigned long long)vm_->ticks, b.id,
                         it->second.name.c_str());
        b.actor->player.stop(at, kFade, it->second.priority);
        b.stoppedOverlays[it->second.name] = {at, it->second.start};
        it = b.overlays.erase(it);
    }
    // New ones: cue.
    for (const auto& x : o.animation.overlays) {
        if (b.overlays.count(x.id))
            continue;
        Bound::PlayingOverlay p;
        p.name = x.name;
        try {
            const Animation& anim = b.actor->animation(index_, x.name);
            const bool carry = std::find(o.carryOverlays.begin(), o.carryOverlays.end(), x.name) != o.carryOverlays.end();
            const int header = carry ? std::max<int>(anim.priority, 5) : anim.priority;
            p.priority = kOverlayPriority + header * 1000000 + int(x.id % 1000000);
            // Grasps play once (into the carry); what the VM ends or holds
            // loops until it is taken off.
            const bool loop = x.name.find("grasp") == std::string::npos && x.loops != 1;
            // The same looping idle started again right after it went off
            // (the route's waiting tree starts and stops the idle system
            // every (1.25 - distance) * 24 ticks, as the game's data does):
            // it goes on from where it was instead of from frame 0, so the
            // breathing does not start over each time (a visual choice).
            double phase = 0;
            if (const auto st = b.stoppedOverlays.find(x.name);
                loop && st != b.stoppedOverlays.end() && at - st->second.first <= kRestartGap)
                phase = at - st->second.second;
            p.start = at - phase;
            b.actor->cue(at, anim, {.fade = kFade, .loop = loop, .priority = p.priority, .phase = phase});
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "tick %llu sim %d overlay on: %s (kind %d, loops %d)\n",
                             (unsigned long long)vm_->ticks, b.id, x.name.c_str(), x.oeType, x.loops);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: overlay %s\n", b.id, e.what());
        }
        b.overlays.emplace(x.id, std::move(p));
    }
}

// After a tick: what each Sim should play, cued at that tick's time.
void LotSimulation::sampleAll(double at) {
    if (onObjectAnimation)
        for (const auto& [id, o] : vm_->objects()) {
            if (o.person)
                continue;
            uint32_t& seen = objectAnimSerials_[id];
            if (o.animation.overlaySerial == seen)
                continue;
            seen = o.animation.overlaySerial;
            if (!o.animation.overlays.empty())
                onObjectAnimation(id, o.animation.overlays.back().name, at);
        }
    for (auto& b : bound_) {
        const vm::Object* o = vm_->object(b.id);
        if (!o)
            continue;
        b.motion.sample(*vm_, *o);
        playOverlays(b, *o, at);
        const auto& s = b.motion.current();
        if (o->person && o->person->data[vm::persondata::PersonAge] != b.age) {
            // The life stage as ComputeAging maps person data 58 (docs/cas-mac.md §17.2).
            b.age = o->person->data[vm::persondata::PersonAge];
            const int a = b.age;
            const uint32_t bit = a < 2 ? 0x20 : a == 2 ? 0x01 : a <= 15 ? 0x02 : a <= 18 ? 0x04 : a <= 50 ? 0x08 : 0x10;
            if (onAgeChange)
                onAgeChange(b.id, *b.actor, bit);
        }
        if (o->person && o->person->data[kCurrentOutfit] != b.outfit) {
            b.outfit = o->person->data[kCurrentOutfit];
            if (const uint32_t bit = vm::outfitCategoryBit(b.outfit); bit && onOutfitChange)
                onOutfitChange(b.id, *b.actor, bit);
        }
        std::string want = b.idle;
        bool loop = true;
        float fade = 0.2f;
        if (!s.climb.empty()) {
            // Stair phases chain without blending (each starts where the last ended).
            const bool chained = b.playing.find("stairs-") != std::string::npos;
            want = s.climb, loop = false, fade = chained ? 0.0f : 0.1f;
        } else if (s.walking && !s.walk.empty())
            want = s.walk;
        else if (!s.animation.empty())
            want = s.animation, loop = false;
        // OS2_DEBUG_VM=1: each Sim's state after each tick.
        if (std::getenv("OS2_DEBUG_VM")) {
            auto it = vm_->threads.find(b.id);
            std::fprintf(stderr, "tick %llu sim %d at %.2f,%.2f walking %d anim '%s' playing '%s' thread %d %s\n",
                         (unsigned long long)vm_->ticks, b.id, s.x, s.y, s.walking, s.animation.c_str(), b.playing.c_str(),
                         it == vm_->threads.end() ? -1 : int(it->second.status),
                         it == vm_->threads.end() ? "" : it->second.error.substr(0, 200).c_str());
        }
        const bool restart = (!s.climb.empty() && s.climbSerial != b.climbSerial) ||
                             (s.climb.empty() && !s.animation.empty() && s.serial != b.serial);
        b.climbSerial = s.climbSerial;
        if (want == b.playing && !restart)
            continue;
        // Seated (in a chair's or a bed's slot) with nothing to play: the
        // body holds the last pose (the get-in's end, under the chair's
        // idles) instead of standing up in the standing idle between two of
        // them, or while the trees stop the idles before the get-out.
        if (want == b.idle && seat && seat(b.id))
            continue;
        try {
            const Animation& anim = b.actor->animation(index_, want);
            const bool animated = s.climb.empty() && !(s.walking && !s.walk.empty()) && !s.animation.empty();
            // Animate Sim's animations play in their own frame: turned by
            // their header heading (a chair's get-in from its corner).
            // An Animate Sim animation that keys only part of the body (the
            // face's "a-noExpression") plays over what the body does instead
            // of replacing it (the Sim in a chair kept its seat; inferred).
            if (b.partial >= 0)
                b.actor->player.stop(at, fade, b.partial), b.partial = -1;
            const bool partial = animated && !(anim.flags & 1);
            if (partial)
                b.partial = std::max<int>(1, anim.priority);
            const bool mirror = animated ? s.flipped : (!s.climb.empty() && s.turnMirror);
            b.actor->cue(at, anim, {.fade = fade, .loop = loop, .priority = partial ? b.partial : -1,
                                    .mirror = mirror, .turn = animated ? anim.headingOffset : 0.0f});
            if (onSimAnimation)
                onSimAnimation(b.id, *b.actor, s.climb.empty() && !(s.walking && !s.walk.empty()) && !s.animation.empty() ? &anim : nullptr, at);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: %s\n", b.id, e.what());
        }
        b.playing = want;
        b.serial = s.serial;
    }
}

// Look At's target: a Sim's head (its actor's bone), else the object's
// position at 0.75 over its floor, about a table's or a counter's height
// (inferred: the editor's strings name a "default 3/4 height").
std::optional<Vec3> LotSimulation::lookTarget(const vm::Object& o) const {
    if (!o.lookAt || (o.lookAt->ticks > 0 && vm_->ticks >= o.lookAt->since + uint64_t(o.lookAt->ticks)))
        return std::nullopt;
    const vm::Object* x = vm_->object(o.lookAt->target);
    if (!x || x->id == o.id)
        return std::nullopt;
    for (const auto& b : bound_)
        if (b.id == x->id) {
            const int head = b.actor->skin.skeleton.find("head");
            if (head >= 0 && size_t(head) < b.actor->bones.size())
                return b.actor->bones[size_t(head)].t;
            return Vec3{b.actor->position[0], b.actor->position[1], b.actor->position[2] + 1.6f};
        }
    if (!x->position.inWorld())
        return std::nullopt;
    return Vec3{x->position.x, x->position.y, floorHeight(x->position.level, x->position.x, x->position.y) + 0.75f};
}

double LotSimulation::speedScale() const {
    const auto& g = vm_->globals;
    if (g[vm::global::Paused])
        return 0;
    switch (g[vm::global::Speed]) {
    case 3: return speedScales[1];
    case 4: return speedScales[2];
    default: return speedScales[0];
    }
}

double LotSimulation::update(double seconds) {
    // The game speed scales the scene clock the VM and the animations run on.
    scene_ += std::clamp(seconds - caller_, 0.0, 0.25) * speedScale();
    caller_ = seconds;
    // Dialogs time out on real time, also while they pause the game.
    vm_->dialogClockIsTicks = false;
    vm::autoTimeOutDialogs(*vm_, seconds);
    updateScene(scene_);
    return scene_;
}

void LotSimulation::updateScene(double seconds) {
    const double rate = vm_->clock.ticksPerSecond; // ticks per second of the scene clock
    // After a stall (loading, a window drag) the VM does not race to catch
    // up: past 125 ticks behind (the game's limit per frame), its clock
    // slips instead.
    constexpr uint64_t kMaxCatchUp = 125;
    double vmSeconds = seconds - slip_;
    if (vmSeconds * rate > double(vm_->ticks + kMaxCatchUp)) {
        slip_ = seconds - double(vm_->ticks + kMaxCatchUp) / rate;
        vmSeconds = seconds - slip_;
    }
    seconds = vmSeconds;
    // The actors show the moment one tick behind the VM.
    const auto target = uint64_t(std::max(0.0, std::floor(seconds * rate))) + 1;
    // A dialog that blocks the game (Dialog, "engage and block") pauses it:
    // the VM's clock slips while the player has not answered (or the
    // dialog times out on real time: LotSimulation::update).
    if (vm::dialogPausesGame(*vm_)) {
        slip_ = seconds + slip_ - double(vm_->ticks - 1) / rate;
        seconds = double(vm_->ticks - 1) / rate;
    }
    while (vm_->ticks < target && !vm::dialogPausesGame(*vm_)) {
        vm_->tick();
        sampleAll(double(vm_->ticks - 1) / rate + slip_); // in the caller's clock, as the actors play
    }
    const float t = float(std::clamp(seconds * rate - double(vm_->ticks - 1), 0.0, 1.0));
    const float dt = float(std::clamp(seconds - last_, 0.0, 0.1));
    last_ = seconds;
    for (auto& b : bound_) {
        const float x = b.motion.x(t), y = b.motion.y(t);
        const float z = b.motion.z(t, [this](int level, float fx, float fy) { return floorHeight(level, fx, fy); });
        b.actor->position = {x, y, z + b.zOffset};
        b.actor->heading = b.motion.turn(dt);
        if (seat && !b.motion.current().onStep)
            if (const auto at = seat(b.id)) {
                const Vec3 f = at->rotate({0, 1, 0});
                b.actor->position = at->t;
                b.actor->heading = std::atan2(-f[0], f[1]);
                b.motion.setHeading(b.actor->heading);
            }
        b.actor->rootMotion = false;
        if (const vm::Object* o = vm_->object(b.id)) {
            const auto target = lookTarget(*o);
            if (std::getenv("OS2_DEBUG_LOOK") && target.has_value() != b.look->target().has_value())
                std::fprintf(stderr, "tick %llu sim %d %s %d\n", (unsigned long long)vm_->ticks, b.id,
                             target ? "looks at" : "stops looking at", o->lookAt ? o->lookAt->target : 0);
            b.look->setTarget(target);
        }
    }
}

} // namespace os2
