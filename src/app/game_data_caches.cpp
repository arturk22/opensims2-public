#include "app/game_data_caches.hpp"

#include "formats/resindex.hpp"

#include <cstdio>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace os2 {

namespace {

// Built before main() (a namespace-scope object, not a function static), so
// the C++ runtime destroys it after the quit handler has emptied it
// (game_data_caches.hpp).
GameDataCaches gGameDataCaches;

size_t imageBytesOf(const RgbaImage* img) { return img ? img->pixels.size() : 0; }

size_t pickMeshBytes(const PickMesh& m) {
    size_t n = m.corners.size() * sizeof(simd_float3) + m.triangleMask.size() * sizeof(int) +
               m.uvs.size() * sizeof(simd_float2);
    for (const auto& mask : m.masks)
        n += mask.alpha.size();
    return n;
}

// About what a vector of Sim parts takes (the tree nodes and strings of
// their properties, their references): for the debug line.
size_t simPartsBytes(const std::vector<SimPart>& parts) {
    auto heap = [](const std::string& t) { return t.size() > 22 ? t.size() + 1 : 0; }; // (libc++: 22 chars inline)
    size_t n = parts.capacity() * sizeof(SimPart);
    for (const auto& p : parts) {
        n += p.references.capacity() * sizeof(Tgi);
        for (const auto& [k, v] : p.properties)
            n += sizeof(std::pair<const std::string, std::string>) + 32 + heap(k) + heap(v);
    }
    return n;
}

} // namespace

GameDataCaches& gameDataCaches() { return gGameDataCaches; }

const SimPart* GameDataCaches::Cas::part(const Tgi& k) {
    if (byKey.empty() && catalog)
        for (const auto& p : catalog->parts)
            byKey.emplace(std::make_pair(p.tgi.group, p.tgi.instance), &p);
    auto it = byKey.find({k.group, k.instance});
    return it == byKey.end() ? nullptr : it->second;
}

GameDataCaches::Cas& GameDataCaches::casCatalog(ResourceIndex& index) {
    if (!cas.catalog) {
        cas.catalog = std::make_unique<cas::CasCatalog>(cas::CasCatalog::load(index));
        cas.baker = std::make_unique<cas::FaceBaker>(index, *cas.catalog);
    }
    return cas;
}

GameDataCaches::Cas& GameDataCaches::casWithThumbnails(ResourceIndex& index, const std::filesystem::path& resDir) {
    casCatalog(index);
    if (!cas.thumbs)
        cas.thumbs = std::make_unique<cas::CasThumbnails>(resDir / "UserData/Thumbnails/CASThumbnails.package");
    return cas;
}

const std::vector<SimPart>& GameDataCaches::simPartsFor(ResourceIndex& index) {
    if (simPartsOf != &index) {
        simParts = loadSimParts(index);
        simPartsOf = &index;
    }
    return simParts;
}

size_t GameDataCaches::imageBytes() const {
    size_t n = 0;
    for (const auto& [key, img] : casPreviews)
        n += imageBytesOf(img.get());
    for (const auto& [guid, img] : memoryIcons)
        n += imageBytesOf(img.get());
    for (const auto& [cres, mesh] : pickMeshes)
        if (mesh)
            n += pickMeshBytes(*mesh);
    for (const auto& [key, mask] : pickMasks)
        if (mask)
            n += mask->alpha.size();
    if (cas.thumbs)
        n += cas.thumbs->cachedBytes();
    return n;
}

bool GameDataCaches::trim(size_t cap) {
    if (imageBytes() <= cap)
        return false;
    casPreviews.clear();
    memoryIcons.clear();
    pickMeshes.clear();
    pickMasks.clear();
    if (cas.thumbs)
        cas.thumbs->clearCache();
    return true;
}

void GameDataCaches::clear() {
    casPreviews.clear();
    memoryIcons.clear();
    pickMeshes.clear();
    pickMasks.clear();
    objectThumbIndex.clear(); // (pointers into objectThumbs' entries)
    objectThumbs.reset();
    cas.byKey.clear(); // (pointers into the catalog)
    cas.thumbs.reset();
    cas.baker.reset(); // (reads the catalog)
    cas.catalog.reset();
    wants.reset();
    simParts.clear();
    simParts.shrink_to_fit();
    simPartsOf = nullptr;
}

std::string GameDataCaches::sizes() const {
    std::string out;
    auto add = [&out](const char* name, size_t n) {
        out += (out.empty() ? "" : " ") + std::string(name) + "=" + std::to_string(n);
    };
    auto kb = [](size_t bytes) { return (bytes + 1023) / 1024; };
    size_t previews = 0, icons = 0, meshes = 0, masks = 0;
    for (const auto& [key, img] : casPreviews)
        previews += imageBytesOf(img.get());
    for (const auto& [guid, img] : memoryIcons)
        icons += imageBytesOf(img.get());
    for (const auto& [cres, mesh] : pickMeshes)
        if (mesh)
            meshes += pickMeshBytes(*mesh);
    for (const auto& [key, mask] : pickMasks)
        if (mask)
            masks += mask->alpha.size();
    add("casPreviews", casPreviews.size());
    add("casPreviewsKB", kb(previews));
    add("casThumbs", cas.thumbs ? cas.thumbs->cachedCount() : 0);
    add("casThumbsKB", kb(cas.thumbs ? cas.thumbs->cachedBytes() : 0));
    add("memoryIcons", memoryIcons.size());
    add("memoryIconsKB", kb(icons));
    add("pickMeshes", pickMeshes.size());
    add("pickMeshesKB", kb(meshes));
    add("pickMasks", pickMasks.size());
    add("pickMasksKB", kb(masks));
    add("imagesKB", kb(imageBytes()));
    add("objectThumbIndex", objectThumbIndex.size());
    add("casCatalog", cas.catalog != nullptr);
    add("casParts", cas.catalog ? cas.catalog->parts.size() : 0);
    add("casPartsKB", kb(cas.catalog ? simPartsBytes(cas.catalog->parts) : 0));
    add("wants", wants != nullptr);
    add("simParts", simParts.size());
    add("simPartsKB", kb(simPartsBytes(simParts)));
    return out;
}

size_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return size_t(info.resident_size);
#endif
    return 0;
}

} // namespace os2
