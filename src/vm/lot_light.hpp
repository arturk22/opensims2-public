#pragma once

// The lot's light maps without a window (round n42, docs/motives-mac.md
// 8.8): the environment motive's light term (AggregateIllumination ->
// LookupFloorLightMap -> ColourToIlluminance, docs/motives-mac.md 8.4) for
// hosts with no renderer (s2pkg, the tests), made as the app's LotLighting
// makes them (app/lot_lighting.hpp): the lot's light-map atlas from
// buildLotModel, the lamps of the objects the app draws (each light of
// their models at its Change Light level, placed where the app's
// VmObjectView draws them), the light maps of the clock's lighting state
// (formats/lot_lightmap, formats/lot_diffusion), and the floor's light
// under a Sim (formats/lot_lamps).
//
// The objects are placed as VmObjectView places them (app/vm_objects.cpp:
// the cObject's saved place while the VM leaves the object there, the slot
// bone of its container, the VM's position), without what only a renderer
// has: objects in a Sim's hands or in the slots of an object the VM
// animates are left out (no lamp is), and the floor height in a pool is
// the pool level's floor. The state changes at once (the app crossfades
// the light maps over the state's transition, 2 s of its clock).

#include "formats/lighting_config.hpp"
#include "formats/lot_lamps.hpp"
#include "formats/lot_lightmap.hpp"
#include "formats/xform.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace os2 {
class DbpfPackage;
struct Lot;
class ResourceIndex;
struct Skeleton;
}

namespace os2::vm {

class VM;
struct Object;

// A window's or door's lights are portals (the outside's light coming in),
// at level 100 (app/lot_lighting.cpp).
bool lampIsPortal(const Object& o);
// A door or arch: OBJD type 8 (the portal builder's +0x3c, docs/lighting-mac.md 11.4).
bool lampIsDoorOrArch(const Object& o);
// The level (0-100) a light of an object shines at: a portal's 100; the
// level Change Light gave it; untouched since the lot loaded, the lamps'
// on/off in semi-attribute 2 (inferred), or `defaultLevel` when >= 0.
int lampLevel(const VM& m, const Object& o, const std::string& light, int defaultLevel = -1);

class HeadlessLotLighting {
public:
    // The lot as the app loads it (`lot` before dropLeftoverCObjects, which
    // this does on its own copy), the game's TSData/Res, and the VM after
    // the lot is loaded (the objects it has now are the ones the app draws
    // from the lot's cObjects; those it creates later are added).
    HeadlessLotLighting(ResourceIndex& index, const Lot& lot, DbpfPackage& lotPackage,
                        const std::filesystem::path& resRoot, VM& machine);
    ~HeadlessLotLighting();
    HeadlessLotLighting(const HeadlessLotLighting&) = delete;
    HeadlessLotLighting& operator=(const HeadlessLotLighting&) = delete;

    // Sets VM::floorIllumination to this (the VM must outlive neither).
    void install();
    // The light under a point (LotLighting::floorIllumination): the lamps
    // and the light maps worked out again first when the VM ticked since.
    std::optional<float> floorIllumination(int vmLevel, float x, float y);
    // Works the lamps and the light maps out for the VM's state now.
    void update();

    const std::vector<LotLamp>& lamps() const { return lamps_; }
    const LotLightMapLayout& layout() const { return layout_; }
    const std::string& stateName() const { return current_; }
    // Test aid (LotLighting::setDefaultLampLevel, the app's --lamps).
    void setDefaultLampLevel(int level) { defaultLevel_ = level; }

private:
    struct Baseline {
        float x = -1, y = -1;
        int level = 0, direction = 0;
        int16_t container = 0, slot = -1;
    };
    struct Drawn {
        std::string cres;
        std::optional<Xform> saved;
        std::vector<Xform> pose; // saved local pose, empty: rest
        std::optional<int16_t> graphic;
        Baseline base;
    };
    const Skeleton* skeleton(const std::string& cres);
    const std::vector<ModelLight>& modelLights(const std::string& cres);
    std::string cresFor(int16_t id);
    std::string slotBone(int16_t container, int slot);
    int16_t modelFrame(int16_t id) const;
    std::optional<Xform> compute(int16_t id, int depth);
    std::vector<Xform> localPose(int16_t id, const Skeleton& s) const;
    float floorHeight(int vmLevel, float x, float y) const;
    void addPlaced(const Lot& lot);
    void updateObjects();

    ResourceIndex& index_;
    VM& vm_;
    std::unique_ptr<Lot> lot_;
    LotLightMapLayout layout_;
    LightingConfig config_;
    LightOverrides overrides_;
    std::map<int16_t, Drawn> drawn_;
    std::map<int16_t, bool> triedCreate_;
    std::map<int16_t, const void*> seenDefinition_;
    std::map<int16_t, std::string> cres_;
    std::map<std::string, std::unique_ptr<Skeleton>> skeletons_;
    std::map<std::string, std::vector<ModelLight>> modelLights_;
    std::map<int16_t, std::optional<Xform>> frame_;
    std::vector<LotLamp> lamps_;
    std::vector<std::pair<std::string, std::shared_ptr<const LotLightMap>>> lightMaps_;
    std::shared_ptr<const LotLightMap> shown_;
    std::string current_;
    int defaultLevel_ = -1;
    std::optional<uint64_t> updatedAt_;
};

} // namespace os2::vm
