#pragma once

// The lot's objects drawn where the VM says they are (plain C++, driven from
// main.mm): objects the VM moves, turns, takes out of the world, deletes or
// creates, and contained objects (data 2 "container id", 3 "slot number")
// drawn at their container's slot: a bone of a Sim's skeleton (a hand, the
// head's plumbob grip) or a slot node of the container's model.
//
// Slots name their bones in the game's data: each SLOT entry's first
// Sims 2 field is an index into the object's STR# 0x90 ("Model - Bones");
// a containment slot number counts the SLOT's type 0 entries (as Snap Into
// and the slot data owners do). Sims' slots 0 and 1 are r_hand_grip and
// l_hand_grip, 2 plumbbob_grip ("Head Slots Holder"), ... (the person
// template's SLOT). A contained object's origin goes to the slot's bone,
// unless its own model has a node of the same name (the newspaper's
// "r_hand_grip"): then that node goes to the bone (inferred). Objects in a
// slot are saved at (0, 0) in the lot's cObjects, so this is the only place
// they are drawn.

#include "formats/xform.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace os2 {

class LotSimulation;
class Renderer;
class ResourceIndex;
struct SimActor;
struct Skeleton;

class VmObjectView {
public:
    VmObjectView(Renderer& renderer, ResourceIndex& index, LotSimulation& simulation);
    ~VmObjectView();
    VmObjectView(const VmObjectView&) = delete;
    VmObjectView& operator=(const VmObjectView&) = delete;

    // A lot object drawn by the renderer as `instance` of a static model of
    // `cres`; `saved` is its cObject placement, or nothing when it was saved
    // in a slot (then it is hidden until the VM places it). `definition` is
    // the cObject's OBJD file name: the lots keep stale cObjects whose IDs
    // other objects took since, so only a cObject whose VM object of the same
    // ID has that OBJD follows the VM; the others stay where they were saved.
    // Returns whether it follows the VM.
    // `design` is the cObject's design choice (subset -> material), kept
    // under the materials the VM's Change Material sets.
    bool addPlaced(int16_t id, const std::string& definition, const std::string& cres, int instance,
                   std::optional<Xform> saved, const std::map<std::string, std::string>& design = {});
    // A Sim's actor (contained objects follow its bones).
    void addSim(int16_t id, SimActor* actor);
    // An object now drawn by a skinned actor (posed by an animation the VM
    // plays on it) instead of its static instance, which is hidden.
    void setActor(int16_t id, SimActor* actor);
    bool hasModel(int16_t id) const { return drawn_.count(id) != 0; }
    // The materials an object is drawn with (its design choice under what
    // Change Material set), subset -> material.
    std::map<std::string, std::string> materialsOf(int16_t id);
    // Called when the VM changes the materials of an object drawn by an
    // actor; the host rebuilds the actor's model with materialsOf(id).
    std::function<void(int16_t id)> onActorMaterials;
    // The CRES of an object drawn here, empty if none.
    std::string cresOf(int16_t id) const;

    // Places every object for the VM's current state: the Sims' actors must
    // be posed already (their bones), the objects' actors are posed after
    // (their placement is set here). Also draws objects the VM created.
    void update();

    // Keeps an object out of sight whatever the VM does (the host draws it
    // its own way: the plumbob over the selected Sim).
    void setHidden(int16_t id, bool hidden);
    // The object whose slots hold `id`, through containers in containers
    // (itself when it is in no slot).
    int16_t outermostContainer(int16_t id) const;

    // Where an object is drawn (its model's origin), if it is drawn.
    std::optional<Xform> placement(int16_t id);

private:
    struct Baseline {
        float x = -1, y = -1;
        int level = 0, direction = 0;
        int16_t container = 0, slot = -1;
    };
    struct Drawn {
        std::string cres;
        int instance = -1;
        SimActor* actor = nullptr;
        std::optional<Xform> saved;
        std::map<std::string, std::string> design;
        std::map<std::string, std::string> vmMaterials; // Object::materials drawn
        Baseline base;
        bool hidden = false;
        bool debugMoved = false; // OS2_DEBUG_PLACE
        std::string debugMaterials;
    };
    const Skeleton* skeleton(const std::string& cres);
    std::string cresFor(int16_t id); // drawn, else from the object's OBJD
    std::string slotBone(int16_t container, int slot);
    std::optional<Xform> compute(int16_t id, int depth);
    void place(Drawn& d, const std::optional<Xform>& at);
    void addCreated(int16_t id);
    // Redraws an object with the materials the VM set on it (Change Material).
    void updateMaterials(int16_t id, Drawn& d);
    int model(const std::string& cres, const std::map<std::string, std::string>& materials);

    Renderer& renderer_;
    ResourceIndex& index_;
    LotSimulation& sim_;
    std::map<int16_t, Drawn> drawn_;
    std::map<int16_t, SimActor*> sims_;
    std::map<std::string, std::unique_ptr<Skeleton>> skeletons_; // null if it failed to load
    std::map<int16_t, std::string> cres_;          // objects' CRES from their OBJD (cache)
    std::map<std::string, int> models_;            // CRES + materials -> renderer model (-1 failed)
    std::map<std::string, std::vector<std::string>> subsets_; // CRES -> its meshes' subset names
    std::map<int16_t, bool> triedCreate_;
    std::set<int16_t> suppressed_;
    std::map<int16_t, std::optional<Xform>> frame_; // this update's placements
};

} // namespace os2
