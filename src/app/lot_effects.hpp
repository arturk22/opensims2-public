#pragma once

// The lot's particle effects (plain C++, driven from main.mm): the effects
// the VM's Effect Stop/Start primitives start on objects and Sims
// (vm::ActiveEffect, names from STR# 0x8F), run by fx::EffectSystem at the
// object's placement (Sims' balloon slot: above the head) and drawn by
// ParticleLayer in the renderer's scene hook. --effect <name>[@<object id>
// [:<bone index>] | @x,y,z] starts an effect by hand (test aid). See docs/effects-mac.md.

#include "formats/effects.hpp"
#include "formats/image.hpp"
#include "formats/xform.hpp"
#include "render/particle_system.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

class DbpfPackage;
struct DbpfEntry;
class ParticleLayer;
class Renderer;
class ResourceIndex;
class VmObjectView;
struct SimActor;
struct RgbaImage;
namespace vm {
class VM;
struct ActiveEffect;
}

class LotEffects {
public:
    // Loads the game's effects (Res/Effects/effects.package through `index`)
    // and hooks the particle layer into `renderer`. Throws if they cannot be loaded.
    // `resDir` is TSData/Res (object thumbnails for balloon icons).
    LotEffects(Renderer& renderer, ResourceIndex& index, const std::filesystem::path& resDir);
    ~LotEffects();
    LotEffects(const LotEffects&) = delete;
    LotEffects& operator=(const LotEffects&) = delete;

    // Starts/stops what the VM started/stopped, moves effects with their
    // objects, advances them to `seconds` (the scene clock) and hands the
    // particles to the renderer. `vm`/`view` may be null (no VM running).
    using SimLookup = std::function<const SimActor*(int16_t id)>;
    void update(double seconds, vm::VM* vm, VmObjectView* view, const SimLookup& sims);

    // A hand-started effect at a fixed point, or on an object (followed).
    // Returns false if there is no such effect.
    // `slot` >= 0 attaches it to that node of the object (STR# 0x90 index,
    // as Effect Stop/Start's slot type 2).
    bool addManual(const std::string& name, int16_t object, fx::V3 at, int slot = -1);

    // Effects by hand at a placement (a cinematic's, the animations'):
    // start returns -1 if there is no such effect.
    int start(const std::string& name, const fx::Placement& at) { return system_->start(name, at); }
    void move(int handle, const fx::Placement& at) { system_->move(handle, at); }
    void stop(int handle, bool hard) { system_->stop(handle, hard); }

    // The effect events of these actors' animations (ANIM event type 8:
    // "effectName=...|boneName=...|op=softStart|hardStart|softStop|hardStop")
    // since the last call, up to `seconds` (the actors' clock): started on
    // the bone (the actor's placement if it has no such bone), moved with
    // it, stopped by their stop event or when their animation is over.
    void animationEffects(const std::vector<const SimActor*>& actors, double seconds);

    // Where the host draws an object somewhere else than the VM's view says
    // (the app's own plumbob over the selected Sim, standing in for the
    // game's hidden one): effects on it follow, keeping their node's offset.
    std::function<std::optional<Xform>(int16_t id)> placementOverride;

    // The icon of a balloon the app draws itself (Effect Stop/Start flags
    // 0x10 a Sim's portrait, 0x2000 a want's or fear's icon, 0x1000 by ID):
    // set by the host; empty = the effect's own icon.
    std::function<std::optional<RgbaImage>(const vm::ActiveEffect&)> iconImage;

    const fx::EffectsLibrary& library() const { return *library_; }
    size_t running() const { return system_->instanceCount(); }

private:
    struct Running {
        int handle = -1;
        bool seen = false;
    };
    struct Manual {
        std::string name;
        int16_t object = 0;
        fx::V3 at{};
        int handle = -1;
        int slot = -1;
    };
    std::optional<fx::Placement> placementOf(int16_t id, int8_t slotType, uint8_t slot, VmObjectView* view,
                                             const SimLookup& sims, vm::VM* vm, const std::string& named = {}) const;

    // The texture name of the thumbnail of the object with this GUID
    // (registered with the layer), empty if there is none.
    std::string thumbnail(vm::VM& vm, uint32_t guid);

    Renderer& renderer_;
    ResourceIndex& index_;
    // Effect models (Block::Model) as renderer instances, by (instance, part).
    std::map<std::string, int> effectModels_;            // CRES -> renderer model (-1: failed)
    std::map<std::pair<int, int>, int> modelInstances_;  // (effect handle, part) -> renderer instance
    std::vector<int> freeModelInstances_;                // hidden, by model: reused
    std::map<int, int> instanceModel_;                   // renderer instance -> model
    std::vector<fx::EffectModel> models_;
    void placeModels();
    std::unique_ptr<DbpfPackage> thumbnails_;
    std::map<uint32_t, const DbpfEntry*> thumbIndex_;
    std::map<uint32_t, std::string> thumbNames_;
    int iconSerial_ = 0;
    std::set<std::string> iconNames_;
    std::unique_ptr<fx::EffectsLibrary> library_;
    std::unique_ptr<fx::EffectSystem> system_;
    std::unique_ptr<ParticleLayer> layer_;
    std::map<std::pair<int16_t, uint32_t>, Running> vm_; // (object, effect serial)
    std::vector<Manual> manual_;
    struct AnimEffect {
        const SimActor* actor = nullptr;
        std::string name, bone;
        int handle = -1;
        double end = 0; // when the animation that started it is over
    };
    std::vector<AnimEffect> animEffects_;
    double animScanned_ = -1;
    std::vector<fx::ParticleQuad> quads_;
    double last_ = -1;
};

} // namespace os2
