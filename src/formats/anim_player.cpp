#include "anim_player.hpp"

#include <cstdio>

#include "resindex.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace os2 {

std::vector<float> boneMask(const Skeleton& s, std::string_view root) {
    const int r = s.find(root);
    if (r < 0)
        return {};
    std::vector<float> mask(s.bones.size(), 0.0f);
    mask[size_t(r)] = 1;
    // Parents come before their children.
    for (size_t i = size_t(r) + 1; i < s.bones.size(); ++i) {
        const int p = s.bones[i].parent;
        if (p >= 0 && mask[size_t(p)] > 0)
            mask[i] = 1;
    }
    return mask;
}

std::vector<bool> keyedBones(const Animation::Target& target, const Skeleton& s) {
    std::vector<bool> keyed(s.bones.size(), false);
    for (const auto& c : target.channels) {
        if ((c.attribute() != Animation::Rotation && c.attribute() != Animation::Translation) || c.isDelta())
            continue;
        const int b = s.find(c.name);
        if (b >= 0)
            keyed[size_t(b)] = true;
    }
    // A delta-only target's chains (the walk postures' legs) solve nothing
    // of their own: their goals are deltas too.
    if (!target.ikChains.empty() && target.hasBaseChannels()) {
        std::unordered_map<uint32_t, int> byCrc;
        for (size_t i = 0; i < s.bones.size(); ++i)
            byCrc.emplace(nameInstanceHiHash(s.bones[i].name), int(i));
        for (const auto& chain : target.ikChains) {
            auto it = byCrc.find(chain.crcs[2]);
            const auto begin = byCrc.find(chain.crcs[0]);
            if (it == byCrc.end() || begin == byCrc.end())
                continue;
            // End bone up to the begin bone.
            for (int b = it->second; b >= 0; b = s.bones[size_t(b)].parent) {
                keyed[size_t(b)] = true;
                if (b == begin->second)
                    break;
            }
        }
    }
    return keyed;
}

std::vector<int> mirrorBones(const Skeleton& s) {
    std::vector<int> twins(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        twins[i] = int(i);
        const std::string& n = s.bones[i].name;
        if (n.size() > 2 && n[1] == '_' && (n[0] == 'l' || n[0] == 'r' || n[0] == 'L' || n[0] == 'R')) {
            std::string other = n;
            other[0] = (n[0] == 'l' || n[0] == 'L') ? 'r' : 'l';
            if (const int t = s.find(other); t >= 0)
                twins[i] = t;
        }
    }
    return twins;
}

namespace {
// S x S with S the reflection x -> -x: still a rigid transform.
Xform reflectX(const Xform& x) {
    Xform o;
    o.q = {x.q.x, -x.q.y, -x.q.z, x.q.w};
    o.t = {-x.t[0], x.t[1], x.t[2]};
    return o;
}
} // namespace

std::vector<Xform> mirrorPose(const Skeleton& s, const std::vector<int>& twins, const std::vector<Xform>& restWorld,
                              const std::vector<Xform>& local) {
    const size_t n = s.bones.size();
    if (twins.size() != n || restWorld.size() != n || local.size() != n)
        return local;
    const std::vector<Xform> world = s.world(local);
    std::vector<Xform> out(n);
    for (size_t b = 0; b < n; ++b) {
        const size_t t = size_t(twins[b]);
        out[b] = reflectX(world[t].then(restWorld[t].inverse())).then(restWorld[b]);
    }
    std::vector<Xform> result(n);
    for (size_t b = 0; b < n; ++b) {
        const int p = s.bones[b].parent;
        result[b] = p >= 0 ? out[size_t(p)].inverse().then(out[b]) : out[b];
    }
    return result;
}

Quat deltaPower(const Quat& q, float w) {
    // As ChannelData does: the half angle from acos(w) (none outside
    // (-1, 1)), scaled by the weight; no flip to the shorter arc.
    if (q.w <= -1 || q.w >= 1)
        return Quat{};
    const float a = std::acos(q.w);
    const float s = std::sin(a);
    const float k = s != 0 ? std::sin(a * w) / s : 0.0f;
    return {q.x * k, q.y * k, q.z * k, std::cos(a * w)};
}

DeltaPose AnimPlayer::mirrorDeltas(const DeltaPose& d, std::vector<float>& weights) const {
    // The deltas as a pose over the rest pose, mirrored like a pose, and
    // taken back off the rest pose; each bone takes its twin's.
    const size_t n = skeleton_->bones.size();
    const std::vector<Xform> rest = skeleton_->restPose();
    std::vector<Xform> p = rest;
    for (size_t b = 0; b < n; ++b) {
        if (d.has[b] & 1)
            p[b].q = rest[b].q * d.delta[b].q;
        if (d.has[b] & 2)
            for (int c = 0; c < 3; ++c)
                p[b].t[size_t(c)] += d.delta[b].t[size_t(c)];
    }
    const std::vector<Xform> m = mirrorPose(*skeleton_, twins_, restWorld_, p);
    DeltaPose out;
    out.delta.assign(n, Xform{});
    out.has.assign(n, 0);
    std::vector<float> w(n, 0.0f);
    for (size_t b = 0; b < n; ++b) {
        const size_t t = size_t(twins_[b]);
        out.has[b] = d.has[t];
        w[b] = weights[t];
        out.delta[b].q = normalized(conjugate(rest[b].q) * m[b].q);
        for (int c = 0; c < 3; ++c)
            out.delta[b].t[size_t(c)] = m[b].t[size_t(c)] - rest[b].t[size_t(c)];
    }
    weights = std::move(w);
    return out;
}

void AnimPlayer::prepareMirror() {
    if (!skeleton_ || !twins_.empty())
        return;
    twins_ = mirrorBones(*skeleton_);
    restWorld_ = skeleton_->world(skeleton_->restPose());
}

void blendPose(std::vector<Xform>& from, const std::vector<Xform>& to, float weight, const std::vector<float>& mask) {
    const size_t n = std::min(from.size(), to.size());
    for (size_t i = 0; i < n; ++i) {
        const float w = weight * (i < mask.size() ? mask[i] : (mask.empty() ? 1.0f : 0.0f));
        if (w <= 0)
            continue;
        if (w >= 1) {
            from[i] = to[i];
            continue;
        }
        from[i].q = nlerp(from[i].q, to[i].q, w);
        for (int k = 0; k < 3; ++k)
            from[i].t[size_t(k)] += (to[i].t[size_t(k)] - from[i].t[size_t(k)]) * w;
    }
}

void AnimPlayer::play(const Animation& anim, double now, const Options& o) {
    // An object whose layers have all finished keeps their pose as its base
    // before a new one (pose() does it too), also when nobody draws it.
    if (hold_ && skeleton_ && !layers_.empty() && allFinished(now))
        pose(now);
    Layer l;
    l.anim = &anim;
    l.target = skeleton_ && !skeleton_->bones.empty() ? anim.findTarget(anim.skeletonTag) : nullptr;
    if (!l.target && !anim.targets.empty())
        l.target = &anim.targets[0];
    // An object's animation (its skeleton "book", "chairdining") on a Sim's
    // skeleton ("auSkel", "cuSkel", ...) binds no bone of it: the game binds a
    // target to the skeleton it is named for. The VM puts the book's
    // "o2a-book-carry" on the reader as an overlay (it keys root_trans and
    // root_rot): played on the child, its body sank to the floor a tile off
    // and jumped back and forth as it walked (N001_Lot11, OS2_DEBUG_JUMPS).
    auto simSkeleton = [](std::string_view n) { // "auSkel_cres", "cuskel"
        std::string low;
        for (char c : n)
            low += char(std::tolower(static_cast<unsigned char>(c)));
        return low.find("skel") != std::string::npos;
    };
    if (skeleton_ && simSkeleton(skeleton_->name) && l.target && !simSkeleton(l.target->tag))
        l.target = nullptr;
    l.rate = o.rate > 0 ? o.rate : 1.0f;
    l.start = now - std::max(o.phase, 0.0) / l.rate;
    l.fadeIn = std::max(o.fade, 0.0f);
    l.loop = o.loop;
    // Without a bone mask the animation is the Sim's main one: it takes the
    // base layer (0) whatever its header priority, so that a walk (0)
    // replaces an idle (2) instead of playing under it (which left the Sims
    // sliding in their idle pose, and the child idles fading in from a
    // T-pose). The header priority orders the masked overlays.
    l.priority = o.priority >= 0 ? o.priority : (o.mask.empty() ? 0 : int(anim.priority));
    l.mask = o.mask;
    l.mirror = o.mirror;
    l.turn = o.turn;
    l.level = o.level >= 0 ? o.level : l.priority;
    l.scale = std::clamp(o.weight, 0.0f, 1.0f);
    l.ikObjectBone = o.ikObjectBone;
    l.expires = o.expires;
    if (l.mirror)
        prepareMirror();
    if (skeleton_ && l.target) {
        // Full-body (flag bit 0) keys every bone, but the delta channels
        // only add to the bones (a delta-only "SWAT" talk shift is
        // full-body too).
        if ((anim.flags & 1) && l.target->hasBaseChannels())
            l.keyed.assign(skeleton_->bones.size(), true);
        else
            l.keyed = keyedBones(*l.target, *skeleton_);
        l.delta = l.target->hasDeltaChannels();
    }
    for (auto& other : layers_)
        if (other.priority == l.priority && other.fadeOutStart < 0) {
            other.fadeOutStart = now;
            other.fadeOut = l.fadeIn;
            other.replaced = true;
        }
    // The first base layer has nothing to fade from (fading in from the
    // rest pose would show a T-pose).
    bool alone = true;
    for (const auto& other : layers_)
        alone = alone && other.priority != l.priority;
    if (alone && l.priority == 0)
        l.fadeIn = 0;
    layers_.push_back(std::move(l));
    std::stable_sort(layers_.begin(), layers_.end(), [](const Layer& a, const Layer& b) {
        return a.level != b.level ? a.level < b.level : a.priority < b.priority;
    });
}

void AnimPlayer::stop(double now, float fade, int priority) {
    if (hold_ && skeleton_) {
        base_ = pose(now);
        std::erase_if(layers_, [&](const Layer& l) { return priority < 0 || l.priority == priority; });
        return;
    }
    for (auto& l : layers_)
        if ((priority < 0 || l.priority == priority) && l.fadeOutStart < 0) {
            l.fadeOutStart = now;
            l.fadeOut = fade;
        }
}

bool AnimPlayer::playsOn(const Layer& l, double now) const {
    if (l.fadeOutStart >= 0)
        return false;
    if (l.loop)
        return true;
    return now < l.start + (l.anim ? l.anim->duration / 1000.0 / l.rate : 0.0);
}

void AnimPlayer::stayInWorld(const Xform& delta, double now) {
    for (auto& l : layers_) {
        // A looping layer still playing goes on in the new place; one that
        // plays once (held or not yet ended) or fades out stays.
        if (l.loop && playsOn(l, now))
            continue;
        l.frame = delta.then(l.frame);
        l.framed = true;
    }
}

void AnimPlayer::applyFrame(const Layer& l, std::vector<Xform>& p) const {
    if (!l.framed || !skeleton_)
        return;
    for (size_t i = 0; i < p.size() && i < skeleton_->bones.size(); ++i) {
        const int parent = skeleton_->bones[i].parent;
        if (parent >= 0 && skeleton_->bones[size_t(parent)].parent < 0)
            p[i] = l.frame.then(p[i]);
    }
}

float AnimPlayer::weight(const Layer& l, double now) const {
    if (now < l.start)
        return 0;
    float w = 1;
    if (l.fadeIn > 0)
        w = std::clamp(float((now - l.start) / l.fadeIn), 0.0f, 1.0f);
    if (l.fadeOutStart >= 0)
        w *= l.fadeOut > 0 ? std::clamp(1.0f - float((now - l.fadeOutStart) / l.fadeOut), 0.0f, 1.0f)
                           : (now >= l.fadeOutStart ? 0.0f : 1.0f);
    return w * l.scale;
}

std::vector<Xform> AnimPlayer::layerPose(const Layer& l, float ticks) const {
    std::vector<Xform> p = samplePose(*l.target, *skeleton_, ticks);
    if (l.turn != 0) {
        // The bones under the skeleton's top node (auskel's root_trans).
        const Xform turn{quatFromEulerXYZ(0, 0, l.turn), {0, 0, 0}};
        for (size_t i = 0; i < p.size() && i < skeleton_->bones.size(); ++i) {
            const int parent = skeleton_->bones[i].parent;
            if (parent >= 0 && skeleton_->bones[size_t(parent)].parent < 0)
                p[i] = turn.then(p[i]);
        }
    }
    return p;
}

std::string AnimPlayer::describe(double now) const {
    std::string out;
    char buf[256];
    for (const auto& l : layers_) {
        std::snprintf(buf, sizeof buf, "%s[%s L%d P%d w%.2f%s%s%s]", out.empty() ? "" : " ",
                      l.anim ? l.anim->resource.fileName.c_str() : "(held)", l.level, l.priority, l.anim ? weight(l, now) : 1.0f,
                      l.loop ? " loop" : "", l.mask.empty() ? "" : " masked", l.target && !l.target->ikChains.empty() ? " ik" : "");
        out += buf;
    }
    return out;
}

std::vector<Xform> AnimPlayer::pose(double now) {
    if (!skeleton_)
        return {};
    std::vector<Xform> out = base_.size() == skeleton_->bones.size() ? base_ : skeleton_->restPose();
    std::erase_if(layers_, [&](const Layer& l) { return l.fadeOutStart >= 0 && weight(l, now) <= 0; });
    // A layer that played once and ended gives way to every other: the
    // game's request expires at its end and its bones keep the values it
    // left only where no other channel writes them (cChannelUberOutlet
    // writes no bone without a channel). Held at the bottom level, its last
    // frame shows only there, and what plays at any level takes the bones it
    // keys (a seated Sim's get-in, level 2, under the chair's idles at
    // level 1: they showed nowhere), while a new layer of its priority
    // still crossfades from it. (Inferred: the expiry as a level.)
    bool expired = false;
    for (auto& l : layers_)
        if (!l.loop && l.expires && l.level != kExpiredLevel && l.fadeOutStart < 0 && l.anim &&
            now >= l.start + l.anim->duration / 1000.0 / l.rate) {
            l.level = kExpiredLevel;
            expired = true;
        }
    if (expired)
        std::stable_sort(layers_.begin(), layers_.end(), [](const Layer& a, const Layer& b) {
            return a.level != b.level ? a.level < b.level : a.priority < b.priority;
        });
    const size_t n = skeleton_->bones.size();
    // Each layer's pose and per-bone weight (fade x request weight x keyed x mask).
    struct Posed {
        std::vector<Xform> pose;
        std::vector<float> w;
        int level = 0;
        DeltaPose deltas;        // the layer's delta channels (empty: none)
        std::vector<float> dw;   // their weight per bone (fade x request weight x mask)
    };
    std::vector<Posed> posed;
    posed.reserve(layers_.size());
    // Every layer done and constant: the pose becomes the base below.
    const bool settle = hold_ && !layers_.empty() && allFinished(now);
    std::vector<int> posedOf(layers_.size(), -1); // a layer's entry in `posed`
    for (size_t li = 0; li < layers_.size(); ++li) {
        const Layer& l = layers_[li];
        if (!l.target)
            continue;
        const float w = weight(l, now);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0) * l.rate), l.loop);
        std::vector<Xform> p = layerPose(l, ticks);
        const Xform frame{quatFromEulerXYZ(0, 0, l.turn), {0, 0, 0}};
        std::vector<float> mask(n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            mask[i] = (i < l.keyed.size() && l.keyed[i] ? w : 0.0f) *
                      (l.mask.empty() ? 1.0f : (i < l.mask.size() ? l.mask[i] : 0.0f));
        DeltaPose deltas;
        std::vector<float> dw;
        if (l.delta) {
            deltas = sampleDeltas(*l.target, *skeleton_, ticks);
            dw.assign(n, 0.0f);
            for (size_t i = 0; i < n; ++i)
                dw[i] = w * (l.mask.empty() ? 1.0f : (i < l.mask.size() ? l.mask[i] : 0.0f));
            if (l.mirror && twins_.size() == n)
                deltas = mirrorDeltas(deltas, dw);
        }
        if (l.mirror && twins_.size() == n) {
            // IK first (the chains name their own side), then the whole pose flipped.
            if (!l.target->ikChains.empty() && l.target->hasBaseChannels()) {
                const std::vector<Xform> goals = samplePose(*l.target, *skeleton_, ticks); // unturned: applyIk turns them
                applyIk(*l.target, *skeleton_, ticks, p, &goals, frame, &l.ikObjectBone, true);
            }
            p = mirrorPose(*skeleton_, twins_, restWorld_, p);
            std::vector<float> flipped(n);
            for (size_t i = 0; i < n; ++i)
                flipped[i] = mask[size_t(twins_[i])];
            mask = std::move(flipped);
        }
        applyFrame(l, p);
        if (p.size() < n)
            continue;
        posedOf[li] = int(posed.size());
        posed.push_back({std::move(p), std::move(mask), l.level, std::move(deltas), std::move(dw)});
    }
    bool anyDelta = false;
    for (const auto& x : posed)
        anyDelta = anyDelta || !x.dw.empty();
    if (anyDelta && deltaToIk_.size() != n) {
        // The bones whose deltas apply even with no base channel on them
        // (SetupChannel, BV 0xd5b8d4, calls SetApplyDeltaDataToIK(true) on
        // the outlets of head, neck and spine0-2: the look-at drives them).
        deltaToIk_.assign(n, false);
        for (const char* name : {"head", "neck", "spine0", "spine1", "spine2"})
            if (const int b = skeleton_->find(name); b >= 0)
                deltaToIk_[size_t(b)] = true;
    }
    // Per bone, the channel blender: levels from the highest down (layers_
    // is sorted by level), the remaining weight shared out, then a running
    // weighted average of the poses in that order, normalized.
    std::vector<float> share(posed.size());
    // What each layer ends up with on each bone (its share of the blend):
    // its IK chains weigh by it, so that an idle's hands under a higher
    // level's eating arm give way as its channels do.
    std::vector<std::vector<float>> owns(posed.size(), std::vector<float>(n, 0.0f));
    // The deltas each bone took (for the IK goals below).
    std::vector<Xform> applied;
    std::vector<uint8_t> appliedHas;
    if (anyDelta) {
        applied.assign(n, Xform{});
        appliedHas.assign(n, 0);
    }
    for (size_t b = 0; b < n; ++b) {
        float remaining = 1;
        for (size_t k = posed.size(); k-- > 0 && remaining > 0;) {
            // The level's total on this bone.
            size_t first = k;
            while (first > 0 && posed[first - 1].level == posed[k].level)
                --first;
            float sum = 0;
            for (size_t j = first; j <= k; ++j)
                sum += posed[j].w[b];
            const float scale = sum >= 1 ? remaining / sum : remaining;
            for (size_t j = first; j <= k; ++j)
                share[j] = posed[j].w[b] * scale;
            remaining = sum >= 1 ? 0 : remaining * (1 - sum);
            k = first;
        }
        float cum = 0;
        Xform acc = out[b];
        float total = 0;
        for (size_t k = 0; k < posed.size(); ++k)
            total += share[k];
        for (size_t k = 0; k < posed.size() && total > 0; ++k)
            owns[k][b] = share[k] / total;
        for (size_t k = posed.size(); k-- > 0;) {
            const float a = share[k];
            share[k] = 0;
            if (a <= 0)
                continue;
            const Xform& x = posed[k].pose[b];
            cum += a;
            if (cum == a) {
                acc = x;
                continue;
            }
            const float t = a / cum;
            acc.q = nlerp(acc.q, x.q, t);
            for (int c = 0; c < 3; ++c)
                acc.t[size_t(c)] += (x.t[size_t(c)] - acc.t[size_t(c)]) * t;
        }
        if (cum > 0)
            out[b] = acc;
        // The delta channels over the base mix (cChannelUberOutletQ::
        // ChannelData, BV 0xd5e1c4): from the highest priority down, each
        // rotation raised to its weight (q^w: the angle times w about its
        // axis) and multiplied on, then base * delta (the outlet's "delta
        // base order", 1 from its constructor); the translations added
        // times their weights, over a base mix only (cChannelUberOutletXYZ,
        // BV 0xd6b0fc).
        if (anyDelta && (cum > 0 || deltaToIk_[b])) {
            Quat dq{};
            Vec3 dt{0, 0, 0};
            for (size_t k = posed.size(); k-- > 0;) {
                const Posed& x = posed[k];
                if (x.dw.empty() || x.dw[b] <= 0 || !x.deltas.has[b])
                    continue;
                const float w = x.dw[b];
                if (x.deltas.has[b] & 1)
                    dq = dq * deltaPower(x.deltas.delta[b].q, w);
                if ((x.deltas.has[b] & 2) && cum > 0)
                    for (int c = 0; c < 3; ++c)
                        dt[size_t(c)] += x.deltas.delta[b].t[size_t(c)] * w;
            }
            out[b].q = normalized(out[b].q * dq);
            for (int c = 0; c < 3; ++c)
                out[b].t[size_t(c)] += dt[size_t(c)];
            applied[b] = {dq, dt};
            appliedHas[b] = 1 | (cum > 0 ? 2 : 0);
        }
    }
    std::vector<std::pair<IkReach, const Animation*>> reaches;
    // IK over the blend: each layer's chains reach its goals from the body
    // as blended so far (an a2o animation's hands find the object even under
    // a carry pose of higher priority, which the trees leave on while
    // reading the paper; the drink's left hand reaches the mug where the
    // carry's IK put the right hand). Each chain counts by its layer's share
    // of the chain's end bone (the hand's, the foot's), whole (one chain
    // mixed by several shares bends off its goal), and the layers' solutions
    // of a bone are a running weighted average from what the blend leaves to
    // it: a level-1 idle's hands give way to a level-2 eating arm as its
    // channels do, and two layers fading across mix as their channels.
    struct IkLayer {
        size_t layer;
        std::vector<float> mask;
    };
    std::vector<IkLayer> ikLayers;
    std::vector<float> ikTotal(n, 0.0f);
    for (size_t li = 0; li < layers_.size(); ++li) {
        const Layer& l = layers_[li];
        // (A delta-only target's chains have delta goals: nothing to reach.)
        if (!l.target || l.target->ikChains.empty() || l.mirror || !l.target->hasBaseChannels() || posedOf[li] < 0)
            continue;
        const std::vector<float>& own = owns[size_t(posedOf[li])];
        std::vector<float> mask(n, 0.0f);
        for (const auto& chain : l.target->ikChains) {
            int end = -1;
            for (size_t b = 0; b < n && end < 0; ++b)
                if (nameInstanceHiHash(skeleton_->bones[b].name) == chain.crcs[2])
                    end = int(b);
            if (end < 0)
                continue;
            for (int b = end; b >= 0; b = skeleton_->bones[size_t(b)].parent) {
                mask[size_t(b)] = own[size_t(end)];
                if (nameInstanceHiHash(skeleton_->bones[size_t(b)].name) == chain.crcs[0])
                    break;
            }
        }
        for (size_t b = 0; b < n; ++b)
            ikTotal[b] += mask[b];
        ikLayers.push_back({li, std::move(mask)});
    }
    std::vector<float> ikCum(n);
    for (size_t b = 0; b < n; ++b)
        ikCum[b] = std::max(0.0f, 1 - ikTotal[b]); // the blend's own share
    for (const IkLayer& k : ikLayers) {
        const Layer& l = layers_[k.layer];
        const std::vector<float>& own = owns[size_t(posedOf[k.layer])];
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0) * l.rate), l.loop);
        // The goal and pole nodes as the animation keys them: applyIk turns
        // them by the layer's frame and turn (its modelFrame). Taken from the
        // turned pose, the nodes under the skeleton's top node (the feet's
        // *_foot_ikctr and *_foot_ikpole) were turned twice: a child's seated
        // c2o-chairDining clips, made facing their header heading of 180
        // degrees, put the feet behind the seat (the child kneeling in the
        // chair, shins through its back).
        std::vector<Xform> goals = samplePose(*l.target, *skeleton_, ticks);
        // The goal and pole nodes with the deltas their outlets took (the
        // sofa bounces' *_handcontrol1 and *_ikctr deltas move the hands and
        // feet the sitting animation's chains hold): the IK reads the
        // node's channel outlet, base * delta (cActiveAnimIK's
        // Translation/RotationChannelOutlet).
        for (size_t b = 0; b < appliedHas.size(); ++b)
            if (appliedHas[b]) {
                goals[b].q = normalized(goals[b].q * applied[b].q);
                if (appliedHas[b] & 2)
                    for (int c = 0; c < 3; ++c)
                        goals[b].t[size_t(c)] += applied[b].t[size_t(c)];
            }
        std::vector<Xform> solved = out;
        std::vector<IkReach> reach;
        applyIk(*l.target, *skeleton_, ticks, solved, &goals, l.frame.then(Xform{quatFromEulerXYZ(0, 0, l.turn), {0, 0, 0}}),
                &l.ikObjectBone, false, &reach);
        for (size_t b = 0; b < n; ++b) {
            if (k.mask[b] <= 0)
                continue;
            ikCum[b] += k.mask[b];
            out[b].q = nlerp(out[b].q, solved[b].q, ikCum[b] > 0 ? std::min(1.0f, k.mask[b] / ikCum[b]) : 1.0f);
        }
        for (auto& r : reach)
            if (r.endBone >= 0 && size_t(r.endBone) < n && r.weight * own[size_t(r.endBone)] >= 0.99f)
                reaches.push_back({r, l.anim});
    }
    // How far the drawn ends are from the goals fully on (test aid).
    ikMisses_.clear();
    if (!reaches.empty()) {
        const std::vector<Xform> world = skeleton_->world(out);
        for (const auto& [r, anim] : reaches) {
            if (r.endBone < 0 || size_t(r.endBone) >= world.size())
                continue;
            const Vec3& e = world[size_t(r.endBone)].t;
            ikMisses_.push_back({skeleton_->bones[size_t(r.endBone)].name, anim->resource.fileName,
                                 std::hypot(e[0] - r.goal[0], e[1] - r.goal[1], e[2] - r.goal[2])});
        }
    }
    if (settle) {
        // The game's requests expire when they end and the bones they wrote
        // keep their values (cChannelUberOutlet writes no bone that no channel
        // feeds): the object stays as it was left, and what plays later
        // starts over that pose (the VM never takes a dining chair's
        // overlays off: each sit and get-up adds two).
        base_ = out;
        layers_.clear();
    }
    return out;
}

bool AnimPlayer::allFinished(double now) const {
    // A second past the end, so that no event of theirs is still to fire.
    constexpr double kSettle = 1.0;
    for (const auto& l : layers_) {
        if (l.loop || l.fadeOutStart >= 0 || l.delta)
            return false;
        if (l.target && !l.target->ikChains.empty())
            return false;
        if (l.anim && l.anim->velocity > 0)
            return false;
        const double length = l.anim ? l.anim->duration / 1000.0 / l.rate : 0;
        if (now < l.start + std::max<double>(l.fadeIn, length) + kSettle)
            return false;
    }
    return true;
}

std::map<std::string, float> AnimPlayer::morphs(double now) const {
    std::map<std::string, float> out;
    for (const auto& l : layers_) {
        if (!l.anim)
            continue;
        const float w = l.replaced ? (weight(l, now) > 0 ? 1.0f : 0.0f) : weight(l, now);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0) * l.rate), l.loop);
        for (const auto& t : l.anim->targets)
            for (const auto& c : t.channels) {
                if (c.attribute() != Animation::MorphWeight || c.components.empty())
                    continue;
                const float v = c.components[0].sample(ticks, c.duration());
                float& dst = out[c.name];
                dst += (v - dst) * w;
            }
    }
    return out;
}

float AnimPlayer::velocity(double now) const {
    float v = 0;
    for (const auto& l : layers_)
        if (l.anim && l.anim->velocity > 0 && l.mask.empty())
            v += weight(l, now) * l.anim->velocity * Animation::kTicksPerSecond * l.rate;
    return v;
}

float AnimPlayer::advance(double now) {
    if (now < lastAdvance_) {
        lastAdvance_ = now;
        return 0;
    }
    // Trapezoids over small steps, so that fades are followed.
    float d = 0;
    double t = lastAdvance_;
    while (t < now) {
        const double next = std::min(now, t + 1.0 / 60);
        d += float(next - t) * 0.5f * (velocity(t) + velocity(next));
        t = next;
    }
    lastAdvance_ = now;
    return d;
}

std::vector<AnimPlayer::FiredEvent> AnimPlayer::eventsBetween(double from, double to) const {
    std::vector<FiredEvent> out;
    if (to <= from)
        return out;
    for (const auto& l : layers_) {
        if (!l.anim || (l.fadeOutStart >= 0 && l.fadeOutStart <= from))
            continue;
        const double duration = l.anim->duration / 1000.0 / l.rate; // 1 tick = 1 ms
        for (const auto& e : l.anim->events) {
            const double t = e.ticks / 1000.0 / l.rate;
            if (!l.loop || duration <= 0) {
                const double at = l.start + t;
                if (at > from && at <= to && (l.fadeOutStart < 0 || at < l.fadeOutStart))
                    out.push_back({at, l.anim, &e});
                continue;
            }
            // Cycles k with from < start + t + k * duration <= to.
            const double first = std::ceil((from - l.start - t) / duration + 1e-9);
            for (double k = std::max(0.0, first);; ++k) {
                const double at = l.start + t + k * duration;
                if (at > to || (l.fadeOutStart >= 0 && at >= l.fadeOutStart))
                    break;
                if (at > from)
                    out.push_back({at, l.anim, &e});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
    return out;
}

} // namespace os2
