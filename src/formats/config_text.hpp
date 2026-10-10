#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// Shared lexing for the game's line-based text configurations (cameras,
// lighting): each line is words, quoted strings and parenthesised number
// lists ("(1, .94, .875)" or "(70 40 25)"); '#' starts a comment.
struct ConfigToken {
    enum Kind { Word, List } kind;
    std::string word;
    std::vector<float> list;
};

std::vector<ConfigToken> tokenizeConfigLine(std::string_view line);

// First number of a list, or the word parsed as a float (throws std::logic_error if it is not one).
float configNumber(const ConfigToken& t);

// Calls `fn` for each line of `text`.
template <typename Fn>
void forEachConfigLine(std::string_view text, Fn&& fn) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        fn(text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    }
}

std::string lowerAscii(std::string_view s);
std::string readTextFile(const std::filesystem::path& path);

} // namespace os2
