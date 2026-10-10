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
    return {quatFromEulerXYZ(0, 0, heading), position};
}

void SimActor::cue(double at, const Animation& anim, AnimPlayer::Options options) {
    auto it = cues.begin();
    while (it != cues.end() && it->at <= at)
        ++it;
    cues.insert(it, Cue{at, &anim, std::move(options)});
}

void SimActor::update(double now) {
    while (!cues.empty() && cues.front().at <= now) {
        player.play(*cues.front().anim, cues.front().at, cues.front().options);
        cues.erase(cues.begin());
    }
    if (rootMotion) {
        const float d = player.advance(now);
        position[0] += -std::sin(heading) * d;
        position[1] += std::cos(heading) * d;
    }
    std::vector<Xform> world = skin.skeleton.world(player.pose(now));
    const Xform at = placement();
    for (auto& w : world)
        w = at.then(w);
    skin.pose(world, posed);
}

} // namespace os2
