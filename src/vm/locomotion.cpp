#include "locomotion.hpp"

#include "animation.hpp"
#include "vm.hpp"

#include "formats/strings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace os2::vm {

namespace {

AnimationLibrary& clips(VM& vm) {
    if (!vm.animations.index())
        vm.animations.setIndex(vm.code().index());
    return vm.animations;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

// One name of a set's string: "turnInPlace-alt@1.5(turnInPlace@1.5)" is
// the name "turnInPlace-alt", the role "turnInPlace", played 1.5 times as
// fast.
struct Item {
    std::string name, role;
    float speed = 1;
};

std::vector<Item> setString(VM& vm, const Object& sim, bool run, bool turns) {
    LocomotionSet set{kGlobalGroup, 0};
    if (!sim.animation.locomotion.empty())
        set = sim.animation.locomotion.back();
    const StringTable* table = vm.code().strings(set.group, animlist::Locomotion);
    if (!table)
        return {};
    const size_t index = size_t(set.set) * 6 + (run ? 3 : 0) + (turns ? 0 : 1);
    std::vector<Item> out;
    const std::string s = table->get(index, lang::EnglishUS);
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos)
            end = s.size();
        std::string text = trimmed(s.substr(start, end - start));
        start = end + 1;
        if (text.empty())
            continue;
        Item it;
        if (const size_t open = text.find('('); open != std::string::npos) {
            const size_t close = text.find(')', open);
            it.role = trimmed(text.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1));
            text = trimmed(text.substr(0, open));
        }
        if (const size_t at = text.find('@'); at != std::string::npos) {
            it.speed = std::max(0.01f, std::strtof(text.c_str() + at + 1, nullptr));
            text.resize(at);
        }
        if (const size_t at = it.role.find('@'); at != std::string::npos)
            it.role.resize(at);
        it.name = trimmed(text);
        out.push_back(std::move(it));
    }
    return out;
}

// The resource for a set's name: "<age>-<gender>-<name>", "<age>-<name>",
// then the adult ones.
std::string resolve(VM& vm, const Object& sim, const std::string& name) {
    AnimationLibrary& lib = clips(vm);
    const std::string p = agePrefix(sim), g = genderName(sim);
    for (const std::string& c : {p + "-" + g + "-" + name, p + "-" + name, "a-" + g + "-" + name, "a-" + name})
        if (lib.exists(c))
            return c;
    return {};
}

float weightOf(const Object& sim, int field) {
    const int16_t v = sim.person && size_t(field) < sim.person->data.size() ? sim.person->data[size_t(field)] : 0;
    return std::clamp(float(v) / 100.0f, 0.0f, 1.0f);
}

} // namespace

namespace {
// What the gait and the clips depend on: the set on top of the stack, the
// age and sex, the walk style (and for the gait the six weights).
std::string cacheKey(const Object& sim) {
    LocomotionSet set{kGlobalGroup, 0};
    if (!sim.animation.locomotion.empty())
        set = sim.animation.locomotion.back();
    std::string key = std::to_string(set.group) + ':' + std::to_string(set.set) + ':' + agePrefix(sim) + ':' +
                      genderName(sim) + ':' + std::to_string(sim.data[17]);
    return key;
}
} // namespace

Gait computeGait(VM& vm, const Object& sim, bool shortWay);
std::optional<LocoPick> computeClip(VM& vm, const Object& sim, const std::string& role, int steps);

Gait gaitOf(VM& vm, const Object& sim, bool shortWay) {
    std::string key = cacheKey(sim) + (shortWay ? ":s" : ":w");
    for (int f = 112; f <= 117; ++f)
        key += ':' + std::to_string(sim.person && size_t(f) < sim.person->data.size() ? sim.person->data[size_t(f)] : 0);
    auto& cache = vm.locoCache.gaits;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() > 4096)
        cache.clear();
    Gait g = computeGait(vm, sim, shortWay);
    if (vm.animations.index() || !g.animation.empty())
        cache.emplace(std::move(key), g);
    return g;
}

Gait computeGait(VM& vm, const Object& sim, bool shortWay) {
    Gait g;
    g.run = sim.data[17] == 1;
    const std::vector<Item> items = setString(vm, sim, g.run, false);
    const std::string kind = g.run ? "run" : "walk";
    // The cycles "<walk|run>[Short]-alt<k>-..." (not the blends).
    std::array<std::string, 20> cycle;
    const std::string prefix = kind + (shortWay ? "Short" : "") + "-alt";
    for (const auto& it : items) {
        if (!it.name.starts_with(prefix) || it.name.find("Blend") != std::string::npos)
            continue;
        const int k = std::atoi(it.name.c_str() + prefix.size());
        if (k >= 0 && k < 20 && cycle[size_t(k)].empty())
            cycle[size_t(k)] = it.name;
    }
    std::array<float, 20> w{};
    const float mood = weightOf(sim, 117), rest = 1 - mood;
    w[0] = weightOf(sim, 112) * rest;
    w[1] = weightOf(sim, 113) * rest;
    const int16_t personality = sim.person ? sim.person->data[114] : 0;
    if (personality >= 2 && personality < 20)
        w[size_t(personality)] = weightOf(sim, 115) * rest;
    const int16_t moodIndex = sim.person ? sim.person->data[116] : 0;
    if (moodIndex >= 2 && moodIndex < 20)
        w[size_t(moodIndex)] += mood;
    std::array<float, 20> speed{}, secs{};
    std::array<std::string, 20> resource;
    float total = 0;
    for (size_t k = 0; k < 20; ++k) {
        if (cycle[k].empty() || (k != 0 && w[k] <= 0.01f))
            continue;
        resource[k] = resolve(vm, sim, cycle[k]);
        speed[k] = resource[k].empty() ? 0 : clips(vm).tilesPerSecond(resource[k]);
        secs[k] = resource[k].empty() ? 0 : clips(vm).seconds(resource[k]).value_or(0);
        if (speed[k] > 0 && w[k] > 0)
            total += w[k];
    }
    if (total > 0) {
        size_t best = 0;
        float seconds = 0;
        for (size_t k = 0; k < 20; ++k) {
            if (speed[k] <= 0 || w[k] <= 0)
                continue;
            g.speed += speed[k] * w[k] / total;
            seconds += secs[k] * w[k] / total;
            if (w[k] > w[best] || speed[best] <= 0)
                best = k;
        }
        g.animation = resource[best];
        g.seconds = seconds;
        for (size_t k = 0; k < 20; ++k)
            if (speed[k] > 0 && w[k] > 0)
                g.cycles.push_back({resource[k], w[k] / total, false, seconds > 0 && secs[k] > 0 ? secs[k] / seconds : 1.0f});
        // The heaviest first (the base layer the others blend over).
        std::stable_sort(g.cycles.begin(), g.cycles.end(), [](const LocoLayer& a, const LocoLayer& b) { return a.weight > b.weight; });
    } else if (!resource[0].empty() && speed[0] > 0) {
        // Every weight zero: alt0's speed (the set's speed falls back to it).
        g.animation = resource[0];
        g.speed = speed[0];
        g.seconds = secs[0];
        g.cycles.push_back({resource[0], 1, false, 1});
    }
    if (g.animation.empty() || g.speed <= 0) {
        if (shortWay)
            return Gait{};
        // No weighed cycle (a set without alternates, the swimming or the
        // ghosts' private one): the role's walk or run.
        g = Gait{};
        g.run = sim.data[17] == 1;
        g.animation = locomotionAnimation(vm, sim, kind);
        g.speed = g.animation.empty() ? 0 : clips(vm).tilesPerSecond(g.animation);
        g.seconds = g.animation.empty() ? 0 : clips(vm).seconds(g.animation).value_or(0);
        if (!g.animation.empty())
            g.cycles.push_back({g.animation, 1, false, 1});
    }
    return g;
}

std::optional<LocoPick> locoClip(VM& vm, const Object& sim, const std::string& role, int steps) {
    std::string key = cacheKey(sim) + ':' + role + ':' + std::to_string(steps);
    auto& cache = vm.locoCache.clips;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() > 4096)
        cache.clear();
    auto pick = computeClip(vm, sim, role, steps);
    if (vm.animations.index() || pick)
        cache.emplace(std::move(key), pick);
    return pick;
}

std::optional<LocoPick> computeClip(VM& vm, const Object& sim, const std::string& role, int steps) {
    const bool run = sim.data[17] == 1;
    const std::vector<Item> items = setString(vm, sim, run, true);
    const std::string degrees = std::to_string(45 * std::min(std::abs(steps), 4));
    for (const std::string& r : {role, role.ends_with("R") ? role.substr(0, role.size() - 1) : std::string()}) {
        if (r.empty())
            continue;
        for (const auto& it : items) {
            if (it.role != r)
                continue;
            // "turnAndGo-alt" -> "turnAndGo-alt-90"; "step-alt" and
            // "turnInPlace-alt" name "step-90" and "TurnInPlace-90" (the
            // resource names are not case sensitive).
            std::string base = it.name;
            std::vector<std::string> names{base + "-" + degrees};
            if (base.ends_with("-alt"))
                names.push_back(base.substr(0, base.size() - 4) + "-" + degrees);
            for (const auto& n : names)
                if (std::string res = resolve(vm, sim, n); !res.empty())
                    return LocoPick{res, steps > 0, it.speed};
        }
    }
    return std::nullopt;
}

} // namespace os2::vm
