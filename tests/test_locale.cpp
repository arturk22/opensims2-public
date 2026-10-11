// Locales: ResourceIndex prefers Res/Locale/<locale> and leaves the other
// locales out; language codes and the preferred string language.
#include "formats/dbpf.hpp"
#include "formats/locale.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using namespace os2;
namespace fs = std::filesystem;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static const Tgi kShared{0x53545223, 0x7F000001, 1, 0}, kCzechOnly{0x53545223, 0x7F000001, 2, 0};

static void package(const fs::path& path, std::initializer_list<std::pair<Tgi, std::string>> resources) {
    fs::create_directories(path.parent_path());
    DbpfWriter w;
    for (const auto& [tgi, text] : resources)
        w.add(tgi, std::vector<uint8_t>(text.begin(), text.end()));
    w.save(path);
}

static std::string text(ResourceIndex& index, const Tgi& tgi) {
    auto bytes = index.read(tgi);
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("(none)");
}

static void testResourceIndex() {
    const fs::path root = fs::temp_directory_path() / "os2_test_locale" / "Res";
    fs::remove_all(root.parent_path());
    package(root / "UI/ui.package", {{kShared, "base"}});
    package(root / "Locale/Brazilian Portuguese/UI/ui.package", {{kShared, "pt-br"}});
    package(root / "Locale/Czech/UI/ui.package", {{kShared, "cz"}, {kCzechOnly, "cz"}});
    std::ofstream(root / "Locale/Czech/UI/FontStyle.ini") << "x";

    ResourceIndex br(root, "Brazilian Portuguese");
    check(br.locale() == "Brazilian Portuguese" && br.packageCount() == 2, "only the chosen locale is indexed");
    check(text(br, kShared) == "pt-br", "the locale's package replaces the base game's");
    check(text(br, kCzechOnly) == "(none)", "other locales are left out");

    ResourceIndex cz(root, "Czech");
    check(text(cz, kShared) == "cz" && text(cz, kCzechOnly) == "cz", "another locale");
    ResourceIndex none(root, "Klingon");
    check(text(none, kShared) == "base" && none.packageCount() == 1, "a locale without a folder: the base game");
    ResourceIndex sub(root / "UI");
    check(sub.locale().empty() && text(sub, kShared) == "base", "a root without a Locale folder");

    check(availableLocales(root) == std::vector<std::string>{"Brazilian Portuguese", "Czech"}, "available locales");
    check(localizedFile(root, "UI/FontStyle.ini", "Czech") == root / "Locale/Czech/UI/FontStyle.ini" &&
              localizedFile(root, "UI/FontStyle.ini", "Thai") == root / "UI/FontStyle.ini",
          "localized file");
    fs::remove_all(root.parent_path());
}

static void testLanguages() {
    check(localeLanguage("Brazilian Portuguese") == lang::BrazilianPortuguese && localeLanguage("English") == 1 &&
              localeLanguage("Czech") == 26 && localeLanguage("Korean") == 20 && localeLanguage("Nowhere") == 0,
          "locale language codes");
    StringTable t;
    t.entries = {{1, "Trash", "", 0}, {14, "Lixo PT", "", 0}, {35, "Lixo", "", 0}, {26, "Odpad", "", 0}};
    lang::setPreferred(lang::BrazilianPortuguese);
    check(t.get(0) == "Lixo", "preferred language: Brazilian Portuguese");
    lang::setPreferred(26);
    check(t.get(0) == "Odpad", "preferred language: Czech");
    lang::setPreferred(3);
    check(t.get(0) == "Trash", "a language without strings falls back to US English");
    t.entries = {{1, "Trash", "", 0}, {35, "Lixo", "", 0}};
    check(t.get(0, lang::Portuguese) == "Lixo", "Portuguese falls back to Brazilian Portuguese");
    lang::setPreferred(lang::BrazilianPortuguese);
}

int main() {
    testResourceIndex();
    testLanguages();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("locale: all tests passed\n");
    return failures ? 1 : 0;
}
