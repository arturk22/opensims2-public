// The lot's actors: the Sims the VM makes while the lot plays, and every
// actor posed each frame (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// Sims the VM makes while the lot plays (a baby born, the debug
// tombstone's "New Baby"): a person object the lot did not load gets an
// actor from its character file (a newborn's from the baby character
// writer, gBornCharacters; anyone else's by its OBJD GUID in the
// neighborhood's Characters folder or the session's), attached to the VM
// like the lot's own Sims. Persons without a character file (templates of
// NPCs not written yet) stay undrawn.

std::optional<fs::path> characterFileOf(uint32_t guid) {
    auto& byDir = gScene.characterFiles; // (the neighborhood's and session's files: per scene)
    for (const fs::path& dir : {os2::sessionCharactersDir(), gLotCharactersDir}) {
        if (dir.empty())
            continue;
        auto& files = byDir[dir];
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".package" || std::any_of(files.begin(), files.end(), [&](const auto& f) {
                    return f.second == e.path();
                }))
                continue;
            try {
                os2::DbpfPackage pkg(e.path());
                for (const auto& x : pkg.entries())
                    if (x.tgi.type == 0x4F424A44) { // OBJD
                        files[os2::ObjectDefinition::parse(pkg.read(x)).guid()] = e.path();
                        break;
                    }
            } catch (const std::exception& err) {
                os2::logOnce("character files", e.path().filename().string(), err);
            }
        }
        if (auto it = files.find(guid); it != files.end())
            return it->second;
    }
    return std::nullopt;
}

void adoptVmSims(os2::Renderer& renderer) {
    if (!gSimulation || !gIndex || !gObjectView)
        return;
    os2::vm::VM& m = gSimulation->machine();
    // Sims whose object the VM removed (an NPC gone home, a carpool or bus
    // driver whose vehicle left): their actors go, or they stood frozen in
    // their last pose where they were, following whatever object got the
    // ID next (live QA 15: the drivers sitting in the street, the maid
    // kneeling by a puddle for a day). The actor itself stays allocated
    // (other views may still point at it), moved out of sight.
    for (int16_t id : gSimulation->goneSims()) {
        gSimulation->detach(id);
        for (auto it = gLotSims.begin(); it != gLotSims.end();)
            if (it->id == id) {
                simd_float4x4 away = matrix_identity_float4x4;
                away.columns[3] = simd_make_float4(0, 0, -1000, 1);
                if (const int instance = renderer.instanceOf(it->actor->model); instance >= 0)
                    renderer.setInstanceTransform(instance, away);
                gCarriedSims.erase(it->actor);
                it = gLotSims.erase(it);
            } else {
                ++it;
            }
        gObjectView->removeSim(id);
        if (gLighting)
            gLighting->removeSim(id);
        if (gPie)
            gPie->setMovingBox(os2::vm::ObjectId(id), simd_float3(0), simd_float3(0));
        gSimsLookedAt.erase(id); // a Sim given the ID later is adopted anew
        std::printf("opensims2: Sim %d leaves the lot (its object is gone)\n", id);
    }
    std::vector<int16_t> fresh;
    for (const auto& [id, o] : m.objects())
        if (o.person && (o.position.inWorld() || o.data[os2::vm::objdata::ContainerId] != 0) && !gSimsLookedAt.count(id))
            fresh.push_back(id);
    for (int16_t id : fresh) {
        gSimsLookedAt.insert(id);
        if (std::any_of(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; }))
            continue;
        const os2::vm::Object& o = *m.object(id);
        const int16_t neighbor = o.person->data[os2::vm::persondata::NeighborId];
        std::optional<fs::path> file;
        if (auto it = gBornCharacters.find(neighbor); it != gBornCharacters.end())
            file = it->second;
        else if (const os2::vm::Neighbor* n = m.neighbor(neighbor))
            file = characterFileOf(n->guid);
        // The game's unique NPCs (the Grim Reaper, the Pollination
        // Technician) keep their character in their object's group.
        if (const os2::vm::Neighbor* n = m.neighbor(neighbor); !file && n && n->guid) {
            auto& embedded = gScene.embeddedCharacters; // (written in the session's folder: per scene)
            auto [it, fresh] = embedded.try_emplace(n->guid);
            if (fresh)
                try {
                    char stem[32];
                    std::snprintf(stem, sizeof stem, "Unique_%08X.package", n->guid);
                    const fs::path out = os2::sessionCharactersDir() / "NPCs" / stem;
                    if (os2::cas::writeEmbeddedCharacter(*gIndex, n->guid, out))
                        it->second = out;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "opensims2: Sim %d (neighbor %d): %s\n", id, neighbor, e.what());
                }
            file = it->second;
        }
        if (!file) {
            std::fprintf(stderr, "opensims2: Sim %d (neighbor %d): no character file, not drawn\n", id, neighbor);
            continue;
        }
        try {
            auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(*gIndex, simParts(*gIndex), *file));
            actor->position = {o.position.x, o.position.y, gSimulation->floorHeight(o.position.level, o.position.x, o.position.y)};
            actor->heading = -float(o.data[os2::vm::objdata::Direction]) * float(M_PI) / 4; // as cinematicOrigin (inferred)
            const char age = char(std::tolower(static_cast<unsigned char>(actor->skin.skeleton.name.empty() ? 'a' : actor->skin.skeleton.name[0])));
            const std::string idle = age == 'b'   ? "b-floor-idle-loopBreathe"
                                     : age == 'p' ? "p-idle-neutral-lookAround"
                                     : age == 'c' ? "c-idle-neutral-armsSide-wtC-breatheLoop50"
                                                  : "a-idle-neutral-armsSide-wtC-breatheLoop";
            actor->cue(gSceneNow, actor->animation(*gIndex, idle));
            actor->model = renderer.addObject(actor->skin.model, *gIndex, true);
            renderer.addInstance(actor->model, matrix_identity_float4x4);
            os2::DbpfPackage character(*file);
            gSimulation->attach(*actor, id, os2::readCharacter(character, *gIndex), idle);
            gObjectView->addSim(id, actor.get());
            if (gLighting)
                gLighting->addSim(id, actor.get());
            std::printf("opensims2: Sim %d (neighbor %d) joins the lot at tick %llu: %s, skeleton %s, at (%.2f, %.2f)\n", id,
                        neighbor, (unsigned long long)m.ticks, file->filename().string().c_str(),
                        actor->skin.skeleton.name.c_str(), o.position.x, o.position.y);
            std::fflush(stdout);
            gLotSims.push_back({actor.get(), id, *file, idle});
            gActors.push_back(std::move(actor));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: %s\n", id, e.what());
        }
    }
}

// The Sims whose hands reach another Sim's bones (a kiss, a hug, a slow
// dance: the IK object of their Animate Sim or overlays) posed after them:
// posed again once all are, their goals are of the same frame (the seat
// module's rule 5; a frame late, the hands were 6-10 cm off).
std::vector<os2::SimActor*> simsReachingLaterSims() {
    std::vector<os2::SimActor*> out;
    if (!gSimulation)
        return out;
    std::map<int16_t, size_t> order;
    for (size_t i = 0; i < gLotSims.size(); ++i)
        order[gLotSims[i].id] = i;
    for (size_t i = 0; i < gLotSims.size(); ++i) {
        const os2::vm::Object* o = gSimulation->machine().object(gLotSims[i].id);
        if (!o || !o->person)
            continue;
        std::vector<int16_t> targets{o->animation.eventObject};
        for (const auto& ov : o->animation.overlays)
            targets.push_back(ov.ikObject);
        for (int16_t t : targets)
            if (const auto it = order.find(t); t && it != order.end() && it->second > i) {
                out.push_back(gLotSims[i].actor);
                break;
            }
    }
    return out;
}

// Poses every actor at `seconds` and hands the vertices to the renderer.
// The Sims first: objects in their hands follow their bones (gObjectView),
// then the objects' actors at the places that gives.
void poseActors(os2::Renderer& renderer, double seconds) {
    pollTutorial(seconds); // (the real clock)
    if (gSimulation)
        seconds = gSimulation->update(seconds); // the scene clock (game speed)
    // The objects the VM animates, posed at this time before the Sims: the
    // Sims' IK goals on their bones (type 2: a chair's pull and seat-grab
    // targets for its get-in and get-out) are read from them as the Sims
    // are posed. Posed only after the Sims, a chair's bones were a frame old,
    // or as old as the last shot without a window: the hands reached where
    // the chair had been (an arm hanging long and straight, or stretched
    // back to the table). They are posed again below, at the places the
    // Sims' hands give the objects they hold.
    for (auto& [id, a] : gObjectActors)
        if (a)
            a->update(seconds);
    adoptVmSims(renderer);
    keepSelectedSimHome();
    updateCinematic(renderer, seconds); // first: a playing scene's camera cuts the walls
    updateLotView(renderer, false);
    if (gDoors && gSimulation) {
        std::vector<os2::Vec3> sims;
        for (const auto& s : gLotSims)
            sims.push_back(s.actor->position);
        gDoors->update(gSimulation->machine(), sims, seconds);
    }
    auto isObject = [](const os2::SimActor* a) {
        for (const auto& [id, o] : gObjectActors)
            if (o == a)
                return true;
        return false;
    };
    // Body shape from person data 81 "Body Flags" and 58 age
    // (os2::bodyShapeMorphs); OS2_BODY_FLAGS=<flags> overrides the flags (test aid).
    if (gSimulation)
        for (auto& s : gLotSims)
            if (const auto* o = gSimulation->machine().object(s.id); o && o->person) {
                const char* force = std::getenv("OS2_BODY_FLAGS");
                s.actor->bodyMorphs = os2::bodyShapeMorphs(force ? int16_t(std::atoi(force)) : o->person->data[81],
                                                           o->person->data[58]);
            }
    // A carried baby or toddler (contained in a Sim's slot, "Pickup - Do
    // Carry Sim") is drawn at the carrier's slot bone, after the carrier is
    // posed (the VM keeps it where it was picked up).
    std::map<const os2::SimActor*, std::pair<const os2::SimActor*, std::string>> carried;
    std::set<const os2::SimActor*>& wasCarried = gCarriedSims;
    gCarriers.clear();
    if (gSimulation && gObjectView)
        for (const auto& s : gLotSims) {
            const auto* o = gSimulation->machine().object(s.id);
            const int16_t c = o && o->person ? o->data[os2::vm::objdata::ContainerId] : 0;
            const auto* co = c ? gSimulation->machine().object(c) : nullptr;
            const auto carrier = co && co->person ? std::find_if(gLotSims.begin(), gLotSims.end(),
                                                                  [&](const LotSim& x) { return x.id == c; })
                                                  : gLotSims.end();
            if (carrier != gLotSims.end()) {
                std::string bone = gObjectView->containerSlotBone(c, o->data[os2::vm::objdata::SlotNumber]);
                if (bone.empty())
                    bone = "BellyTarget_surface"; // the carry animations' grip (inferred)
                carried[s.actor] = {carrier->actor, bone};
                gCarriers[s.actor] = carrier->actor;
                wasCarried.insert(s.actor);
            } else if (wasCarried.erase(s.actor)) {
                s.actor->transform.reset();
            }
        }
    for (auto& a : gActors)
        if (!isObject(a.get()) && !carried.count(a.get()))
            a->update(seconds);
    // The carry poses snap the carried Sim's root to the carrier's slot
    // bone ("snapObjectRoot=buSkel:__buSkel__|snapTargetSlot=auSkel:
    // BellyTarget_surface" in the a2b pick up / put down animations): the
    // bone's whole transform places it (b2a-carry-belly holds it there).
    for (auto& [a, by] : carried) {
        const int b = by.first->skin.skeleton.find(by.second);
        const_cast<os2::SimActor*>(a)->transform =
            b >= 0 && size_t(b) < by.first->bones.size() ? by.first->bones[size_t(b)] : by.first->placement();
        const_cast<os2::SimActor*>(a)->update(seconds);
    }
    // The carriers again: their hands reach the carried Sim's bones by IK
    // (a2b-carry-belly's left hand under the baby's bottom, the bottle
    // feeder's right hand at its head: IK targets of type 2), which only now
    // stand where the carrier holds them (the slot bone does not move with
    // the arms, so one more pass settles it).
    {
        std::set<const os2::SimActor*> again;
        for (const auto& [a, by] : carried)
            again.insert(by.first);
        for (os2::SimActor* a : simsReachingLaterSims())
            again.insert(a);
        for (auto& a : gActors)
            if (again.count(a.get()) && !carried.count(a.get()))
                a->update(seconds);
    }
    if (gSnaps)
        gSnaps->update(seconds);
    choosePlumbob(renderer);
    if (gObjectView)
        gObjectView->update();
    if (gLighting)
        gLighting->update(seconds);
    else if (gStaticShadowLight && gLightMapLayout && os2::graphicsOptions().simShadows) {
        // Without the VM the Sims still cast their shadows (cShadow style 1,
        // docs/lighting-mac.md 9): inside or outside by the light maps' room
        // under the feet, on the floor nearest their height.
        std::vector<os2::Renderer::ShadowCaster> casters;
        const os2::LotLightMapLayout& lm = *gLightMapLayout;
        for (const auto& s : gLotSims) {
            const int instance = renderer.instanceOf(s.actor->model);
            if (instance < 0)
                continue;
            const os2::Xform at = s.actor->placement();
            const os2::Vec3 feet = at.apply({0, 0, 0}), side = at.apply({1, 0, 0});
            int level = lm.minLevel;
            for (int l = lm.minLevel; l < lm.minLevel + lm.levels; ++l)
                if (std::fabs(lm.elevationAt(l, feet[0], feet[1]) - feet[2]) < std::fabs(lm.elevationAt(level, feet[0], feet[1]) - feet[2]))
                    level = l;
            os2::Renderer::ShadowCaster c;
            c.instance = instance;
            c.feet = simd_make_float3(feet[0], feet[1], feet[2]);
            c.heading = std::atan2(side[1] - feet[1], side[0] - feet[0]);
            c.outside = lm.outdoor(level, lm.roomAt(level, feet[0], feet[1]));
            c.direction = c.outside ? gStaticShadowLight->sunTravel : simd_make_float3(0, 0, -1);
            c.color = gStaticShadowLight->outdoorColor;
            casters.push_back(c);
        }
        renderer.setShadowCasters(std::move(casters));
    }
    for (auto& a : gActors) {
        if (isObject(a.get()))
            a->update(seconds);
        renderer.updateObject(a->model, a->posed);
    }
    placePlumbob(renderer, seconds);
    sampleDrawnPlaces(seconds);
    updateLotView(renderer, true);
    if (gTv)
        gTv->update(seconds);
    updateEffects(renderer, seconds);
    updateCensor(renderer);
    updateAudio(seconds);
}

} // namespace os2app
