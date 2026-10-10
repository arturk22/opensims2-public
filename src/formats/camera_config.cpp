#include "camera_config.hpp"

#include "config_text.hpp"

#include <algorithm>
#include <stdexcept>

namespace os2 {

namespace {

using Token = ConfigToken;

// A table operand: a parenthesised list or a single bare number.
std::vector<float> table(const Token& t) {
    return t.kind == Token::List ? t.list : std::vector<float>{std::stof(t.word)};
}

void applyDirective(CameraDef& c, const std::string& key, const std::vector<Token>& t) {
    if (key == "zoom" && t.size() >= 3) {
        c.defaultZoom = int(configNumber(t[1]));
        c.distances = table(t[2]);
    } else if (key == "fov" && t.size() >= 2) {
        c.fov = table(t[1]);
    } else if (key == "pitch" && t.size() >= 3) {
        c.pitchMin = table(t[1]);
        c.pitchMax = table(t[2]);
    } else if (key == "clip" && t.size() >= 3) {
        c.nearClip = table(t[1]);
        c.farClip = table(t[2]);
    } else if (key == "orientation" && t.size() >= 3) {
        c.defaultOrientation = int(configNumber(t[1]));
        c.yaws = table(t[2]);
    } else if (key == "smoothing" && t.size() >= 3) {
        c.zoomSmoothing = std::max(1, int(configNumber(t[1])));
        c.spinSmoothing = std::max(1, int(configNumber(t[2])));
    } else if (key == "slope" && t.size() >= 2) {
        c.slope = configNumber(t[1]);
    } else if (key == "wheel" && t.size() >= 2) {
        c.wheel = configNumber(t[1]);
    } else if (key == "setparam" && t.size() >= 3) {
        c.params[lowerAscii(t[1].word)] = configNumber(t[2]);
    }
    // Other directives (controller-specific bare parameters, "type",
    // "constrain", ...) are not needed yet.
}

void parseInto(std::vector<CameraDef>& cams, std::string_view text, const std::filesystem::path& dir) {
    int skipDepth = 0;
    forEachConfigLine(text, [&](std::string_view line) {
        std::vector<Token> t = tokenizeConfigLine(line);
        if (t.empty() || t[0].kind != Token::Word)
            return;
        const std::string key = lowerAscii(t[0].word);

        if (key == "if") {
            ++skipDepth;
            return;
        }
        if (key == "endif") {
            skipDepth = std::max(0, skipDepth - 1);
            return;
        }
        if (skipDepth > 0 || key == "version")
            return;

        if (key == "include" || key == "sinclude") {
            if (t.size() < 2)
                return;
            std::filesystem::path p = dir / t[1].word;
            if (!std::filesystem::exists(p)) {
                if (key == "sinclude")
                    return;
                throw std::runtime_error("camera config: missing include " + p.string());
            }
            parseInto(cams, readTextFile(p), p.parent_path());
            return;
        }
        if (key == "camera") {
            CameraDef c;
            if (t.size() > 1)
                c.name = t[1].word;
            if (t.size() > 2)
                c.controller = t[2].word;
            cams.push_back(std::move(c));
            return;
        }
        if (cams.empty())
            return; // directive before any camera
        try {
            applyDirective(cams.back(), key, t);
        } catch (const std::logic_error&) {
            // non-numeric operand (std::stof): not a directive we model
        }
    });
}

} // namespace

float CameraDef::param(std::string_view n, float fallback) const {
    auto it = params.find(lowerAscii(n));
    return it == params.end() ? fallback : it->second;
}

std::vector<CameraDef> parseCameraConfig(std::string_view text, const std::filesystem::path& dir) {
    std::vector<CameraDef> cams;
    parseInto(cams, text, dir);
    return cams;
}

std::vector<CameraDef> loadCameraConfig(const std::filesystem::path& path) {
    return parseCameraConfig(readTextFile(path), path.parent_path());
}

const CameraDef* findCamera(const std::vector<CameraDef>& cameras, std::string_view name) {
    const std::string want = lowerAscii(name);
    for (const auto& c : cameras)
        if (lowerAscii(c.name) == want)
            return &c;
    return nullptr;
}

} // namespace os2
