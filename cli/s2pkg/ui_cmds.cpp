// s2pkg commands for UI scripts, UI images and fonts.
#include "ui_cmds.hpp"

#include "formats/resindex.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_script.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
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

} // namespace

const char* uiUsage() {
    return "       s2pkg ui-list <Res-dir>\n"
           "       s2pkg ui-dump <Res-dir> <instance-hex> [group-hex] [--all]\n"
           "       s2pkg ui-sweep <Res-dir>\n"
           "       s2pkg ui-image <Res-dir|file.package> <group-hex> <instance-hex> <out.png>\n";
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
    return -1;
}

} // namespace os2
