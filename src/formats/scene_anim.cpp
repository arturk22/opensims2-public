// The animation queue of a scene graph record (scene_anim.hpp).

#include "scene_anim.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"

#include <algorithm>

namespace os2 {

namespace {

constexpr size_t kFileNameSize = 64;
constexpr uint32_t kTagAnimatable = 0x7D4F2BB8, kTagAnimRequest = 0x09D0CE69, kTagChoreoAnim = 0xC9DE69B6,
                   kTagChoreo = 0x3AFF30DB, kTagBlend = 0x8B969D64, kTagLookAt = 0x2A6F2E97;

void skipTransform(BinReader& r) { r.skip(28); }

// nTSSG::cObject::LoadFromStream (BV 0x65fae8), versions 0x0C-0x11.
void skipObject(BinReader& r, uint32_t v) {
    if (v < 0x0D) {
        r.str7();
        r.u32();
    } else if (v > 0x0D) {
        r.str7();
    }
    for (uint32_t n = r.u32(); n; --n) {
        r.str7();
        if (v > 0x10)
            r.u32();
        for (uint32_t m = r.u32(); m; --m) {
            r.str7();
            r.str7();
        }
    }
    if (v > 0x0E)
        skipTransform(r);
    for (uint32_t n = r.u32(); n; --n) {
        r.str7();
        skipTransform(r);
        if (v < 0x0F)
            r.f32();
    }
    if (v > 0x0F)
        for (uint32_t n = r.u32(); n; --n) {
            r.str7();
            r.str7();
            r.f32();
        }
}

// cAnimRequest's reader (BV 0x6ab788).
void readAnimRequest(BinReader& r, SceneAnimRequest& q) {
    if (r.u32() != kTagAnimRequest)
        throw FormatError("not a cAnimRequest");
    const uint32_t v = q.version = r.u32();
    q.id = r.u32();
    q.name = r.str7();
    if (v > 2)
        q.channel = r.str7();
    q.arType = r.u32();
    if (v > 6)
        q.oeType = r.u32();
    q.stackObject = r.u16();
    q.eventTree = r.u16();
    q.interruptible = r.u32() != 0;
    if (v > 8)
        q.hurrySpeed = r.f32();
    q.beginAction = r.f32();
    q.endAction = r.f32();
    q.blendIn = r.u32() != 0;
    q.blendOut = r.u32() != 0;
    q.actualEndAction = r.u32();
    q.target = r.u32();
    if (v > 3)
        skipTransform(r);
    q.slot = r.u32();
    q.sameRoom = r.u32() != 0;
    if (v > 1)
        q.objectGuid = r.u32();
    if (v > 4) {
        q.state = r.u32();
        q.movedObject = r.u32();
    }
    if (v > 5)
        q.objectAnim = r.str7();
    if (v > 7 && r.u32() != 0)
        throw FormatError("cAnimRequest with an end event callback"); // its layout is not known
    if (v > 9) {
        q.transformParent = r.u32();
        q.transformParent2 = r.u32();
    }
    if (v > 10)
        q.container = r.u32();
}

// cChoreoRequest::LoadFromStream (BV 0xd9404c).
void readChoreo(BinReader& r, SceneAnimRequest& q) {
    if (r.u32() != kTagChoreo)
        throw FormatError("not a cChoreoRequest");
    r.u32();
    q.status = r.u32();
    r.u32();
    q.start = r.i32();
    r.u32(); // actual end ticks
    q.portion = r.f32();
    r.f32(); // end portion
    q.expiration = r.u32();
    q.priority = r.u32();
    q.weight = r.f32();
    if (r.u32()) { // cBlendFunction2 (BV 0xd33360)
        if (r.u32() != kTagBlend)
            throw FormatError("not a cBlendFunction2");
        r.u32();
        r.u32();
        r.f32();
        for (uint32_t n = r.u32(); n; --n)
            r.skip(16);
    }
    q.mirrored = r.u32() != 0;
    q.loops = r.u32();
    q.speed = r.f32();
    q.heading = r.f32();
    for (int i = 0; i < 3; ++i) {
        skipTransform(r);
        r.f32();
    }
    q.current = r.i32();
    r.u32();
    r.u32();
    r.u32();
    r.f32();
    r.u32(); // external request
    q.played = r.f32();
}

// cChoreoRequestAnim::LoadFromStream (BV 0xd52b0c).
void readChoreoAnim(BinReader& r, SceneAnimRequest& q) {
    if (r.u32() != kTagChoreoAnim)
        throw FormatError("not a cChoreoRequestAnim");
    q.choreoVersion = r.u32();
    readChoreo(r, q);
    if (q.choreoVersion > 1)
        r.f32();
}

} // namespace

std::optional<SceneAnimatable> readSceneAnimatable(std::span<const uint8_t> record) {
    try {
        BinReader r(record);
        r.skip(kFileNameSize);
        uint32_t animatable = 0, object = 0;
        for (int depth = 0;; ++depth) {
            if (depth == 8)
                return std::nullopt;
            const uint32_t tag = r.u32();
            const uint32_t version = r.u32();
            const std::string cls = r.str7();
            if (tag == kTagAnimatable)
                animatable = version;
            if (cls == "cObject") {
                object = version;
                break;
            }
        }
        if (animatable < 0x08 || animatable > 0x10 || object < 0x0C || object > 0x11)
            return std::nullopt;
        skipObject(r, object);
        SceneAnimatable a;
        a.version = animatable;
        a.begin = r.pos();
        r.f32();
        if (r.u32()) {
            skipTransform(r);
            skipTransform(r);
        }
        if (animatable < 0x0F) {
            // Two requests of the old layout (on_load_choreo_request_anim:
            // the choreography request, the animation, its handler kind).
            for (int i = 0; i < 2; ++i)
                if (r.u32()) {
                    SceneAnimRequest q;
                    readChoreoAnim(r, q);
                    r.str7();
                    r.u32();
                }
            r.u32();
            r.u32();
        }
        a.counterAt = r.pos();
        a.counter = r.u32();
        if (animatable > 0x0B)
            r.u32();
        if (animatable < 0x0A)
            r.u32();
        if (animatable > 0x08)
            r.u32();
        if (animatable > 0x0A)
            r.u32();
        if (animatable > 0x0C)
            r.f32();
        if (animatable > 0x0D) {
            if (animatable == 0x10)
                r.u32();
            r.u32();
        }
        skipTransform(r);
        a.requestsAt = r.pos();
        for (uint32_t n = r.u32(); n; --n) {
            SceneAnimRequest q;
            q.begin = r.pos();
            q.kind = r.u32();
            if (q.kind == 1) {
                readAnimRequest(r, q);
                readChoreoAnim(r, q);
                q.animation = r.str7();
                r.u32();
            } else if (q.kind == 2) {
                readAnimRequest(r, q);
                // cChoreoRequestLookAt::LoadFromStream (BV 0xd68284).
                if (r.u32() != kTagLookAt)
                    throw FormatError("not a cChoreoRequestLookAt");
                const uint32_t lv = r.u32();
                readChoreoAnim(r, q);
                r.str7();
                if (lv == 2)
                    r.skip(12);
                r.f32();
                if (lv >= 5)
                    r.f32();
                r.skip(12);
                if (lv > 3)
                    r.u32();
                r.u32();     // on_save_choreo_request_look_at's flag
                r.skip(12);  // the target point
                r.u32();
                r.f32();
            } else if (q.kind != 0) {
                return std::nullopt;
            }
            q.end = r.pos();
            a.requests.push_back(std::move(q));
        }
        a.end = r.pos();
        return a;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::string animChannelName(int level, bool sim) {
    static const char* const kSim[] = {"sil", "sim", "sih", "siu"};
    static const char* const kOverlay[] = {"osl", "ovl", "obj", "ovm", "obj", "ovh", "obj", "ovu"};
    if (sim)
        return kSim[std::clamp(level / 2, 0, 3)];
    return kOverlay[std::clamp(level, 0, 7)];
}

int animChannelLevel(const std::string& c) {
    static const std::pair<const char*, int> kNames[] = {{"sil", 0}, {"sim", 2}, {"sih", 4}, {"siu", 6},
                                                         {"osl", 0}, {"ovl", 1}, {"ovm", 3}, {"ovh", 5},
                                                         {"ovu", 7}, {"obj", 2}};
    for (const auto& [n, l] : kNames)
        if (c == n)
            return l;
    return 2;
}

std::vector<uint8_t> writeAnimSimRequest(const AnimSimRequest& r) {
    BinWriter w;
    auto transform = [&] {
        for (int i = 0; i < 6; ++i)
            w.f32(0);
        w.f32(1);
    };
    w.u32(1);
    // cAnimRequest (BV 0x6abc9c), as try_animate_sim fills it.
    w.u32(kTagAnimRequest);
    w.u32(11);
    w.u32(r.id);
    w.str7(r.name);
    w.str7(r.channel.empty() ? animChannelName(r.level, true) : r.channel);
    w.u32(r.arType);
    w.u32(r.oeType);
    w.u16(r.stackObject);
    w.u16(r.eventTree);
    w.u32(r.interruptible);
    w.f32(1);
    w.f32(-1);
    w.f32(-1);
    w.u32(r.blendIn);
    w.u32(r.blendOut);
    w.u32(0x7FFFFF00);
    w.u32(r.target);
    transform();
    w.u32(0xFFFFFFFF);
    w.u32(1);
    w.u32(r.objectGuid);
    w.u32(0);
    w.u32(r.movedObject);
    w.str7("");
    w.u32(0);
    w.u32(0xFFFFFFFF);
    w.u32(0);
    w.u32(0);
    // cChoreoRequestAnim version 1 (the base game's lots), cChoreoRequest 7.
    w.u32(kTagChoreoAnim);
    w.u32(1);
    w.u32(kTagChoreo);
    w.u32(7);
    w.u32(1); // status: playing
    w.u32(0);
    const float speed = r.speed > 0 ? r.speed : 1.0f;
    w.i32(r.current - int32_t(r.played / speed));
    w.u32(0x7FFFFF00);
    w.f32(r.length > 0 ? std::min(r.played / r.length, 1.0f) : 0.0f);
    w.f32(1);
    w.u32(r.blendOut ? 5 : 3); // the expiration rule (5 with a blend out in the game's lots, else 3)
    w.u32(uint32_t(r.level));
    w.f32(r.weight);
    // The weight blend: in over the blend time, held, out over it.
    constexpr float kBlend = 233.33333f;
    const float in = r.blendIn ? kBlend : 0.0f, out = r.blendOut ? kBlend : 0.0f;
    w.u32(1);
    w.u32(kTagBlend);
    w.u32(1);
    w.u32(1);
    w.f32(0);
    w.u32(3);
    w.u32(0), w.f32(0.001f), w.f32(1), w.f32(in);
    w.u32(1), w.f32(1), w.f32(1), w.f32(std::max(r.length / speed - in - out, 0.0f));
    w.u32(2), w.f32(1), w.f32(0.001f), w.f32(out);
    w.u32(r.mirrored);
    w.u32(r.loops);
    w.f32(speed);
    w.f32(r.heading);
    for (int i = 0; i < 3; ++i) {
        transform();
        w.f32(1);
    }
    w.i32(r.current);
    w.u32(1);
    w.u32(0);
    w.u32(0x7FFFFF00);
    w.u32(0x4EFFFFFF); // f32 2^31
    w.u32(1);
    w.f32(r.played);
    w.str7(r.name + "_anim");
    w.u32(1);
    return std::move(w.bytes());
}

std::vector<uint8_t> withSceneRequests(std::span<const uint8_t> record, const SceneAnimatable& a,
                                       const std::vector<std::vector<uint8_t>>& requests, uint32_t counter) {
    std::vector<uint8_t> out(record.begin(), record.begin() + std::ptrdiff_t(a.requestsAt));
    for (int i = 0; i < 4; ++i)
        out[a.counterAt + size_t(i)] = uint8_t(counter >> (8 * i));
    BinWriter w;
    w.u32(uint32_t(requests.size()));
    for (const auto& q : requests)
        w.raw(q);
    out.insert(out.end(), w.bytes().begin(), w.bytes().end());
    out.insert(out.end(), record.begin() + std::ptrdiff_t(a.end), record.end());
    return out;
}

} // namespace os2
