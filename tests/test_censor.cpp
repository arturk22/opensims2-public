// The censor (cPerson::UpdateCensor, cDisplayListCensorRenderStrategy): the
// boxes a nude Sim gets for its person data 30 flags, from the body
// geometry bound to each bone, and their screen rectangles.
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_censor.hpp"
#include "formats/sim_character.hpp"
#include "render/renderer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A skeleton of the censor's bones and one body part with two vertices per
// bone (a unit-ish box around a point), plus a hair part on the head.
static std::unique_ptr<SimActor> makeActor() {
    Skeleton s;
    const char* names[] = {"root_trans", "pelvis", "spine1", "head", "l_hand", "r_hand", "l_foot", "r_foot"};
    for (int i = 0; i < 8; ++i) {
        Skeleton::Bone b;
        b.name = names[i];
        b.parent = i ? 0 : -1;
        b.joint = i;
        s.bones.push_back(b);
    }
    SkinnedModel skin(s);
    ObjectModel::Part body;
    body.shape = "amBodyNaked_shpe";
    const float at[8][3] = {{0, 0, 0}, {0, 0, 1.0f}, {0, 0.05f, 1.4f}, {0, 0, 1.7f}, {-0.8f, 0, 1.5f}, {0.8f, 0, 1.5f}, {-0.1f, 0, 0.05f}, {0.1f, 0, 0.05f}};
    for (uint8_t b = 1; b < 8; ++b)
        for (int k = 0; k < 2; ++k) {
            const float d = k ? 0.1f : -0.1f;
            body.mesh.positions.push_back({at[b][0] + d, at[b][1] + d, at[b][2] + d});
            body.mesh.bones.push_back({b, 0xFF, 0xFF, 0xFF});
            body.mesh.weights.push_back({1, 0, 0, 0});
        }
    // A vertex mostly bound to the pelvis though listed second.
    body.mesh.positions.push_back({0, 0, 0.85f});
    body.mesh.bones.push_back({6, 1, 0xFF, 0xFF});
    body.mesh.weights.push_back({0.3f, 0.7f, 0, 0});
    ObjectModel::Part hair;
    hair.shape = "amHairShort_shpe";
    hair.mesh.positions.push_back({0, 0, 3});
    hair.mesh.bones.push_back({3, 0xFF, 0xFF, 0xFF});
    hair.mesh.weights.push_back({1, 0, 0, 0});
    skin.model.parts = {body, hair};
    std::vector<int> joints = {0, 1, 2, 3, 4, 5, 6, 7};
    skin.jointMaps = {joints, joints};
    skin.rigidBones = {-1, -1};
    auto a = std::make_unique<SimActor>(std::move(skin));
    a->posed = a->skin.model; // posed where modelled (world = model space here)
    return a;
}

static void testBoxes() {
    const auto actor = makeActor();
    const SimActor& a = *actor;
    check(censorBoxes(a, 0, false).empty(), "dressed (flags 0): no box");
    check(censorBoxes(a, 0, true).empty(), "dressed woman: no box");

    const CensorBox pelvis = boneGeometryBox(a, "pelvis");
    check(near(pelvis.lo[2], 0.85f) && near(pelvis.hi[2], 1.1f), "the pelvis box: its vertices, the one bound most to it included");
    check(boneGeometryBox(a, "head").hi[2] < 2, "the hair is not the head's geometry");

    // A man: pelvis only (SpineIfFemale ignored), x and y widened to 0.8 x size.
    auto man = censorBoxes(a, censor::Pelvis | censor::SpineIfFemale, false);
    check(man.size() == 1, "man in the shower: one box");
    if (man.size() == 1) {
        check(near(man[0].lo[0], -0.16f) && near(man[0].hi[0], 0.16f), "man: pelvis widened in x (0.8 x size each side)");
        check(near(man[0].lo[2], 0.85f) && near(man[0].hi[2], 1.1f), "man: pelvis height kept");
    }
    // A woman: the pelvis and the chest (0.7 of its height, 0.05 up) in one box.
    auto woman = censorBoxes(a, censor::Pelvis | censor::SpineIfFemale, true);
    check(woman.size() == 1, "woman in the shower: one box");
    if (woman.size() == 1) {
        check(near(woman[0].lo[2], 0.85f), "woman: from the pelvis");
        check(near(woman[0].hi[2], 1.4f + 0.05f + 0.07f), "woman: up to the chest box (0.35 x size over its centre + 0.05)");
        check(near(woman[0].lo[0], -0.1f) && near(woman[0].hi[0], 0.1f), "woman: pelvis not widened");
    }
    check(censorBoxes(a, censor::SpineIfFemale, false).empty(), "a man's chest is not censored");

    auto hands = censorBoxes(a, censor::LeftHand | censor::RightHand, false);
    check(hands.size() == 2, "hands: a box each");
    if (hands.size() == 2)
        check(hands[0].hi[0] < 0 && hands[1].lo[0] > 0, "hands: left then right");

    auto full = censorBoxes(a, censor::FullBody, true);
    check(full.size() == 1, "full body: one box");
    if (full.size() == 1)
        check(near(full[0].lo[2], -0.05f) && near(full[0].hi[2], 1.8f) && near(full[0].lo[0], -0.9f) && near(full[0].hi[0], 0.9f),
              "full body: feet to head, hand to hand");
}

static void testScreenRect() {
    simd_float4x4 vp = matrix_identity_float4x4; // clip = world
    Renderer::CensorBox box{simd_make_float3(-0.5f, -0.5f, 0.2f), simd_make_float3(0.5f, 0.25f, 0.4f)};
    auto r = Renderer::censorRect(box, vp, 100, 100);
    check(r.has_value(), "a box in view has a rectangle");
    if (r) {
        check(r->x0 == 25 && r->x1 == 75, "rectangle x in pixels");
        check(r->y0 == 37 && r->y1 == 75, "rectangle y in pixels, y down, snapped outwards");
        check(near(r->depth, 0.2f), "rectangle at the box's nearest depth");
    }
    Renderer::CensorBox off{simd_make_float3(2, 2, 0.2f), simd_make_float3(3, 3, 0.4f)};
    check(!Renderer::censorRect(off, vp, 100, 100), "a box off screen has none");
    Renderer::CensorBox edge{simd_make_float3(0.5f, -0.5f, 0.2f), simd_make_float3(3, 0.5f, 0.4f)};
    auto e = Renderer::censorRect(edge, vp, 100, 100);
    check(e && e->x1 == 100 && e->x0 == 75, "a box over the edge is clipped to the viewport");
    simd_float4x4 behind = matrix_identity_float4x4;
    behind.columns[3][3] = -1; // w < 0
    check(!Renderer::censorRect(box, behind, 100, 100), "a box behind the eye has none");
    // In a mirror (kRenderTypeCensorInMirror): the same box seen by the
    // reflected camera (view * R, R = I - 2 n n^T for the plane x = 0) is
    // projected to the mirrored rectangle.
    simd_float4x4 refl = matrix_identity_float4x4;
    refl.columns[0][0] = -1;
    Renderer::CensorBox side{simd_make_float3(0.1f, -0.5f, 0.2f), simd_make_float3(0.5f, 0.25f, 0.4f)};
    auto direct = Renderer::censorRect(side, vp, 100, 100);
    auto mirrored = Renderer::censorRect(side, simd_mul(vp, refl), 100, 100);
    check(direct && mirrored && direct->x0 == 55 && direct->x1 == 75 && mirrored->x0 == 25 && mirrored->x1 == 45 &&
              mirrored->y0 == direct->y0 && mirrored->y1 == direct->y1,
          "the reflected view's rectangle is the mirrored one");
}

// With the game's data (OS2_DATA): the tutorial's Joe, naked, gets one box
// at his hips; dressed in everyday clothes his flags are 0 (no box).
static void testJoe() {
    const char* data = std::getenv("OS2_DATA");
    if (!data || !std::filesystem::exists(std::filesystem::path(data) / "TSData/Res"))
        return;
    ResourceIndex index(std::filesystem::path(data) / "TSData/Res");
    const auto parts = loadSimParts(index);
    const auto file = std::filesystem::path(data) / "TSData/Res/UserData/Neighborhoods/Tutorial/Characters/Tutorial_User00000.package";
    if (!std::filesystem::exists(file))
        return;
    SimActor joe(loadCharacterSim(index, parts, file, 0x080));
    joe.update(0);
    auto boxes = censorBoxes(joe, censor::Pelvis | censor::SpineIfFemale, false);
    check(boxes.size() == 1, "Joe naked: one box");
    if (boxes.size() == 1) {
        const CensorBox& b = boxes[0];
        check(b.lo[2] > 0.8f && b.lo[2] < 0.95f && b.hi[2] > 1.05f && b.hi[2] < 1.2f, "Joe: the box spans his hips");
        check(b.lo[0] < -0.15f && b.hi[0] > 0.15f, "Joe: the box covers his hips' width");
    }
    SimActor dressed(loadCharacterSim(index, parts, file, 0x001));
    dressed.update(0);
    check(censorBoxes(dressed, 0, false).empty(), "Joe dressed: no box");
}

int main() {
    testBoxes();
    testScreenRect();
    testJoe();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all censor tests passed\n");
    return failures ? 1 : 0;
}
