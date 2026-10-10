#include "lighting_config.hpp"

#include "config_text.hpp"

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
    // Indoor diffusion, portal modifiers, flat shading, fog, ... are not
    // modelled yet.
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

} // namespace os2
