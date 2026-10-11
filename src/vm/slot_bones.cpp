// Routing slots at the bones of an object's model (cTSRoute::
// GetPossibleSlotsFromModelRoutingSlot in the Mac binary, BV 0x475b98).
#include "vm.hpp"

#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/skeleton.hpp"

#include <cmath>
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
        } catch (const std::exception&) {
        }
        it = skeletons.emplace(key, sk).first;
    }
    return it->second.get();
}

} // namespace

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
    const std::vector<Xform> world = sk->world(sk->restPose());
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
