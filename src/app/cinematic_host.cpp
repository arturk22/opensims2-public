// The host of the cinematics (app/cinematic_player.hpp): the scenes the VM
// asks for or --cinematic, the state they force, the camera, the snapshot
// crossfades and the letterbox (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

// Cinematics (app/cinematic_player.hpp): the Cinematic primitive's scene
// (camera from its origin actor, the actors' automatic animations, the
// light rig on the actors of its render groups), drawn letterboxed with its
// post-processing (render/screen_fx_layer.hpp) and a snapshot crossfade at
// its crossFadeSnap events and when it ends; or --cinematic
// <scene>[@id[,id...]] (the actors in the CINE's order; default: the lot's
// Sims for the Sim actors) to try one. Everything runs on the scene clock.
std::unique_ptr<os2::ScreenFxLayer> gScreenFx;
os2::ScreenFxLayer::Params& gScreenFxParams = gScene.screenFxParams;
uint64_t gCinematicSerialSeen = 0;          // the last VM request taken (CinematicRequest::serial)
std::optional<double>& gCinematicStartAt = gScene.cinematicStartAt;    // when the prepared scene starts (scene clock; may be past)
double& gCinematicPrepared = gScene.cinematicPrepared;             // scene clock when it was prepared (waits for an actor's animation)
double& gCinematicLast = gScene.cinematicLast;                  // scene clock of the last update
bool& gCinematicWholeHouse = gScene.cinematicWholeHouse;          // the scene shows the whole house (flag 0x08, moveIn)
std::map<int16_t, double>& gSimAnimationAt = gScene.simAnimationAt;  // each Sim's last Animate Sim start (scene clock)
double& gCrossfadeAt = gScene.crossfadeAt;        // scene clock of the last snapshot
double& gSceneNow = gScene.sceneNow;              // the scene clock of the last poseActors
std::optional<os2::FrameView>& gLastFrameView = gScene.lastFrameView; // what was drawn last (snapshots)
uint32_t gLastFrameW = 1280, gLastFrameH = 720;
constexpr double kCrossfadeSeconds = 2.0; // crossfadeshort/standard: screen mode 4, length 2, strength 1 -> 0
constexpr double kCinematicWait = 1.0;    // without "Start Animations Now": the longest wait for an actor

CinematicForced& gCinematicForced = gScene.cinematicForced;

void forceCinematicState(os2::Renderer& renderer, bool wholeHouse) {
    if (!gCinematicForced.active) {
        gCinematicForced = {};
        gCinematicForced.active = true;
        if (gSimulation) {
            const auto& g = gSimulation->machine().globals;
            gCinematicForced.speed = g[os2::vm::global::Speed];
            gCinematicForced.paused = g[os2::vm::global::Paused];
        }
        // cTSSGSystem's Start (BV FUN_007c3898, docs/ui-live-mac.md 8d):
        // the floor and the wall mode are kept for Stop; the floor goes to
        // the first actor's (the top one for the whole house) and the walls
        // to cutaway (up with the roof for the whole house); the actors where
        // they stand become the cutaway's targets (AddTargetLocation) and the
        // cutaway exposes them (SetCinematicsCutaway; cinematicLotView).
        // (The game skips the floor, the mode and the targets with a flag
        // read from a property, +0x194, off by default: not done here.)
        // (the wall view: updateLotView rebuilds the lot model for it)
        gCinematicForced.walls = gWallView.mode();
        gWallView.setMode(wholeHouse ? 3 : 1);
        // The floor is the origin actor's (the CINE's OriginActorIndex, the
        // one the camera and the light rig follow: Start's +0x12C, its level
        // less one; ProposeAccept's is its second actor), not the first's.
        std::optional<int> firstLevel;
        if (gSimulation && gCinematic) {
            const os2::vm::VM& m = gSimulation->machine();
            const auto& actors = gCinematic->actors();
            const uint32_t origin = gCinematic->camera().scene().originActor;
            const int16_t originId = origin < actors.size() && actors[origin] ? actors[origin]
                                     : actors.empty()                         ? int16_t(0)
                                                                              : actors.front();
            for (const int16_t id : actors) {
                const auto it = id > 0 ? m.objects().find(os2::vm::ObjectId(id)) : m.objects().end();
                if (it == m.objects().end() || !it->second.position.inWorld())
                    continue;
                const auto& p = it->second.position;
                const int level = std::max(p.level - 1, 0); // the VM counts from 1
                if (!firstLevel && id == originId)
                    firstLevel = level;
                gCinematicForced.targets.push_back({p.x, p.y, float(level)});
            }
        }
        if (gCameraFollow) {
            gCinematicForced.level = gCameraFollow->level();
            if (wholeHouse)
                gCameraFollow->setLevel(gCameraFollow->highestLevel());
            else if (firstLevel)
                gCameraFollow->setLevel(*firstLevel);
        }
        (void)renderer;
    }
    // Held for the whole scene, the speed buttons waiting (ours: Start and
    // Stop, BV 0x7c3898/0x7c56b4, do not touch the simulator's speed; they
    // save and set the object at +0xB4's vtable 0x74/0x5C, which would be
    // the animation timer's MaxElapsedTicksPerFrame, to 1).
    if (gSimulation) {
        auto& g = gSimulation->machine().globals;
        g[os2::vm::global::Speed] = 2;
        g[os2::vm::global::Paused] = 0;
    }
}

void restoreCinematicState(os2::Renderer& renderer) {
    if (!gCinematicForced.active)
        return;
    if (gSimulation) {
        auto& g = gSimulation->machine().globals;
        g[os2::vm::global::Speed] = gCinematicForced.speed;
        g[os2::vm::global::Paused] = gCinematicForced.paused;
    }
    if (gCinematicForced.walls)
        gWallView.setMode(*gCinematicForced.walls);
    (void)renderer;
    if (gCinematicForced.level && gCameraFollow)
        gCameraFollow->setLevel(*gCinematicForced.level);
    gCinematicForced = {};
}

// Whether a cinematic's camera is the one drawn now.
bool cinematicPlaying() { return gCinematic && gCinematic->active(gSceneNow); }

// Where an actor stands: a Sim's or an animated object's placement, else
// its VM position and direction (object data 1, eighths of a turn).
std::optional<os2::Xform> cinematicOrigin(int16_t id) {
    for (const auto& s : gLotSims)
        if (s.id == id)
            return s.actor->placement();
    if (auto it = gObjectActors.find(id); it != gObjectActors.end() && it->second)
        return it->second->placement();
    if (gObjectView && gObjectView->hasModel(id))
        if (auto x = gObjectView->placement(id))
            return x;
    if (!gSimulation) {
        // No VM (--cinematic on a lot without --run-vm): where the lot put it
        // (the moveIn's mailbox; it fell back to the first Sim before).
        if (auto it = gPlacedObjects.find(id); it != gPlacedObjects.end() && !it->second.contained) {
            const auto& q = it->second.rotation;
            return os2::Xform{os2::Quat{q[0], q[1], q[2], q[3]}, it->second.position};
        }
        return std::nullopt;
    }
    const os2::vm::Object* o = gSimulation->machine().object(id);
    if (!o || !o->position.inWorld())
        return std::nullopt;
    const float heading = -float(o->data[1]) * float(M_PI) / 4; // inferred direction convention
    return os2::Xform{os2::quatFromEulerXYZ(0, 0, heading),
                      {o->position.x, o->position.y, gSimulation->floorHeight(o->position.level, o->position.x, o->position.y)}};
}

// The renderer's snapshot of the last frame drawn, for a crossfade from it.
void cinematicSnapshot(os2::Renderer& renderer, double now) {
    if (!gScreenFx || !gLastFrameView)
        return;
    os2::ScreenFxLayer::Params keep = gScreenFxParams;
    gScreenFxParams.snapshotAlpha = 0; // the frame as it was, without an older snapshot
    gScreenFxParams.flash[3] = 0;
    renderer.setReflections(reflectionsOn());
    const os2::RgbaImage img = renderer.capture(gLastFrameW, gLastFrameH, *gLastFrameView, simd_make_float4(0, 0, 0, 1));
    gScreenFxParams = keep;
    gScreenFx->setSnapshot(img);
    gCrossfadeAt = now;
}

// The lot as a playing scene's camera sees it: the walls cut away for its
// view, not the live camera's ("Show Entire House" already put the walls up
// and the top floor: forceCinematicState). Back to the live view when the
// scene ends.
os2::LotViewOptions cinematicLotView() {
    const auto shot = gCinematic->camera().shot(gSceneNow);
    std::array<float, 2> look{shot.forward.x, shot.forward.y};
    if (std::hypot(look[0], look[1]) < 1e-3f) // looking straight down: the camera's up is "into the scene"
        look = {shot.up.x, shot.up.y};
    os2::LotViewOptions v = gWallView.options(look, viewLevel());
    v.camera = std::array<float, 3>{shot.eye.x, shot.eye.y, shot.eye.z}; // the scene camera's position
    // Expose (cWallCutawayManager, docs/ui-live-mac.md 8d): the targets on
    // the level shown; one outdoors exposes its near half circle, one
    // indoors puts its room in the memory of the last 3 rooms
    // (cutawayMemorySize), whose walls facing the camera are cut.
    if (gCinematicForced.active && gLotShape && v.walls == os2::LotViewOptions::Walls::Cutaway &&
        v.level != os2::LotViewOptions::kAllLevels) {
        v.cinematics = true;
        constexpr size_t kCutawayMemorySize = 3;
        auto& memory = gScene.cutawayRooms;
        for (const auto& t : gCinematicForced.targets) {
            if (int(t[2]) != v.level)
                continue;
            const uint16_t room = os2::lotRoomAt(*gLotShape, v.level, t[0], t[1]);
            if (!room) {
                v.exposeOutside.push_back({t[0], t[1], gLotShape->elevationAt(v.level, t[0], t[1])});
            } else if (std::find(memory.begin(), memory.end(), room) == memory.end()) {
                if (memory.size() >= kCutawayMemorySize)
                    memory.erase(memory.begin());
                memory.push_back(room);
            }
        }
        v.exposeRooms = memory;
    }
    return v;
}

// Prepares a scene at `preparedAt` (scene clock: when the VM asked for it,
// which can be before this frame); with `now` it starts then, else when
// one of its actors starts an animation from then on, or a second later.
void startCinematic(os2::Renderer& renderer, const std::string& scene, std::vector<int16_t> actors, int16_t origin,
                    bool mirror, bool now, double preparedAt, bool guessSims, bool wholeHouse) {
    if (gCinematic) {
        std::printf("opensims2: cinematic %s: another cinematic is playing\n", scene.c_str());
        std::fflush(stdout);
        return;
    }
    try {
        os2::CinematicPlayer::Host host;
        host.sim = [](int16_t id) -> os2::SimActor* {
            for (const auto& s : gLotSims)
                if (s.id == id)
                    return s.actor;
            return nullptr;
        };
        host.objectAnimation = [](int16_t id, const std::string& anim, double at) -> os2::SimActor* {
            if (gSimulation && gSimulation->onObjectAnimation)
                gSimulation->onObjectAnimation(id, anim, at);
            auto it = gObjectActors.find(id);
            return it == gObjectActors.end() ? nullptr : it->second;
        };
        host.placement = cinematicOrigin;
        auto player = std::make_unique<os2::CinematicPlayer>(renderer, *gIndex, std::move(host), scene, actors,
                                                             origin, mirror);
        std::printf("opensims2: cinematic %s prepared at %.2f s (%.2f s, camera %s%s%s%s)\n",
                    player->camera().scene().name.c_str(), std::isfinite(preparedAt) ? preparedAt : gSceneNow,
                    player->camera().duration(),
                    player->camera().scene().cameraAnimation.c_str(), mirror ? ", mirrored" : "",
                    now ? "" : ", waits for its actors", wholeHouse ? ", whole house" : "");
        player->setGuessSimAnimations(guessSims);
        gCinematic = std::move(player);
        gCinematicPrepared = preparedAt;
        gCinematicWholeHouse = wholeHouse || strcasecmp(scene.c_str(), "moveIn") == 0; // PrepCinematicScene forces moveIn's
        gCinematicStartAt.reset();
        if (now)
            gCinematicStartAt = preparedAt;
        else // an actor that already started its part (the same tick as the primitive)
            for (int16_t a : gCinematic->actors())
                if (auto it = gSimAnimationAt.find(a); a && it != gSimAnimationAt.end() && it->second >= preparedAt - 1e-3)
                    gCinematicStartAt = std::min(gCinematicStartAt.value_or(it->second), it->second);
    } catch (const std::exception& e) {
        std::printf("opensims2: cinematic %s: %s\n", scene.c_str(), e.what());
    }
    std::fflush(stdout);
}

// A Sim starts an animation at `at`: an actor of the prepared scene starts it.
void cinematicActorAnimates(int16_t id, double at) {
    gSimAnimationAt[id] = at;
    if (!gCinematic || gCinematic->started() || gCinematicStartAt)
        return;
    for (int16_t a : gCinematic->actors())
        if (a == id && at >= gCinematicPrepared - 1e-3) {
            gCinematicStartAt = at;
            return;
        }
}

// Starts the cinematics the VM asked for since the last call; runs and ends
// the one playing. Called every frame and, headless, every step of the VM
// between shots, so a scene starts when the tree asked for it.
void updateCinematic(os2::Renderer& renderer, double now) {
    gSceneNow = now;
    for (const auto& r : os2::vm::cinematicRequests()) {
        if (r.serial <= gCinematicSerialSeen)
            continue;
        gCinematicSerialSeen = r.serial;
        if (std::getenv("OS2_DEBUG_CINE"))
            std::fprintf(stderr, "opensims2: tick %llu: object %d asks for cinematic \"%s\" (flags 0x%02X, %zu actors)\n",
                         (unsigned long long)r.tick, r.me, r.scene.c_str(), r.flags, r.actors.size());
        if (r.scene.empty())
            continue;
        const double at = gSimulation ? std::min(now, gSimulation->tickTime(r.tick)) : now;
        std::vector<int16_t> actors(r.actors.begin(), r.actors.end());
        startCinematic(renderer, r.scene, actors, r.me, r.mirrored(), r.startNow(), at, false, r.wholeHouse());
    }
    if (gLighting && !gLighting->extraLights)
        gLighting->extraLights = [](int16_t id) {
            std::vector<os2::LotLighting::ExtraLight> out;
            if (gCinematic)
                for (const auto& l : gCinematic->lightsFor(id))
                    out.push_back({l.position, l.color, l.falloff, l.range, l.spot, l.cone, l.penumbra, l.direction});
            return out;
        };
    if (gCinematic && !gCinematic->started()) {
        if (!gCinematicStartAt && now - gCinematicPrepared >= kCinematicWait)
            gCinematicStartAt = gCinematicPrepared + kCinematicWait;
        if (gCinematicStartAt) {
            const double at = std::min(*gCinematicStartAt, now);
            gCinematicLast = at - 1e-3;
            if (gCinematic->start(at)) {
                std::printf("opensims2: cinematic %s starts at %.2f s%s\n", gCinematic->camera().scene().name.c_str(), at,
                            gCinematicWholeHouse ? " (whole house)" : "");
                forceCinematicState(renderer, gCinematicWholeHouse);
                if (gGameAudio) // PlayCinematicScene's sound dipping (docs/audio.md)
                    gGameAudio->setDipping(true);
            } else {
                std::printf("opensims2: cinematic %s: its origin actor is not in the world\n",
                            gCinematic->camera().scene().name.c_str());
                gCinematic.reset();
            }
            std::fflush(stdout);
        }
    }
    if (gCinematic && gCinematic->started()) {
        if (!gCinematic->active(now)) {
            std::printf("opensims2: cinematic %s ended at %.2f s\n", gCinematic->camera().scene().name.c_str(), now);
            std::fflush(stdout);
            cinematicSnapshot(renderer, now); // the last shot fades into the game's camera
            gCinematic->finish();
            gCinematic.reset();
            restoreCinematicState(renderer);
            if (gGameAudio) // UpdateCinematicScene ends the dipping with the scene
                gGameAudio->setDipping(false);
        } else {
            forceCinematicState(renderer, gCinematicWholeHouse);
            gCinematic->update(now);
            const double t0 = gCinematicLast - gCinematic->camera().startTime(), t1 = now - gCinematic->camera().startTime();
            for (const auto& e : gCinematic->camera().effects())
                if (e.at > t0 - 1e-6 && e.at <= t1 && e.name.rfind("crossFadeSnap", 0) == 0) {
                    cinematicSnapshot(renderer, now);
                    break;
                }
            gCinematicLast = now;
        }
    }
    os2::ScreenFxLayer::Params p;
    p.time = float(now);
    if (gCinematic && gCinematic->started()) {
        const auto s = gCinematic->camera().shot(now);
        p.letterbox = s.letterbox;
        p.grain = s.grain;
        p.vignette = s.vignette;
        p.bloom = s.bloom;
    }
    const double since = now - gCrossfadeAt;
    if (since >= 0 && since < kCrossfadeSeconds)
        p.snapshotAlpha = float(1.0 - since / kCrossfadeSeconds);
    gScreenFxParams = p;
}

// The frame's view: the cinematic camera while one plays.
os2::FrameView cinematicView(const os2::FrameView& normal, double /*now*/, float aspect) {
    os2::FrameView out = normal;
    if (gCinematic && gCinematic->active(gSceneNow))
        out = {gCinematic->camera().view(gSceneNow), gCinematic->camera().projection(gSceneNow, aspect)};
    gLastFrameView = out;
    return out;
}

// Draws the screen effects (letterbox, crossfade, ...) after the overlays the renderer already has.
void attachLetterbox(os2::Renderer& renderer, MTL::Device* device, uint32_t color, uint32_t depth) {
    if (!gScreenFx)
        gScreenFx = std::make_unique<os2::ScreenFxLayer>(device, color, depth);
    auto previous = renderer.overlay();
    renderer.setOverlay([previous](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gScreenFx) // under the UI
            gScreenFx->encode(enc, w, h, gScreenFxParams);
        // A scene plays without the live UI (inferred), but a dialog it
        // waits on still shows.
        if (previous && (!cinematicPlaying() || (gPie && gPie->dialogOpen())))
            previous(enc, w, h);
    });
}

} // namespace os2app
