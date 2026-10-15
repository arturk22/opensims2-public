#include "lighting_config.hpp"

#include "config_text.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace os2 {

namespace {

std::array<float, 3> vec3(const ConfigToken& t) {
    if (t.kind != ConfigToken::List || t.list.size() != 3)
        throw std::invalid_argument("expected (x, y, z)");
    return {t.list[0], t.list[1], t.list[2]};
}

void applyDirective(LightingState& s, const std::string& key, const std::vector<ConfigToken>& t) {
    if (key == "skylight" && t.size() >= 3) {
        s.skyColor = vec3(t[1]);
        s.skyStrength = configNumber(t[2]);
    } else if (key == "light" && t.size() >= 5) {
        LightingState::Light l;
        l.name = t[1].word;
        l.direction = vec3(t[2]);
        l.color = vec3(t[3]);
        l.intensity = configNumber(t[4]);
        for (auto& old : s.lights)
            if (lowerAscii(old.name) == lowerAscii(l.name)) {
                old = std::move(l);
                return;
            }
        s.lights.push_back(std::move(l));
    }
    else if (key == "modifiers" && t.size() >= 4) {
        LightingState::Modifier m;
        m.practical = configNumber(t[2]);
        m.diffusion = configNumber(t[3]);
        if (t.size() >= 6) {
            m.practicalFalloff = configNumber(t[4]);
            m.diffusionFalloff = configNumber(t[5]);
        }
        s.modifiers[lowerAscii(t[1].word)] = m;
    } else if (key == "unlitroomcolor" && t.size() >= 2) {
        LightingState::UnlitRoom u;
        u.color = vec3(t[1]);
        for (size_t i = 2; i + 1 < t.size(); ++i)
            if (lowerAscii(t[i].word) == "-threshold")
                u.threshold = configNumber(t[i + 1]);
        s.unlitRooms.push_back(u);
    } else if (key == "transitiontimes" && t.size() >= 3) {
        s.transitionIn = configNumber(t[1]);
        s.transitionOut = configNumber(t[2]);
    } else if (key == "environment" && t.size() >= 2) {
        const auto c = vec3(t[1]);
        const float k = t.size() >= 3 ? configNumber(t[2]) : 1.0f;
        s.environment = {c[0] * k, c[1] * k, c[2] * k};
    } else if (key == "projectiveshadowstrength" && t.size() >= 2) {
        s.projectiveShadowStrength = configNumber(t[1]);
    } else if (key == "diffparam" && t.size() >= 3) {
        s.diffParams[lowerAscii(t[1].word)] = t[2].kind == ConfigToken::List ? t[2].list : std::vector<float>{configNumber(t[2])};
    }
    // Flat shading, fog, ... are not modelled yet.
}

} // namespace

std::vector<LightingState::Light> LightingState::objectLights() const {
    std::vector<Light> out;
    for (const auto& l : lights)
        if (!lowerAscii(l.name).starts_with("simfill"))
            out.push_back(l);
    return out;
}

const LightingState* LightingConfig::state(std::string_view name) const {
    const std::string want = lowerAscii(name);
    for (const auto& s : states)
        if (lowerAscii(s.name) == want)
            return &s;
    return nullptr;
}

LightingConfig parseLightingConfig(std::string_view text) {
    LightingConfig cfg;
    LightingState* current = nullptr;
    forEachConfigLine(text, [&](std::string_view line) {
        const auto t = tokenizeConfigLine(line);
        if (t.empty() || t[0].kind != ConfigToken::Word)
            return;
        const std::string key = lowerAscii(t[0].word);
        if (key == "lightingstate") {
            if (t.size() < 2)
                return;
            LightingState s;
            s.name = t[1].word;
            if (t.size() >= 4 && t[2].word == ":") {
                if (const LightingState* parent = cfg.state(t[3].word))
                    s = *parent;
                s.name = t[1].word;
                s.parent = t[3].word;
            }
            cfg.states.push_back(std::move(s));
            current = &cfg.states.back();
            return;
        }
        if (key == "end") {
            current = nullptr;
            return;
        }
        if (!current) {
            if ((key == "set" || key == "setf" || key == "seti" || key == "setb") && t.size() >= 3) {
                const auto& v = t[2];
                std::string value = v.word;
                if (v.kind == ConfigToken::List) {
                    for (size_t i = 0; i < v.list.size(); ++i)
                        value += (i ? ", " : "(") + std::to_string(v.list[i]);
                    value += ")";
                }
                cfg.globals[lowerAscii(t[1].word)] = value;
            }
            return;
        }
        try {
            applyDirective(*current, key, t);
        } catch (const std::logic_error&) {
            // malformed operand: ignore the line, as for unknown directives
        }
    });
    return cfg;
}

LightingConfig loadLightingConfig(const std::filesystem::path& path) {
    return parseLightingConfig(readTextFile(path));
}

namespace {

struct OverrideParser {
    std::filesystem::path dir;
    LightOverrides out;
    struct Macro {
        std::string param;
        std::vector<std::string> lines;
    };
    std::map<std::string, Macro> macros;
    int depth = 0;

    // "Name(arg)" -> {name, arg}
    static bool call(std::string_view s, std::string& name, std::string& arg) {
        const size_t open = s.find('('), close = s.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            return false;
        auto trim = [](std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
                v.remove_suffix(1);
            return std::string(v);
        };
        name = lowerAscii(trim(s.substr(0, open)));
        arg = trim(s.substr(open + 1, close - open - 1));
        return !name.empty();
    }

    void override(std::string_view line) {
        const auto t = tokenizeConfigLine(line);
        if (t.size() < 4 || t[0].kind != ConfigToken::Word || lowerAscii(t[0].word) != "lightattriboverride")
            return;
        std::vector<float> v;
        if (t[3].kind == ConfigToken::List)
            v = t[3].list;
        else
            try {
                v.push_back(configNumber(t[3]));
            } catch (const std::logic_error&) {
                return;
            }
        out[lowerAscii(t[1].word)][lowerAscii(t[2].word)] = v;
    }

    void parse(std::string_view text) {
        Macro* defining = nullptr;
        forEachConfigLine(text, [&](std::string_view raw) {
            std::string_view line = raw.substr(0, raw.find('#'));
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            if (line.empty())
                return;
            const std::string lower = lowerAscii(line.substr(0, std::min<size_t>(line.size(), 16)));
            if (lower.starts_with("enddef")) {
                defining = nullptr;
            } else if (defining) {
                defining->lines.emplace_back(line);
            } else if (lower.starts_with("define ")) {
                std::string name, param;
                if (call(line.substr(7), name, param))
                    defining = &(macros[name] = Macro{param, {}});
            } else if (lower.starts_with("create ")) {
                std::string name, arg;
                if (!call(line.substr(7), name, arg))
                    return;
                auto it = macros.find(name);
                if (it == macros.end())
                    return;
                for (std::string l : it->second.lines) {
                    const std::string ref = "&" + it->second.param;
                    for (size_t at; (at = l.find(ref)) != std::string::npos;)
                        l.replace(at, ref.size(), arg);
                    override(l);
                }
            } else if (lower.starts_with("sinclude ") || lower.starts_with("include ")) {
                const size_t q0 = line.find('"'), q1 = line.rfind('"');
                if (q0 == std::string_view::npos || q1 <= q0 || depth > 8 || dir.empty())
                    return;
                const auto file = dir / std::string(line.substr(q0 + 1, q1 - q0 - 1));
                std::error_code ec;
                if (!std::filesystem::exists(file, ec))
                    return;
                ++depth;
                parse(readTextFile(file));
                --depth;
            } else {
                override(line);
            }
        });
    }
};

} // namespace

LightOverrides parseLightOverrides(std::string_view text, const std::filesystem::path& includeDir) {
    OverrideParser p;
    p.dir = includeDir;
    p.parse(text);
    return std::move(p.out);
}

LightOverrides loadLightOverrides(const std::filesystem::path& lotLightsFile) {
    return parseLightOverrides(readTextFile(lotLightsFile), lotLightsFile.parent_path());
}

// FUN_0070daa8 (Mac BV), roomLightingType 2-8 (docs/lighting-mac.md 4, 11.1).
RoomLights roomLights(const std::array<std::array<float, 3>, 6>& I, const RoomLightTuning& t) {
    using V = std::array<float, 3>;
    auto add = [](V a, V b) { return V{a[0] + b[0], a[1] + b[1], a[2] + b[2]}; };
    auto mul = [](V a, float k) { return V{a[0] * k, a[1] * k, a[2] * k}; };
    const V sides = add(add(I[0], I[1]), add(I[2], I[3]));
    std::array<V, 6> L{};
    for (size_t i = 0; i < 4; ++i)
        L[i] = mul(add(I[i], mul(I[4], t.sideCrossover)), t.side * t.overall);
    for (size_t j = 4; j < 6; ++j)
        L[j] = mul(add(I[j], mul(sides, t.bounceCrossover)), t.bounce * t.overall);
    RoomLights out;
    for (const V& v : L) // FUN_00700710: the sum of the six / 6
        out.ambient = add(out.ambient, v);
    out.ambient = mul(out.ambient, 1.0f / 6.0f);
    for (V& v : L)
        v = add(v, mul(out.ambient, -1));
    // MaxAbsElement (0x1808d8c): max(|r|, |g|, |b|); FUN_00700590 / 0x700610
    // take the largest over the six (the four sides) directions, the first
    // of equals.
    auto maxAbs = [](const V& c) { return std::max({std::fabs(c[0]), std::fabs(c[1]), std::fabs(c[2])}); };
    static const V travel[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const int n = std::clamp(t.type, 0, 8), choices = n < 3 ? 4 : 6;
    for (int k = 0; k < n; ++k) {
        size_t best = 0;
        float bestValue = maxAbs(L[0]);
        for (size_t d = 1; d < size_t(choices); ++d)
            if (const float v = maxAbs(L[d]); bestValue < v)
                best = d, bestValue = v;
        V dir = travel[best];
        if (best < 4)
            dir[2] -= t.sideSlope; // the table's vector less sideSlope x (0, 0, 1)
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        RoomLights::Delta delta;
        delta.towardLight = {-dir[0] / len, -dir[1] / len, -dir[2] / len};
        delta.color = mul(L[best], t.directionalBoost);
        delta.specular = t.specular;
        out.deltas.push_back(delta);
        L[best] = V{0, 0, 0};
    }
    return out;
}

} // namespace os2
