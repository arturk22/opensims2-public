// s2pkg commands for UI scripts, UI images and fonts.
#include "ui_cmds.hpp"

#include "formats/font.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_screen.hpp"
#include "formats/ui_script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

namespace {

uint32_t parseHex(const char* s) { return uint32_t(std::stoul(s, nullptr, 16)); }

template <class K>
std::vector<std::pair<size_t, K>> byCount(const std::map<K, size_t>& m) {
    std::vector<std::pair<size_t, K>> v;
    for (const auto& [k, n] : m)
        v.emplace_back(n, k);
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return v;
}

bool isImageKey(const std::string& key) { return key.ends_with("image") && key != "edgeimage"; }
// Type 0 is shared with other resources; UI scripts live in the ui.package files.
bool isUiPackage(const ResourceIndex::Location& loc) { return loc.package->path().filename() == "ui.package"; }

bool isStringKey(const std::string& key) { return key == "captionres" || key == "tipres"; }

std::string oneLine(std::string s, size_t max = 60) {
    for (auto& c : s)
        if (c == '\r' || c == '\n')
            c = ' ';
    if (s.size() > max)
        s = s.substr(0, max) + "...";
    return s;
}

void dumpElement(UiResources& res, const UiElement& e, int depth, bool all) {
    const auto a = e.area();
    std::printf("%*s%s id=0x%08X area=(%d,%d,%d,%d)%s", depth * 2, "", e.clsid().c_str(), e.id(), a[0], a[1], a[2], a[3],
                e.visible() ? "" : " hidden");
    if (const auto* c = e.attr("caption"); c && !c->empty())
        std::printf(" caption=\"%s\"", oneLine(*c).c_str());
    for (const auto& [k, v] : e.attrs) {
        if (isStringKey(k)) {
            auto ref = uiStringRef(v);
            auto s = ref ? res.string(*ref) : std::nullopt;
            std::printf(" %s=%s%s%s%s", k.c_str(), v.c_str(), s ? "\"" : " (missing)", s ? oneLine(*s).c_str() : "", s ? "\"" : "");
        } else if (isImageKey(k)) {
            auto ref = uiResourceRef(v);
            const RgbaImage* img = ref ? res.image(*ref) : nullptr;
            if (img)
                std::printf(" %s=%s %ux%u", k.c_str(), v.c_str(), img->width, img->height);
            else
                std::printf(" %s=%s (missing)", k.c_str(), v.c_str());
        } else if (all && k != "clsid" && k != "id" && k != "area" && k != "caption" && !k.starts_with("winflag_")) {
            std::printf(" %s=%s", k.c_str(), oneLine(v).c_str());
        }
    }
    std::printf("\n");
    for (const auto& c : e.children)
        dumpElement(res, c, depth + 1, all);
}

int cmdUiList(const char* resDir) {
    ResourceIndex index(resDir);
    UiResources res(index, resDir);
    size_t scripts = 0, reia = 0;
    for (const auto& loc : index.ofType(kTypeUiScript)) {
        if (!isUiPackage(loc))
            continue;
        const auto bytes = index.read(loc);
        const Tgi& t = loc.entry->tgi;
        if (isReia(bytes)) {
            std::printf("%08X %08X  Reia video, %zu bytes\n", t.group, t.instance, bytes.size());
            ++reia;
            continue;
        }
        const UiScript s = UiScript::parse(bytes);
        ++scripts;
        std::string caption = s.roots.empty() ? "" : s.roots[0].get("caption");
        std::printf("%08X %08X  %4zu windows  %zu roots  \"%s\"  (%s)\n", t.group, t.instance, s.elementCount(),
                    s.roots.size(), oneLine(caption, 40).c_str(),
                    loc.package->path().parent_path().filename().string().c_str());
    }
    std::printf("%zu scripts, %zu Reia videos\n", scripts, reia);
    return 0;
}

int cmdUiDump(const char* resDir, uint32_t instance, uint32_t group, bool all) {
    ResourceIndex index(resDir);
    UiResources res(index, resDir);
    auto s = res.script(instance, group);
    if (!s) {
        std::fprintf(stderr, "no UI script %08X-%08X\n", group, instance);
        return 1;
    }
    for (const auto& w : s->warnings)
        std::printf("warning: %s\n", w.c_str());
    for (const auto& r : s->roots)
        dumpElement(res, r, 0, all);
    return 0;
}

struct SweepStats {
    std::map<std::string, size_t> classes, tags;
    size_t images = 0, imagesOk = 0, strings = 0, stringsOk = 0, nullRefs = 0, malformedRefs = 0;
    std::map<std::string, size_t> missingImages;
};

void sweepElement(UiResources& res, const UiElement& e, SweepStats& st) {
    ++st.classes[e.clsid()];
    ++st.tags[e.tag];
    for (const auto& [k, v] : e.attrs) {
        if ((isImageKey(k) || isStringKey(k)) && (v == "{00000000,00000000}" || v.empty())) {
            ++st.nullRefs;
            continue;
        }
        if (isImageKey(k)) {
            ++st.images;
            auto ref = uiResourceRef(v);
            if (!ref)
                ++st.malformedRefs;
            else if (res.image(*ref))
                ++st.imagesOk;
            else {
                char key[32];
                std::snprintf(key, sizeof key, "%08X-%08X-%08X%s", ref->type, ref->group, ref->instance,
                              res.index().find(Tgi{ref->type, ref->group, ref->instance, 0}) ? " (undecodable)" : "");
                ++st.missingImages[key];
            }
        } else if (isStringKey(k)) {
            ++st.strings;
            auto ref = uiStringRef(v);
            if (!ref)
                ++st.malformedRefs;
            else if (res.string(*ref))
                ++st.stringsOk;
        }
    }
    for (const auto& c : e.children)
        sweepElement(res, c, st);
}

int cmdUiSweep(const char* resDir) {
    ResourceIndex index(resDir);
    UiResources res(index, resDir);
    size_t scripts = 0, reia = 0, elements = 0, warned = 0, warnings = 0;
    SweepStats st;
    for (const auto& loc : index.ofType(kTypeUiScript)) {
        if (!isUiPackage(loc))
            continue;
        const auto bytes = index.read(loc);
        if (isReia(bytes)) {
            ++reia;
            continue;
        }
        const UiScript s = UiScript::parse(bytes);
        ++scripts;
        elements += s.elementCount();
        if (!s.warnings.empty()) {
            ++warned;
            warnings += s.warnings.size();
            std::printf("%08X-%08X: %s\n", loc.entry->tgi.group, loc.entry->tgi.instance, s.warnings[0].c_str());
        }
        for (const auto& r : s.roots)
            sweepElement(res, r, st);
    }
    std::printf("UI resources: %zu scripts (%zu windows), %zu Reia videos; %zu scripts with warnings (%zu warnings)\n",
                scripts, elements, reia, warned, warnings);
    std::printf("image references: %zu/%zu decoded; string references: %zu/%zu resolved; %zu null, %zu malformed\n",
                st.imagesOk, st.images, st.stringsOk, st.strings, st.nullRefs, st.malformedRefs);
    std::printf("window classes:\n");
    for (const auto& [n, k] : byCount(st.classes))
        std::printf("  %6zu %s\n", n, k.c_str());
    if (!st.missingImages.empty()) {
        std::printf("missing images (%zu distinct):\n", st.missingImages.size());
        size_t shown = 0;
        for (const auto& [n, k] : byCount(st.missingImages))
            if (shown++ < 15)
                std::printf("  %6zu %s\n", n, k.c_str());
    }

    // Every IMG resource, by file format.
    std::map<std::string, size_t> formats, failures;
    size_t total = 0, decoded = 0;
    for (const auto& loc : index.ofType(kTypeUiImage)) {
        const auto bytes = index.read(loc);
        ++total;
        ++formats[uiImageFormatName(detectUiImage(bytes))];
        std::string err;
        if (decodeUiImage(bytes, &err))
            ++decoded;
        else
            ++failures[err];
    }
    std::printf("IMG resources: %zu/%zu decoded\n", decoded, total);
    for (const auto& [n, k] : byCount(formats))
        std::printf("  %6zu %s\n", n, k.c_str());
    for (const auto& [n, k] : byCount(failures))
        std::printf("  failed %6zu %s\n", n, k.c_str());
    return 0;
}

int cmdUiImage(const char* resDir, uint32_t group, uint32_t instance, const char* out) {
    std::optional<std::vector<uint8_t>> bytes;
    if (std::filesystem::is_regular_file(resDir)) {
        DbpfPackage pkg(resDir);
        if (const auto* e = pkg.find(Tgi{kTypeUiImage, group, instance, 0}))
            bytes = pkg.read(*e);
    } else {
        ResourceIndex index(resDir);
        bytes = index.read(Tgi{kTypeUiImage, group, instance, 0});
    }
    if (!bytes) {
        std::fprintf(stderr, "no IMG %08X-%08X\n", group, instance);
        return 1;
    }
    std::string err;
    auto img = decodeUiImage(*bytes, &err);
    if (!img) {
        std::fprintf(stderr, "%s: %s\n", uiImageFormatName(detectUiImage(*bytes)), err.c_str());
        return 1;
    }
    writePng(out, *img);
    std::printf("%s %ux%u -> %s\n", uiImageFormatName(detectUiImage(*bytes)), img->width, img->height, out);
    return 0;
}


// Every glyph of every font in the directory: loads, outlines and rasterizes.
int cmdFontSweep(const char* dir) {
    size_t fonts = 0, failed = 0;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        if (de.path().extension() != ".mxf")
            continue;
        ++fonts;
        try {
            auto f = Font::loadFile(de.path());
            size_t errors = 0, empty = 0, latin = 0;
            for (uint32_t g = 0; g < f->glyphCount; ++g) {
                try {
                    if (rasterizeGlyph(*f, g, 16).alpha.empty())
                        ++empty;
                } catch (const std::exception& e) {
                    if (++errors <= 3)
                        std::printf("  glyph %u: %s\n", g, e.what());
                }
            }
            for (char32_t c = 0x20; c <= 0xFF; ++c)
                if (c < 0x7F || c >= 0xA0)
                    latin += f->glyph(c).has_value();
            std::printf("%-36s %-32s %6zu glyphs, %4.0f units/em, ascent %5.0f descent %5.0f, Latin-1 %3zu/191, "
                        "%zu blank, %zu errors\n",
                        de.path().filename().string().c_str(),
                        f->name.c_str(), f->glyphCount, f->unitsPerEm, f->ascent, f->descent, latin, empty, errors);
            failed += errors ? 1 : 0;
        } catch (const std::exception& e) {
            ++failed;
            std::printf("%-36s FAILED: %s\n", de.path().filename().string().c_str(), e.what());
        }
    }
    std::printf("%zu fonts, %zu with errors\n", fonts, failed);
    return failed ? 1 : 0;
}

// Renders text in a font file (.mxf) or a FontStyle.ini style (with the Fonts directory).
int cmdFontText(const char* fontOrDir, const char* styleOrSize, const char* text, const char* out) {
    std::unique_ptr<Font> owned;
    std::unique_ptr<FontLibrary> lib;
    const Font* font = nullptr;
    float ppem = 0;
    if (std::filesystem::is_directory(fontOrDir)) {
        lib = std::make_unique<FontLibrary>(fontOrDir);
        const FontStyle* st = lib->style(styleOrSize);
        if (!st) {
            std::fprintf(stderr, "no font style %s\n", styleOrSize);
            return 1;
        }
        font = lib->font(*st);
        ppem = float(st->size);
        std::printf("style %s: %s %d%s\n", st->name.c_str(), st->faces.empty() ? "" : st->faces[0].c_str(), st->size,
                    st->bold ? " bold" : "");
    } else {
        owned = Font::loadFile(fontOrDir);
        font = owned.get();
        ppem = std::stof(styleOrSize);
    }
    if (!font) {
        std::fprintf(stderr, "font not found\n");
        return 1;
    }
    const auto u = decodeUtf8(text);
    const TextMetrics m = measureText(*font, u, ppem);
    RgbaImage img;
    img.width = uint32_t(std::ceil(m.width)) + 8;
    img.height = uint32_t(std::ceil(m.ascent + m.descent)) + 8;
    img.pixels.assign(size_t(img.width) * img.height * 4, 0);
    for (size_t i = 0; i < img.pixels.size(); i += 4)
        img.pixels[i] = 40, img.pixels[i + 1] = 50, img.pixels[i + 2] = 110, img.pixels[i + 3] = 255;
    const uint8_t white[4] = {255, 255, 255, 255};
    drawText(img, *font, u, 4, 4 + std::round(m.ascent), ppem, white);
    writePng(out, img);
    std::printf("%s at %.0f px: %.1f px wide, ascent %.1f, descent %.1f, %zu missing -> %s\n", font->name.c_str(), ppem,
                m.width, m.ascent, m.descent, m.missing, out);
    return 0;
}

// Lays out a UI script and draws it on the CPU.
int cmdUiRender(const char* resDir, uint32_t instance, const char* out, int argc, char** argv) {
    UiLayoutOptions opt;
    for (int i = 0; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--show")
            for (const auto& id : uiList(std::string("(") + argv[i + 1] + ")"))
                opt.show.insert(uint32_t(std::stoul(id, nullptr, 16)));
        else if (a == "--size")
            std::sscanf(argv[i + 1], "%dx%d", &opt.canvasWidth, &opt.canvasHeight);
    }
    ResourceIndex index(resDir);
    UiResources res(index, resDir);
    FontLibrary fonts(std::filesystem::path(resDir) / "UI/Fonts");
    auto script = res.script(instance);
    if (!script) {
        std::fprintf(stderr, "no UI script %08X\n", instance);
        return 1;
    }
    const UiScreen screen = layoutUi(*script, res, fonts, opt);
    for (const auto& w : screen.warnings)
        std::printf("warning: %s\n", w.c_str());
    writePng(out, composeUi(screen));
    std::printf("%zu windows, %zu drawn, %zu texts, %zu quads, %zu missing images -> %s\n", screen.windows, screen.drawn,
                screen.texts, screen.quads.size(), screen.missingImages, out);
    return 0;
}

} // namespace

const char* uiUsage() {
    return "       s2pkg ui-list <Res-dir>\n"
           "       s2pkg ui-dump <Res-dir> <instance-hex> [group-hex] [--all]\n"
           "       s2pkg ui-sweep <Res-dir>\n"
           "       s2pkg ui-image <Res-dir|file.package> <group-hex> <instance-hex> <out.png>\n"
           "       s2pkg ui-render <Res-dir> <instance-hex> <out.png> [--show id,id...] [--size WxH]\n"
           "       s2pkg font-sweep <Fonts-dir>\n"
           "       s2pkg font-text <file.mxf> <pixels> <text> <out.png>\n"
           "       s2pkg font-text <Fonts-dir> <style|0xGUID> <text> <out.png>\n";
}

int uiCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "ui-list" && argc == 3)
        return cmdUiList(argv[2]);
    if (cmd == "ui-sweep" && argc == 3)
        return cmdUiSweep(argv[2]);
    if (cmd == "ui-dump" && argc >= 4 && argc <= 6) {
        bool all = std::string(argv[argc - 1]) == "--all";
        const int n = argc - (all ? 1 : 0);
        return cmdUiDump(argv[2], parseHex(argv[3]), n == 5 ? parseHex(argv[4]) : kUiGroup, all);
    }
    if (cmd == "ui-image" && argc == 6)
        return cmdUiImage(argv[2], parseHex(argv[3]), parseHex(argv[4]), argv[5]);
    if (cmd == "ui-render" && argc >= 5)
        return cmdUiRender(argv[2], parseHex(argv[3]), argv[4], argc - 5, argv + 5);
    if (cmd == "font-sweep" && argc == 3)
        return cmdFontSweep(argv[2]);
    if (cmd == "font-text" && argc == 6)
        return cmdFontText(argv[2], argv[3], argv[4], argv[5]);
    return -1;
}

} // namespace os2
