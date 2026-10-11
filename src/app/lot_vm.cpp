#include "lot_vm.hpp"

#include "formats/dbpf.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_character.hpp"
#include "vm/animation.hpp"
#include "vm/autonomy.hpp"
#include "vm/hood_bridge.hpp"
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
    m.setClockGlobals();
    const LotObjects saved = LotObjects::load(lotPackage);
    const vm::LotLoad load = vm::loadLot(m, lot, saved, vm::charactersDirOf(lotFile));
    const vm::ResumeResult r = vm::startSavedLot(m, load, saved, fromInit);
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
    bound_.push_back(std::move(b));
}

void LotSimulation::walk(int16_t sim, int16_t target, std::optional<uint16_t> animation) {
    vm::startScriptedWalk(*vm_, sim, target, animation);
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
                onObjectAnimation(id, o.animation.overlays.back(), at);
        }
    for (auto& b : bound_) {
        const vm::Object* o = vm_->object(b.id);
        if (!o)
            continue;
        b.motion.sample(*vm_, *o);
        const auto& s = b.motion.current();
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
        try {
            const Animation& anim = b.actor->animation(index_, want);
            b.actor->cue(at, anim, {.fade = fade, .loop = loop});
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: %s\n", b.id, e.what());
        }
        b.playing = want;
        b.serial = s.serial;
    }
}

void LotSimulation::update(double seconds) {
    const double rate = vm_->clock.ticksPerMinute; // ticks per second at normal speed
    // After a stall (loading, a window drag) the VM does not race to catch
    // up: past a second behind, its clock slips instead.
    constexpr uint64_t kMaxCatchUp = 30;
    double vmSeconds = seconds - slip_;
    if (vmSeconds * rate > double(vm_->ticks + kMaxCatchUp)) {
        slip_ = seconds - double(vm_->ticks + kMaxCatchUp) / rate;
        vmSeconds = seconds - slip_;
    }
    seconds = vmSeconds;
    // The actors show the moment one tick behind the VM.
    const auto target = uint64_t(std::max(0.0, std::floor(seconds * rate))) + 1;
    // A dialog that blocks the game (Dialog, "engage and block") pauses it:
    // the VM's clock slips while the player has not answered.
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
        b.actor->rootMotion = false;
    }
}

} // namespace os2
