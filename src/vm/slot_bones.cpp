// Routing slots at the bones of an object's model (cTSRoute::
// GetPossibleSlotsFromModelRoutingSlot in the Mac binary, BV 0x475b98).
#include "vm.hpp"
#include "formats/log_once.hpp"

#include "formats/anim.hpp"
#include "formats/anim_player.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace os2::vm {

namespace {

// The object's skeleton (its CRES by the OBJD's model name), read once per
// OBJD; null without one.
const Skeleton* skeletonOf(CodeLibrary& code, const Object& target) {
    constexpr uint32_t kTypeCresId = 0xE519C933, kTypeObjdId = 0x4F424A44;
    static std::map<std::pair<uint32_t, uint16_t>, std::shared_ptr<Skeleton>> skeletons;
    const auto key = std::pair{target.group, target.definitionInstance};
    auto it = skeletons.find(key);
    if (it == skeletons.end()) {
        std::shared_ptr<Skeleton> sk;
        try {
            ResourceIndex& index = *code.index();
            const std::string cres = objectCresName(index, Tgi{kTypeObjdId, target.group, target.definitionInstance, 0});
            if (auto loc = index.findByName(cres + "_cres", kTypeCresId))
                sk = std::make_shared<Skeleton>(Skeleton::fromCres(Rcol(index.read(*loc))));
        } catch (const std::exception& err) {
            os2::logOnce("slot skeleton", std::to_string(target.group) + ":" + std::to_string(target.definitionInstance), err);
        }
        it = skeletons.emplace(key, sk).first;
    }
    return it->second.get();
}

} // namespace

// The binary takes a bone slot's point from the object's scene graph as it
// is posed now (the object's vtable 0x148, cObject::SlotBone): a bone under
// an animated one moves with the animation, and an object keeps the pose
// its last animation left (the lot saves it: nTSSG::cObject's transform
// nodes). Here: the rest pose, the saved or held bones over it, and the
// last Animate Object animation playing on the object at its time (the one
// the app draws), mirrored if it plays flipped.
std::vector<Xform> VM::objectPose(const Object& target) {
    if (!target.definition || !code().index() || target.person)
        return {};
    const Skeleton* sk = skeletonOf(code(), target);
    if (!sk)
        return {};
    std::vector<Xform> pose = sk->restPose();
    for (const auto& [name, x] : target.pose)
        if (const int b = sk->find(name); b >= 0 && size_t(b) < pose.size())
            pose[size_t(b)] = x;
    const auto& ov = target.animation.overlays;
    if (ov.empty())
        return pose;
    const Overlay& o = ov.back();
    const auto anim = animations.animation(o.name);
    if (!anim)
        return pose;
    const Animation::Target* t = anim->findTarget(anim->skeletonTag);
    if (!t && !anim->targets.empty())
        t = &anim->targets[0];
    if (!t)
        return pose;
    const float ticks = std::clamp(o.played, 0.0f, float(anim->duration));
    std::vector<Xform> p = samplePose(*t, *sk, ticks);
    std::vector<bool> keyed = keyedBones(*t, *sk);
    if (o.flipped) {
        const std::vector<int> twins = mirrorBones(*sk);
        p = mirrorPose(*sk, twins, sk->world(sk->restPose()), p);
        std::vector<bool> k(keyed.size());
        for (size_t i = 0; i < k.size() && i < twins.size(); ++i)
            k[i] = keyed[size_t(twins[i])];
        keyed = std::move(k);
    }
    for (size_t i = 0; i < pose.size() && i < p.size() && i < keyed.size(); ++i)
        if (keyed[i])
            pose[i] = p[i];
    return pose;
}

float VM::rootRestY(const Object& target) {
    if (!target.definition || !code().index() || target.person)
        return 0;
    const Skeleton* sk = skeletonOf(code(), target);
    const int root = sk ? sk->find("root_trans") : -1;
    return root < 0 ? 0.0f : sk->restPose()[size_t(root)].t[1];
}

void VM::holdObjectPose(Object& target) {
    if (target.animation.overlays.empty())
        return;
    const Skeleton* sk = target.definition && code().index() && !target.person ? skeletonOf(code(), target) : nullptr;
    if (!sk)
        return;
    const std::vector<Xform> pose = objectPose(target);
    if (pose.size() != sk->bones.size())
        return;
    target.pose.clear();
    for (size_t i = 0; i < pose.size(); ++i)
        target.pose.emplace_back(sk->bones[i].name, pose[i]);
}

// The bones a routing slot names, in the object's model space (tiles; +Y the
// model's forward axis, Z up), with each bone's facing. The Sims 2's SLOT
// entries name the bones by index into the object's STR# 0x90 ("Model -
// Bones") in the first u32 after the Sims 1 fields (Slots::Slot::ts2Extra[0]);
// an entry may list several bones, separated by commas (the sedan's
// "Multislot": "route_vehiclesedan_5_w, route_vehiclesedan_3_w, ..."), and
// the binary makes one goal per bone (the count from the object's vtable
// 0x140, each point from 0x148). A slot with flag 0x20000 (Slots::Slot::
// unknown10) stands at its bones: RoutingSlotOffset leaves such slots out of
// the offset it computes from the slot's own fields.
//
// Facing: the binary gives each goal the object's direction plus the bone's
// (`objDir + (8 - k) mod 8`, k the bone's turn from the object's vtable 0x14c,
// FUN_008b6334), whatever the slot's own facing field says; so a car that
// snaps to its "slot_travel_drive" bone, two tiles ahead, keeps its heading.
// Here k is the bone's rest-pose forward axis (+Y) turned about Z, in
// eighths of a turn counter-clockwise (the game's directions go clockwise);
// inferred from the conversion, not read from the model code.
std::vector<VM::SlotBone> VM::slotBones(const Slots::Slot& slot, const Object& target) {
    constexpr uint16_t kModelBoneNames = 0x90;
    std::vector<SlotBone> out;
    if (!target.definition || !code().index())
        return out;
    const StringTable* names = code().strings(target.group, kModelBoneNames);
    if (!names || slot.ts2Extra[0] >= names->count(lang::EnglishUS))
        return out;
    const Skeleton* sk = skeletonOf(code(), target);
    if (!sk)
        return out;
    const std::string list = names->get(slot.ts2Extra[0], lang::EnglishUS);
    const std::vector<Xform> world = sk->world(objectPose(target));
    size_t start = 0;
    for (int16_t index = 0; start <= list.size(); ++index) {
        size_t end = list.find(',', start);
        if (end == std::string::npos)
            end = list.size();
        std::string bone = list.substr(start, end - start);
        while (!bone.empty() && bone.front() == ' ')
            bone.erase(bone.begin());
        while (!bone.empty() && bone.back() == ' ')
            bone.pop_back();
        start = end + 1;
        if (bone.empty())
            continue;
        const int b = sk->find(bone);
        if (b < 0)
            continue;
        const Xform& x = world[size_t(b)];
        const Vec3 fwd = x.rotate(Vec3{0, 1, 0});
        int turn = 0; // counter-clockwise eighths
        if (std::hypot(fwd[0], fwd[1]) > 1e-3f)
            turn = int(std::lround(std::atan2(-fwd[0], fwd[1]) / (M_PI / 4)));
        out.push_back({x.t[0], x.t[1], x.t[2], (((8 - turn) % 8) + 8) % 8, index});
        if (std::getenv("OS2_DEBUG_SLOTBONES"))
            std::fprintf(stderr, "slot bone %s #%d at (%.2f, %.2f, %.2f) fwd (%.2f, %.2f) facing +%d\n", bone.c_str(), index,
                         x.t[0], x.t[1], x.t[2], fwd[0], fwd[1], out.back().facing);
    }
    return out;
}

std::optional<std::array<float, 3>> VM::slotBone(const Slots::Slot& slot, const Object& target) {
    const auto bones = slotBones(slot, target);
    if (bones.empty())
        return std::nullopt;
    return std::array<float, 3>{bones[0].x, bones[0].y, bones[0].z};
}

} // namespace os2::vm
