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
struct ObjectModel;
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
    // under the materials the VM's Change Material sets. `pose`: the local
    // pose of its model as the lot saved it (formats/skinning.hpp savedPose;
    // empty: the rest pose), which its slots' bones follow and which its
    // static model is drawn in when the view redraws it.
    bool addPlaced(int16_t id, const std::string& definition, const std::string& cres, int instance,
                   std::optional<Xform> saved, const std::map<std::string, std::string>& design = {},
                   std::vector<Xform> pose = {});
    // A Sim's actor (contained objects follow its bones).
    void addSim(int16_t id, SimActor* actor);
    void removeSim(int16_t id) { sims_.erase(id); }
    // An object now drawn by a skinned actor (posed by an animation the VM
    // plays on it) instead of its static instance, which is hidden.
    void setActor(int16_t id, SimActor* actor);
    bool hasModel(int16_t id) const { return drawn_.count(id) != 0; }
    // Whether the last update() drew the object (not hidden: out of the
    // world, on a level the camera hides, on a wall cut down); true for an
    // object not drawn here.
    bool shown(int16_t id) const;
    // The materials an object is drawn with (its design choice under what
    // Change Material set), subset -> material.
    std::map<std::string, std::string> materialsOf(int16_t id);
    // Called when the VM changes the materials of an object drawn by an
    // actor; the host rebuilds the actor's model with materialsOf(id).
    std::function<void(int16_t id)> onActorMaterials;
    // Called on a model loaded with the VM's materials before it is added
    // (a TV channel's material becomes its picture: app/tv_broadcast.hpp).
    std::function<void(ObjectModel& model, const std::map<std::string, std::string>& materials)> adjustModel;
    // The Design tool's choice (subset -> material) replaces the cObject's
    // design choice; the object is drawn again with it.
    void setDesign(int16_t id, const std::map<std::string, std::string>& design);
    // The design choice an object is drawn with (the cObject's, or setDesign's).
    std::map<std::string, std::string> designOf(int16_t id) const;
    // The CRES of an object drawn here, empty if none.
    std::string cresOf(int16_t id) const;

    // Places every object for the VM's current state: the Sims' actors must
    // be posed already (their bones), the objects' actors are posed after
    // (their placement is set here). Also draws objects the VM created.
    void update();

    // Forgets a deleted object (Buy mode deletes ghosts and sold objects):
    // its instance is hidden, and an object that gets its ID later is drawn
    // anew from its own OBJD.
    void forget(int16_t id);

    // Keeps an object out of sight whatever the VM does (the host draws it
    // its own way: the plumbob over the selected Sim).
    void setHidden(int16_t id, bool hidden);
    // Objects whose placement this refuses are not drawn (the levels above
    // the one the camera shows, what hangs on walls cut down: app/wall_view.hpp);
    // null draws all.
    std::function<bool(int16_t id, const Xform&)> visible;
    // The object whose slots hold `id`, through containers in containers
    // (itself when it is in no slot).
    int16_t outermostContainer(int16_t id) const;

    // Every object drawn here: its ID, renderer instance (-1 if none) and
    // actor (null unless the VM animates it).
    void forEachDrawn(const std::function<void(int16_t id, int instance, SimActor* actor)>& fn) const {
        for (const auto& [id, d] : drawn_)
            fn(id, d.instance, d.actor);
    }

    // Where an object is drawn (its model's origin), if it is drawn.
    std::optional<Xform> placement(int16_t id);
    // Where a bone or model node of an object is: a Sim's posed bone, else
    // the node of the object's model (rest pose) at its placement; nothing
    // if the object or the node is missing (effect slots).
    std::optional<Xform> nodePlacement(int16_t id, const std::string& node);
    // Where a Sim in a containment slot of an object (not a Sim) sits: the
    // container's placement with its slot's bone (as its actor poses it at
    // `now`, if the VM animates it and now >= 0; else the rest pose), else
    // its origin; nothing when the Sim is in no such slot.
    std::optional<Xform> seatPlacement(int16_t sim, double now = -1);
    // The bone of an object's containment slot `slot` (empty if none).
    std::string containerSlotBone(int16_t id, int slot) { return slotBone(id, slot); }
    // The bone of the object's slot `slot` among its SLOT entries of `type`
    // (Effect Stop/Start's slot types 0/1 fall back to the type-4 "target" entries).
    std::string typedSlotBone(int16_t id, int slot, int type) { return slotBone(id, slot, type); }

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
        std::vector<Xform> pose; // saved local pose, empty: rest
        std::map<std::string, std::string> vmMaterials; // Object::materials drawn
        std::optional<int16_t> graphic;                 // object data 0 when first drawn, then as changed
        Baseline base;
        bool hidden = false;
        bool debugMoved = false; // OS2_DEBUG_PLACE
        std::string debugMaterials;
    };
    const Skeleton* skeleton(const std::string& cres);
    std::string cresFor(int16_t id); // drawn, else from the object's OBJD
    std::string slotBone(int16_t container, int slot, int type = 0);
    std::optional<Xform> compute(int16_t id, int depth);
    // The object whose model frame holds `id`'s slot bones: the lead tile of
    // a multi-tile object whose part is not drawn itself (its model is the
    // lead's), else `id`.
    int16_t modelFrame(int16_t id) const;
    void place(Drawn& d, const std::optional<Xform>& at);
    void addCreated(int16_t id);
    // Redraws an object with the materials the VM set on it (Change Material).
    void updateMaterials(int16_t id, Drawn& d);
    // The VM's "change graphic" (object data 0 written: the error string 0x48
    // "Change graphic expression called on non 3D object"): the model becomes
    // the object's STR# 0x85 entry of that number (the modular stairs'
    // railings, "Set Model by Type"). Only a change made while the lot is
    // up: the saved lots keep their models in the cObjects (their stairs
    // have data 0 = 0 with railing models).
    void updateGraphic(int16_t id, Drawn& d);
    int model(const std::string& cres, const std::map<std::string, std::string>& materials,
              const std::vector<Xform>& pose = {});
    // The local pose an object's model is drawn in when no animation poses
    // it: the lot's saved pose, else the skeleton's rest pose.
    std::vector<Xform> localPose(int16_t id, const Skeleton& s) const;

    Renderer& renderer_;
    ResourceIndex& index_;
    LotSimulation& sim_;
    std::map<int16_t, Drawn> drawn_;
    std::map<int16_t, SimActor*> sims_;
    std::map<std::string, std::unique_ptr<Skeleton>> skeletons_; // null if it failed to load
    std::map<int16_t, std::string> cres_;          // objects' CRES from their OBJD (cache)
    std::map<std::string, int> models_;            // CRES + materials -> renderer model (-1 failed)
    std::map<std::string, std::vector<std::string>> subsets_; // CRES -> its meshes' subset names
    // The definition each ID had when last seen: the VM reuses the IDs of
    // removed objects (the carpool's car takes a deleted object's ID), and a
    // new object under an old ID is drawn anew.
    std::map<int16_t, const void*> seenDefinition_;
    std::map<int16_t, bool> triedCreate_;
    std::set<int16_t> suppressed_;
    std::map<int16_t, std::optional<Xform>> frame_; // this update's placements
};

} // namespace os2
