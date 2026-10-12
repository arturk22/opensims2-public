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

std::vector<Item> setString(VM& vm, const Object& sim, bool run, bool turns, bool blends = false) {
    LocomotionSet set{kGlobalGroup, 0};
    if (!sim.animation.locomotion.empty())
        set = sim.animation.locomotion.back();
    const StringTable* table = vm.code().strings(set.group, animlist::Locomotion);
    if (!table)
        return {};
    const size_t index = size_t(set.set) * 6 + (run ? 3 : 0) + (blends ? 2 : turns ? 0 : 1);
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
        // The cycles' poses ("walk-alt<k>-...Blend"), with the long and the
        // short cycles alike (both are type 100 + k): delta channels only
        // (target type 6), which the player multiplies onto the cycles
        // (AnimPlayer, docs/primitives-mac.md §4) at the cycle's weight.
        // OS2_NO_LOCO_POSES=1 leaves them out (to compare).
        if (!shortWay && !std::getenv("OS2_NO_LOCO_POSES")) {
            const std::string posePrefix = kind + "-alt";
            for (const auto& it : setString(vm, sim, g.run, false, true)) {
                if (!it.name.starts_with(posePrefix) || !it.name.ends_with("Blend"))
                    continue;
                const int k = std::atoi(it.name.c_str() + posePrefix.size());
                if (k < 0 || k >= 20 || speed[size_t(k)] <= 0 || w[size_t(k)] <= 0)
                    continue;
                if (std::string res = resolve(vm, sim, it.name); !res.empty())
                    g.poses.push_back({res, w[size_t(k)] / total, false, 1});
            }
        }
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
    if (role == "stepInPlace") {
        // In the walks' string, of no angle ("stepInPlace-alt" names
        // "a-stepInPlace").
        for (const auto& it : setString(vm, sim, run, false)) {
            if (it.role != role)
                continue;
            std::vector<std::string> names{it.name};
            if (it.name.ends_with("-alt"))
                names.push_back(it.name.substr(0, it.name.size() - 4));
            for (const auto& n : names)
                if (std::string res = resolve(vm, sim, n); !res.empty())
                    return LocoPick{res, false, it.speed};
        }
        return std::nullopt;
    }
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

Gait blendGaits(const Gait& full, const Gait& shortGait, float w) {
    w = std::clamp(w, 0.0f, 1.0f);
    if (shortGait.cycles.empty() || shortGait.seconds <= 0 || w <= 0)
        return full;
    if (full.cycles.empty() || full.seconds <= 0 || w >= 1) {
        Gait g = shortGait;
        g.poses = full.poses;
        return g;
    }
    Gait g;
    g.run = full.run;
    g.seconds = (1 - w) * full.seconds + w * shortGait.seconds;
    const float stride = (1 - w) * full.speed * full.seconds + w * shortGait.speed * shortGait.seconds;
    g.speed = stride / g.seconds;
    for (const LocoLayer& l : full.cycles)
        g.cycles.push_back({l.animation, l.weight * (1 - w), l.mirror, l.rate * full.seconds / g.seconds});
    for (const LocoLayer& l : shortGait.cycles)
        g.cycles.push_back({l.animation, l.weight * w, l.mirror, l.rate * shortGait.seconds / g.seconds});
    std::stable_sort(g.cycles.begin(), g.cycles.end(), [](const LocoLayer& a, const LocoLayer& b) { return a.weight > b.weight; });
    g.animation = g.cycles.front().animation;
    g.poses = full.poses;
    return g;
}

float strideWeight(const Gait& full, const Gait& shortGait, float stretch, float clipsLong, float clipsShort) {
    if (shortGait.cycles.empty() || full.seconds <= 0 || shortGait.seconds <= 0)
        return 0;
    const float sLong = full.speed * full.seconds, sShort = shortGait.speed * shortGait.seconds;
    if (sLong - sShort <= 1e-4f)
        return 0;
    const float avail = stretch - clipsLong;
    const float halves = avail <= 0 ? 0 : std::ceil(avail / (0.5f * sLong) - 1e-4f);
    const float denominator = (clipsLong - clipsShort) + halves * 0.5f * (sLong - sShort);
    if (denominator <= 1e-5f)
        return avail < 0 ? 1.0f : 0.0f;
    return std::clamp((halves * 0.5f * sLong - avail) / denominator, 0.0f, 1.0f);
}

namespace {
// The uniform cubic B-spline's basis and its derivative at u.
std::array<float, 4> basis(float u) {
    const float v = 1 - u, u2 = u * u, u3 = u2 * u;
    return {v * v * v / 6, (3 * u3 - 6 * u2 + 4) / 6, (-3 * u3 + 3 * u2 + 3 * u + 1) / 6, u3 / 6};
}
std::array<float, 4> basisSlope(float u) {
    const float v = 1 - u;
    return {-0.5f * v * v, 1.5f * u * u - 2 * u, -1.5f * u * u + u + 0.5f, 0.5f * u * u};
}
} // namespace

LocoCurve::LocoCurve(float x, float y, const std::vector<std::array<float, 2>>& through) {
    points = through.size();
    if (through.empty())
        return;
    control.reserve(points + 5);
    control.assign(3, {x, y});
    control.insert(control.end(), through.begin(), through.end());
    control.push_back(through.back());
    control.push_back(through.back());
    const size_t n = control.size() - 3;
    length.resize(n);
    after.assign(n, 0);
    table.resize(n * (kSamples + 1));
    for (size_t j = 0; j < n; ++j) {
        std::array<float, 2> prev = at(j, 0);
        float sum = 0;
        table[j * (kSamples + 1)] = 0;
        for (int k = 1; k <= kSamples; ++k) {
            const std::array<float, 2> p = at(j, float(k) / kSamples);
            sum += std::hypot(p[0] - prev[0], p[1] - prev[1]);
            table[j * (kSamples + 1) + size_t(k)] = sum;
            prev = p;
        }
        length[j] = sum;
    }
    for (size_t j = n - 1; j > 0; --j)
        after[j - 1] = after[j] + length[j];
}

std::array<float, 2> LocoCurve::at(size_t j, float u) const {
    const auto b = basis(u);
    std::array<float, 2> p{0, 0};
    for (size_t k = 0; k < 4; ++k)
        p[0] += b[k] * control[j + k][0], p[1] += b[k] * control[j + k][1];
    return p;
}

std::array<float, 2> LocoCurve::slope(size_t j, float u) const {
    const auto b = basisSlope(u);
    std::array<float, 2> p{0, 0};
    for (size_t k = 0; k < 4; ++k)
        p[0] += b[k] * control[j + k][0], p[1] += b[k] * control[j + k][1];
    return p;
}

float LocoCurve::parameterAt(size_t j, float arc) const {
    const float* t = &table[j * (kSamples + 1)];
    if (arc <= 0)
        return 0;
    if (arc >= t[kSamples])
        return 1;
    int k = 1;
    while (k < kSamples && t[k] < arc)
        ++k;
    const float span = t[k] - t[k - 1];
    const float f = span > 1e-7f ? (arc - t[k - 1]) / span : 0;
    return (float(k - 1) + f) / kSamples;
}

size_t LocoCurve::passedAfter(size_t j) const {
    // Segment j runs from near control point j + 1 to near j + 2, and
    // control point 3 + k is route point k.
    const size_t m = points;
    if (j + 1 >= segments())
        return m; // the last one ends on the last point
    if (j < 1)
        return 0;
    return std::min(j, m > 0 ? m - 1 : 0);
}

float LocoCurve::remaining(size_t j, float arc) const {
    if (j >= segments())
        return 0;
    return std::max(0.0f, length[j] - arc) + after[j];
}

} // namespace os2::vm
