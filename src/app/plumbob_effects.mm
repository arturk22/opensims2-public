// The plumbob over the selected Sim, the censor over nude Sims and the lot's
// particle effects (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// The plumbob over the selected Sim (with the VM): the game's "plumbob"
// model, spinning about Z once every 2.316 s as its "o-plumbob-spin"
// animation turns its bone, floating over the Sim's head (the top of its
// posed mesh, less the height the mesh is modelled at; the gap is a guess).
//
// The game's own plumbob is a VM object, "PlumbBob", held in slot 0 of a
// "Head Slots Holder" that a Sim holds in its slot 2 (bone plumbbob_grip):
// its Main ("Set Mood Effect & Material") picks its colour with Change
// Material from the owner's aspiration score (person data 160; STR# 0x88
// plumbBob_coreColor00/25/50/75/100, platinum). When that object is over
// the selected Sim the VM draws it (app/vm_objects.hpp) and the app's is
// hidden; otherwise (its "Main - Update My Owner" has not moved it to the
// Sim the player picked) the app draws its own over the selected Sim with
// the colour the game's one has, and the game's is hidden, so that there is
// one plumbob.
constexpr float kPlumbobGap = 0.2f, kPlumbobSpinSeconds = 2.316f;
bool& gVmPlumbob = gScene.vmPlumbob;           // the VM's PlumbBob is over the selected Sim
std::set<int16_t>& gHiddenPlumbobs = gScene.hiddenPlumbobs; // the game's PlumbBobs the app hides (its own stands in)
std::optional<os2::Xform>& gAppPlumbobAt = gScene.appPlumbobAt; // where the app's plumbob is drawn, if it is
std::map<std::string, int>& gPlumbobModels = gScene.plumbobModels; // plumbob model per core material (renderer ids)
std::string& gPlumbobMaterial = gScene.plumbobMaterial;      // the app plumbob's core material ("" = the model's)
void choosePlumbob(os2::Renderer& renderer) {
    if (!gSimulation || !gObjectView || !gPie)
        return;
    const bool wasVm = gVmPlumbob;
    gVmPlumbob = false;
    std::string material;
    for (const auto& [id, o] : gSimulation->machine().objects()) {
        if (o.person || !o.definition || o.definition->fileName != "PlumbBob")
            continue;
        const bool over = gObjectView->hasModel(id) && gObjectView->outermostContainer(id) == gPie->sim();
        // No plumbob in a scene: the CINEs that show one make their own
        // ("plumbobCinematic" actors in homeBirth, playInBed; inferred).
        gObjectView->setHidden(id, !over || gVmPlumbob || cinematicPlaying());
        if (!over || gVmPlumbob)
            gHiddenPlumbobs.insert(id);
        else
            gHiddenPlumbobs.erase(id);
        if (over && !gVmPlumbob)
            gVmPlumbob = true;
        if (material.empty() || over) {
            if (auto it = o.materials.find("core"); it != o.materials.end())
                material = it->second;
            else if (!o.materials.empty())
                material = o.materials.begin()->second;
        }
    }
    if (gVmPlumbob != wasVm && std::getenv("OS2_DEBUG_VM"))
        std::fprintf(stderr, "opensims2: the game's PlumbBob is %sover the selected Sim %d\n", gVmPlumbob ? "" : "not ",
                     int(gPie->sim()));
    if (gVmPlumbob || material == gPlumbobMaterial || gPlumbob < 0 || !gIndex)
        return;
    auto& models = gPlumbobModels;
    auto it = models.find(material);
    if (it == models.end()) {
        int m = -1;
        try {
            m = renderer.addObject(os2::loadObjectModel(*gIndex, "plumbob", {{"core", material}}), *gIndex);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: plumbob %s: %s\n", material.c_str(), e.what());
        }
        it = models.emplace(material, m).first;
    }
    if (it->second >= 0)
        renderer.setInstanceModel(gPlumbob, it->second);
    gPlumbobMaterial = material;
}

// The censor over nude Sims (cPerson::UpdateCensor every frame,
// formats/sim_censor.hpp): person data 30 picks the body parts, the boxes
// follow the posed body and go when the flags are cleared (the shower's and
// the tub's trees set and clear them around the naked outfit).
// OS2_DEBUG_CENSOR=1 prints each Sim's flags and boxes when they change;
// OS2_NO_CENSOR=1 draws none and OS2_FORCE_CENSOR=<flags> sets the flags of
// every Sim (test aids: before/after shots).
void updateCensor(os2::Renderer& renderer) {
    std::vector<os2::Renderer::CensorBox> boxes;
    auto& lastReport = gScene.censorReports; // (by Sim ID: per scene)
    if (gSimulation && gObjectView && !std::getenv("OS2_NO_CENSOR")) {
        auto& m = gSimulation->machine();
        for (const LotSim& sim : gLotSims) {
            const int16_t id = sim.id;
            os2::SimActor* actor = sim.actor;
            const os2::vm::Object* o = m.object(id);
            if (!actor || !o || !o->person || !gObjectView->shown(id))
                continue;
            static const char* forced = std::getenv("OS2_FORCE_CENSOR"); // test aid: these flags for every Sim
            const uint16_t flags = forced ? uint16_t(std::strtoul(forced, nullptr, 0))
                                          : uint16_t(o->person->data[size_t(os2::censor::kPersonData)]);
            const bool female = o->person->data[65] == 1; // Gender (STR# 0xC8)
            const auto found = os2::censorBoxes(*actor, flags, female);
            for (const auto& b : found)
                boxes.push_back({simd_make_float3(b.lo[0], b.lo[1], b.lo[2]), simd_make_float3(b.hi[0], b.hi[1], b.hi[2])});
            if (std::getenv("OS2_DEBUG_CENSOR")) {
                char line[256];
                std::snprintf(line, sizeof line, "flags 0x%02x, %zu box(es)", flags, found.size());
                if (lastReport[id] != line) {
                    lastReport[id] = line;
                    std::fprintf(stderr, "opensims2: censor of %d: %s\n", id, line);
                    for (const auto& b : found)
                        std::fprintf(stderr, "opensims2:   (%.2f %.2f %.2f) - (%.2f %.2f %.2f)\n", b.lo[0], b.lo[1], b.lo[2], b.hi[0],
                                     b.hi[1], b.hi[2]);
                }
            }
        }
    }
    renderer.setCensorBoxes(std::move(boxes));
}

// The lot's particle effects (app/lot_effects.hpp): what the VM starts, and
// --effect <name>[@<object id>[:<bone index>] | @x,y,z] (repeatable) by hand.
std::unique_ptr<os2::LotEffects>& gEffects = gScene.effects;
bool& gEffectsFailed = gScene.effectsFailed;
void updateEffects(os2::Renderer& renderer, double seconds) {
    if (!gEffects && !gEffectsFailed && gIndex) {
        try {
            gEffects = std::make_unique<os2::LotEffects>(renderer, *gIndex, gameDir() / "TSData/Res");
            // The game's hidden PlumbBob's glow goes with the app's plumbob.
            gEffects->placementOverride = [](int16_t id) -> std::optional<os2::Xform> {
                if (gAppPlumbobAt && gHiddenPlumbobs.count(id))
                    return gAppPlumbobAt;
                return std::nullopt;
            };
            // Balloon icons the app draws: a Sim's portrait (0x10), a want's or fear's icon (0x2000).
            gEffects->iconImage = [](const os2::vm::ActiveEffect& e) -> std::optional<os2::RgbaImage> {
                if (e.iconSim && gSimulation) {
                    // The variable holds the Sim's neighbor ID (the memories'
                    // and socials' balloons; inferred), else its object ID.
                    for (const auto& [id, o] : gSimulation->machine().objects())
                        if (o.person && os2::vm::neighborIdOf(o) == e.iconSim)
                            if (auto p = simPortrait(id))
                                return p;
                    if (auto p = neighborPortrait(e.iconSim))
                        return p;
                    return simPortrait(e.iconSim);
                }
                if (e.wantSim && gSimulation && gIndex) {
                    os2::vm::VM& m = gSimulation->machine();
                    const os2::vm::Object* sim = m.object(e.wantSim);
                    os2::vm::WantSystem* ws = os2::vm::wantSystem(m);
                    const os2::vm::SimWants* w = sim && ws ? os2::vm::simWants(m, os2::vm::neighborIdOf(*sim)) : nullptr;
                    const auto& list = w ? (e.wantFear ? w->fears : w->wants) : std::vector<os2::vm::WantPtr>{};
                    if (e.wantSlot >= 0 && size_t(e.wantSlot) < list.size() && list[size_t(e.wantSlot)])
                        if (const auto* d = ws->library->definition(list[size_t(e.wantSlot)]->data.guid))
                            if (uint32_t icon = d->primaryIcon ? d->primaryIcon : d->secondaryIcon)
                                if (auto bytes = gIndex->read(os2::Tgi{os2::kTypeUiImage, os2::kUiImageGroup, icon, 0}))
                                    return os2::decodeUiImage(*bytes);
                }
                return std::nullopt;
            };
            NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
            for (NSUInteger i = 1; i + 1 < args.count; ++i) {
                if (![args[i] isEqualToString:@"--effect"])
                    continue;
                std::string spec = args[i + 1].UTF8String, name = spec;
                int16_t object = 0;
                int slot = -1;
                os2::fx::V3 at{};
                if (auto a = spec.find('@'); a != std::string::npos) {
                    name = spec.substr(0, a);
                    const std::string where = spec.substr(a + 1);
                    if (std::sscanf(where.c_str(), "%f,%f,%f", &at[0], &at[1], &at[2]) != 3) {
                        object = int16_t(std::stoi(where));
                        if (auto c = where.find(':'); c != std::string::npos)
                            slot = std::stoi(where.substr(c + 1));
                    }
                }
                if (!gEffects->addManual(name, object, at, slot))
                    std::fprintf(stderr, "opensims2: --effect: no effect named %s\n", name.c_str());
            }
        } catch (const std::exception& e) {
            gEffectsFailed = true;
            std::fprintf(stderr, "opensims2: effects: %s\n", e.what());
        }
    }
    if (!gEffects)
        return;
    // The effect events of the animations playing (Sims, animated objects, a cinematic's actors).
    std::vector<const os2::SimActor*> animated;
    for (const auto& s : gLotSims)
        animated.push_back(s.actor);
    for (const auto& [id, a] : gObjectActors)
        if (a)
            animated.push_back(a);
    if (gCinematic)
        for (const auto* a : gCinematic->createdActors())
            animated.push_back(a);
    gEffects->animationEffects(animated, seconds);
    gEffects->update(seconds, gSimulation ? &gSimulation->machine() : nullptr, gObjectView.get(),
                     [](int16_t id) -> const os2::SimActor* {
                         for (const auto& s : gLotSims)
                             if (s.id == id)
                                 return s.actor;
                         return nullptr;
                     });
    if (gAudio) { // the effects' sounds (fire, flies, sparks, ...)
        std::vector<os2::fx::EffectSound> sounds;
        gEffects->collectSounds(sounds);
        gAudio->effectSounds(sounds);
    }
}

void placePlumbob(os2::Renderer& renderer, double seconds) {
    gAppPlumbobAt.reset();
    if (gPlumbob < 0 || !gPie)
        return;
    simd_float4x4 at = matrix_identity_float4x4;
    at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
    for (const auto& s : gLotSims) {
        if (s.id != gPie->sim() || gVmPlumbob || cinematicPlaying())
            continue;
        // Not over a Sim out of the world (at work, at school): it hung in
        // the sky over where the actor was last (live QA 15).
        if (!simInTheWorld(s.id))
            continue;
        float top = s.actor->position[2];
        for (const auto& p : s.actor->posed.parts)
            for (const auto& v : p.mesh.positions)
                top = std::max(top, v[2]);
        const float angle = float(std::fmod(seconds / kPlumbobSpinSeconds, 1.0) * 2 * M_PI);
        at = simd_matrix4x4(simd_quaternion(angle, simd_make_float3(0, 0, 1)));
        at.columns[3] =
            simd_make_float4(s.actor->position[0], s.actor->position[1], top + kPlumbobGap - gPlumbobBottom, 1);
        gAppPlumbobAt = os2::Xform{os2::quatFromEulerXYZ(0, 0, angle),
                                   {s.actor->position[0], s.actor->position[1], top + kPlumbobGap - gPlumbobBottom}};
        if (std::getenv("OS2_DEBUG_FX_MOVE"))
            std::fprintf(stderr, "opensims2: app plumbob at (%.2f %.2f %.2f)\n", s.actor->position[0], s.actor->position[1],
                         top + kPlumbobGap - gPlumbobBottom);
    }
    renderer.setInstanceTransform(gPlumbob, at);
}

} // namespace os2app
