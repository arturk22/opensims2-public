#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// The animation queue a lot save keeps in an object's scene graph record
// (cObject 0xFA1C39F7, instance = object ID), after the cObject part:
// nTSSG::cAnimatable::SaveToStream/LoadFromStream (Mac BV 0x68c868 /
// 0x695884, docs/save-mac.md §7c). A Sim's record is cPerson < cLocomotable
// < cAnimatable < cObject (the headers outermost first, each class's data
// after its base's), so the queue sits between the cObject data and the
// cLocomotable/cPerson data.
//
// The cAnimatable part (version 0x0F/0x10 read; the base game writes 0x10):
//   f32 skeleton stretch ; u32 has-node [2 x transform] ;
//   u32 request counter (DAT_02779c34: Animate Sim's request IDs use it) ;
//   [v > 0xB: u32 DAT_02779c30] ; u32 +0x13C ; u32 +0x140 ; f32 +0x144 ;
//   [v < 0x10: u32 +0x14C | u32 +0x148 ; u32 +0x14C] ; transform +0x160 ;
//   u32 n ; n requests (on_save_anim_request, BV 0x695f34):
//     u32 kind: 0 none, 1 an animation (cAnimRequest + cChoreoRequestAnim +
//     the animation's name + its event handler kind), 2 a look-at
//     (cAnimRequest + cChoreoRequestLookAt + target point, u32, f32).
// A transform is 3 f32 + a quaternion (4 f32).
struct SceneAnimRequest {
    uint32_t kind = 0;
    size_t begin = 0, end = 0; // byte range in the record, the kind included
    // nTSSG::cAnimRequest (tag 0x09D0CE69, version 11 in the game's lots;
    // field names from the getters' order in cIAnimRequest's vtable).
    uint32_t version = 0;
    uint32_t id = 0;          // +0x08: Animate Sim's are 0x40000000 | flip | counter << 21 (IsBlockingPrimitive)
    std::string name;         // +0x0C, the animation without "_anim"
    std::string channel;      // +0x18, the channel's debug name: "sil"/"sim"/"sih"/"siu" for Animate Sim
    // +0x24: 5 on Animate Sim's, 4 or 6 on overlays (not decoded); +0x28 the
    // tOEType (2 normal, 4 idle, 7 facial: vm::oetype's numbers).
    uint32_t arType = 0, oeType = 0;
    uint16_t stackObject = 0; // +0x34
    uint16_t eventTree = 0;   // +0x36
    bool interruptible = false;
    float hurrySpeed = 1, beginAction = -1, endAction = -1;
    bool blendIn = true, blendOut = true; // +0x48/+0x49 transition blends enabled
    uint32_t actualEndAction = 0x7FFFFF00;
    uint32_t target = 0;      // +0x50: the IK object (Animate Sim's event tree parameter 1)
    uint32_t slot = 0xFFFFFFFF;
    bool sameRoom = true;
    uint32_t objectGuid = 0;  // +0x78: the running tree's object's GUID
    uint32_t state = 0, movedObject = 0;
    std::string objectAnim;
    uint32_t transformParent = 0xFFFFFFFF, transformParent2 = 0, container = 0;
    // cChoreoRequest (tag 0x3AFF30DB, version 7): times are the engine's
    // clock in ms.
    uint32_t choreoVersion = 0; // of the cChoreoRequestAnim (1 in the base game's lots, 2 in the BV's)
    uint32_t status = 0;
    int32_t start = 0, current = 0;
    float portion = 0;        // of the animation done
    uint32_t expiration = 0, priority = 0;
    float weight = 1;
    bool mirrored = false;
    uint32_t loops = 1;
    float speed = 1, heading = 0;
    float played = 0;         // +0xBC "current data source param": how far the animation has played (ms)
    std::string animation;    // the resource ("a-blockedPath-long_anim"), kind 1
    // Animate Sim's request: an animation of a blocking primitive.
    bool blocking() const { return kind == 1 && ((id & 0xE0000000u) == 0x40000000u || (id & 0xE0000000u) == 0xA0000000u); }
};

struct SceneAnimatable {
    uint32_t version = 0;
    size_t begin = 0;         // where the cAnimatable data starts (after the cObject part)
    size_t counterAt = 0;     // the request counter's offset
    uint32_t counter = 0;
    size_t requestsAt = 0;    // the request count's offset
    size_t end = 0;           // after the last request (the subclasses' data follows)
    std::vector<SceneAnimRequest> requests;
};

// The channel names AddChoreoAnimRequest gives a priority level
// (docs/primitives-mac.md §4): Animate Sim "sil" "sim" "sih" "siu" (0, 2,
// 4, 6), Animate Overlay "osl" (0) "ovl" "ovm" "ovh" "ovu" (1, 3, 5, 7),
// Animate Object "obj" (2).
std::string animChannelName(int level, bool sim);
int animChannelLevel(const std::string& channel);

// The animation queue of a scene graph record, or nothing if the record has
// no cAnimatable part or does not read through.
std::optional<SceneAnimatable> readSceneAnimatable(std::span<const uint8_t> record);

// What an animation request is written from (cAnimatable::
// AddChoreoAnimRequest; Animate Sim's as try_animate_sim BV 0x69d634 fills
// it, Animate Overlay's and Animate Object's alike).
struct AnimSimRequest {
    uint32_t id = 0;
    std::string name;   // without "_anim"
    std::string channel; // "sim", "ovl", "obj", ...
    uint32_t arType = 5, oeType = 2;
    uint32_t loops = 1;
    int level = 0;      // the channel priority (Animate Sim 0, 2, 4, 6; overlays 0-7; objects 2)
    uint16_t stackObject = 0, eventTree = 0;
    bool interruptible = false, blendIn = true, blendOut = true;
    uint32_t target = 0, objectGuid = 0;
    // +0x80 (MovedObjectID; 0 in every request of the game's lots): the VM
    // keeps there the Sim whose tree started an overlay when that is not
    // the animated object (its event tree runs as that Sim).
    uint32_t movedObject = 0;
    int32_t current = 0; // the clock (ms)
    float length = 0;    // ms
    float played = 0;    // ms of the animation played
    float speed = 1, weight = 1, heading = 0;
    bool mirrored = false;
};
// The bytes of one request of kind 1 (the kind included) as the base game
// writes one: cAnimRequest version 11, cChoreoRequestAnim version 1 with
// its weight blend (in, hold, out).
std::vector<uint8_t> writeAnimSimRequest(const AnimSimRequest& r);

// The record with its requests replaced by `requests` (each the bytes of a
// whole request, its kind included) and the request counter set.
std::vector<uint8_t> withSceneRequests(std::span<const uint8_t> record, const SceneAnimatable& a,
                                       const std::vector<std::vector<uint8_t>>& requests, uint32_t counter);

} // namespace os2
