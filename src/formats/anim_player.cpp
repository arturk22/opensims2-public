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
    l.start = now;
    l.fadeIn = std::max(o.fade, 0.0f);
    l.loop = o.loop;
    l.priority = o.priority >= 0 ? o.priority : int(anim.priority);
    l.mask = o.mask;
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
    // The first layer of a priority has nothing to fade from.
    bool alone = true;
    for (const auto& other : layers_)
        alone = alone && other.priority != l.priority;
    if (alone && l.priority == 0)
        l.fadeIn = 0;
    layers_.push_back(std::move(l));
    std::stable_sort(layers_.begin(), layers_.end(),
                     [](const Layer& a, const Layer& b) { return a.priority < b.priority; });
}

void AnimPlayer::stop(double now, float fade, int priority) {
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
    return w;
}

std::vector<Xform> AnimPlayer::pose(double now) {
    if (!skeleton_)
        return {};
    std::vector<Xform> out = skeleton_->restPose();
    std::erase_if(layers_, [&](const Layer& l) { return l.fadeOutStart >= 0 && weight(l, now) <= 0; });
    for (const auto& l : layers_) {
        if (!l.target)
            continue;
        // A replaced layer stays whole under the one fading in over it.
        const float w = l.replaced ? (weight(l, now) > 0 ? 1.0f : 0.0f) : weight(l, now);
        if (w <= 0)
            continue;
        const float ticks = animationTime(*l.anim, float(std::max(now - l.start, 0.0)), l.loop);
        std::vector<Xform> p = samplePose(*l.target, *skeleton_, ticks);
        applyIk(*l.target, *skeleton_, ticks, p);
        std::vector<float> mask(skeleton_->bones.size(), 0.0f);
        for (size_t i = 0; i < mask.size(); ++i)
            mask[i] = (i < l.keyed.size() && l.keyed[i] ? 1.0f : 0.0f) *
                      (l.mask.empty() ? 1.0f : (i < l.mask.size() ? l.mask[i] : 0.0f));
        blendPose(out, p, w, mask);
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
