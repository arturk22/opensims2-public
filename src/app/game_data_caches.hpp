#pragma once

// The app's caches of the game's read-only data (DEBITOS-TECNICOS.md 1.3,
// round 24): what is read or decoded from TSData once and kept for the
// app's life because it means the same thing in every scene (keyed by GUID,
// resource or texture name, CRES, or a file of TSData). They were function
// statics in main.mm, pie_menu.cpp and wants_panel.cpp; now they live in
// one object, next to the scene's (SceneState gScene in main.mm), with one
// place that frees them:
//
// - On quit, releaseAppState() (main.mm) clears them right after the scene,
//   in the order of clear() below, while the resource index, the game's
//   audio and the window's Metal device are still alive. As function
//   statics they were destroyed by the C++ runtime at exit, in the reverse
//   order they were first used, before the scene's own teardown ran.
//   gGameDataCaches is a namespace-scope object, built before main(), so
//   the runtime destroys it only after the quit handler (it is empty then).
// - At each scene unload, trim() empties the decoded pictures when they
//   pass kImageCap: the CAS thumbnails (256x256, 256 KB each) grow with
//   every wardrobe, clothing rack and vanity group opened for another age,
//   gender or category, about 13 MB a wardrobe and 99 MB for every
//   wardrobe and vanity of one lot's 4 Sims (tests/scene_cycle.sh). No
//   picture is in use then: the dialogs and panels that hold pointers to
//   them are the scene's.
//
// Threads: every member is used by the thread that owns the scene: the
// loading worker while a scene loads (BackgroundJob, app/scene_loader.hpp),
// the main thread otherwise (it only draws the loading screen meanwhile).
// Starting and joining the worker order the two. The parallel reads inside
// a load (loadSimParts' parallelFor, ResourceIndex::addPackages) touch no
// member: they fill local vectors that the calling thread stores here.
//
// New app-lifetime caches of the game's data go here (scene caches go in
// SceneState); a log-once set of names may stay a function static.

#include "app/pie_menu.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/cas_thumbnails.hpp"
#include "formats/dbpf.hpp"
#include "formats/image.hpp"
#include "formats/sim.hpp"
#include "formats/wants.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace os2 {

class ResourceIndex;

struct GameDataCaches {
    // Above this many bytes of decoded pictures at a scene unload, they are
    // dropped (and decoded again when shown).
    // (OS2_PICTURE_CAP_MB overrides it: tests and measurements.)
    static constexpr size_t kImageCap = size_t(64) << 20;

    // Decoded pictures (the "images" of sizes(); trim() drops them).
    // The vanity's overlay previews by "face texture|overlay texture"
    // (setupWardrobe in main.mm; cas::overlayPreview, 96x96).
    std::map<std::string, std::unique_ptr<RgbaImage>> casPreviews;
    // The memories' icons by the memory object's GUID (wants_panel.cpp).
    std::map<uint32_t, std::unique_ptr<RgbaImage>> memoryIcons;
    // The pick meshes of objects the VM makes in play, by CRES (null: none).
    std::map<std::string, std::shared_ptr<const PickMesh>> pickMeshes;
    // Alpha-tested textures' masks for picking, by "texture@alphaRef"
    // (pie_menu.cpp alphaMask; at most 64x64).
    std::map<std::string, std::optional<PickMesh::Mask>> pickMasks;

    // ObjectThumbnails.package and its entries by instance (the dialogs'
    // pictures of objects, main.mm dialogThumbnail).
    std::map<uint32_t, const DbpfEntry*> objectThumbIndex;
    std::unique_ptr<DbpfPackage> objectThumbs;

    // The CAS data the lot's dialogs use (the wardrobe, the vanity, the
    // clothing rack, a family's bin) and growing up (the age change), each
    // loaded the first time it is needed. One catalog for all of them (they
    // read it only); Create-a-Sim keeps its own, as part of its scene.
    struct Cas {
        std::map<std::pair<uint32_t, uint32_t>, const SimPart*> byKey; // (group, instance) -> catalog part
        std::unique_ptr<cas::CasThumbnails> thumbs; // CASThumbnails.package (decodes and keeps what is shown)
        std::unique_ptr<cas::FaceBaker> baker;      // uses catalog
        std::unique_ptr<cas::CasCatalog> catalog;
        // The catalog's part with this key (GZPS group and instance), if any.
        const SimPart* part(const Tgi& k);
    } cas;

    // The aspiration dialog's want library when the lot runs no want system.
    std::shared_ptr<const wants::Library> wants;

    // Every Sim part (loadSimParts) of the index it was read from.
    std::vector<SimPart> simParts;
    const ResourceIndex* simPartsOf = nullptr;

    // The catalog and face baker, loaded on first use.
    Cas& casCatalog(ResourceIndex& index);
    // ... with CASThumbnails.package (TSData/Res/UserData/Thumbnails).
    Cas& casWithThumbnails(ResourceIndex& index, const std::filesystem::path& resDir);
    // The Sim parts of `index`, read on first use.
    const std::vector<SimPart>& simPartsFor(ResourceIndex& index);

    // Bytes of decoded pictures held (casPreviews, memoryIcons, the CAS
    // thumbnails, pickMeshes and pickMasks).
    size_t imageBytes() const;
    // Drops the decoded pictures when they are past `cap` bytes: only where
    // nothing holds a pointer to one (a scene unload). True if it did.
    bool trim(size_t cap = kImageCap);
    // Frees everything, in this order: the pictures, the object thumbnails,
    // the CAS data (users before what they use), the want library, the Sim
    // parts. On quit only (releaseAppState).
    void clear();
    // Entries and bytes per cache (OS2_DEBUG_SCENE_STATE).
    std::string sizes() const;
};

// The app's one instance.
GameDataCaches& gameDataCaches();

// The process's resident memory in bytes (0 if unknown): for the debug lines.
size_t residentBytes();

} // namespace os2
