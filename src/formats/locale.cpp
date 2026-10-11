#include "locale.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace os2 {

std::string defaultLocale() {
    const char* env = std::getenv("OS2_LOCALE");
    return env && *env ? std::string(env) : std::string(kDefaultLocale);
}

uint8_t localeLanguage(std::string_view locale) {
    static constexpr std::pair<std::string_view, uint8_t> codes[] = {
        {"English", 1},   {"UK English", 2}, {"French", 3},     {"German", 4},
        {"Italian", 5},   {"Spanish", 6},    {"Dutch", 7},      {"Danish", 8},
        {"Swedish", 9},   {"Norwegian", 10}, {"Finnish", 11},   {"Portuguese", 14},
        {"Japanese", 15}, {"Polish", 16},    {"Simplified Chinese", 17}, {"Traditional Chinese", 18},
        {"Thai", 19},     {"Korean", 20},    {"Czech", 26},     {"Brazilian Portuguese", 35},
    };
    for (const auto& [name, code] : codes)
        if (name == locale)
            return code;
    return 0;
}

std::vector<std::string> availableLocales(const std::filesystem::path& resRoot) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(resRoot / "Locale", ec))
        if (e.is_directory())
            out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

std::filesystem::path localizedFile(const std::filesystem::path& resRoot, const std::filesystem::path& relative,
                                    std::string_view locale) {
    const auto local = resRoot / "Locale" / std::string(locale) / relative;
    return std::filesystem::exists(local) ? local : resRoot / relative;
}

} // namespace os2
