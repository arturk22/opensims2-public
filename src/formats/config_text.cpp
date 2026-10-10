#include "config_text.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace os2 {

std::vector<ConfigToken> tokenizeConfigLine(std::string_view line) {
    std::vector<ConfigToken> out;
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
                throw std::runtime_error("config: unterminated '('");
            ConfigToken t{ConfigToken::List, {}, {}};
            std::string body(line.substr(i + 1, end - i - 1));
            for (char& ch : body)
                if (ch == ',')
                    ch = ' ';
            const char* p = body.c_str();
            for (char* next = nullptr;; p = next) {
                float v = std::strtof(p, &next);
                if (next == p)
                    break;
                t.list.push_back(v);
            }
            out.push_back(std::move(t));
            i = end + 1;
        } else if (c == '"') {
            size_t end = line.find('"', i + 1);
            if (end == std::string_view::npos)
                throw std::runtime_error("config: unterminated string");
            out.push_back({ConfigToken::Word, std::string(line.substr(i + 1, end - i - 1)), {}});
            i = end + 1;
        } else {
            size_t end = i;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '(' &&
                   line[end] != '#')
                ++end;
            out.push_back({ConfigToken::Word, std::string(line.substr(i, end - i)), {}});
            i = end;
        }
    }
    return out;
}

float configNumber(const ConfigToken& t) {
    if (t.kind == ConfigToken::List)
        return t.list.empty() ? 0.0f : t.list.front();
    return std::stof(t.word);
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace os2
