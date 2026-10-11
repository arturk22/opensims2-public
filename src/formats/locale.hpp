#pragma once

// The game's locales: TSData/Res/Locale/<name>/ holds what each language
// replaces or adds (layout of the 2004 PT-BR CD, 20 locales):
//   UI/ui.package        UI scripts, images and strings (15 locales; a
//                        DBSegment of Sys/default.ini: Res\Locale\%Language%\UI\ui.package)
//   UI/CaSIEUI.data      Body Shop UI (8; Sys/CSResources.ini)
//   UI/Fonts/FontStyle.ini  font styles of the Asian locales (5)
//   Sims3D/Objects.package  objects (9; not Brazilian Portuguese)
//   Sound/Sfx2.package   voices and sounds (18)
//   Bins/<name> Bin.bundle.package  (Japanese, Korean, Traditional Chinese)
//   Config/globalProps.xml, Movies/ealogo_audio.movie
// Strings (STR#, CTSS, TTAs) carry every language in the same resource, by
// language code.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

constexpr std::string_view kDefaultLocale = "Brazilian Portuguese";

// The locale to use: the environment variable OS2_LOCALE (a folder name
// under Res/Locale, e.g. "English"), else Brazilian Portuguese.
std::string defaultLocale();

// Language code of the STR#/CTSS/TTAs entries of a locale ("Brazilian
// Portuguese" -> 35), 0 if unknown. Checked against the game's catalog strings
// (every locale with strings but UK English, which falls back to US English).
uint8_t localeLanguage(std::string_view locale);

// The locale folders under Res/Locale, sorted.
std::vector<std::string> availableLocales(const std::filesystem::path& resRoot);

// A file as the locale has it: Res/Locale/<locale>/<relative> if it exists,
// else Res/<relative> (e.g. "UI/Fonts/FontStyle.ini").
std::filesystem::path localizedFile(const std::filesystem::path& resRoot, const std::filesystem::path& relative,
                                    std::string_view locale = kDefaultLocale);

} // namespace os2
