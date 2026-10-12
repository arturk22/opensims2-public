#include "anim_player.hpp"

#include "resindex.hpp"

#include <algorithm>
#include <cmath>
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
        if (c.attribute() != Animation::Rotation && c.attribute() != Animation::Translation)
            continue;
        const int b = s.find(c.name);
        if (b >= 0)
            keyed[size_t(b)] = true;
    }
    if (!target.ikChains.empty()) {
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
    Layer l;
    l.anim = &anim;
    l.target = skeleton_ && !skeleton_->bones.empty() ? anim.findTarget(anim.skeletonTag) : nullptr;
    if (!l.target && !anim.targets.empty())
        l.target = &anim.targets[0];
    l.start = now - std::max(o.phase, 0.0);
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
    if (l.mirror)
        prepareMirror();
    if (skeleton_ && l.target) {
        if (anim.flags & 1)
            l.keyed.assign(skeleton_->bones.size(), true);
        else
            l.keyed = keyedBones(*l.target, *skeleton_);
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

std::vector<Xform> AnimPlayer::pose(double now) {
    if (!skeleton_)
        return {};
    std::vector<Xform> out = base_.size() == skeleton_->bones.size() ? base_ : skeleton_->restPose();
    std::erase_if(layers_, [&](const Layer& l) { return l.fadeOutStart >= 0 && weight(l, now) <= 0; });
    const size_t n = skeleton_->bones.size();
    // Each layer's pose and per-bone weight (fade x request weight x keyed x mask).
    struct Posed {
        std::vector<Xform> pose;
        std::vector<float> w;
        int level = 0;
    };
    std::vector<Posed> posed;
    posed.reserve(layers_.size());
    for (const auto& l : layers_) {
        if (!l.target)
            continue;
        const float w = weight(l, now);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0)), l.loop);
        std::vector<Xform> p = layerPose(l, ticks);
        const Xform frame{quatFromEulerXYZ(0, 0, l.turn), {0, 0, 0}};
        std::vector<float> mask(n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            mask[i] = (i < l.keyed.size() && l.keyed[i] ? w : 0.0f) *
                      (l.mask.empty() ? 1.0f : (i < l.mask.size() ? l.mask[i] : 0.0f));
        if (l.mirror && twins_.size() == n) {
            // IK first (the chains name their own side), then the whole pose flipped.
            if (!l.target->ikChains.empty()) {
                const std::vector<Xform> goals = p;
                applyIk(*l.target, *skeleton_, ticks, p, &goals, frame);
            }
            p = mirrorPose(*skeleton_, twins_, restWorld_, p);
            std::vector<float> flipped(n);
            for (size_t i = 0; i < n; ++i)
                flipped[i] = mask[size_t(twins_[i])];
            mask = std::move(flipped);
        }
        if (p.size() < n)
            continue;
        posed.push_back({std::move(p), std::move(mask), l.level});
    }
    // Per bone, the channel blender: levels from the highest down (layers_
    // is sorted by level), the remaining weight shared out, then a running
    // weighted average of the poses in that order, normalized.
    std::vector<float> share(posed.size());
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
    }
    // IK over the blend: each layer's chains reach its goals from the
    // blended body (an a2o animation's hands find the object even under a
    // carry pose of higher priority, which the trees leave on while reading
    // the paper).
    for (const auto& l : layers_) {
        if (!l.target || l.target->ikChains.empty() || l.mirror)
            continue;
        const float w = std::min(weight(l, now), 1.0f);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0)), l.loop);
        const std::vector<Xform> goals = layerPose(l, ticks);
        std::vector<Xform> solved = out;
        applyIk(*l.target, *skeleton_, ticks, solved, &goals, Xform{quatFromEulerXYZ(0, 0, l.turn), {0, 0, 0}});
        blendPose(out, solved, w, l.mask);
    }
    return out;
}

std::map<std::string, float> AnimPlayer::morphs(double now) const {
    std::map<std::string, float> out;
    for (const auto& l : layers_) {
        if (!l.anim)
            continue;
        const float w = l.replaced ? (weight(l, now) > 0 ? 1.0f : 0.0f) : weight(l, now);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0)), l.loop);
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
            v += weight(l, now) * l.anim->velocity * Animation::kTicksPerSecond;
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
        if (l.fadeOutStart >= 0 && l.fadeOutStart <= from)
            continue;
        const double duration = l.anim->duration / 1000.0; // 1 tick = 1 ms
        for (const auto& e : l.anim->events) {
            const double t = e.ticks / 1000.0;
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
