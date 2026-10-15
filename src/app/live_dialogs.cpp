// What the live mode's dialogs show: the Sims' portraits, the dialogs'
// thumbnails, the aspiration dialog's lifetime goals, and the wardrobe,
// clothing rack and vanity over the family's clothing bin (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// A Sim's portrait: its character file's IMG 0x10.
std::optional<os2::RgbaImage> simPortrait(int16_t id) {
    for (const auto& s : gLotSims) {
        if (s.id != id)
            continue;
        try {
            // The small portrait: IMG 0x10, 0x08 or 0x02 by the Sim's age
            // (0x1000, 0x800, 0x200 are the large ones).
            os2::DbpfPackage character(s.character);
            for (const auto& e : character.entries())
                if (e.tgi.type == os2::kTypeUiImage && e.tgi.instance < 0x100)
                    return os2::decodeUiImage(character.read(e));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: portrait: %s\n", e.what());
        }
    }
    return std::nullopt;
}

// The portrait of a neighbor who is not on the lot (a memory's balloon):
// the character file in the lot's neighborhood whose OBJD GUID is the
// neighbor's (the files are read once, for their GUIDs).
std::optional<os2::RgbaImage> neighborPortrait(int16_t neighborId) {
    auto& byGuid = gScene.portraitFiles; // (the neighborhood's files: per scene)
    auto& scanned = gScene.portraitFilesDir;
    if (!gSimulation || gLotSims.empty())
        return std::nullopt;
    const auto& nbs = gSimulation->machine().neighbors;
    const auto n = nbs.find(neighborId);
    if (n == nbs.end() || !n->second.guid)
        return std::nullopt;
    const fs::path dir = gLotSims.front().character.parent_path();
    if (scanned != dir) {
        scanned = dir;
        byGuid.clear();
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(dir, ec))
            if (f.path().extension() == ".package")
                try {
                    byGuid[os2::hood::Character::load(f.path()).guid] = f.path();
                } catch (const std::exception& err) {
                    os2::logOnce("neighbor portraits", f.path().filename().string(), err);
                }
    }
    const auto it = byGuid.find(n->second.guid);
    if (it == byGuid.end())
        return std::nullopt;
    try {
        os2::DbpfPackage character(it->second);
        for (const auto& e : character.entries())
            if (e.tgi.type == os2::kTypeUiImage && e.tgi.instance < 0x100)
                return os2::decodeUiImage(character.read(e));
    } catch (const std::exception& err) {
        os2::logOnce("neighbor portrait", it->second.filename().string(), err);
    }
    return std::nullopt;
}

// The dialogs' 40x40 pictures (DialogWindow::thumbnail): a Sim's portrait
// (on the lot, else the neighborhood's character file), an object's
// catalog thumbnail (ObjectThumbnails.package, keyed as the live UI keys it).
const os2::RgbaImage* dialogThumbnail(int16_t object, int16_t neighbor) {
    auto& cache = gDialogThumbnails; // (the lot's object IDs: per scene)
    auto& thumbs = gGameData.objectThumbs; // (the game's ObjectThumbnails.package: for the app's life)
    auto& thumbIndex = gGameData.objectThumbIndex;
    if (!gSimulation)
        return nullptr;
    const auto key = std::make_pair(object, neighbor);
    if (auto it = cache.find(key); it != cache.end())
        return it->second.get();
    std::optional<os2::RgbaImage> img;
    os2::vm::VM& m = gSimulation->machine();
    if (const os2::vm::Object* o = object ? m.object(object) : nullptr) {
        if (o->person) {
            img = simPortrait(object);
            if (!img)
                img = neighborPortrait(o->person->data[os2::vm::persondata::NeighborId]);
        } else if (o->definition) {
            try {
                if (!thumbs) {
                    thumbs = std::make_unique<os2::DbpfPackage>(gameDir() / "TSData/Res/UserData/Thumbnails/ObjectThumbnails.package");
                    for (const auto& e : thumbs->entries())
                        thumbIndex.emplace(e.tgi.instance, &e);
                }
                const os2::vm::Object* lead = m.object(os2::vm::multiTileGroup(*o));
                const os2::vm::Object& t = lead && lead->definition ? *lead : *o;
                if (const os2::StringTable* names = m.code().strings(t.group, 0x85)) {
                    const std::string model = names->get(t.definition->modelIndex(), os2::lang::EnglishUS);
                    if (auto e = thumbIndex.find(os2::objectThumbnailInstance(t.group, model)); e != thumbIndex.end())
                        img = os2::decodeUiImage(thumbs->read(*e->second));
                }
                // An object without a model in the thumbnails (the phone's
                // services, "Phone Service - Maid" and the others, run from
                // the lot's object list): cTSUIThumbnailService falls back to
                // a picture the object's file carries (GetEmbeddedThumbnail);
                // the services' group has IMG 1, a service worker's silhouette
                // (inferred: which instance the service reads was not settled).
                if (!img && gIndex)
                    if (auto data = gIndex->read(os2::Tgi{0x856DDBAC, t.group, 1, 0}))
                        img = os2::decodeUiImage(*data);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: dialog thumbnail: %s\n", e.what());
            }
        }
    } else if (neighbor) {
        if (const os2::vm::Object* sim = m.neighborSim(neighbor))
            img = simPortrait(sim->id);
        if (!img)
            img = neighborPortrait(neighbor);
    }
    auto& slot = cache[key];
    if (img)
        slot = std::make_unique<os2::RgbaImage>(std::move(*img));
    return slot.get();
}

// The clothing bin of a Sim's family (the VM's, from the neighborhood's
// BINX items plus what the clothing rack added); null without a family.
// A family with no bin at all (made by an older build of our CAS, which
// wrote none) gets its members' clothes on the lot as
// cTSFamily::AddMemberClothing gives them (cas::familyBinClothing), kept
// in the VM so that saving writes the bin (the moment is inferred: the
// game makes the bin with the family, family_write.hpp).
const std::vector<os2::Tgi>* familyBin(int16_t sim, const os2::cas::CasCatalog* catalog = nullptr) {
    if (!gSimulation)
        return nullptr;
    os2::vm::VM& m = gSimulation->machine();
    const os2::vm::Object* o = m.object(sim);
    if (!o || !o->person)
        return nullptr;
    const int16_t family = o->person->data[os2::vm::persondata::FamilyNumber];
    auto f = m.families.find(family);
    if (f == m.families.end())
        return nullptr;
    auto& filled = gScene.wardrobeFilled; // (family numbers: per scene; round 20)
    // Only real families (1..0x7FD9) have a bin, as cTSFamily::AddMemberClothing
    // (BV 0x47ec68) checks; the NPC family 0x7FFF never gets one.
    if (f->second.wardrobe.empty() && catalog && family > 0 && family < 0x7FDA && filled.insert(family).second)
        for (const auto& ls : gLotSims) {
            const os2::vm::Object* member = m.object(ls.id);
            if (!member || !member->person || member->person->data[os2::vm::persondata::FamilyNumber] != family)
                continue;
            try {
                os2::DbpfPackage pkg(ls.character);
                const auto spec = os2::cas::readCharacterSpec(pkg, 0xFFFFFFFF, *catalog);
                for (const os2::Tgi& k : os2::cas::familyBinClothing(*catalog, spec))
                    if (std::find(f->second.wardrobe.begin(), f->second.wardrobe.end(), k) == f->second.wardrobe.end())
                        f->second.wardrobe.push_back(k);
            } catch (const std::exception& err) {
                os2::logOnce("family bin", ls.character.filename().string(), err);
            }
        }
    return &f->second.wardrobe;
}
bool inBin(const std::vector<os2::Tgi>& bin, const os2::Tgi& k) {
    return std::any_of(bin.begin(), bin.end(), [&](const os2::Tgi& b) {
        return b.type == k.type && b.group == k.group && b.instance == k.instance;
    });
}

// The aspiration dialog's "Big Life Moments" (cTSWinProcWantsCatalog::
// LoadPowerWantsByAspiration): the want simulator's lifetime goals of the
// aspiration (GetLifetimeGoals, filled by AddLifetimeGoal from WantTuning's
// <lifetimeGoals>; the names to bits as the dialog's buttons number them:
// Romance 1, Family 2, Wealth 4, Reputation 0x10, Knowledge 0x20, Grow Up
// 0x40, inferred from the names) with each definition's icons.
void setupAspirationWants(os2::ResourceIndex& index) {
    gPie->dialogWindow().lifetimeWants = [&index](uint32_t aspiration) {
        std::vector<os2::DialogWindow::LifetimeWant> out;
        auto& own = gGameData.wants; // (the game's WantTuning: for the app's life)
        std::shared_ptr<const os2::wants::Library> lib;
        if (gSimulation)
            if (os2::vm::WantSystem* ws = os2::vm::wantSystem(gSimulation->machine()))
                lib = ws->library;
        if (!lib) {
            if (!own)
                own = std::make_shared<const os2::wants::Library>(os2::wants::Library::load(index));
            lib = own;
        }
        static const std::map<std::string, uint32_t> kBits = {{"Romance", 0x01},    {"Family", 0x02},
                                                              {"Wealth", 0x04},     {"Reputation", 0x10},
                                                              {"Knowledge", 0x20},  {"Grow Up", 0x40}};
        for (const auto& [name, goals] : lib->tuning.lifetimeGoals) {
            auto b = kBits.find(name);
            if (b == kBits.end() || b->second != aspiration)
                continue;
            for (uint32_t guid : goals)
                if (const os2::wants::Definition* def = lib->definition(guid)) {
                    os2::DialogWindow::LifetimeWant w;
                    w.icon = def->primaryIcon ? def->primaryIcon : def->secondaryIcon;
                    w.modifier = def->primaryIcon ? def->secondaryIcon : 0;
                    w.fear = def->isFear();
                    w.power = def->level == os2::wants::Level::Power;
                    w.name = lib->text(*def);
                    out.push_back(std::move(w));
                }
        }
        return out;
    };
}

// The wardrobe dialog (cTSWinProcClothingDialog, app/dialog_window.hpp):
// the catalog's tops, bottoms and whole outfits of the Sim's age and gender
// in the category (CAS parts, with their CASThumbnails pictures), and OK
// writes the Sim's character file with the new outfit (beside the
// session's files, as a grown-up Sim's) and re-dresses it if it wears that
// category.
void setupWardrobe(os2::ResourceIndex& index) {
    using Cas = os2::GameDataCaches::Cas;
    // (the game's CAS catalog and thumbnails: for the app's life, gGameData)
    auto load = [&index]() -> Cas& { return gGameData.casWithThumbnails(index, gameDir() / "TSData/Res"); };
    auto simFile = [](int16_t id) -> LotSim* {
        for (auto& s : gLotSims)
            if (s.id == id)
                return &s;
        return nullptr;
    };
    os2::DialogWindow& w = gPie->dialogWindow();
    w.outfits = [load, simFile](int16_t sim, uint32_t category) -> std::optional<os2::DialogWindow::Outfits> {
        LotSim* s = simFile(sim);
        if (!s)
            return std::nullopt;
        try {
            Cas& c = load();
            os2::DbpfPackage pkg(s->character);
            const os2::cas::CharacterSpec spec = os2::cas::readCharacterSpec(pkg, 0xFFFFFFFF, *c.catalog);
            os2::DialogWindow::Outfits o;
            const uint32_t parts[3] = {os2::cas::part::Top, os2::cas::part::Bottom, os2::cas::part::Body};
            const auto wornParts = spec.outfits.count(category) ? spec.outfits.at(category) : std::map<uint32_t, std::string>{};
            // cTSWinProcClothingDialog::SetParams lists the family's clothing
            // bin (its BINX items) of the Sim's age, gender and category
            // (familyBin fills a family's missing bin from its members); an
            // empty one still shows the catalog's (inferred).
            const std::vector<os2::Tgi>* bin = familyBin(sim, c.catalog.get());
            for (int t = 0; t < 3; ++t) {
                std::vector<const os2::SimPart*> candidates;
                if (bin && !bin->empty()) {
                    // The bin's items as they are (the Body Shop's "CASIE_"
                    // copies and CAS-hidden ones included) with the age,
                    // gender, category and part bits, once each.
                    for (const os2::Tgi& k : *bin) {
                        const os2::SimPart* p = c.part(k);
                        if (p && (p->number("age") & spec.age) && (p->number("gender") & spec.gender) &&
                            (p->number("category") & category) && (p->number("outfit") & parts[t]) &&
                            std::find(candidates.begin(), candidates.end(), p) == candidates.end())
                            candidates.push_back(p);
                    }
                } else {
                    candidates = c.catalog->outfitParts(parts[t], spec.age, spec.gender, category);
                }
                for (const os2::SimPart* p : candidates) {
                    const bool worn = wornParts.count(parts[t]) && strcasecmp(wornParts.at(parts[t]).c_str(), p->name().c_str()) == 0;
                    os2::DialogWindow::OutfitItem item;
                    item.part = p->name();
                    item.thumb = c.thumbs->image(os2::cas::entryThumbnailId(*p, spec.age, spec.gender));
                    if (worn)
                        o.current[size_t(t)] = int(o.lists[size_t(t)].size());
                    o.lists[size_t(t)].push_back(std::move(item));
                }
            }
            o.tab = wornParts.count(os2::cas::part::Body) || o.lists[0].empty() ? 2 : 0;
            return o;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: wardrobe: %s\n", e.what());
            return std::nullopt;
        }
    };
    // The clothing rack and the try-on (cTSWinProcClothingPurchaseDialog /
    // ClothingTryOnDialog): the catalog's clothing of the age, gender and
    // category (inferred: who fills the grids' lists was not found; the
    // catalog is what CAS offers), owned when in the family's bin, worn
    // when the Sim wears it in that category.
    auto readSpec = [load, simFile](int16_t sim) -> std::optional<os2::cas::CharacterSpec> {
        LotSim* s = simFile(sim);
        if (!s)
            return std::nullopt;
        try {
            Cas& c = load();
            os2::DbpfPackage pkg(s->character);
            os2::cas::CharacterSpec spec = os2::cas::readCharacterSpec(pkg, 0xFFFFFFFF, *c.catalog);
            for (const auto& e : pkg.entries())
                if (e.tgi.type == 0x4F424A44) { // OBJD: the Sim's GUID stays
                    spec.guid = os2::ObjectDefinition::parse(pkg.read(e)).guid();
                    break;
                }
            return spec;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: character of Sim %d: %s\n", sim, e.what());
            return std::nullopt;
        }
    };
    // Writes the Sim's character file with `spec` (beside the session's
    // files, as a grown-up Sim's) and dresses it again if it wears `category`.
    auto writeSpec = [&index, load, simFile](int16_t sim, const os2::cas::CharacterSpec& spec, uint32_t category,
                                             const std::string& what) {
        LotSim* s = simFile(sim);
        if (!s)
            return;
        try {
            Cas& c = load();
            const fs::path dir = os2::sessionCharactersDir(); // the save copies it to the neighborhood
            const fs::path out = dir / (s->character.stem().string() + ".package");
            const fs::path tmp = dir / (s->character.stem().string() + ".new.package");
            os2::cas::writeCharacter(index, *c.catalog, *c.baker, spec, s->character.stem().string(), tmp, true);
            fs::rename(tmp, out);
            s->character = out;
            std::printf("opensims2: Sim %d, outfit 0x%03X: %s (%s)\n", sim, category, what.c_str(), out.string().c_str());
            // Wearing that category now: dressed again from the new file.
            const os2::vm::Object* o = gSimulation ? gSimulation->machine().object(sim) : nullptr;
            if (o && o->person && os2::vm::outfitCategoryBit(o->person->data[8]) == category && s->actor &&
                gSimulation->onOutfitChange)
                gSimulation->onOutfitChange(sim, *s->actor, category);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: character of Sim %d: %s\n", sim, e.what());
        }
    };
    w.applyOutfit = [readSpec, writeSpec](int16_t sim, uint32_t category, const std::string& top, const std::string& bottom,
                                          const std::string& body) {
        auto spec = readSpec(sim);
        if (!spec)
            return;
        auto& outfit = spec->outfits[category];
        if (!body.empty()) {
            outfit.erase(os2::cas::part::Top), outfit.erase(os2::cas::part::Bottom);
            outfit[os2::cas::part::Body] = body;
        } else {
            outfit.erase(os2::cas::part::Body);
            if (!top.empty())
                outfit[os2::cas::part::Top] = top;
            if (!bottom.empty())
                outfit[os2::cas::part::Bottom] = bottom;
        }
        writeSpec(sim, *spec, category, body.empty() ? top + " + " + bottom : body);
    };
    w.simAgeGender = [readSpec](int16_t sim) -> std::pair<uint32_t, uint32_t> {
        const auto spec = readSpec(sim);
        return spec ? std::make_pair(spec->age, spec->gender) : std::make_pair(0u, 0u);
    };
    w.shopItems = [load, readSpec](int16_t sim, uint32_t category, uint32_t partBit, uint32_t age, uint32_t gender) {
        std::vector<os2::DialogWindow::ShopItem> out;
        try {
            Cas& c = load();
            const auto spec = readSpec(sim);
            const bool self = spec && spec->age == age && spec->gender == gender;
            std::string worn;
            if (self && spec->outfits.count(category) && spec->outfits.at(category).count(partBit))
                worn = spec->outfits.at(category).at(partBit);
            const std::vector<os2::Tgi>* bin = familyBin(sim, c.catalog.get());
            for (const os2::SimPart* p : c.catalog->outfitParts(partBit, age, gender, category)) {
                os2::DialogWindow::ShopItem i;
                i.part = p->name();
                i.key = p->tgi;
                i.thumb = c.thumbs->image(os2::cas::entryThumbnailId(*p, age, gender));
                i.owned = bin && inBin(*bin, p->tgi);
                i.worn = !worn.empty() && strcasecmp(worn.c_str(), i.part.c_str()) == 0;
                out.push_back(std::move(i));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: clothing rack: %s\n", e.what());
        }
        return out;
    };

    // The rack's OK (cTSWinProcClothingPurchaseDialog::SaveCartStandard, BV
    // 0x406bfc): the cart's tops and bottoms (whole outfits left out) give
    // the ages wanted per gender (gender 1 female, 2 male); for each age bit
    // of the seven, cTSFamily::GetNumCompleteOutfits (BV 0x47f258) of the
    // family bin with those items must be above the bin's alone, for each
    // gender wanted, or the rack asks. GetNumCompleteOutfits counts the
    // items of the age, gender, species 1 and the category (or none) with
    // the outfit bits 4 (tops), 0x10 (bottoms), 8 (whole): tops x bottoms +
    // whole outfits.
    w.cartCompletesOutfits = [load](int16_t sim, uint32_t category, const std::vector<os2::Tgi>& cart) {
        Cas& c = load();
        std::vector<const os2::SimPart*> bin, added;
        if (const std::vector<os2::Tgi>* b = familyBin(sim, c.catalog.get()))
            for (const os2::Tgi& k : *b)
                if (const os2::SimPart* p = c.part(k))
                    bin.push_back(p);
        uint32_t female = 0, male = 0;
        for (const os2::Tgi& k : cart)
            if (const os2::SimPart* p = c.part(k); p && !(p->number("outfit") & 8)) {
                if (p->number("gender") & 1)
                    female |= p->number("age");
                if (p->number("gender") & 2)
                    male |= p->number("age");
                added.push_back(p);
            }
        auto count = [category](const std::vector<const os2::SimPart*>& a, const std::vector<const os2::SimPart*>& b,
                                uint32_t age, uint32_t gender) {
            int tops = 0, bottoms = 0, whole = 0;
            for (const auto* list : {&a, &b})
                for (const os2::SimPart* p : *list) {
                    const uint32_t cat = p->number("category"), species = p->property("species").empty() ? 1 : p->number("species");
                    if (!(p->number("age") & age) || !(p->number("gender") & gender) || !(species & 1) ||
                        (cat && !(cat & category)))
                        continue;
                    const uint32_t o = p->number("outfit");
                    if (o & 4)
                        ++tops;
                    else if (o & 0x10)
                        ++bottoms;
                    else if (o & 8)
                        ++whole;
                }
            return tops * bottoms + whole;
        };
        for (uint32_t age = 1; age <= 0x40; age <<= 1) {
            bool more = true;
            if (female & age)
                more = count(bin, {}, age, 1) < count(bin, added, age, 1);
            if (male & age)
                more = more && count(bin, {}, age, 2) < count(bin, added, age, 2);
            if (!more)
                return false;
        }
        return true;
    };
    // The vanity (cTSWinProcVanityDialog): the look being edited per Sim,
    // the choices of each group as CAS offers them (cas_mode_panels.cpp), OK
    // writing the character file and dressing the Sim again in what it
    // wears (OnOk: person data 8, 119, 120).
    // (edits: by Sim ID, gScene.mirrorEdits; previews: by the game's face and
    // texture names, for the app's life, gGameData.casPreviews.)
    struct Choice {
        const os2::RgbaImage* thumb = nullptr;
        bool selected = false;
        std::function<void(os2::cas::CharacterSpec&)> apply;
    };
    auto choices = [&index, load](const os2::cas::CharacterSpec& s, int group) {
        namespace cas = os2::cas;
        Cas& c = load();
        const cas::CasCatalog& cat = *c.catalog;
        std::vector<Choice> items;
        auto lower = [](std::string t) {
            for (char& ch : t)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return t;
        };
        auto overlayThumb = [&](const os2::SimPart& o) -> const os2::RgbaImage* {
            if (const os2::RgbaImage* t = c.thumbs->image(cas::entryThumbnailId(o, s.age, s.gender)))
                return t;
            const std::string face = cas::faceSkinTexture(index, cas::agePrefix(s.age, s.gender, true), s.skinTone);
            const std::string tex = cas::overlayTexture(index, o);
            auto& previews = gGameData.casPreviews;
            auto it = previews.find(face + "|" + tex);
            if (it == previews.end()) {
                auto img = cas::overlayPreview(index, face, tex, o.number("subtype"));
                it = previews.emplace(face + "|" + tex, img ? std::make_unique<os2::RgbaImage>(std::move(*img)) : nullptr).first;
            }
            return it->second.get();
        };
        const std::string hairTone = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
        auto overlayItems = [&](uint32_t subtype, int bin, bool removable) {
            auto same = [&cat, subtype, bin](const std::string& f) {
                const os2::SimPart* o = cat.byFamily(cat.textureOverlays, f);
                return o && o->number("subtype") == subtype && (bin < 0 || int(o->number("bin")) == bin);
            };
            std::string current;
            for (const auto& f : s.overlays)
                if (same(f))
                    current = f;
            if (removable)
                items.push_back({nullptr, current.empty(), [same](cas::CharacterSpec& e) {
                                     std::erase_if(e.overlays, same);
                                 }});
            std::vector<std::string> seen;
            for (const os2::SimPart* o : cat.overlaysFor(subtype, s.age, s.gender)) {
                if (bin >= 0 && int(o->number("bin")) != bin)
                    continue;
                const std::string fam = cas::family(*o);
                if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                    continue;
                // Brows and facial hair: one per style (the colour follows the hair).
                if ((subtype == cas::overlay::Brows || subtype == cas::overlay::FacialHair) && lower(o->property("hairtone")) != hairTone)
                    continue;
                seen.push_back(fam);
                items.push_back({overlayThumb(*o), current == fam, [same, fam](cas::CharacterSpec& e) {
                                     std::erase_if(e.overlays, same);
                                     e.overlays.push_back(fam);
                                 }});
            }
        };
        auto meshItems = [&](uint32_t subtype, uint32_t bin, bool removable) {
            auto same = [&cat, subtype, bin](const std::string& f) {
                const os2::SimPart* o = cat.byFamily(cat.meshOverlays, f);
                return o && o->number("subtype") == subtype && o->number("bin") == bin;
            };
            const bool any = std::any_of(s.meshOverlays.begin(), s.meshOverlays.end(), same);
            if (removable)
                items.push_back({nullptr, !any, [same](cas::CharacterSpec& e) { std::erase_if(e.meshOverlays, same); }});
            std::vector<std::string> seen;
            for (const os2::SimPart* o : cat.meshOverlaysFor(s.age, s.gender)) {
                if (o->number("subtype") != subtype || o->number("bin") != bin || (o->number("flags") & 1))
                    continue;
                const std::string fam = cas::family(*o);
                const std::string ht = lower(o->property("hairtone"));
                if (!ht.empty() && ht != "00000000-0000-0000-0000-000000000000" && ht != hairTone)
                    continue;
                if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                    continue;
                seen.push_back(fam);
                const bool sel = std::find(s.meshOverlays.begin(), s.meshOverlays.end(), fam) != s.meshOverlays.end();
                items.push_back({c.thumbs->image(cas::entryThumbnailId(*o, s.age, s.gender)), sel, [same, fam](cas::CharacterSpec& e) {
                                     std::erase_if(e.meshOverlays, same);
                                     e.meshOverlays.push_back(fam);
                                 }});
            }
        };
        using G = os2::DialogWindow::VanityGroup;
        switch (group) {
        case G::VanityHair: {
            const auto& everyday = s.outfits.count(cas::category::Everyday) ? s.outfits.at(cas::category::Everyday)
                                                                            : std::map<uint32_t, std::string>{};
            const std::string current = everyday.count(cas::part::Hair) ? lower(everyday.at(cas::part::Hair)) : "";
            for (const os2::SimPart* h : cat.hairStyles(s.age, s.gender, hairTone)) {
                const std::string name = h->name();
                items.push_back({c.thumbs->image(cas::entryThumbnailId(*h, s.age, s.gender)), lower(name) == current,
                                 [name](cas::CharacterSpec& e) {
                                     // OnOk: the style in every outfit but the naked and try-on ones' own.
                                     for (auto& [category, parts] : e.outfits)
                                         if (parts.count(cas::part::Hair) || category == cas::category::Everyday)
                                             parts[cas::part::Hair] = name;
                                 }});
            }
            break;
        }
        case G::VanityHairCustom: {
            // "Custom" (cTSUICASComponentHair, the filter's custom flag): the
            // hairs whose tone is not a regular one (IsRegularHairTone:
            // 00000001..00000005), one per style.
            const auto& everyday = s.outfits.count(cas::category::Everyday) ? s.outfits.at(cas::category::Everyday)
                                                                            : std::map<uint32_t, std::string>{};
            const std::string current = everyday.count(cas::part::Hair) ? lower(everyday.at(cas::part::Hair)) : "";
            std::vector<std::string> seen;
            for (const os2::SimPart* h : cat.outfitParts(cas::part::Hair, s.age, s.gender, 0)) {
                const std::string tone = lower(h->property("hairtone"));
                bool regular = false;
                for (int k = 1; k <= 5; ++k)
                    regular = regular || tone == cas::toneFamily(k);
                const std::string style = cas::hairStyle(*h);
                if (regular || std::find(seen.begin(), seen.end(), style) != seen.end())
                    continue;
                seen.push_back(style);
                const std::string name = h->name();
                items.push_back({c.thumbs->image(cas::entryThumbnailId(*h, s.age, s.gender)), lower(name) == current,
                                 [name](cas::CharacterSpec& e) {
                                     for (auto& [category, parts] : e.outfits)
                                         if (parts.count(cas::part::Hair) || category == cas::category::Everyday)
                                             parts[cas::part::Hair] = name;
                                 }});
            }
            break;
        }
        case G::VanityHairColor:
            for (int k = 0; k < 4; ++k)
                items.push_back({nullptr, s.hairTone == k + 1, [&cat, k](cas::CharacterSpec& e) {
                                     e.hairTone = k + 1;
                                     const std::string tone = cas::toneFamily(k + 1);
                                     for (auto& [category, parts] : e.outfits)
                                         if (auto it = parts.find(cas::part::Hair); it != parts.end())
                                             if (const os2::SimPart* h = cat.byName(cat.parts, it->second))
                                                 it->second = cat.hairInTone(*h, tone)->name();
                                 }});
            break;
        case G::VanityBlush: overlayItems(cas::overlay::Blush, -1, true); break;
        case G::VanityEyeliner: overlayItems(cas::overlay::EyeShadow, 6, true); break;
        case G::VanityEyeShadow: overlayItems(cas::overlay::EyeShadow, 7, true); break;
        case G::VanityLipstick: overlayItems(cas::overlay::Lipstick, -1, true); break;
        case G::VanityGlasses: meshItems(5, 15, true); break;
        case G::VanityCostume: overlayItems(cas::overlay::Costume, -1, true); break;
        case G::VanityBrows: overlayItems(cas::overlay::Brows, -1, false); break;
        case G::VanityStubble: overlayItems(cas::overlay::FacialHair, 1, true); break;
        case G::VanityBeards: {
            // The texture beards, then the mesh ones; one "none" for both,
            // and either kind takes the other off.
            overlayItems(cas::overlay::FacialHair, 2, true);
            const size_t textures = items.size();
            meshItems(0, 2, true);
            if (textures > 0 && items.size() > textures) {
                items[0].selected = items[0].selected && items[textures].selected;
                auto a = items[0].apply, b = items[textures].apply;
                items[0].apply = [a, b](cas::CharacterSpec& e) { a(e), b(e); };
                auto noMesh = b;
                auto noTexture = a;
                items.erase(items.begin() + long(textures));
                for (size_t i = 1; i < items.size(); ++i) {
                    auto pick = items[i].apply;
                    items[i].apply = i < textures ? std::function<void(cas::CharacterSpec&)>([pick, noMesh](cas::CharacterSpec& e) { noMesh(e), pick(e); })
                                                  : std::function<void(cas::CharacterSpec&)>([pick, noTexture](cas::CharacterSpec& e) { noTexture(e), pick(e); });
                }
            }
            break;
        }
        default: break;
        }
        return items;
    };
    auto editOf = [readSpec](int16_t sim) -> os2::cas::CharacterSpec* {
        auto& edits = gScene.mirrorEdits;
        auto it = edits.find(sim);
        if (it == edits.end()) {
            auto spec = readSpec(sim);
            if (!spec)
                return nullptr;
            it = edits.emplace(sim, std::move(*spec)).first;
        }
        return &it->second;
    };
    w.vanityItems = [editOf, choices](int16_t sim, int group) {
        std::vector<os2::DialogWindow::VanityItem> out;
        if (const os2::cas::CharacterSpec* s = editOf(sim))
            for (const Choice& c : choices(*s, group))
                out.push_back({c.thumb, c.selected});
        return out;
    };
    w.vanityPick = [editOf, choices](int16_t sim, int group, int i) {
        if (os2::cas::CharacterSpec* s = editOf(sim)) {
            auto list = choices(*s, group);
            if (i >= 0 && i < int(list.size()))
                list[size_t(i)].apply(*s);
        }
    };
    w.vanityDone = [writeSpec](int16_t sim, bool ok) {
        auto& edits = gScene.mirrorEdits;
        auto it = edits.find(sim);
        if (it == edits.end())
            return;
        if (ok) {
            // OnOk re-dresses the Sim in what it wears (person data 8).
            const os2::vm::Object* o = gSimulation ? gSimulation->machine().object(sim) : nullptr;
            writeSpec(sim, it->second, o && o->person ? os2::vm::outfitCategoryBit(o->person->data[8]) : 0x001, "new look");
        }
        edits.erase(it);
    };


    // Generic Sims Call 4 (add to family) fills the family's bin as
    // cTSFamily::AddMemberClothing does and copies what fits from the old
    // family's (vm::addToFamily); the clothes come from the Sim's character
    // file: a lot Sim's, a newborn's or a neighbor's by its OBJD GUID.
    if (!gSimulation)
        return;
    os2::vm::VM& machine = gSimulation->machine();
    auto characterOf = [&machine](int16_t neighbor) -> std::optional<fs::path> {
        if (auto it = gBornCharacters.find(neighbor); it != gBornCharacters.end())
            return it->second;
        if (const os2::vm::Object* sim = machine.neighborSim(neighbor))
            for (const auto& ls : gLotSims)
                if (ls.id == sim->id)
                    return ls.character;
        if (const os2::vm::Neighbor* n = machine.neighbor(neighbor))
            return characterFileOf(n->guid);
        return std::nullopt;
    };
    auto specOf = [load, characterOf](int16_t neighbor) -> std::optional<os2::cas::CharacterSpec> {
        const auto file = characterOf(neighbor);
        if (!file)
            return std::nullopt;
        try {
            os2::DbpfPackage pkg(*file);
            return os2::cas::readCharacterSpec(pkg, 0xFFFFFFFF, *load().catalog);
        } catch (const std::exception& err) {
            os2::logOnce("family clothing", file->filename().string(), err);
            return std::nullopt;
        }
    };
    machine.memberClothing = [load, specOf](int16_t neighbor) {
        const auto spec = specOf(neighbor);
        return spec ? os2::cas::familyBinClothing(*load().catalog, *spec) : std::vector<os2::Tgi>{};
    };
    // The item's age and gender bits include the Sim's (the third mask, the
    // category, is taken to pass; inferred).
    machine.clothingFits = [load, specOf](int16_t neighbor, const os2::Tgi& k) {
        const auto spec = specOf(neighbor);
        const os2::SimPart* part = load().part(k);
        return spec && part && os2::cas::fits(*part, spec->age, spec->gender);
    };
    // OS2_PROBE_SCENE_CACHES=1 (test aid, tests/scene_cycle.sh): the first
    // lot Sim's family loses its clothing bin (as one an older build of our
    // CAS made) and the wardrobe and the vanity are asked for it, so the
    // per-scene caches (gScene.wardrobeFilled, gScene.mirrorEdits) hold
    // something the switch must clear.
    if (std::getenv("OS2_PROBE_SCENE_CACHES") && !gLotSims.empty()) {
        const int16_t sim = gLotSims.front().id;
        const size_t edits = gScene.mirrorEdits.size(), filled = gScene.wardrobeFilled.size();
        if (const os2::vm::Object* o = machine.object(sim); o && o->person)
            if (auto f = machine.families.find(o->person->data[os2::vm::persondata::FamilyNumber]); f != machine.families.end())
                f->second.wardrobe.clear();
        if (w.outfits)
            w.outfits(sim, 0x001);
        if (w.vanityItems)
            w.vanityItems(sim, 0);
        std::printf("opensims2: scene caches probe: Sim %d: mirrorEdits %zu -> %zu, wardrobeFilled %zu -> %zu\n", sim, edits,
                    gScene.mirrorEdits.size(), filled, gScene.wardrobeFilled.size());
        std::fflush(stdout);
    }
    // OS2_PROBE_APP_CACHES=1 (test aid, tests/scene_cycle.sh): every lot Sim's
    // wardrobe in every category and vanity in every group, as a player who
    // opens all of them (and the memories tab) on every lot: what the app's
    // decoded pictures (gGameData: CAS thumbnails, overlay previews, memory
    // icons) grow to.
    if (std::getenv("OS2_PROBE_APP_CACHES")) {
        namespace cat = os2::cas::category;
        const size_t before = gGameData.imageBytes();
        for (const auto& ls : gLotSims) {
            if (w.outfits)
                for (uint32_t c : {cat::Everyday, cat::Swimwear, cat::Pajamas, cat::Formal, cat::Underwear, cat::Athletic})
                    w.outfits(ls.id, c);
            if (w.vanityItems)
                for (int g = 0; g < os2::DialogWindow::VanityGroups; ++g)
                    w.vanityItems(ls.id, g);
            if (const os2::vm::Object* o = machine.object(ls.id); o && o->person) // the memories tab
                os2::simMemories(machine, os2::vm::neighborIdOf(*o));
        }
        std::printf("opensims2: app caches probe: %zu Sims: pictures %zu KB -> %zu KB (%s) residentMB=%zu\n",
                    gLotSims.size(), before >> 10, gGameData.imageBytes() >> 10, gGameData.sizes().c_str(),
                    os2::residentBytes() >> 20);
        std::fflush(stdout);
    }
}

} // namespace os2app
