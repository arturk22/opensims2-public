// Create-a-Sim's step panels (cas_mode.hpp): CasMode::screen's part for the
// Sim being edited, the six steps' panels (CB980E81, 88, 82, 85, 84 and 83)
// hanging from the sub panel anchor, with their controls and grids.

#include "app/cas_mode_internal.hpp"

#include "app/ui_panel_util.hpp"
#include "formats/cas_thumbnails.hpp"
#include "formats/resindex.hpp"
#include "formats/ui_resources.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

void CasMode::stepScreen(LayoutContext& L) {
    UiScreen& out = L.out;
    auto& rects = L.rects;
    const UiElement& frame = L.frame;
    const float ox = L.ox, oy = L.oy;
    const Font* font = L.font;
    const float ppem = L.ppem;
    auto& text = L.text;
    auto& rect = L.rect;
    auto& target = L.target;
    auto find = [](const UiElement& root, uint32_t id) { return findWindow(root, id); };

    // --- The step panel, hanging from the sub panel anchor.
    auto scriptIt = scripts_.find(kStepScripts[step_]);
    const UiElement* anchor = find(frame, kSubPanelAnchor);
    if (scriptIt == scripts_.end() || !anchor)
        return;
    const UiElement& panel = scriptIt->second.roots.front();
    const auto pa = panel.area();
    float ax = 0, ay = float(pa[3] - pa[1]); // bottom left by default
    if (const UiElement* pan = panel.child(kPanelAnchor)) {
        const auto a = pan->area();
        ax = float(a[0]), ay = float(a[1]);
    }
    // The anchor is hidden (not laid out): its place from the areas on the way down.
    std::function<bool(const UiElement&, float, float, float&, float&)> locate =
        [&](const UiElement& e, float px, float py, float& outX, float& outY) {
            const auto a = e.area();
            const float x = px + float(a[0]), y = py + float(a[1]);
            if (e.id() == kSubPanelAnchor) {
                outX = x, outY = y;
                return true;
            }
            for (const auto& c : e.children)
                if (locate(c, x, y, outX, outY))
                    return true;
            return false;
        };
    float anchorX = 0, anchorY = 0;
    locate(frame, ox, oy, anchorX, anchorY);
    const float panelX = anchorX - ax, panelY = anchorY - ay;
    UiDrawHooks hk;
    hk.rects = &rects;
    cas::CharacterSpec& s = edit_.spec;
    auto pressed = [&](const UiElement* e, bool on) {
        if (e && on)
            hk.buttonState[e] = 2;
    };
    // Grids drawn after layout: (grid window, items).
    struct Item {
        std::string text;
        bool selected = false;
        const RgbaImage* thumb = nullptr; // from CASThumbnails.package
        std::function<void()> pick;
        float slider = NAN; // modifiers: the weight
        std::function<void(float)> slide;
        // Modifiers: the facemodifier's image0id/image1id (ui.package, group
        // 0x499DB772), 4-state button strips with the face at each end of the
        // slider (e.g. gaunt/plump). The game shows no text on these sliders;
        // `text` (the XML's English name) is drawn only when they are missing.
        const RgbaImage* ends[2] = {nullptr, nullptr};
    };
    struct Grid {
        const UiElement* window;
        std::string key;
        int cols, rows;
        std::vector<Item> items;
        const UiElement* prev = nullptr;
        const UiElement* next = nullptr;
    };
    std::vector<Grid> grids;
    auto addGrid = [&](const UiElement* component, uint32_t gridId, const std::string& key, int cols, int rows,
                       std::vector<Item> items, uint32_t prevId = 0x4008, uint32_t nextId = 0x4009) {
        if (!component)
            return;
        const UiElement* g = find(*component, gridId);
        if (!g)
            return;
        grids.push_back({g, key, cols, rows, std::move(items), find(*component, prevId), find(*component, nextId)});
    };
    auto thumb = [&](uint32_t id) { return thumbs_ ? thumbs_->image(id) : nullptr; };
    // A texture overlay: the cached thumbnail, else its texture over the face skin.
    auto overlayThumb = [&](const SimPart& o) -> const RgbaImage* {
        if (const RgbaImage* t = thumb(cas::entryThumbnailId(o, s.age, s.gender)))
            return t;
        const std::string face = cas::faceSkinTexture(index_, cas::agePrefix(s.age, s.gender, true), s.skinTone);
        const std::string tex = cas::overlayTexture(index_, o);
        const std::string key = face + "|" + tex;
        auto it = previews_.find(key);
        if (it == previews_.end()) {
            auto img = cas::overlayPreview(index_, face, tex, o.number("subtype"));
            it = previews_.emplace(key, img ? std::make_unique<RgbaImage>(std::move(*img)) : nullptr).first;
        }
        return it->second.get();
    };
    auto overlayItems = [&](uint32_t subtype, int bin, bool removable) {
        std::vector<Item> items;
        const std::string hair = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
        std::vector<std::string> seen;
        int current = -1;
        for (size_t i = 0; i < s.overlays.size(); ++i)
            if (const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, s.overlays[i]))
                if (o->number("subtype") == subtype && (bin < 0 || int(o->number("bin")) == bin))
                    current = int(i);
        if (removable)
            items.push_back({"(nenhum)", current < 0, nullptr, [this, subtype, bin]() {
                                 pushUndo();
                                 auto& ov = edit_.spec.overlays;
                                 ov.erase(std::remove_if(ov.begin(), ov.end(),
                                                         [&](const std::string& f) {
                                                             const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, f);
                                                             return o && o->number("subtype") == subtype &&
                                                                    (bin < 0 || int(o->number("bin")) == bin);
                                                         }),
                                          ov.end());
                                 changed();
                             }});
        for (const SimPart* o : catalog_->overlaysFor(subtype, s.age, s.gender)) {
            if (bin >= 0 && int(o->number("bin")) != bin)
                continue;
            const std::string fam = cas::family(*o);
            if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                continue;
            // Brows and facial hair: one entry per style (the colour follows the hair).
            if (subtype == cas::overlay::Brows || subtype == cas::overlay::FacialHair) {
                if (lower(o->property("hairtone")) != hair)
                    continue;
            }
            seen.push_back(fam);
            const bool sel = current >= 0 && s.overlays[size_t(current)] == fam;
            items.push_back({label(o->name()), sel, overlayThumb(*o), [this, fam, subtype, bin]() {
                                 pushUndo();
                                 auto& ov = edit_.spec.overlays;
                                 ov.erase(std::remove_if(ov.begin(), ov.end(),
                                                         [&](const std::string& f) {
                                                             const SimPart* x = catalog_->byFamily(catalog_->textureOverlays, f);
                                                             return x && x->number("subtype") == subtype &&
                                                                    (bin < 0 || int(x->number("bin")) == bin);
                                                         }),
                                          ov.end());
                                 ov.push_back(fam);
                                 changed();
                             }});
        }
        return items;
    };
    // Mesh overlays (glasses: subtype 5 bin 15; beards with a mesh: subtype 0
    // bin 2), one per family (a beard in the hair's tone), without the
    // CAS-hidden ones (flag 0x01).
    auto meshOverlayItems = [&](uint32_t subtype, uint32_t bin, bool removable) {
        std::vector<Item> items;
        const std::string hair = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
        auto same = [this, subtype, bin](const std::string& f) {
            const SimPart* o = catalog_->byFamily(catalog_->meshOverlays, f);
            return o && o->number("subtype") == subtype && o->number("bin") == bin;
        };
        bool any = false;
        for (const auto& f : s.meshOverlays)
            any |= same(f);
        if (removable)
            items.push_back({"(nenhum)", !any, nullptr, [this, same]() {
                                 pushUndo();
                                 auto& mo = edit_.spec.meshOverlays;
                                 mo.erase(std::remove_if(mo.begin(), mo.end(), same), mo.end());
                                 changed();
                             }});
        std::vector<std::string> seen;
        for (const SimPart* o : catalog_->meshOverlaysFor(s.age, s.gender)) {
            if (o->number("subtype") != subtype || o->number("bin") != bin || (o->number("flags") & 1))
                continue;
            const std::string fam = cas::family(*o);
            const std::string ht = lower(o->property("hairtone"));
            if (!ht.empty() && ht != "00000000-0000-0000-0000-000000000000" && ht != hair)
                continue;
            if (std::find(seen.begin(), seen.end(), fam) != seen.end())
                continue;
            seen.push_back(fam);
            const bool sel = std::find(s.meshOverlays.begin(), s.meshOverlays.end(), fam) != s.meshOverlays.end();
            items.push_back({label(o->name()), sel, thumb(cas::entryThumbnailId(*o, s.age, s.gender)), [this, fam, same]() {
                                 pushUndo();
                                 auto& mo = edit_.spec.meshOverlays;
                                 mo.erase(std::remove_if(mo.begin(), mo.end(), same), mo.end());
                                 mo.push_back(fam);
                                 changed();
                             }});
        }
        return items;
    };
    auto archetypeItems = [&](const std::string& maskList, const std::string& region) {
        std::vector<Item> items;
        // Not the hidden ones (flag 0x1: the alien and "eelf" heads), as the
        // game's grid leaves them out.
        for (const SimPart* a : catalog_->archetypesFor(s.age, s.gender, cas::flag::Hidden)) {
            const std::string fam = cas::family(*a);
            bool sel;
            if (maskList.empty())
                sel = s.face.archetype == fam;
            else {
                auto it = s.face.regions.find(maskList);
                sel = it != s.face.regions.end() && it->second == fam;
            }
            const RgbaImage* shot = thumb(cas::archetypeThumbnailId(s.age, s.gender, fam, region));
            if (!shot)
                if (auto it = archetypeShots_.find(cas::casFileName(s.age, s.gender) + "|" + fam + "|" +
                                                   std::to_string(kThumbnailSkinTone));
                    it != archetypeShots_.end())
                    shot = it->second.get();
            items.push_back({label(a->name().substr(0, a->name().find('_'))), sel, shot, [this, fam, maskList]() {
                                 pushUndo();
                                 if (maskList.empty()) {
                                     edit_.spec.face.archetype = fam;
                                     edit_.spec.face.structure.reset(); // a saved face gives way to the head
                                     edit_.spec.face.regions.clear();
                                 } else {
                                     edit_.spec.face.regions[maskList] = fam;
                                 }
                                 changed();
                             }});
        }
        return items;
    };
    auto modifierItems = [&](int group) {
        std::vector<Item> items;
        for (const SimPart* m : catalog_->modifiersFor(s.age, s.gender)) {
            if (int(m->number("groupindex")) != group)
                continue;
            const std::string fam = cas::family(*m);
            auto it = s.face.modifiers.find(fam);
            Item item;
            item.text = m->name();
            item.slider = it == s.face.modifiers.end() ? 0.0f : it->second;
            for (int e = 0; e < 2; ++e)
                if (uint32_t id = uint32_t(m->number(e ? "image1id" : "image0id")))
                    item.ends[e] = res_->image(UiResourceRef{kTypeUiImage, 0x499DB772, id});
            const float lo = cas::floatProperty(*m, "negativelimit", -1.5f), hi = cas::floatProperty(*m, "positivelimit", 1.5f);
            item.slide = [this, fam, lo, hi](float t) {
                pushUndo();
                const float w = lo + (hi - lo) * std::clamp(t, 0.0f, 1.0f);
                edit_.spec.face.modifiers[fam] = std::fabs(w) < 0.05f ? 0.0f : w;
                changed();
            };
            item.slider = (item.slider - lo) / (hi - lo);
            items.push_back(std::move(item));
        }
        return items;
    };

    switch (step_) {
    case 0: { // name, gender, age, skin, body
        if (const UiElement* gen = find(panel, 0x8C6B1EB9))
            if (const UiElement* t = gen->child(1))
                hk.captions[t] = s.firstName.empty() ? uiCaption(*t, *res_) : s.firstName + (focus_ == Focus::FirstName ? "|" : "");
        if (const UiElement* bio = find(panel, 0x4CD0FDD9)) {
            (bioOpen_ ? hk.show : hk.hide).insert(bio);
            if (const UiElement* t = bio->child(1); t && (!s.description.empty() || focus_ == Focus::Biography))
                hk.captions[t] = s.description + (focus_ == Focus::Biography ? "|" : "");
        }
        pressed(find(panel, 0x8CB7C11F), bioOpen_);
        pressed(find(panel, 0x401), s.gender == cas::gender::Female);
        pressed(find(panel, 0x400), s.gender == cas::gender::Male);
        const uint32_t ages[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
        for (int k = 0; k < 5; ++k)
            pressed(find(panel, 0x200 + uint32_t(k)), s.age == ages[k]);
        for (int k = 0; k < 4; ++k)
            pressed(find(panel, 0x2CB70000 + uint32_t(k)), s.skinTone == k + 1);
        pressed(find(panel, 0x2C720000), s.fitness != 2);
        pressed(find(panel, 0x2C720001), s.fitness == 2);
        break;
    }
    case 1: // the head bin
        if (const UiElement* g = find(panel, 0x0CFD6EC6))
            hk.hide.insert(g);
        addGrid(&panel, 0x4011, "heads", 3, 3, archetypeItems("", ""));
        break;
    case 2: { // hair and face
        for (int k = 0; k < 7; ++k) {
            if (const UiElement* p = find(panel, kFacePanels[k]))
                (k == faceTab_ ? hk.show : hk.hide).insert(p);
            pressed(find(panel, kFaceTabs[k]), k == faceTab_);
        }
        const UiElement* fp = find(panel, kFacePanels[faceTab_]);
        if (!fp)
            break;
        if (faceTab_ == 0) { // hair
            const UiElement* hc = find(*fp, 0x2CED6A8D);
            std::vector<Item> items;
            const std::string tone = cas::toneFamily(s.age == cas::age::Elder ? 5 : s.hairTone);
            const auto& everyday = s.outfits[cas::category::Everyday];
            const std::string current = everyday.count(cas::part::Hair) ? lower(everyday.at(cas::part::Hair)) : "";
            for (const SimPart* h : catalog_->hairStyles(s.age, s.gender, tone)) {
                const std::string name = h->name();
                items.push_back({label(name), lower(name) == current, thumb(cas::entryThumbnailId(*h, s.age, s.gender)), [this, name]() {
                                     pushUndo();
                                     for (auto& [cat, parts] : edit_.spec.outfits)
                                         if (parts.count(cas::part::Hair) || cat == cas::category::Everyday)
                                             parts[cas::part::Hair] = name;
                                     changed();
                                 }});
            }
            if (hc) {
                std::map<std::string, std::string> c;
                readConstants(*hc, c);
                addGrid(hc, 0x8CB8C2B9, "hair", constantInt(c, "kHairGridNumColumns", 3), constantInt(c, "kHairGridNumRows", 3),
                        std::move(items), 0x8CB8C2C3, 0xACB8C2CF);
                for (int k = 0; k < 4; ++k)
                    pressed(find(*hc, 0x8800 + uint32_t(k)), s.hairTone == k + 1);
            }
        } else {
            // Archetype (region) bin and modifiers, the eyes' colours.
            std::vector<const UiElement*> archComps, modComps;
            std::function<void(const UiElement&)> walk = [&](const UiElement& e) {
                if (e.id() == 0xACB8A9AE)
                    archComps.push_back(&e);
                if (e.id() == 0x4CA50588)
                    modComps.push_back(&e);
                for (const auto& c : e.children)
                    walk(c);
            };
            walk(*fp);
            for (const UiElement* ac : archComps) {
                std::map<std::string, std::string> c;
                readConstants(*ac, c);
                const std::string mask = c.count("kCAFArchetypeRegionMask") ? c["kCAFArchetypeRegionMask"] : "";
                addGrid(ac, 0x4011, "arch" + std::to_string(faceTab_), constantInt(c, "kCAFArchetypesGridNumColumns", 3),
                        constantInt(c, "kCAFArchetypesGridNumRows", 2), archetypeItems(mask, kRegionThumbs[faceTab_]));
            }
            for (const UiElement* mc : modComps) {
                std::map<std::string, std::string> c;
                readConstants(*mc, c);
                addGrid(mc, 0x4011, "mod" + std::to_string(faceTab_), constantInt(c, "kCAFModifiersGridNumColumns", 2),
                        constantInt(c, "kCAFModifiersGridNumRows", 3),
                        modifierItems(constantInt(c, "kCAFModifiersValidGroup", 0)));
            }
            if (faceTab_ == 3)
                if (const UiElement* ec = find(*fp, 0xACB8A9AF)) {
                    std::vector<Item> items;
                    for (const SimPart* o : catalog_->overlaysFor(cas::overlay::Eyes, s.age, s.gender)) {
                        const std::string fam = cas::family(*o);
                        items.push_back({label(o->name()).substr(4), s.eyes == fam, overlayThumb(*o), [this, fam]() {
                                             pushUndo();
                                             edit_.spec.eyes = fam;
                                             changed();
                                         }});
                    }
                    addGrid(ec, 0x4011, "eyes", 5, 1, std::move(items));
                }
        }
        break;
    }
    case 3: { // makeup, costume, glasses, brows, stubble, facial hair
        // The Activate turns off the tabs with nothing for the age and gender.
        for (int k = 0; k < 6; ++k) {
            pressed(find(panel, kCosmeticTabs[k]), k == cosmeticTab_);
            if (const UiElement* t = find(panel, kCosmeticTabs[k]); t && k != cosmeticTab_ && !cosmeticTabHasItems(k))
                hk.buttonState[t] = 0;
        }
        const UiElement* makeup = find(panel, 0x6C6E1002);
        const UiElement* overlays = find(panel, 0x6CA52066);
        if (makeup)
            (cosmeticTab_ == 0 ? hk.show : hk.hide).insert(makeup);
        if (overlays)
            (cosmeticTab_ != 0 ? hk.show : hk.hide).insert(overlays);
        if (cosmeticTab_ == 0 && makeup) {
            const std::pair<uint32_t, std::pair<uint32_t, int>> comps[] = {
                {0x6CA52068, {cas::overlay::Blush, -1}}, {0xAD474C78, {cas::overlay::EyeShadow, 6}},
                {0x6CA52069, {cas::overlay::EyeShadow, 7}}, {0x6CA52067, {cas::overlay::Lipstick, -1}}};
            for (const auto& [id, kind] : comps)
                addGrid(find(*makeup, id), 0x4011, "makeup" + std::to_string(id), 5, 1, overlayItems(kind.first, kind.second, true));
        } else if (overlays) {
            for (int k = 0; k < 6; ++k)
                if (kCosmeticTitles[k])
                    if (const UiElement* t = find(*overlays, kCosmeticTitles[k]))
                        (k == cosmeticTab_ ? hk.show : hk.hide).insert(t);
            std::vector<Item> items;
            switch (cosmeticTab_) {
            case 1: items = overlayItems(cas::overlay::Costume, -1, true); break;
            case 2: items = meshOverlayItems(5, 15, true); break; // glasses
            case 3: items = overlayItems(cas::overlay::Brows, -1, false); break;
            case 4: items = overlayItems(cas::overlay::FacialHair, 1, true); break;
            case 5: { // facial hair: the textures, then the beards with a mesh
                items = overlayItems(cas::overlay::FacialHair, 2, true);
                auto mesh = meshOverlayItems(0, 2, true);
                if (!items.empty() && !mesh.empty()) {
                    // One "(none)" for both kinds.
                    items[0].selected = items[0].selected && mesh[0].selected;
                    auto a = items[0].pick, b = mesh[0].pick;
                    items[0].pick = [a, b]() {
                        a();
                        b();
                    };
                    mesh.erase(mesh.begin());
                }
                // A texture beard and a mesh one exclude each other.
                for (size_t i = 1; i < items.size(); ++i) {
                    auto pick = items[i].pick;
                    items[i].pick = [this, pick]() {
                        auto& mo = edit_.spec.meshOverlays;
                        mo.erase(std::remove_if(mo.begin(), mo.end(), [this](const std::string& f) {
                                     const SimPart* o = catalog_->byFamily(catalog_->meshOverlays, f);
                                     return o && o->number("subtype") == 0;
                                 }),
                                 mo.end());
                        pick();
                    };
                }
                for (auto& m : mesh) {
                    auto pick = m.pick;
                    m.pick = [this, pick]() {
                        auto& ov = edit_.spec.overlays;
                        ov.erase(std::remove_if(ov.begin(), ov.end(), [this](const std::string& f) {
                                     const SimPart* o = catalog_->byFamily(catalog_->textureOverlays, f);
                                     return o && o->number("subtype") == cas::overlay::FacialHair && o->number("bin") == 2;
                                 }),
                                 ov.end());
                        pick();
                    };
                }
                items.insert(items.end(), std::make_move_iterator(mesh.begin()), std::make_move_iterator(mesh.end()));
                break;
            }
            default: break;
            }
            addGrid(overlays, 0x4011, "cosmetic" + std::to_string(cosmeticTab_), 3, 3, std::move(items));
        }
        break;
    }
    case 4: { // clothes
        // Clothing::Activate: a category tab is on when the category has
        // whole outfits (everyday: any part); the part tabs show when it has
        // tops, bottoms and outfits (separates_).
        for (int k = 0; k < 6; ++k) {
            const UiElement* tab = find(panel, 0x02000000 + kClothesTabK[k]);
            pressed(tab, k == clothesCategory_);
            if (tab && k != clothesCategory_ && !clothesCategoryOn(k))
                hk.buttonState[tab] = 0;
            if (const UiElement* t = find(panel, 0x0DE60000 + kClothesTabK[k]))
                (k == clothesCategory_ ? hk.show : hk.hide).insert(t);
        }
        for (int k = 0; k < 3; ++k) {
            const UiElement* tab = find(panel, 0x01000000 + uint32_t(k));
            pressed(tab, k == clothesPart_);
            if (tab && !separates_)
                hk.hide.insert(tab);
        }
        if (const UiElement* g = find(panel, 0x0CFD6EC6))
            hk.hide.insert(g);
        const uint32_t cat = kClothesCategory[clothesCategory_];
        const uint32_t partBit = kClothesParts[clothesPart_];
        std::vector<Item> items;
        const auto& current = s.outfits[cat];
        for (const SimPart* p : catalog_->outfitParts(partBit, s.age, s.gender, cat)) {
            const std::string name = p->name();
            const bool sel = current.count(partBit) && lower(current.at(partBit)) == lower(name);
            items.push_back({label(name), sel, thumb(cas::entryThumbnailId(*p, s.age, s.gender)), [this, name, cat, partBit]() {
                                 pushUndo();
                                 auto& o = edit_.spec.outfits[cat];
                                 if (partBit == cas::part::Body) {
                                     o.erase(cas::part::Top);
                                     o.erase(cas::part::Bottom);
                                 } else {
                                     o.erase(cas::part::Body);
                                     // A top needs a bottom and the other way round.
                                     const uint32_t other = partBit == cas::part::Top ? cas::part::Bottom : cas::part::Top;
                                     if (!o.count(other)) {
                                         const auto list = catalog_->outfitParts(other, edit_.spec.age, edit_.spec.gender, cat);
                                         if (!list.empty())
                                             o[other] = list.front()->name();
                                     }
                                 }
                                 o[partBit] = name;
                                 changed();
                             }});
        }
        if (const UiElement* holder = find(panel, 0x0BD73DA0); holder) {
            Grid g{holder, "clothes" + std::to_string(cat) + "_" + std::to_string(partBit), 3, 3, std::move(items),
                   find(panel, 0x121), find(panel, 0x120)};
            grids.push_back(std::move(g));
        }
        break;
    }
    case 5: { // personality
        const bool young = s.age == cas::age::Toddler || s.age == cas::age::Child || s.age == cas::age::Baby;
        if (const UiElement* e = find(panel, 0x4DA3D18B))
            (young ? hk.show : hk.hide).insert(e);
        if (const UiElement* e = find(panel, 0x2DA3D170))
            (young ? hk.hide : hk.show).insert(e);
        for (int k = 1; k <= 7; ++k)
            pressed(find(panel, 0x0CDC0000 + uint32_t(k)), edit_.aspiration == (1u << (k - 1)));
        const int sign = edit_.zodiacChosen ? edit_.zodiac : cas::zodiacSign(edit_.personality);
        for (int k = 1; k <= 12; ++k) {
            if (const UiElement* bg = find(panel, 0x4CDC0000 + uint32_t(k)))
                (k == sign ? hk.show : hk.hide).insert(bg);
            pressed(find(panel, 0xACDC0000 + uint32_t(k)), k == sign);
        }
        if (const UiElement* t = find(panel, 0x2BE35D53)) {
            auto name = res_->string({packageGroupHash("CAS"), 0x8A, uint32_t(sign - 1)});
            hk.captions[t] = name.value_or(cas::zodiacName(sign));
        }
        for (int k = 0; k < 5; ++k)
            if (const UiElement* bar = find(panel, kTraitBars[k]))
                if (const UiElement* full = bar->child(3))
                    if (auto ref = uiResourceRef(full->get("image")))
                        if (const RgbaImage* img = res_->image(*ref)) {
                            keep_.push_back(std::make_unique<RgbaImage>(segmentFill(*img, float(edit_.personality[k] * 100), 1000, 10)));
                            hk.images[full] = keep_.back().get();
                        }
        if (const UiElement* total = find(panel, 0x4BDEFF05))
            if (const UiElement* full = total->child(3))
                if (auto ref = uiResourceRef(full->get("image")))
                    if (const RgbaImage* img = res_->image(*ref)) {
                        // Points left, from the bottom up.
                        const int left = cas::kPersonalityPoints - edit_.personality.total();
                        RgbaImage cut = *img;
                        const uint32_t keepRows = uint32_t(std::lround(float(img->height) * float(left) / 25.0f));
                        for (uint32_t y = 0; y + keepRows < img->height; ++y)
                            for (uint32_t x = 0; x < img->width; ++x)
                                cut.pixels[(size_t(y) * img->width + x) * 4 + 3] = 0;
                        keep_.push_back(std::make_unique<RgbaImage>(std::move(cut)));
                        hk.images[full] = keep_.back().get();
                    }
        break;
    }
    default: break;
    }
    const size_t panelQuads = out.quads.size();
    layoutUiElement(panel, panelX - float(pa[0]), panelY - float(pa[1]), *res_, *fonts_, out, hk);
    if (step_ == 3 && cosmeticTab_ != 0)
        if (const UiElement* oc = find(panel, 0x6CA52066))
            if (auto r = rects.find(oc); r != rects.end())
                if (const RgbaImage* bg = res_->image(UiResourceRef{kTypeUiImage, 0x499DB772, 0xCA2B0160})) {
                    // The overlays component draws no background of its own in the
                    // script (its code does): the makeup panel's, behind it.
                    UiQuad q;
                    q.image = bg;
                    q.x0 = r->second[0] - 56, q.x1 = q.x0 + 436;
                    q.y0 = r->second[1], q.y1 = r->second[3] + 9;
                    out.quads.insert(out.quads.begin() + std::ptrdiff_t(panelQuads), q);
                }
    if (auto r = rects.find(&panel); r != rects.end())
        solid_.push_back(r->second);

    // Targets of the panel's controls.
    switch (step_) {
    case 0: {
        target(find(panel, 0x401), [this](simd_float2) { setGender(cas::gender::Female, true); });
        target(find(panel, 0x400), [this](simd_float2) { setGender(cas::gender::Male, true); });
        const uint32_t ages[5] = {cas::age::Toddler, cas::age::Child, cas::age::Teen, cas::age::Adult, cas::age::Elder};
        for (int k = 0; k < 5; ++k) {
            const uint32_t a = ages[k];
            target(find(panel, 0x200 + uint32_t(k)), [this, a](simd_float2) {
                if (edit_.spec.age == a)
                    return;
                pushUndo();
                edit_.spec.age = a;
                edit_.spec.outfits.clear();
                // Brows and the like stay; the archetype family carries the face across ages.
                if (!baker_->archetypeFor(edit_.spec.face.archetype, a, edit_.spec.gender))
                    for (const SimPart* ar : catalog_->archetypesFor(a, edit_.spec.gender))
                        if (lower(ar->name()).find("archbase") != std::string::npos)
                            edit_.spec.face.archetype = cas::family(*ar);
                cas::completeSpec(*catalog_, edit_.spec);
                changed();
            });
        }
        for (int k = 0; k < 4; ++k)
            target(find(panel, 0x2CB70000 + uint32_t(k)), [this, k](simd_float2) {
                pushUndo();
                edit_.spec.skinTone = k + 1;
                edit_.spec.outfits.erase(cas::category::Naked);
                cas::completeSpec(*catalog_, edit_.spec);
                changed();
            });
        target(find(panel, 0x2C720000), [this](simd_float2) { setFitnessLevel(500, true); });
        target(find(panel, 0x2C720001), [this](simd_float2) { setFitnessLevel(1000, true); });
        target(find(panel, 0xAC3405DF), [this](simd_float2) { randomize(0); });
        target(find(panel, 0x8CB7C11F), [this](simd_float2) {
            bioOpen_ = !bioOpen_;
            focus_ = bioOpen_ ? Focus::Biography : Focus::None;
            dirty_ = true;
        });
        if (bioOpen_)
            if (const UiElement* bio = find(panel, 0x4CD0FDD9))
                if (const UiElement* t = bio->child(1))
                    target(t, [this](simd_float2) { focus_ = Focus::Biography; });
        if (const UiElement* gen = find(panel, 0x8C6B1EB9))
            target(gen, [this](simd_float2) { focus_ = Focus::FirstName; });
        break;
    }
    case 2:
        for (int k = 0; k < 7; ++k)
            target(find(panel, kFaceTabs[k]), [this, k](simd_float2) { faceTab_ = k; });
        if (faceTab_ == 0)
            if (const UiElement* fp = find(panel, kFacePanels[0]))
                for (int k = 0; k < 4; ++k)
                    target(find(*fp, 0x8800 + uint32_t(k)), [this, k](simd_float2) {
                        pushUndo();
                        edit_.spec.hairTone = k + 1;
                        const std::string tone = cas::toneFamily(k + 1);
                        for (auto& [cat, parts] : edit_.spec.outfits)
                            if (auto it = parts.find(cas::part::Hair); it != parts.end())
                                if (const SimPart* h = catalog_->byName(catalog_->parts, it->second))
                                    it->second = catalog_->hairInTone(*h, tone)->name();
                        changed();
                    });
        break;
    case 3:
        for (int k = 0; k < 6; ++k)
            if (k == cosmeticTab_ || cosmeticTabHasItems(k))
                target(find(panel, kCosmeticTabs[k]), [this, k](simd_float2) { cosmeticTab_ = k; });
        break;
    case 4:
        for (int k = 0; k < 6; ++k)
            if (k == clothesCategory_ || clothesCategoryOn(k))
                target(find(panel, 0x02000000 + kClothesTabK[k]), [this, k](simd_float2) { chooseClothesCategory(k); });
        if (separates_)
            for (int k = 0; k < 3; ++k)
                target(find(panel, 0x01000000 + uint32_t(k)), [this, k](simd_float2) { clothesPart_ = k; });
        break;
    case 5:
        for (int k = 1; k <= 7; ++k)
            target(find(panel, 0x0CDC0000 + uint32_t(k)), [this, k](simd_float2) {
                const bool young = edit_.spec.age == cas::age::Toddler || edit_.spec.age == cas::age::Child;
                if (young != (k == 7))
                    return; // HandleButtonAspirations: teens and up choose; the young grow up
                edit_.aspiration = 1u << (k - 1);
            });
        for (int k = 1; k <= 12; ++k)
            target(find(panel, 0xACDC0000 + uint32_t(k)), [this, k](simd_float2) {
                pushUndo();
                edit_.zodiac = k;
                edit_.zodiacChosen = true;
                edit_.personality = cas::zodiacPersonality(k); // SetZodiacSign
            });
        for (int k = 0; k < 5; ++k)
            if (const UiElement* bar = find(panel, kTraitBars[k]))
                if (const UiElement* full = bar->child(3))
                    if (auto r = rects.find(full); r != rects.end()) {
                        const auto rr = r->second;
                        targets_.push_back({{rr[0] - 8, rr[1] - 2, rr[2] + 4, rr[3] + 2}, [this, k, rr](simd_float2 p) {
                                                pushUndo();
                                                const int v = int(std::ceil((p.x - rr[0]) / (rr[2] - rr[0]) * 10.0f));
                                                static const char* const kPoint[] = {"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
                                                if (playSound && v >= 1 && v <= 10) // ui_cas_personality<n> for the point clicked
                                                    playSound(std::string("ui_cas_personality") + kPoint[v - 1]);
                                                cas::setTrait(edit_.personality, k, std::clamp(v, 0, 10));
                                                edit_.zodiacChosen = false;
                                                edit_.zodiac = cas::zodiacSign(edit_.personality);
                                            }});
                    }
        break;
    default: break;
    }

    // The grids: a page of cells, the selected one highlighted.
    for (auto& g : grids) {
        auto r = rects.find(g.window);
        if (r == rects.end())
            continue;
        const auto gr = r->second;
        const int perPage = std::max(1, g.cols * g.rows);
        const int pages = std::max(1, (int(g.items.size()) + perPage - 1) / perPage);
        int& page = pages_[g.key];
        page = std::clamp(page, 0, pages - 1);
        const float cw = (gr[2] - gr[0]) / float(g.cols), ch = (gr[3] - gr[1]) / float(g.rows);
        for (int i = 0; i < perPage; ++i) {
            const size_t idx = size_t(page * perPage + i);
            if (idx >= g.items.size())
                break;
            Item& it = g.items[idx];
            const float x0 = gr[0] + cw * float(i % g.cols) + 3, y0 = gr[1] + ch * float(i / g.cols) + 3;
            const float x1 = x0 + cw - 6, y1 = y0 + ch - 6;
            if (it.thumb && std::isnan(it.slider)) {
                // The thumbnail, square in the cell; the selected one framed.
                const float side = std::min(x1 - x0, y1 - y0);
                const float tx0 = (x0 + x1 - side) / 2, ty0 = (y0 + y1 - side) / 2;
                if (it.selected)
                    rect(tx0 - 2, ty0 - 2, tx0 + side + 2, ty0 + side + 2, kCellSelected);
                rect(tx0, ty0, tx0 + side, ty0 + side, it.selected ? kThumbSelectedBg : kThumbBg);
                UiQuad q;
                q.image = it.thumb;
                q.x0 = tx0, q.y0 = ty0, q.x1 = tx0 + side, q.y1 = ty0 + side;
                out.quads.push_back(q);
                auto pick = it.pick;
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }, "ui_secondary_button"});
                continue;
            }
            rect(x0, y0, x1, y1, it.selected ? kCellSelected : kCellColor);
            if (!std::isnan(it.slider)) {
                float bx0 = x0 + 8, bx1 = x1 - 8, by0 = y1 - 18, by1 = y1 - 10;
                if (it.ends[0] && it.ends[1]) {
                    // The two end faces (the strip's normal frame, 1 of 4), the
                    // bar between them, centred on the cell.
                    const float side = std::min(y1 - y0 - 8, (x1 - x0) / 4);
                    const float cy = (y0 + y1) / 2;
                    for (int e = 0; e < 2; ++e) {
                        UiQuad q;
                        q.image = it.ends[e];
                        q.x0 = e ? x1 - 4 - side : x0 + 4;
                        q.x1 = q.x0 + side;
                        q.y0 = cy - side / 2, q.y1 = cy + side / 2;
                        q.u0 = 0.25f, q.u1 = 0.5f;
                        out.quads.push_back(q);
                        auto slide = it.slide;
                        const float v = it.slider, step = e ? 0.1f : -0.1f;
                        targets_.push_back({{q.x0, q.y0, q.x1, q.y1},
                                            [slide, v, step](simd_float2) { slide(std::clamp(v + step, 0.0f, 1.0f)); },
                                            "ui_secondary_button"});
                    }
                    bx0 = x0 + 10 + side, bx1 = x1 - 10 - side, by0 = cy - 4, by1 = cy + 4;
                } else {
                    text(x0 + 4, y0 + 4, x1 - x0 - 8, it.text, kCellText);
                }
                rect(bx0, by0, bx1, by1, {0.25f, 0.30f, 0.60f, 1});
                const float mid = (bx0 + bx1) / 2, kx = bx0 + (bx1 - bx0) * it.slider;
                rect(std::min(mid, kx), by0, std::max(mid, kx), by1, {0.45f, 0.85f, 1.0f, 1});
                rect(kx - 3, by0 - 4, kx + 3, by1 + 4, {1, 1, 1, 1});
                auto slide = it.slide;
                targets_.push_back({{bx0 - 6, by0 - 8, bx1 + 6, by1 + 8},
                                    [slide, bx0, bx1](simd_float2 p) { slide((p.x - bx0) / (bx1 - bx0)); }});
            } else {
                // The name, over two lines when it does not fit.
                std::string t = it.text;
                const float maxW = x1 - x0 - 6;
                std::string line2;
                if (font && measureText(*font, decodeUtf8(t), ppem).width > maxW)
                    if (auto sp = t.rfind(' ', t.size() / 2 + 4); sp != std::string::npos && sp > 0)
                        line2 = t.substr(sp + 1), t = t.substr(0, sp);
                const float ty = (y0 + y1) / 2 - (line2.empty() ? 7.0f : 14.0f);
                text(x0 + 3, ty, maxW, t, kCellText);
                if (!line2.empty())
                    text(x0 + 3, ty + 14, maxW, line2, kCellText);
                auto pick = it.pick;
                targets_.push_back({{x0, y0, x1, y1}, [pick](simd_float2) { pick(); }, "ui_secondary_button"});
            }
        }
        const std::string key = g.key;
        if (g.prev)
            target(g.prev, [this, key](simd_float2) { pages_[key] = std::max(0, pages_[key] - 1); });
        if (g.next)
            target(g.next, [this, key, pages](simd_float2) { pages_[key] = std::min(pages - 1, pages_[key] + 1); });
    }
}

bool CasMode::cosmeticTabHasItems(int k) const {
    // The lists the tab shows (stepScreen), counted as the Activate counts
    // its catalog lists (BV FUN_0044515c / base 0xebe1a0).
    const cas::CharacterSpec& s = edit_.spec;
    auto textures = [&](uint32_t subtype, int bin) {
        for (const SimPart* o : catalog_->overlaysFor(subtype, s.age, s.gender))
            if (bin < 0 || int(o->number("bin")) == bin)
                return true;
        return false;
    };
    auto meshes = [&](uint32_t subtype, uint32_t bin) {
        for (const SimPart* o : catalog_->meshOverlaysFor(s.age, s.gender))
            if (o->number("subtype") == subtype && o->number("bin") == bin && !(o->number("flags") & 1))
                return true;
        return false;
    };
    switch (k) {
    case 0:
        return textures(cas::overlay::Blush, -1) || textures(cas::overlay::EyeShadow, 6) ||
               textures(cas::overlay::EyeShadow, 7) || textures(cas::overlay::Lipstick, -1);
    case 1: return textures(cas::overlay::Costume, -1);
    case 2: return meshes(5, 15);
    case 3: return textures(cas::overlay::Brows, -1);
    case 4: return textures(cas::overlay::FacialHair, 1);
    case 5: return textures(cas::overlay::FacialHair, 2) || meshes(0, 2);
    default: return false;
    }
}

void CasMode::chooseClothesCategory(int k) {
    // HandleTabCategory (BV 0x428254): tabs 0x02000000..02 keep the part,
    // the part tabs shown when the category has all three; the others choose
    // the whole outfit and hide them (in the base game only everyday is one
    // of the first three).
    clothesCategory_ = std::clamp(k, 0, 5);
    const uint32_t cat = kClothesCategory[clothesCategory_];
    if (kClothesTabK[clothesCategory_] < 3) {
        separates_ = clothesHas(cas::part::Top, cat) && clothesHas(cas::part::Bottom, cat) && clothesHas(cas::part::Body, cat);
    } else {
        clothesPart_ = 2;
        separates_ = false;
    }
    rebuild();
}

bool CasMode::clothesHas(uint32_t partBit, uint32_t category) const {
    return !catalog_->outfitParts(partBit, edit_.spec.age, edit_.spec.gender, category).empty();
}

bool CasMode::clothesCategoryOn(int k) const {
    const uint32_t cat = kClothesCategory[k];
    if (cat == cas::category::Everyday) // HasOutfits(0x1c, 1)
        return clothesHas(cas::part::Top, cat) || clothesHas(cas::part::Bottom, cat) || clothesHas(cas::part::Body, cat);
    return clothesHas(cas::part::Body, cat);
}

} // namespace os2
