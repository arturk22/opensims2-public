#include "sim_actor.hpp"

#include "rcol.hpp"
#include "resindex.hpp"

#include <cmath>

namespace os2 {

Animation loadAnimation(ResourceIndex& index, std::string name) {
    constexpr uint32_t kTypeAnim = 0xFB00791E;
    if (!name.ends_with("_anim"))
        name += "_anim";
    auto loc = index.findByName(name, kTypeAnim);
    if (!loc)
        throw FormatError("animation not found: " + name);
    const auto bytes = index.read(*loc);
    Rcol rc(bytes);
    const auto* b = rc.find("cAnimResourceConst");
    if (!b)
        throw FormatError(name + " has no cAnimResourceConst");
    return Animation::parse(b->body);
}

const Animation& SimActor::animation(ResourceIndex& index, const std::string& name) {
    const std::string full = name.ends_with("_anim") ? name : name + "_anim";
    for (const auto& a : animations)
        if (a->resource.fileName == full)
            return *a;
    animations.push_back(std::make_unique<Animation>(loadAnimation(index, full)));
    return *animations.back();
}

Xform SimActor::placement() const {
    if (transform)
        return *transform;
    return {quatFromEulerXYZ(0, 0, heading), position};
}

void SimActor::cue(double at, const Animation& anim, AnimPlayer::Options options) {
    auto it = cues.begin();
    while (it != cues.end() && it->at <= at)
        ++it;
    cues.insert(it, Cue{at, &anim, std::move(options)});
}

void SimActor::cueStop(double at, int priority, float fade) {
    auto it = cues.begin();
    while (it != cues.end() && it->at <= at)
        ++it;
    Cue c{at, nullptr, {}};
    c.options.priority = priority;
    c.options.fade = fade;
    cues.insert(it, std::move(c));
}

void SimActor::startCues(double now) {
    while (!cues.empty() && cues.front().at <= now) {
        const Cue& c = cues.front();
        if (c.anim)
            player.play(*c.anim, c.at, c.options);
        else
            player.stop(c.at, c.options.fade, c.options.priority);
        cues.erase(cues.begin());
    }
}

void SimActor::update(double now) {
    startCues(now);
    if (rootMotion) {
        const float d = player.advance(now);
        position[0] += -std::sin(heading) * d;
        position[1] += std::cos(heading) * d;
    }
    std::vector<Xform> local = player.pose(now);
    if (adjust)
        adjust(local, now);
    bones = skin.skeleton.world(local);
    const Xform at = placement();
    for (auto& w : bones)
        w = at.then(w);
    auto morphs = player.morphs(now);
    for (const auto& [name, w] : bodyMorphs)
        morphs[name] = w;
    skin.pose(bones, posed, morphs);
}

std::optional<Xform> SimActor::bonePlacement(std::string_view name, double now) {
    const int b = skin.skeleton.find(name);
    if (b < 0)
        return std::nullopt;
    startCues(now);
    const std::vector<Xform> world = skin.skeleton.world(player.pose(now));
    if (size_t(b) >= world.size())
        return std::nullopt;
    return placement().then(world[size_t(b)]);
}

std::map<std::string, float> bodyShapeMorphs(int16_t bodyFlags, int16_t age) {
    std::map<std::string, float> m;
    if (age <= 2)
        return m;
    const float fat = bodyFlags & 0x1 ? 1.0f : 0.0f;
    m["fattop"] = m["fatbot"] = fat;
    if (age >= 19 && age < 19 + 32) {
        const float preg = bodyFlags & 0x2 ? 1.0f : bodyFlags & 0x4 ? 0.5f : 0.0f;
        m["pregtop"] = m["pregbot"] = preg;
    }
    return m;
}

} // namespace os2
