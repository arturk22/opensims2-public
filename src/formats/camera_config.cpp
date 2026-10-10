#include "camera_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace os2 {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A line split into words, quoted strings and parenthesised lists.
struct Token {
    enum Kind { Word, List } kind;
    std::string word;
    std::vector<float> list;
};

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '#') {
            break;
        } else if (c == '(') {
            size_t end = line.find(')', i);
            if (end == std::string_view::npos)
                throw std::runtime_error("camera config: unterminated '('");
            Token t{Token::List, {}, {}};
            std::istringstream in(std::string(line.substr(i + 1, end - i - 1)));
            float v;
            while (in >> v)
                t.list.push_back(v);
            out.push_back(std::move(t));
            i = end + 1;
        } else if (c == '"') {
            size_t end = line.find('"', i + 1);
            if (end == std::string_view::npos)
                throw std::runtime_error("camera config: unterminated string");
            out.push_back({Token::Word, std::string(line.substr(i + 1, end - i - 1)), {}});
            i = end + 1;
        } else {
            size_t end = i;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '(' &&
                   line[end] != '#')
                ++end;
            out.push_back({Token::Word, std::string(line.substr(i, end - i)), {}});
            i = end;
        }
    }
    return out;
}

float number(const Token& t) {
    if (t.kind == Token::List)
        return t.list.empty() ? 0.0f : t.list.front();
    return std::stof(t.word);
}

// A table operand: a parenthesised list or a single bare number.
std::vector<float> table(const Token& t) {
    return t.kind == Token::List ? t.list : std::vector<float>{std::stof(t.word)};
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

void applyDirective(CameraDef& c, const std::string& key, const std::vector<Token>& t) {
    if (key == "zoom" && t.size() >= 3) {
        c.defaultZoom = int(number(t[1]));
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
        c.defaultOrientation = int(number(t[1]));
        c.yaws = table(t[2]);
    } else if (key == "smoothing" && t.size() >= 3) {
        c.zoomSmoothing = std::max(1, int(number(t[1])));
        c.spinSmoothing = std::max(1, int(number(t[2])));
    } else if (key == "slope" && t.size() >= 2) {
        c.slope = number(t[1]);
    } else if (key == "wheel" && t.size() >= 2) {
        c.wheel = number(t[1]);
    } else if (key == "setparam" && t.size() >= 3) {
        c.params[lower(t[1].word)] = number(t[2]);
    }
    // Other directives (controller-specific bare parameters, "type",
    // "constrain", ...) are not needed yet.
}

void parseInto(std::vector<CameraDef>& cams, std::string_view text, const std::filesystem::path& dir) {
    int skipDepth = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;

        std::vector<Token> t = tokenize(line);
        if (t.empty() || t[0].kind != Token::Word)
            continue;
        const std::string key = lower(t[0].word);

        if (key == "if") {
            ++skipDepth;
            continue;
        }
        if (key == "endif") {
            skipDepth = std::max(0, skipDepth - 1);
            continue;
        }
        if (skipDepth > 0 || key == "version")
            continue;

        if (key == "include" || key == "sinclude") {
            if (t.size() < 2)
                continue;
            std::filesystem::path p = dir / t[1].word;
            if (!std::filesystem::exists(p)) {
                if (key == "sinclude")
                    continue;
                throw std::runtime_error("camera config: missing include " + p.string());
            }
            parseInto(cams, readFile(p), p.parent_path());
            continue;
        }
        if (key == "camera") {
            CameraDef c;
            if (t.size() > 1)
                c.name = t[1].word;
            if (t.size() > 2)
                c.controller = t[2].word;
            cams.push_back(std::move(c));
            continue;
        }
        if (cams.empty())
            continue; // directive before any camera
        try {
            applyDirective(cams.back(), key, t);
        } catch (const std::logic_error&) {
            // non-numeric operand (std::stof): not a directive we model
        }
    }
}

} // namespace

float CameraDef::param(std::string_view n, float fallback) const {
    auto it = params.find(lower(n));
    return it == params.end() ? fallback : it->second;
}

std::vector<CameraDef> parseCameraConfig(std::string_view text, const std::filesystem::path& dir) {
    std::vector<CameraDef> cams;
    parseInto(cams, text, dir);
    return cams;
}

std::vector<CameraDef> loadCameraConfig(const std::filesystem::path& path) {
    return parseCameraConfig(readFile(path), path.parent_path());
}

const CameraDef* findCamera(const std::vector<CameraDef>& cameras, std::string_view name) {
    const std::string want = lower(name);
    for (const auto& c : cameras)
        if (lower(c.name) == want)
            return &c;
    return nullptr;
}

} // namespace os2
