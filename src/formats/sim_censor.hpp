#pragma once

// The censor boxes over a Sim (nTSSG::cPerson::UpdateCensor, Mac BV
// 0x66d318, called by cPerson::OncePerFrameUpdate every frame): person data
// 30 "Censorship Flags" picks the body parts; each part's box is the world
// bounding box of the body geometry bound to its bone, as the Sim is posed
// that frame. The pelvis and, on a woman, the chest make one box; the head,
// hands and feet a box each; "full body" one box around all of them. The
// renderer draws each box's screen rectangle as a mosaic of the scene
// (nTSSG::cDisplayListCensorRenderStrategy; docs/cas-mac.md "Censura").

#include <cstdint>
#include <vector>

#include "scenegraph.hpp"

namespace os2 {

struct SimActor;

namespace censor {
// Person data 30 (TS1's "censorship flags"; UpdateCensor reads it with
// GetPersonData(0x1e)).
constexpr int16_t kPersonData = 30;
enum : uint16_t {
    Pelvis = 0x01,
    SpineIfFemale = 0x02,
    Head = 0x04,
    LeftHand = 0x08,
    RightHand = 0x10,
    LeftFoot = 0x20,
    RightFoot = 0x40,
    FullBody = 0x80,
};
} // namespace censor

struct CensorBox {
    Vec3 lo{0, 0, 0}, hi{0, 0, 0}; // world, Z up
};

// The world bounding box of the vertices of `actor`'s posed body (hair
// left out) whose heaviest bone is `bone`; nothing (lo > hi) if none.
CensorBox boneGeometryBox(const SimActor& actor, const char* bone);

// The boxes UpdateCensor hands the censor render strategy for `flags`
// (person data 30) on `actor` as last posed. A man's pelvis box is widened
// (x and y half extents 0.8 x its size instead of 0.5); a woman's chest box
// (SpineIfFemale) joins her pelvis box. The binary asks the person's vtable
// +0x90 and +0x94; read as cTSPerson::IsMale and IsFemale, declared one
// after the other (0xfa02f4, 0xfa0304): inferred.
std::vector<CensorBox> censorBoxes(const SimActor& actor, uint16_t flags, bool female);

} // namespace os2
