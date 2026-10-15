// macOS shell: an AppKit window whose MTKView drives os2::Renderer every frame
// and feeds input to the lot camera. The renderer itself is plain C++ on
// metal-cpp; this file only bridges the Objective-C objects across.
//
// Camera controls, the game's live-mode keys (ui.package accelerators
// 0x6A9362F2/F5): arrows or WASD move (Shift: faster), z/x or NumPad +/- zoom
// a level, = and - held zoom, , and . held rotate, Control/plain/Shift 4-9
// save/restore/jump to camera positions, Return centres on the selected Sim;
// with the mouse as in the game (app/camera_input.hpp): hold the right button
// and move away from where it went down to scroll (offset x 0.25), hold the
// middle button (or Option + right) to rotate and tilt, the wheel zooms, the
// mouse at the window's edge scrolls (--no-edge-scroll). --lighting <state> picks a lighting state from Lighting.txt
// (day, night, neighbourhood, ...); --lot <N001_Lot82 | file.package> shows a
// lot's terrain, floors, walls, roofs, fences, water and objects (--walls
// up | cutaway | down picks the wall mode, cutaway as seen from the starting view). --sim [body,hair,skin tone] shows
// an adult Sim built from catalog parts, --skinned <cres,cres,...> skinned
// meshes on the adult skeleton; either plays --anim <name> (looped; --time
// <seconds> picks the moment of a --screenshot), --overlay <name>[@bone] over
// the bones under <bone>, or --walk <seconds> (idle, a straight walk along
// --heading <degrees>, idle). --character <N001_User00011 | file.package>
// shows a neighbourhood Sim from its character file; with --lot the lot's
// Sims stand where they were saved, playing an idle (--sim-anim <name>). --play-sound <name | TGI>
// plays a sound (the --sound-index'th sample of a track; --sound-wav <out.wav>
// also saves it) and exits without opening a window. --ui <instance-hex> draws a
// UI script from ui.package over the scene (1024x768 canvas, letterboxed;
// --ui-show <id,id,...> shows windows the script hides, e.g. the main menu's
// background 0da36c7d); --ui live draws the game's live-mode panel instead
// (app/live_ui.hpp: puck, Sim panel, needs, family, action queue, wired to
// the VM; it replaces the pie menu's debug queue, which --debug-queue keeps;
// headless, --click presses its buttons and --ui-hover x,y puts the mouse
// over it). With the VM running, a left click on an object opens
// its pie menu for the selected Sim (--sim-id) and picking an entry queues it
// (Esc closes it); headless: --pie <object> [--pie-select N] and --click x,y
// (repeatable), which also start the VM (app/pie_menu.hpp). --vm runs the lot's objects in the SimAntics VM while it
// is shown, with the Sims following it (walking, animations); --vm-walk
// <sim-id>,<object-id>[,<anim>] makes a Sim walk to an object (then play entry
// <anim> of the global animation list); --run-vm <ticks> runs that many ticks
// (the game's 1000/42 a second at normal speed) before a --screenshot, and --frames <n> saves n screenshots
// spread over them (out-1.png ... out-n.png). A dialog the trees put up is
// shown over the scene and pauses the game until a button is clicked;
// headless: --dialog-answer <n> answers every one with button n, and
// --dialog-click x,y|return|escape (repeatable) clicks (or presses a key for) each dialog in turn,
// --dialog-type <text> types into the text box of a dialog, --dialog-demo <type>[:title[:message]] puts one up.
// --movie <name> plays a .movie fullscreen and quits (headless with --screenshot:
// frames and A/V timestamps, app/movie_host.hpp); --intro plays the start-up
// movies before the scene. With a window, scenes load on a worker thread
// behind the game's Reia loading screens (app/scene_loader.hpp,
// docs/loading-mac.md); headless, --loading-shot <png> [--loading-at <f>]
// loads the first scene that way and saves its loading screen, and
// --loading-hold <seconds> keeps a window's loading screen up that long.

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "app/cinematic_player.hpp"
#include "app/ui_panel_util.hpp"
#include "app/loading_screen.hpp"
#include "app/glide_detector.hpp"
#include "app/jump_detector.hpp"
#include "app/seat_check.hpp"
#include "formats/catalog.hpp"
#include "formats/config_text.hpp"
#include "formats/graphics_options.hpp"
#include "app/scene_loader.hpp"
#include "app/tv_broadcast.hpp"
#include "formats/neighborhood.hpp"
#include "formats/ui_script.hpp"
#include "vm/tutorial.hpp"
#include "vm/wants.hpp"
#include "app/movie_host.hpp"
#include "render/movie_layer.hpp"
#include "render/screen_fx_layer.hpp"
#include "vm/engine.hpp"
#include "vm/dialog.hpp"
#include "vm/go_here.hpp"
#include "audio/play_sound.hpp"
#include "formats/camera_config.hpp"
#include "formats/cas_catalog.hpp"
#include "formats/cas_face.hpp"
#include "formats/cas_thumbnails.hpp"
#include "vm/hood_bridge.hpp"
#include "formats/character_write.hpp"
#include "formats/image.hpp"
#include "formats/dbpf.hpp"
#include "formats/lighting_config.hpp"
#include "formats/lot.hpp"
#include "formats/anim.hpp"
#include "formats/lot_model.hpp"
#include "formats/model.hpp"
#include "formats/sim.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "formats/sim_censor.hpp"
#include "formats/lot_objects.hpp"
#include "formats/skinning.hpp"
#include "formats/font.hpp"
#include "formats/ui_image.hpp"
#include "formats/ui_resources.hpp"
#include "formats/ui_screen.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#include "app/buy_mode.hpp"
#include "app/game_cursors.hpp"
#include "app/design_tool.hpp"
#include "app/build_mode.hpp"
#include "app/data_location.hpp"
#include "app/game_flow.hpp"
#include "app/game_data_caches.hpp"
#include "formats/log_once.hpp"
#include "app/wants_panel.hpp"
#include "app/head_shot.hpp"
#include "app/game_save.hpp"
#include "app/hood_mode.hpp"
#include "formats/lot_arrivals.hpp"
#include "app/main_menu.hpp"
#include "app/options_panel.hpp"
#include "app/user_data.hpp"
#include "app/lot_effects.hpp"
#include "app/cas_mode.hpp"
#include "app/live_ui.hpp"
#include "app/tutorial.hpp"
#include "app/pie_menu.hpp"
#include "lot_audio.hpp"
#include "anim_snaps.hpp"
#include "camera_follow.hpp"
#include "wall_view.hpp"
#include "camera_input.hpp"
#include "lot_vm.hpp"
#include "vm_objects.hpp"
#include "lot_lighting.hpp"
#include "door_portals.hpp"
#include "vm/lot.hpp"
#include "vm/placement.hpp"
#include "vm/baby_character.hpp"
#include "app/app_state.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <strings.h>
#include <sys/resource.h>
#include <sys/syslimits.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace os2app {

// The arguments of the scene being shown (app/game_flow.hpp): the process's
// at start-up, replaced when the game flow switches scenes in place.
NSArray<NSString*>* gSceneArgs = nil;
NSArray<NSString*>* sceneArgs() {
    if (!gSceneArgs)
        gSceneArgs = NSProcessInfo.processInfo.arguments;
    return gSceneArgs;
}
void setSceneArguments(const std::vector<std::string>& args) {
    NSMutableArray<NSString*>* a = [NSMutableArray array];
    for (const auto& s : args)
        [a addObject:[NSString stringWithUTF8String:s.c_str()]];
    gSceneArgs = a;
}

// The extracted game (data/game): --data <dir>, else $OS2_DATA, else the first
// data/game found walking up from the executable (the repo when run from build/),
// else the packaged-app places (app/data_location.hpp).
std::optional<fs::path> findGameDir() {
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--data"])
            return fs::path(args[i + 1].UTF8String);
    if (const char* env = std::getenv("OS2_DATA"))
        return fs::path(env);
    fs::path dir = fs::path(NSBundle.mainBundle.executablePath.UTF8String).parent_path();
    for (; !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "data/game" / kCameraConfig))
            return dir / "data/game";
        if (dir == dir.parent_path())
            break;
    }
    // Packaged app: remembered folder, next to the .app, Application Support.
    return os2::locateGameData(NSBundle.mainBundle.executablePath.UTF8String, kCameraConfig);
}

fs::path gameDir() {
    std::optional<fs::path> game = findGameDir();
    if (!game)
        throw std::runtime_error("data/game not found (pass --data <dir> or set OS2_DATA)");
    return *game;
}

os2::CameraDef loadLiveCamera() {
    const fs::path game = gameDir();
    auto cameras = os2::loadCameraConfig(game / kCameraConfig);
    const os2::CameraDef* live = os2::findCamera(cameras, "Live");
    if (!live)
        throw std::runtime_error("no \"Live\" camera in " + (game / kCameraConfig).string());
    return *live;
}

// The arguments, the executable's full path first (for a restart).
std::vector<std::string> processArguments() {
    std::vector<std::string> out{NSBundle.mainBundle.executablePath.UTF8String};
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i < args.count; ++i)
        out.emplace_back(args[i].UTF8String);
    return out;
}

// Value of `--name <value>`, if given.
std::optional<std::string> argument(NSString* name) {
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:name])
            return std::string(args[i + 1].UTF8String);
    return std::nullopt;
}

// Whether mirrors reflect: the Reflection graphics option (gSettings), or
// --reflections on|off for this run.
bool reflectionsOn() {
    static const std::optional<std::string> forced = argument(@"--reflections");
    if (forced)
        return *forced != "off" && *forced != "0";
    return gSettings.reflections;
}

// OS2_PERF=1: timings of the load phases and of the frames on stdout
// (docs/performance.md); nothing else changes.
bool perfOn() {
    static const bool on = std::getenv("OS2_PERF") != nullptr;
    return on;
}
// OS2_LIVE_SHOT=<file.png>: each SIGUSR1 saves the next frame the window
// draws (the scene and its UI, at the drawable's size) to that file, for
// live QA when a screen capture of the window is not possible.
volatile std::sig_atomic_t gLiveShotWanted = 0;
const char* liveShotFile() {
    static const char* file = [] {
        const char* f = std::getenv("OS2_LIVE_SHOT");
        if (f && *f)
            std::signal(SIGUSR1, [](int) { gLiveShotWanted = 1; });
        return f && *f ? f : nullptr;
    }();
    return file;
}
void perfLap(const char* what) {
    static CFTimeInterval last = CACurrentMediaTime();
    os2::loading::phase(what); // the loading screen's progress (app/scene_loader.hpp)
    if (!perfOn())
        return;
    const CFTimeInterval now = CACurrentMediaTime();
    std::printf("opensims2: perf: %-26s %8.1f ms\n", what, (now - last) * 1000);
    last = now;
}

// The lot to show: --lot, or the lot --hood-lot entered.
std::optional<std::string> lotQuery() {
    return gHoodLot ? gHoodLot : argument(@"--lot");
}

// Objects to show: every --object <CRES name | 0xGUID | OBJD name>, else the
// dining chair (unless a lot is shown).
std::vector<std::string> objectQueries() {
    std::vector<std::string> out;
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--object"])
            out.emplace_back(args[i + 1].UTF8String);
    if (out.empty() && !lotQuery() && !argument(@"--skinned") && !argument(@"--sim") && !argument(@"--character"))
        out.emplace_back("0xEC7618F6"); // Chair - Dining - Value1
    return out;
}

// A lot package given as a path, or by name ("N001_Lot82") under the game's neighborhoods.
fs::path lotPath(const std::string& query) {
    if (fs::exists(query))
        return query;
    const fs::path hoods = gameDir() / "TSData/Res/UserData/Neighborhoods";
    const std::string file = query.ends_with(".package") ? query : query + ".package";
    if (fs::exists(hoods))
        for (const auto& hood : fs::directory_iterator(hoods))
            if (fs::exists(hood.path() / "Lots" / file))
                return hood.path() / "Lots" / file;
    throw std::runtime_error("lot not found: " + query);
}

// Places the lot's objects that have a model and stand in the world. Models
// are uploaded once per CRES and design choice. Returns the instances placed.
size_t loadLotObjects(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                      const std::string& lotName) {
    std::map<std::string, int> models; // CRES + design -> model id, -1 if it failed
    std::map<std::string, std::pair<simd_float3, simd_float3>> bounds; // for picking
    std::map<std::string, std::shared_ptr<const os2::PickMesh>> meshes;
    std::map<std::string, os2::Skeleton> skeletons; // CRES + design -> the model's skeleton
    // Some models were renamed after the lots were saved (old modular stairs);
    // then the object's OBJD, found by file name, names the current one.
    std::map<std::string, os2::Tgi> definitions; // lowercase OBJD file name -> OBJD
    auto lower = [](std::string s) {
        for (auto& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto cresOf = [&](const os2::Lot::Object& o) {
        if (index.findByName(o.model + "_cres", kTypeCres))
            return o.model;
        if (definitions.empty())
            for (const auto& loc : index.ofType(kTypeObjd))
                definitions.emplace(lower(os2::ObjectDefinition::parse(index.read(loc)).fileName), loc.entry->tgi);
        std::string name = o.definition.ends_with("_objt") ? o.definition.substr(0, o.definition.size() - 5)
                                                           : o.definition;
        auto it = definitions.find(lower(name));
        std::string cres = it == definitions.end() ? std::string() : os2::objectCresName(index, it->second);
        return cres.empty() ? o.model : cres;
    };
    size_t placed = 0;
    for (const auto& o : lot.objects) {
        if (o.model.empty() || o.model == "nullModel" || o.model == "noModel" || o.model == "null" ||
            o.model == "placeholder" || o.person)
            continue; // ("placeholder": the controllers' marker box, app/vm_objects.cpp; "null": "Routing - Look At")
        // Objects in a slot are saved at (0, 0): drawn only where the VM's
        // containment puts them (app/vm_objects.hpp).
        const bool contained = o.position[0] == 0 && o.position[1] == 0;
        std::string key = o.model;
        for (const auto& [subset, material] : o.materials)
            key += "|" + subset + "=" + material;
        auto it = models.find(key);
        if (it == models.end()) {
            int id = -1;
            try {
                const auto model = os2::loadObjectModel(index, cresOf(o), o.materials);
                for (const auto& w : model.warnings)
                    std::fprintf(stderr, "opensims2: %s: %s: %s\n", lotName.c_str(), model.name.c_str(), w.c_str());
                id = renderer.addObject(model, index);
                bounds[key] = os2::modelBounds(model);
                meshes[key] = os2::makePickMesh(model, &index);
                skeletons[key] = model.skeleton;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: %s: %s (%s): %s\n", lotName.c_str(), o.model.c_str(),
                             o.definition.c_str(), e.what());
            }
            it = models.emplace(key, id).first;
        }
        if (it->second < 0)
            continue;
        // Drawn as the lot saved its bones (nTSSG::cObject's transform
        // nodes): a dining chair pushed in under its table (its push-in's
        // last frame) rather than its model's rest pose, pulled out.
        std::vector<os2::Xform> pose;
        if (!o.nodes.empty())
            if (auto p = os2::savedPose(skeletons[key], o.nodes)) {
                pose = std::move(*p);
                const std::string posedKey = key + "@" + os2::poseKey(pose);
                auto pit = models.find(posedKey);
                if (pit == models.end()) {
                    int id = -1;
                    try {
                        const auto model = os2::posedModel(os2::loadObjectModel(index, cresOf(o), o.materials), pose);
                        id = renderer.addObject(model, index);
                        bounds[posedKey] = os2::modelBounds(model);
                        meshes[posedKey] = os2::makePickMesh(model, &index);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "opensims2: %s: %s posed: %s\n", lotName.c_str(), o.model.c_str(), e.what());
                    }
                    pit = models.emplace(posedKey, id).first;
                }
                if (pit->second >= 0) {
                    it = pit;
                    key = posedKey;
                } else {
                    pose.clear();
                }
            }
        const auto& q = o.rotation;
        simd_float4x4 at = simd_matrix4x4(simd_quaternion(q[0], q[1], q[2], q[3]));
        at.columns[3] = simd_make_float4(o.position[0], o.position[1], o.position[2], 1);
        if (contained)
            at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
        const int instance = renderer.addInstance(it->second, at);
        if (const char* f = std::getenv("OS2_DEBUG_OBJECTS"); f && o.definition.find(f) != std::string::npos)
            std::fprintf(stderr, "object %u (%s) at %.2f %.2f %.2f%s\n", o.instance & 0xFFFF, o.definition.c_str(),
                         o.position[0], o.position[1], o.position[2], contained ? " contained" : "");
        // Stale cObjects share IDs with live ones: the first (or one in the
        // world) keeps the ID; app/vm_objects.cpp checks it against the VM.
        const int16_t id = int16_t(o.instance & 0xFFFF);
        if (!gPlacedObjects.count(id) || (gPlacedObjects[id].contained && !contained))
            gPlacedObjects[id] = {cresOf(o), o.materials, o.position, o.rotation, o.definition, instance, contained, pose};
        if (contained) {
            gPickBoxes.push_back({true, o.instance, bounds[key].first, bounds[key].second, at, meshes[key]});
            continue;
        }
        gPickBoxes.push_back({false, o.instance, bounds[key].first, bounds[key].second, at, meshes[key]});
        ++placed;
    }
    return placed;
}

// A character file given as a path, or by name ("N001_User00011") under the
// game's neighborhoods.
fs::path characterPath(const std::string& query) {
    if (fs::exists(query))
        return query;
    const fs::path hoods = gameDir() / "TSData/Res/UserData/Neighborhoods";
    const std::string file = query.ends_with(".package") ? query : query + ".package";
    if (fs::exists(hoods))
        for (const auto& hood : fs::directory_iterator(hoods))
            if (fs::exists(hood.path() / "Characters" / file))
                return hood.path() / "Characters" / file;
    throw std::runtime_error("character not found: " + query);
}

// The Sims on a lot: each cPerson with a saved state in the world, built from
// its character file (N001/Characters/<N001_User#####>.package, named by the
// cObject's "N001_User00011 - Don_objt"), standing where it was saved and
// playing an idle animation (--sim-anim <name> picks another).

// Page Up / Page Down and the puck's floor buttons ("Subir um Andar (Page
// Up)", "Descer um Andar (Page Down)"): in Buy mode the floor objects go on
// (the camera follows it), else the floor the camera shows.
void changeFloor(int delta) {
    if (gBuy && gBuy->active()) {
        if (gBuy->changeLevel(delta) && gCameraFollow)
            gCameraFollow->setLevel(gBuy->level() - 1);
    } else if (gBuild && gBuild->active() && gCameraFollow) {
        if (gBuild->level() - 1 + delta >= std::min(0, gBuild->level() - 1) && gCameraFollow->setLevel(gBuild->level() - 1 + delta))
            gBuild->setLevel(gCameraFollow->level() + 1);
    } else if (gCameraFollow) {
        // A level shown together with the one below (a foundation's top,
        // WallView::shownLevel) is stepped over.
        // The pool's level (-1) is no floor of the lot: the buttons stop at
        // the ground (base game: no basements; LotCameraFollow::follow keeps
        // a swimming Sim's view there too). Inferred, live QA round 20.
        const int now = gCameraFollow->level(), shown = gWallView.shownLevel(now);
        const int lowest = std::max(gCameraFollow->lowestLevel(), std::min(0, now));
        int to = delta > 0 ? std::min(std::max(now + delta, shown + 1), std::max(gWallView.topLevel(), now)) : now + delta;
        while (delta < 0 && to > lowest && gWallView.shownLevel(to) == shown)
            --to;
        gCameraFollow->setLevel(std::max(to, std::min(lowest, now)));
    }
}

// Esc on the lot with the options panel (or one of its windows: Graphics,
// Audio...) open closes it, window and panel together, before Esc can leave
// the lot. True if it closed something.
bool escapeOptions() {
    const bool panel = gLive && gLive->optionsOpen(), window = gOptions && gOptions->active();
    if (window)
        gOptions->closeWindow();
    if (panel)
        gLive->closeOptions();
    if (panel || window)
        std::printf("opensims2: escape: options %s%s closed\n", panel ? "panel" : "", window ? (panel ? " and window" : "window") : "");
    return panel || window;
}

// The camera shows the selected Sim's level (cCameraLive::Update).
void followSelectedSim() {
    if (gCameraFollow && gSimulation && gPie)
        gCameraFollow->follow(gSimulation->machine(), gPie->sim());
}

void startAudio(os2::ResourceIndex& index, const os2::Lot& lot) {
    if (!gGameAudio)
        return;
    os2::LotAudio::Options o;
    o.music = argument(@"--music").value_or("");
    gAudio = std::make_unique<os2::LotAudio>(*gGameAudio, index, lot, o);
    for (const auto& s : gLotSims) {
        os2::DbpfPackage character(s.character);
        const std::string shoe = os2::characterShoe(index, os2::readCharacter(character, index), gAudio->sound());
        gAudio->attachSim(*s.actor, s.id, shoe.empty() ? "rub" : shoe);
    }
}

void updateAudio(double seconds) {
    if (!gAudio)
        return;
    os2::LotAudio::Listener l;
    if (gCamera) {
        const simd_float3 p = gCamera->pivot(), look = p - gCamera->eye();
        l.x = p.x, l.y = p.y, l.z = std::max(0.0f, p.z - 1);
        l.rightX = look.y, l.rightY = -look.x; // forward x up
        l.zoom = std::clamp(gCamera->zoom() / 5.0f, 0.0f, 1.0f);
    }
    // The mode's music state: Build, Buy, or (live mode) the lot's: only the
    // stereos, as the game's music manager switches it.
    gGameAudio->setMusic(gBuild && gBuild->active() ? "Build"
                         : gBuy && gBuy->active()   ? "Buy"
                                                    : gAudio->liveMusic());
    gAudio->update(gSimulation ? &gSimulation->machine() : nullptr, seconds, l);
    if (gTv && gSimulation) {
        // A channel's sound comes out of its first receiver (the lowest
        // object ID showing it), as the binary's sound emitter proxy does
        // (app/tv_broadcast.hpp); the other TVs on the channel are silent.
        std::vector<os2::LotAudio::TvSource> on;
        std::set<std::string> heard;
        for (const auto& [id, o] : gSimulation->machine().objects()) // (by ID)
            for (const auto& [group, material] : o.materials)
                if (const auto* s = gTv->isChannel(material) ? gTv->sound(material) : nullptr) {
                    if (heard.insert(material).second)
                        on.push_back({id, s->serial, s->pcm, std::max(0.0, gTv->now() - s->started)});
                    break;
                }
        gAudio->tvSounds(gSimulation->machine(), on);
    }
}

// --vm / --run-vm: the lot's objects in the VM, with its Sims attached to
// their actors; --vm-walk <sim>,<object>[,<anim>] starts a scripted walk.
void startSimulation(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                     const fs::path& lotFile) {
    CFTimeInterval start = CACurrentMediaTime();
    gSimulation = std::make_unique<os2::LotSimulation>(index, lot, pkg, lotFile,
                                                      [sceneArgs() containsObject:@"--init"]);
    // The Sims the first day's "reset lot" made (a family moving in).
    for (const auto& [id, file] : gSimulation->arrivedSims())
        if (const os2::vm::Object* o = gSimulation->machine().object(id))
            spawnSimActor(renderer, index, file, id,
                          simd_make_float3(o->position.x, o->position.y,
                                           gSimulation->floorHeight(o->position.level, o->position.x, o->position.y)),
                          float(o->data[os2::vm::objdata::Direction]) * float(M_PI) / 4, 0.37 * double(gLotSims.size()),
                          "arrived");
    for (const auto& s : gLotSims) {
        os2::DbpfPackage character(s.character);
        gSimulation->attach(*s.actor, s.id, os2::readCharacter(character, index), s.idle);
    }
    perfLap("  LotSimulation");
    gObjectView = std::make_unique<os2::VmObjectView>(renderer, index, *gSimulation);
    // TV channels and arcade screens (app/tv_broadcast.hpp): a broadcast's material shows its movies.
    try {
        gTv = std::make_unique<os2::TvBroadcast>(renderer, gameDir() / "TSData/Res");
        gObjectView->adjustModel = [](os2::ObjectModel& m, const std::map<std::string, std::string>& materials) {
            if (gTv)
                gTv->adjust(m, materials);
        };
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: tv: %s\n", e.what());
    }
    gSnaps = std::make_unique<os2::AnimSnapView>(renderer, index, *gObjectView, gSimulation->machine());
    gSimulation->seats.slot = [](int16_t id, double now) { return gObjectView ? gObjectView->slotPlacement(id, now) : std::nullopt; };
    gSimulation->objectBone = [](int16_t id, uint32_t crc) -> std::optional<os2::Xform> {
        return gObjectView ? gObjectView->boneByHash(id, crc) : std::nullopt;
    };
    gSimulation->actorOf = [](int16_t id) -> const os2::SimActor* {
        const auto it = gObjectActors.find(id);
        return it != gObjectActors.end() ? it->second : nullptr;
    };
    gSimulation->onSimAnimation = [](int16_t id, os2::SimActor& actor, const os2::Animation* anim, double at) {
        gSnaps->simAnimation(id, actor, anim, at);
        if (anim)
            cinematicActorAnimates(id, at);
    };
    for (const auto& [id, p] : gPlacedObjects) {
        std::optional<os2::Xform> saved;
        if (!p.contained)
            saved = os2::Xform{os2::Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]},
                               {p.position[0], p.position[1], p.position[2]}};
        gObjectView->addPlaced(id, p.definition, p.cres, p.instance, saved, p.materials, p.pose);
    }
    for (const auto& s : gLotSims)
        gObjectView->addSim(s.id, s.actor);
    perfLap("  object view");
    try {
        const fs::path lights = gameDir() / "TSData/Res/Lights";
        gLighting = std::make_unique<os2::LotLighting>(renderer, index, *gSimulation, *gObjectView,
                                                       os2::loadLightingConfig(lights / "Lighting.txt"),
                                                       os2::loadLightOverrides(lights / "LotLights.txt"));
        for (const auto& s : gLotSims)
            gLighting->addSim(s.id, s.actor);
        if (gLightMapLayout)
            gLighting->setLightMapLayout(*gLightMapLayout);
        // --lighting <state> keeps that state instead of the clock's.
        if (auto state = argument(@"--lighting"))
            gLighting->forceState(*state);
        // --lamps <0-100>: test aid, lamps Change Light has not touched shine at that level.
        if (auto lamps = argument(@"--lamps"))
            gLighting->setDefaultLampLevel(std::stoi(*lamps));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: lighting: %s\n", e.what());
    }
    perfLap("  LotLighting");
    // A Sim that changes clothes gets the outfit of its character file.
    gSimulation->onOutfitChange = [&renderer, &index](int16_t id, os2::SimActor& actor, uint32_t category) {
        const auto it = std::find_if(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; });
        if (it == gLotSims.end())
            return;
        try {
            actor.skin = os2::loadCharacterSim(index, simParts(index), it->character, category);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int model = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, model);
            actor.model = model;
            std::printf("opensims2: Sim %d now wears outfit category 0x%03X\n", id, category);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: outfit 0x%03X: %s\n", id, category, e.what());
        }
    };
    // The dialogs' $AspirationString and the newborns' names read Text/*.package (vm/dialog.hpp).
    os2::vm::textTables() = [](uint32_t group, uint32_t instance) -> const os2::StringTable* {
        try {
            if (!gTexts && gIndex)
                gTexts = std::make_unique<os2::UiResources>(*gIndex, gameDir() / "TSData/Res");
            return gTexts ? gTexts->table(group, instance) : nullptr;
        } catch (const std::exception& err) {
            os2::logOnce("text tables", std::to_string(group) + ":" + std::to_string(instance), err);
            return nullptr;
        }
    };
    // Babies born in play get their character files (vm/baby_character.hpp),
    // kept with the session's until the game is saved.
    if (const auto q = lotQuery())
        try {
            gLotCharactersDir = os2::vm::charactersDirOf(lotPath(*q));
            os2::vm::installBabyCharacterWriter(index, gLotCharactersDir, os2::sessionCharactersDir(), &gBornCharacters);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: babies' characters: %s\n", e.what());
        }
    // A Sim that grows up (the Age primitive) gets its character file grown
    // into the new age (cas::writeAgedCharacter: MakeNewAge's hair and
    // clothes, the face of the new age from its LxNR), written beside the
    // session's other files, and is rebuilt from it.
    gSimulation->onAgeChange = [&renderer, &index](int16_t id, os2::SimActor& actor, uint32_t ageBit) {
        const auto it = std::find_if(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; });
        if (it == gLotSims.end())
            return;
        try {
            auto& cas = gGameData.casCatalog(index); // (the game's data: for the app's life)
            const os2::cas::CasCatalog& catalog = *cas.catalog;
            os2::cas::FaceBaker& baker = *cas.baker;
            // The save copies it to the neighborhood; an NPC made in play
            // stays in the session's NPCs folder (vm/baby_character.hpp).
            const fs::path session = os2::sessionCharactersDir();
            const fs::path dir = it->character.parent_path() == session / "NPCs" ? session / "NPCs" : session;
            const fs::path out = dir / (it->character.stem().string() + ".package");
            const fs::path tmp = dir / (it->character.stem().string() + ".new.package");
            os2::cas::writeAgedCharacter(index, catalog, baker, it->character, ageBit, tmp, uint32_t(id) * 7919u + ageBit);
            fs::rename(tmp, out);
            it->character = out;
            actor.skin = os2::loadCharacterSim(index, simParts(index), it->character, 1);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int model = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, model);
            actor.model = model;
            std::printf("opensims2: Sim %d grew into age 0x%02X (%s)\n", id, ageBit, out.string().c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %d: age 0x%02X: %s\n", id, ageBit, e.what());
        }
    };
    // Objects the VM animates (doors, fridges, phones, ...) swap their static
    // model for a skinned one of the same CRES, posed by the animation.
    auto& objectActor = gScene.objectActor; // (the scene's renderer: per scene)
    objectActor = [&renderer, &index](int16_t id) -> os2::SimActor* {
        auto it = gObjectActors.find(id);
        if (it == gObjectActors.end()) {
            const auto placed = gPlacedObjects.find(id);
            if (placed == gPlacedObjects.end())
                return nullptr; // no model in the world
            // Not drawn by the object view (out of the world: saved at
            // (0, 0) in no slot, as the PlumbBobs of an empty lot): an actor
            // would be drawn at the lot's corner, where nobody hides it
            // (live QA, N002_Lot23: a plumbob floating past the lot's edge).
            if (!gObjectView || !gObjectView->hasModel(id)) {
                if (std::getenv("OS2_DEBUG_VM"))
                    std::fprintf(stderr, "opensims2: object %d (%s) out of the world: not animated\n", id,
                                 placed->second.definition.c_str());
                return nullptr;
            }
            const PlacedObject& p = placed->second;
            try {
                const os2::ObjectModel model = os2::loadObjectModel(index, p.cres, gObjectView->materialsOf(id));
                os2::SkinnedModel skin(model.skeleton);
                skin.add(model);
                auto actor = std::make_unique<os2::SimActor>(std::move(skin));
                // From the pose the lot saved (a chair pushed in), and kept
                // where its animations leave it.
                if (p.pose.size() == actor->skin.skeleton.bones.size())
                    actor->player.setBasePose(p.pose);
                actor->player.setHoldPose(true);
                actor->position = {p.position[0], p.position[1], p.position[2]};
                actor->heading = 2 * std::atan2(p.rotation[2], p.rotation[3]);
                actor->model = renderer.addObject(actor->skin.model, index, true);
                renderer.addInstance(actor->model, matrix_identity_float4x4);
                gObjectView->setActor(id, actor.get()); // hides the static instance
                it = gObjectActors.emplace(id, actor.get()).first;
                gActors.push_back(std::move(actor));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: object %d: %s\n", id, e.what());
                gObjectActors.emplace(id, nullptr);
                return nullptr;
            }
        }
        return it->second;
    };
    if (const char* f = std::getenv("OS2_FORCE_OBJECT_ACTOR")) // test aid: this object animated from the start
        gScene.objectActor(int16_t(std::atoi(f)));
    // Once per name: the base game's trees ask for some the data lacks (the
    // double bed's "-", o2a-bookcase-get-middle: only the child's o2c-
    // exists) and the game shows nothing for them.
    static auto reportMissing = [](int16_t id, const std::string& name, const std::exception& e) {
        static std::set<std::string> reported; // (animation names: for the app's life)
        if (reported.insert(name).second)
            std::fprintf(stderr, "opensims2: object %d: %s: %s\n", id, name.c_str(), e.what());
    };
    gSimulation->onObjectAnimation = [&index](int16_t id, const std::string& name, double at) {
        if (gSnaps)
            gSnaps->objectAnimation(id, name, at);
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d starts %s (%s)\n", id, name.c_str(),
                         gPlacedObjects.count(id) ? "placed" : "not placed");
        os2::SimActor* actor = gScene.objectActor(id);
        if (!actor)
            return;
        try {
            // Mirrored as the VM says (a chair pulled out to the side the Sim gets in from).
            const os2::vm::Object* vo = gSimulation ? gSimulation->machine().object(id) : nullptr;
            const bool mirror = vo && !vo->animation.overlays.empty() && vo->animation.overlays.back().name == name &&
                                vo->animation.overlays.back().flipped;
            actor->cue(at, actor->animation(index, name), {.fade = 0.1f, .loop = false, .mirror = mirror});
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "opensims2: object %d plays %s\n", id, name.c_str());
        } catch (const std::exception& e) {
            reportMissing(id, name, e);
        }
    };
    // What the VM plays on an object, overlay by overlay, as VM::objectPose
    // poses it: each overlay its own layer (priority and level 1 + its id:
    // a later one takes the bones it keys whole), mirrored as the VM plays
    // it, and taken off when the VM takes it off, the object keeping the
    // pose of that moment (AnimPlayer::setHoldPose). A two-tile door's
    // leaves are one animation played twice, the second flipped: as one
    // layer replacing the other, only one leaf opened.
    gSimulation->onObjectOverlay = [&index](int16_t id, const std::string& name, bool flipped, uint32_t overlay,
                                            double at) {
        if (gSnaps)
            gSnaps->objectAnimation(id, name, at);
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d starts %s%s (overlay %u, %s) at %.2f\n", id, name.c_str(),
                         flipped ? " flipped" : "", overlay, gPlacedObjects.count(id) ? "placed" : "not placed", at);
        os2::SimActor* actor = gScene.objectActor(id);
        if (!actor)
            return;
        try {
            const int layer = 1 + int(overlay & 0x3FFFFFFF);
            actor->cue(at, actor->animation(index, name),
                       {.fade = 0.1f, .loop = false, .priority = layer, .mirror = flipped, .level = layer});
        } catch (const std::exception& e) {
            reportMissing(id, name, e);
        }
    };
    gSimulation->onObjectOverlayEnd = [](int16_t id, uint32_t overlay, double at) {
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d ends overlay %u at %.2f\n", id, overlay, at);
        const auto it = gObjectActors.find(id);
        if (it != gObjectActors.end() && it->second)
            it->second->cueStop(at, 1 + int(overlay & 0x3FFFFFFF), 0);
    };
    // An animated object whose material the VM changes (the plumbob's
    // colour) gets its skinned model again with the new materials.
    gObjectView->onActorMaterials = [&renderer, &index](int16_t id) {
        const auto it = gObjectActors.find(id);
        if (it == gObjectActors.end() || !it->second)
            return;
        os2::SimActor& actor = *it->second;
        try {
            const os2::ObjectModel model = os2::loadObjectModel(index, gObjectView->cresOf(id), gObjectView->materialsOf(id));
            os2::SkinnedModel skin(model.skeleton);
            skin.add(model);
            actor.skin = std::move(skin);
            actor.player.setSkeleton(&actor.skin.skeleton);
            const int m = renderer.addObject(actor.skin.model, index, true);
            renderer.retargetInstances(actor.model, m);
            actor.model = m;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: object %d: %s\n", id, e.what());
        }
    };
    gDoors = std::make_unique<os2::DoorPortals>(
        [](int16_t id, const std::string& name, double at) {
            if (gSimulation && gSimulation->onObjectAnimation)
                gSimulation->onObjectAnimation(id, name, at);
        },
        [](int16_t id) -> std::optional<os2::Xform> {
            if (gObjectView && gObjectView->hasModel(id))
                return gObjectView->placement(id);
            const auto p = gPlacedObjects.find(id); // a door the VM does not have: where it was saved
            if (p == gPlacedObjects.end() || p->second.contained)
                return std::nullopt;
            const auto& q = p->second.rotation;
            return os2::Xform{os2::Quat{q[0], q[1], q[2], q[3]}, {p->second.position[0], p->second.position[1], p->second.position[2]}};
        });
    for (const auto& [id, p] : gPlacedObjects)
        if (p.definition.rfind("Door", 0) == 0 && !p.contained)
            gDoors->addDoor(id);
    // --object-anim <id>,<animation>: test aid, an object plays an animation
    // as if the VM's Animate Object had started it.
    if (auto anim = argument(@"--object-anim")) {
        const auto comma = anim->find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("--object-anim: expected <object-id>,<animation>");
        gSimulation->onObjectAnimation(int16_t(std::stoi(anim->substr(0, comma))), anim->substr(comma + 1), 0);
    }
    // --look-at <sim>,<object-id>: test aid, the Sim (ID or name) looks at
    // the object as if the VM's Look At had pointed it there, until changed.
    if (auto look = argument(@"--look-at")) {
        const auto comma = look->find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("--look-at: expected <sim>,<object-id>");
        const os2::vm::ObjectId sim = os2::vm::findSim(gSimulation->machine(), look->substr(0, comma));
        if (auto* o = gSimulation->machine().object(sim))
            o->lookAt = os2::vm::Object::LookAt{int16_t(std::stoi(look->substr(comma + 1))), 0, 0};
        else
            throw std::runtime_error("--look-at: no Sim " + look->substr(0, comma));
    }
    // --outfit <type>: test aid, every Sim changes into outfit type <type>
    // (person data 8, STR# 0xFA: 0 everyday, 2 swimwear, 3 athletic, 4
    // formal, 5 pyjamas, 11 underwear, ...).
    if (auto outfit = argument(@"--outfit"))
        for (const auto& s : gLotSims)
            if (auto* o = gSimulation->machine().object(s.id); o && o->person)
                o->person->data[8] = int16_t(std::stoi(*outfit));
    if (auto walk = argument(@"--vm-walk")) {
        int sim = 0, target = 0, anim = -1;
        if (std::sscanf(walk->c_str(), "%d,%d,%d", &sim, &target, &anim) < 2)
            throw std::runtime_error("--vm-walk: expected <sim-id>,<object-id>[,<anim>]");
        gSimulation->walk(int16_t(sim), int16_t(target),
                          anim >= 0 ? std::optional<uint16_t>(uint16_t(anim)) : std::nullopt);
    }
    std::printf("opensims2: VM: %s, %zu Sims attached (%.2f s)\n", gSimulation->summary().c_str(), gLotSims.size(),
                CACurrentMediaTime() - start);
    // --vm-create GUID[:attr=value,...]: test aid, an object made out of the
    // world (vm::createTestObject; a party controller 0x2C91C16A:2=1 makes
    // the birthday play its cinematic).
    // "...@x,y[,level]" puts it on that tile (the debug tombstone's "New Baby" places the baby beside it).
    // Repeatable (a baby's tombstone and a crib). A final "!" starts its
    // Main (a Fire 0x24C95F99@x,y,level! burns, wakes the room's smoke alarm).
    std::vector<std::string> creates;
    {
        NSArray<NSString*>* a = sceneArgs();
        for (NSUInteger i = 1; i + 1 < a.count; ++i)
            if ([a[i] isEqualToString:@"--vm-create"])
                creates.push_back(a[i + 1].UTF8String);
    }
    for (const std::string& specText : creates) {
        const std::string* spec = &specText;
        const auto at = spec->find('@');
        const os2::vm::ObjectId id = os2::vm::createTestObject(gSimulation->machine(), spec->substr(0, at));
        int x = 0, y = 0, level = 1;
        std::string where = "out of the world";
        if (at != std::string::npos && std::sscanf(spec->c_str() + at + 1, "%d,%d,%d", &x, &y, &level) >= 2) {
            const os2::vm::Placement p = os2::vm::checkPlacement(gSimulation->machine(), id, {x, y, level}, 0);
            if (p.ok)
                os2::vm::applyPlacement(gSimulation->machine(), id, p), where = "placed";
            else
                where = "not placed: " + p.reason;
        }
        // "...!" at the end: its Main starts once placed (a Fire that burns).
        if (!spec->empty() && spec->back() == '!' && gSimulation->machine().startMain(id))
            where += ", Main started";
        std::printf("opensims2: created object %d (%s)\n", id, where.c_str());
    }
    // --vm-queue <object>:<entry>: test aid, the selected Sim (--sim-id) queues
    // the pie entry (English name, debug and cheat entries too) of an object
    // found by ID or name, also one out of the world (the debug tombstone
    // --vm-create 0x0CB53A3A: "New Baby", "CINECAM BABY", "Age Transition Cinematic").
    if (auto spec = argument(@"--vm-queue")) {
        os2::vm::VM& m = gSimulation->machine();
        const auto colon = spec->find(':');
        const std::string what = spec->substr(0, colon), entry = colon == std::string::npos ? "" : spec->substr(colon + 1);
        os2::vm::ObjectId target = 0;
        for (const auto& [id, o] : m.objects())
            if (!o.person && o.definition &&
                (what == std::to_string(id) || strcasestr(o.definition->fileName.c_str(), what.c_str())))
                target = target ? target : id;
        const os2::vm::ObjectId sim = os2::vm::findSim(m, argument(@"--sim-id").value_or(""));
        os2::vm::PieOptions opt;
        opt.language = os2::lang::EnglishUS;
        bool queued = false;
        if (target && sim)
            for (const auto& e : os2::vm::pieMenu(m, sim, target, opt))
                if (!queued && strcasecmp(e.name.c_str(), entry.c_str()) == 0)
                    if (auto q = os2::vm::interactionFromPie(m, e)) {
                        os2::vm::addAction(m, *m.object(sim), *q);
                        queued = true;
                    }
        std::printf("opensims2: --vm-queue %s: %s\n", spec->c_str(), queued ? "queued" : "no such entry");
    }
    // --vm-go-here x,y[,level]: test aid, a click on the ground there and the
    // selected Sim's first Go Here entry ("Vir Aqui") queued (vm/go_here.hpp).
    if (auto spec = argument(@"--vm-go-here")) {
        os2::vm::VM& m = gSimulation->machine();
        float x = 0, y = 0;
        int level = 1;
        std::sscanf(spec->c_str(), "%f,%f,%d", &x, &y, &level);
        const os2::vm::ObjectId sim = os2::vm::findSim(m, argument(@"--sim-id").value_or(""));
        const os2::vm::ObjectId dest = sim ? os2::vm::createGoHereDestination(m, sim, x, y, level) : 0;
        bool queued = false;
        if (dest)
            for (const auto& e : os2::vm::pieMenu(m, sim, dest))
                if (!queued && e.available() && !e.debug)
                    if (auto q = os2::vm::interactionFromPie(m, e)) {
                        q->pushedBy = sim;
                        os2::vm::enqueue(m, *m.object(sim), *q);
                        queued = true;
                    }
        if (dest)
            os2::vm::releaseDestination(m, dest);
        std::printf("opensims2: --vm-go-here %s: %s\n", spec->c_str(), queued ? "queued" : "nothing");
    }
    // --motives <n>: test aid, every Sim's motives at n (instead of the saved ones).
    if (auto m = argument(@"--motives"))
        os2::vm::setAllMotives(gSimulation->machine(), int16_t(std::stoi(*m)));
    if (std::getenv("OS2_TRACE_VM")) // test aid: every instruction run (group:tree#pc opcode, stack object)
        gSimulation->machine().trace = [](const os2::vm::Thread&, const os2::vm::Frame& f, const os2::Behavior::Instruction& in) {
            std::fprintf(stderr, "trace: %08X:%04X #%u op 0x%X (stack object %d)\n", f.group, f.instance, f.pc, in.opcode, f.stackObject);
        };
    // Pie menus over the same VM.
    gPie = std::make_unique<os2::PieMenuHost>(gSimulation->machine(), gameDir() / "TSData/Res/UI/Fonts",
                                              argument(@"--sim-id").value_or(""));
    // A go-here destination goes when its menu closes, unless an interaction on it was queued.
    gPie->onClose = [](os2::vm::ObjectId id) { os2::vm::releaseDestination(gPie->vm(), id); };
    gPie->simAway = [](os2::vm::ObjectId sim) { return !simInTheWorld(int16_t(sim)); };
    for (const auto& b : gPickBoxes)
        if (b.contained)
            gPie->addFollowingPickBox(b.instance, b.lo, b.hi, b.mesh);
        else
            gPie->addPickBox(b.instance, b.lo, b.hi, b.at, b.mesh);
    // Objects in slots are picked where the VM view draws them.
    gPie->setPlacementSource([](os2::vm::ObjectId id) -> std::optional<simd_float4x4> {
        if (!gObjectView || !gObjectView->hasModel(id))
            return std::nullopt;
        const auto x = gObjectView->placement(id);
        if (!x)
            return std::nullopt;
        simd_float4x4 m = simd_matrix4x4(simd_quaternion(x->q.x, x->q.y, x->q.z, x->q.w));
        m.columns[3] = simd_make_float4(x->t[0], x->t[1], x->t[2], 1);
        return m;
    });
    // Objects the view hides (windows and doors of walls cut down) are not picked.
    gPie->drawn = [](os2::vm::ObjectId id) { return !gObjectView || gObjectView->shown(int16_t(id)); };
    // A floor shown stops the ray (a Sim under the floor shown is not picked).
    gPie->floorHit = [](simd_float3 eye, simd_float3 dir) { return floorUnderRay(eye, dir); };
    // The levels above the camera's are not drawn: their objects are not picked.
    gPie->pickable = [](simd_float3 lo, simd_float3) {
        return !gLotShape || os2::WallView::visible(*gLotShape, viewLevel(), lo.x, lo.y, lo.z, gWallView.lowestCut(viewLevel()));
    };
    // Windows, doors, curtains and what hangs on walls (the model's bottom
    // over the floor): hidden with the wall they are on when it is cut down.
    // Decided once per object (bought ones by their OBJD name only).
    gWallHung.clear();
    for (const auto& b : gPickBoxes)
        if (b.lo.z > 0.5f)
            gWallHung[int16_t(b.instance & 0xFFFF)] = true;
    gObjectView->visible = [](int16_t id, const os2::Xform& at) {
        if (!gLotShape)
            return true;
        if (!os2::WallView::visible(*gLotShape, viewLevel(), at.t[0], at.t[1], at.t[2], gWallView.lowestCut(viewLevel())))
            return false;
        auto it = gWallHung.find(id);
        if (it == gWallHung.end()) {
            const os2::vm::Object* o = gSimulation ? gSimulation->machine().object(id) : nullptr;
            std::string def = o && o->definition ? o->definition->fileName : "";
            std::transform(def.begin(), def.end(), def.begin(), [](unsigned char c) { return std::tolower(c); });
            const bool hung = def.starts_with("window") || def.starts_with("door") || def.starts_with("curtain") ||
                              def.starts_with("blinds") || def.starts_with("drapes");
            it = gWallHung.emplace(id, hung).first;
        }
        return !it->second || !onCutWall(at);
    };
    perfLap("  VM hooks");
    setupPieMenu(index);
    perfLap("  pie menu");
    if (liveUiRequested())
        setupLiveUi(index);
    perfLap("  live UI");
    if (liveUiRequested() || startMode()) {
        setupBuyMode(renderer, index);
        perfLap("  Buy mode");
        setupBuildMode(renderer, index);
        perfLap("  Build mode");
    }
    if (liveUiRequested())
        setupOptions(index); // below: the puck's Options mode
    if (liveUiRequested())
        setupTutorial(index); // below: a lot of the Tutorial neighborhood
    // --game-speed <2|3|4>: the game speed global (2 normal, 3 fast, 4 fastest), as the live UI's speed buttons.
    if (auto sp = argument(@"--game-speed"))
        gSimulation->machine().globals[os2::vm::global::Speed] = int16_t(std::clamp(std::stoi(*sp), 2, 4));
    // --dialog-answer <n>: every dialog the trees put up gets button n at once.
    if (auto a = argument(@"--dialog-answer"))
        gSimulation->machine().autoDialogAnswer = std::stoi(*a);
    // --dialog-demo <type>[:<title>[:<message>]]: test aid, puts a dialog of that Dialog type up for the
    // selected Sim, to see the window (answer it with --dialog-click / --dialog-type, or keys): 0 message,
    // 1 yes/no, 2 yes/no/cancel, 3 text entry ("0i": a message with the Sim's icon, "0c": with a close box),
    // 23 baby name ("23g" a girl, "23t" twins, "23r" a rename), 5 services / 6 invite / 7 party phone books
    // over the lot's objects and neighbors, 17 the wardrobe ("17f" formal wear), 16 the clothing rack
    // ("16f" formal), 20 the try-on, 21 the vanity.
    if (auto spec = argument(@"--dialog-demo"); spec && gPie) {
        os2::vm::VM& m = gSimulation->machine();
        os2::vm::Dialog d;
        d.id = 0x7000;
        d.me = gPie->sim();
        d.type = std::atoi(spec->c_str());
        d.cls = os2::vm::dialogClass(d.type);
        std::vector<std::string> parts{""};
        for (char ch : *spec)
            if (ch == ':')
                parts.emplace_back();
            else
                parts.back().push_back(ch);
        const std::string kind = parts[0];
        d.title = parts.size() > 1 ? parts[1] : "";
        d.message = parts.size() > 2 ? parts[2] : "Isto é uma mensagem de teste do diálogo, longa o bastante para quebrar em mais de uma linha na janela.";
        using namespace os2::vm::dialogtype;
        if (d.type == YesNo)
            d.buttons = {"Sim", "Não"};
        else if (d.type == YesNoCancel)
            d.buttons = {"Sim", "Não", "Cancelar"};
        else if (d.cls.cls == os2::vm::DialogClass::Object)
            d.buttons = {"OK"};
        else
            d.buttons = {"OK", "Cancelar"};
        if (kind == "0i")
            d.iconObject = gPie->sim();
        if (kind == "0c")
            d.continueOthers = true;
        if (d.cls.cls == os2::vm::DialogClass::SelectItems) { // 11/13: a rack of the lot's first objects, several picks
            d.multiSelect = d.obeyPrice = true;
            d.fillFunds = 300;
            for (const auto& [oid, o] : m.objects())
                if (!o.person && o.definition && o.definition->price() > 0 && d.entries.size() < 7) {
                    os2::vm::DialogEntry e;
                    e.id = oid, e.guid = o.guid(), e.name = os2::vm::displayName(m, o), e.price = o.definition->price();
                    d.entries.push_back(std::move(e));
                }
        }
        if (d.cls.cls == os2::vm::DialogClass::Fill) { // 14: buying food (40 of 200 units, §20 per 50, §50 delivery)
            d.fillUnits = d.fillValue = 40, d.fillCost = 20, d.fillDelivery = 50, d.fillMax = 200;
            d.fillFunds = 1000;
        }
        if (d.cls.cls == os2::vm::DialogClass::SetAspiration) { // 24: the selected Sim's aspiration
            if (const os2::vm::Object* o = m.object(gPie->sim()); o && o->person) {
                d.aspirationNeighbor = o->person->data[os2::vm::persondata::NeighborId];
                d.female = o->person->data[65] == 1;
                d.text = os2::vm::displayName(m, *o);
            }
        }
        if (d.cls.cls == os2::vm::DialogClass::Clothing) { // 17: the wardrobe, everyday clothes ("17f": formal)
            d.outfitSim = gPie->sim();
            d.outfitCategory = kind == "17f" ? 0x020 : 0x001;
        }
        if (d.cls.cls == os2::vm::DialogClass::ClothingPurchase || d.cls.cls == os2::vm::DialogClass::ClothingTryOn) {
            // 16: the clothing rack (everyday, §150 an item; "16f": formal), 20: the try-on.
            d.outfitSim = gPie->sim();
            d.outfitCategory = kind == "16f" || kind == "20f" ? 0x020 : 0x001;
            d.outfitSeparates = d.outfitCategory == 0x001;
            d.pricePerItem = 150;
            d.fillFunds = 2000;
            if (const os2::vm::Object* o = m.object(gPie->sim()); o && o->person)
                if (auto f = m.families.find(o->person->data[os2::vm::persondata::FamilyNumber]); f != m.families.end())
                    d.fillFunds = f->second.funds;
        }
        if (d.cls.cls == os2::vm::DialogClass::Vanity) // 21: the mirror's "Change Appearance"
            d.outfitSim = gPie->sim();
        if (d.type == BabyName) {
            const bool girl = kind == "23g";
            d.names.push_back({0, girl ? "Bebezinha" : "Bebezinho", girl});
            if (kind == "23t")
                d.names.push_back({0, "Bebezinha", true});
            d.namePanel = kind == "23r" ? 0xCDC2CAA4 : d.names.size() > 1 ? 0xCDC2CAA6 : 0xCDC2CAA5;
            d.message.clear();
        }
        if (d.cls.cls == os2::vm::DialogClass::PhoneServices) {
            d.message.clear();
            for (const auto& [oid, o] : m.objects())
                if (!o.person && o.definition && d.entries.size() < 8) {
                    os2::vm::DialogEntry e;
                    e.id = oid;
                    e.name = os2::vm::displayName(m, o);
                    if (!e.name.empty())
                        d.entries.push_back(std::move(e));
                }
            if (!d.entries.empty())
                d.selection = {d.entries[0].id};
        }
        if (d.cls.cls == os2::vm::DialogClass::PhoneNeighbors || d.cls.cls == os2::vm::DialogClass::PhoneParty) {
            d.message.clear();
            // The neighbors a call list could hold: ones with a family (the
            // hood's ancestors, family 0, are not callable: N002 has three
            // "Mãe Rainha" of them), not the caller's own family; the
            // relationship as setupPhone reads it (the trees fill the real lists).
            const os2::vm::Object* me = m.object(gPie->sim());
            const int16_t myFamily = me && me->person ? me->person->data[os2::vm::persondata::FamilyNumber] : 0;
            for (const auto& [id, n] : m.neighbors) {
                const int16_t family = n.personData[os2::vm::persondata::FamilyNumber];
                if (n.firstName.empty() || d.entries.size() >= 9 || !family || family == myFamily ||
                    !m.families.count(family))
                    continue;
                os2::vm::DialogEntry e;
                e.id = id;
                e.guid = n.guid;
                e.first = n.firstName, e.last = n.lastName;
                e.name = n.lastName.empty() ? n.firstName : n.firstName + " " + n.lastName;
                const auto key = std::make_pair(m.relationshipKey(d.me), os2::vm::VM::RelationshipKey{1, id});
                if (auto r = m.relationships.find(key); r != m.relationships.end()) {
                    const auto& vars = r->second;
                    e.relationship = (vars.size() > 0 ? vars[0] : 0) + (vars.size() > 2 ? vars[2] : 0);
                    e.flags = vars.size() > 1 ? (vars[1] & 0xFFFF) : 0;
                }
                d.entries.push_back(std::move(e));
            }
            if (d.cls.cls == os2::vm::DialogClass::PhoneParty) {
                d.maxSelection = 10;
                if (d.entries.size() > 2)
                    d.selection = {d.entries[1].id, d.entries[2].id};
            } else if (!d.entries.empty()) {
                d.selection = {d.entries[0].id};
            }
        }
        m.dialogs[d.me] = std::move(d);
    }
    try {
        const auto plumbob = os2::loadObjectModel(index, "plumbob");
        gPlumbob = renderer.addInstance(renderer.addObject(plumbob, index), matrix_identity_float4x4);
        gPlumbobBottom = os2::modelBounds(plumbob).first.z;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: plumbob: %s\n", e.what());
    }
    // The Sim the player controls is the VM's "selected person ID" (global
    // 3), as when the game selects a Sim: the game's PlumbBob follows it.
    if (gPie->sim())
        gSimulation->machine().globals[os2::vm::global::SelectedPersonId] = gPie->sim();
    const os2::vm::Object* sim = gPie->vm().object(gPie->sim());
    std::printf("opensims2: pie menus for Sim %d (%s)\n", gPie->sim(),
                sim && sim->definition ? sim->definition->fileName.c_str() : "none");
}

// The pie menu's constants (ui.package UI script 0x090617B7), the selected
// Sim's portrait (its character file's IMG 0x10) as the head, and the item
// sounds through the lot's audio.
void setupPieMenu(os2::ResourceIndex& index) {
    os2::UiResources ui(index, gameDir() / "TSData/Res");
    if (auto script = ui.script(0x090617B7))
        gPie->setStyle(os2::PieMenuStyle::fromConstants(os2::uiConstants(*script)));
    if (auto p = simPortrait(gPie->sim()))
        gPie->setPortrait(std::move(*p));
    gPie->setResources(index, gameDir() / "TSData/Res"); // the dialog window's scripts
    gPie->setDialogThumbnails(dialogThumbnail);
    setupWardrobe(index);
    setupAspirationWants(index);
    if (gAudio)
        gPie->setSoundPlayer([](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        });
}

bool liveUiRequested() {
    return argument(@"--ui").value_or("") == "live";
}

// The mode flag the lot opens with: --mode, else Buy for a lot without a
// family (app/game_flow.hpp lotStartMode), else none (Live).
std::optional<std::string> startMode() {
    const std::string m = os2::lotStartMode(!gLotVacant, argument(@"--mode"));
    return m.empty() ? std::nullopt : std::optional<std::string>(m);
}

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
                    candidates = c.catalog->outfitParts(parts[t], spec.age, spec.gender, category, spec.fitness == 2);
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
            for (const os2::SimPart* p : c.catalog->outfitParts(partBit, age, gender, category, self && spec->fitness == 2)) {
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
    // the choices of each group as CAS offers them (cas_mode.cpp), OK
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

// --ui live: the live-mode panel (app/live_ui.hpp) in place of the pie
// menu's debug queue (--debug-queue keeps it). Picking a Sim there selects
// it as --sim-id does (global 3, the pie menu's Sim and head).
void setupLiveUi(os2::ResourceIndex& index) {
    gLive = std::make_unique<os2::LiveUi>(gSimulation->machine(), index, gameDir() / "TSData/Res");
    for (const auto& s : gLotSims)
        if (auto p = simPortrait(s.id))
            gLive->setPortrait(s.id, std::move(*p));
    gLive->setSim(gPie->sim());
    gLive->simAway = [](os2::vm::ObjectId id) { return !simInTheWorld(int16_t(id)); };
    gLive->onSelectSim = [](os2::vm::ObjectId id) {
        gSimulation->machine().globals[os2::vm::global::SelectedPersonId] = id;
        gPie->close();
        gPie->setSim(id);
        if (auto p = simPortrait(id))
            gPie->setPortrait(std::move(*p));
    };
    gLive->onCentreSim = [](os2::vm::ObjectId id) {
        if (gCameraFollow)
            gCameraFollow->centre(gSimulation->machine(), id);
    };
    if (gAudio)
        gLive->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
    // The options panel: "Vizinhança" is the way back to the neighborhood the lot was entered from (as Esc).
    // Leaving asks whether to save and switches scenes in place (game flow, below).
    if (auto hood = argument(@"--hood"); hood && gHoodLot) {
        gLive->setCanLeave(true);
        gLive->onNeighborhood = [] { leaveLot(); };
    }
    gLive->setLiveEnabled(!gLotVacant); // no Sims: the Live button is off (HandleLiveModeButton)
    gLive->onQuit = [] { quitFromLot(); };
    gLive->onSave = [] { saveGameNow(); };
    gLive->onOptionWindow = [](int window) {
        if (gOptions)
            gOptions->toggleWindow(window);
    };
    gLive->onFloor = [](int delta) { changeFloor(delta); };
    gLive->onWallMode = [](int mode) { setWallMode(mode); };
    gLive->setWallMode(gWallView.mode());
    gLive->onZoom = [](int steps) {
        if (gCamera)
            gCamera->zoomBy(steps);
    };
    gLive->onRotate = [](int steps) {
        if (gCamera)
            gCamera->rotateBy(steps);
    };
    // Relationship thumbnails of Sims off the lot: the small portrait of the
    // character file (in the lot's neighborhood) whose OBJD has the GUID.
    if (auto query = lotQuery())
        gLive->neighborPortrait = [dir = lotPath(*query).parent_path().parent_path() / "Characters",
                                   byGuid = std::make_shared<std::map<uint32_t, fs::path>>()](uint32_t guid)
            -> std::optional<os2::RgbaImage> {
            std::error_code ec;
            if (byGuid->empty() && fs::is_directory(dir, ec))
                for (const auto& de : fs::directory_iterator(dir, ec))
                    try {
                        os2::DbpfPackage p(de.path());
                        for (const auto& e : p.entries())
                            if (e.tgi.type == kTypeObjd) {
                                byGuid->emplace(os2::ObjectDefinition::parse(p.read(e)).guid(), de.path());
                                break;
                            }
                    } catch (const std::exception& err) {
                        os2::logOnce("neighbor portraits", de.path().filename().string(), err);
                    }
            const auto it = byGuid->find(guid);
            if (it == byGuid->end())
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
        };
    gLive->onFollowSim = [](os2::vm::ObjectId id) {
        if (gCameraFollow)
            gCameraFollow->setTracked(id);
    };
    if (![sceneArgs() containsObject:@"--debug-queue"])
        gPie->setShowQueue(false);
    std::printf("opensims2: live UI: %s, %zu family members\n", gLive->ok() ? "loaded" : "missing scripts",
                gLive->family().size());
}

// A lot of the Tutorial neighborhood: its tutorial controllers (created if
// the lot has none, vm::ensureTutorialControllers) and the tutorial
// manager's UI over the live panel (app/tutorial.hpp).
void setupTutorial(os2::ResourceIndex& index) {
    if (!gSimulation || !gLive || gLotFile.parent_path().parent_path().filename() != "Tutorial")
        return;
    os2::vm::VM& vm = gSimulation->machine();
    const int created = os2::vm::ensureTutorialControllers(vm);
    // The tutorial lots are saved paused; the episode plays in time (step 10:
    // "this is the only mode in which time passes"), so it starts unpaused
    // (inferred) and the controller's first press of Buy mode pauses it.
    vm.globals[os2::vm::global::Paused] = 0;
    gTutorialRequestsSeen = vm.engineRequests.size();
    gTutorial = std::make_unique<os2::TutorialOverlay>(vm, index, gameDir() / "TSData/Res");
    // The controls: Buy's and Build's panels while they are up, the live
    // panel and the puck.
    gTutorial->locate = [](uint32_t window, uint32_t control) -> std::optional<std::array<float, 4>> {
        // The main window's controls are the puck's (the panels reuse small ids).
        if (window == os2::LiveUi::kMainWindow)
            if (auto r = gLive->controlRect(window, control))
                return r;
        if (gBuy && gBuy->active())
            if (auto r = gBuy->controlRect(window, control))
                return r;
        if (gBuild && gBuild->active())
            if (auto r = gBuild->controlRect(window, control))
                return r;
        return gLive->controlRect(window, control);
    };
    gTutorial->pressControl = [](uint32_t window, uint32_t control) {
        const auto r = gTutorial->locate(window, control);
        if (!r)
            return;
        const simd_float2 p = simd_make_float2(((*r)[0] + (*r)[2]) / 2, ((*r)[1] + (*r)[3]) / 2);
        gTutorial->clicked(p); // as a click: the control's listeners, then the control
        if (gLive->overModeButton(p) && gLive->press(p))
            return;
        if (gBuy && gBuy->active() && gBuy->press(p))
            return;
        if (gBuild && gBuild->active() && gBuild->press(p))
            return;
        gLive->press(p);
    };
    gLive->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    if (gBuy) {
        gBuy->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    }
    if (gBuild) {
        gBuild->allowPress = [](simd_float2 p) { return gTutorial->allows(p); };
    }
    std::printf("opensims2: tutorial: %d controller(s) created\n", created);
}

// Selecting a Sim of the family other than by its thumbnail: a right click
// on it, or the space bar for the next one (the game's controls, as the
// tutorial's second lesson teaches them). False if it is not one of them.
bool selectFamilySim(os2::vm::ObjectId id) {
    if (!gLive || !id || id == gLive->sim())
        return false;
    const auto family = gLive->family();
    if (std::find(family.begin(), family.end(), id) == family.end())
        return false;
    if (!simInTheWorld(int16_t(id))) // at work: its thumbnail is greyed, no orders
        return false;
    std::printf("opensims2: select Sim %d\n", id);
    if (gLive->onSelectSim)
        gLive->onSelectSim(id);
    gLive->setSim(id);
    std::fflush(stdout);
    return true;
}

void selectNextFamilySim() {
    if (!gLive)
        return;
    const auto family = gLive->family();
    if (family.size() < 2)
        return;
    auto it = std::find(family.begin(), family.end(), gLive->sim());
    selectFamilySim(it == family.end() || std::next(it) == family.end() ? family.front() : *std::next(it));
}

// A selected Sim that leaves the world (off to work) hands the selection to
// a family member who is home: its thumbnail is greyed and it takes no
// orders (live QA 15: the Sim at work could be selected and got "Receber
// Correio"). All of them away: the selection stays.
void keepSelectedSimHome() {
    if (!gLive || !gPie || !gSimulation || !gLive->sim() || simInTheWorld(int16_t(gLive->sim())))
        return;
    for (const os2::vm::ObjectId id : gLive->family())
        if (id != gLive->sim() && selectFamilySim(id))
            return;
}

// The tutorial each frame: its UI Effects, and the end of an episode
// (Generic Sims Call 0 "Exit Lot", from "Simulation - Exit Tutorial"): back
// to the main menu it was played from.
void pollTutorial(double seconds) {
    if (!gTutorial || !gSimulation)
        return;
    gTutorial->update(seconds);
    auto& requests = gSimulation->machine().engineRequests;
    if (gTutorialRequestsSeen > requests.size())
        gTutorialRequestsSeen = 0;
    for (; gTutorialRequestsSeen < requests.size(); ++gTutorialRequestsSeen)
        if (requests[gTutorialRequestsSeen].call == 0 && !gPendingScene) {
            const auto& t = requests[gTutorialRequestsSeen].temps;
            std::printf("opensims2: tutorial: episode over (state %d), exit lot%s\n",
                        gSimulation->machine().globals[26], t[2] ? ", saving" : "");
            std::fflush(stdout);
            // Exit Lot's temp 2: save the lot first (its SIMI keeps global 26
            // "Tutorial State", which the episode list reads).
            if (t[2])
                saveGameNow();
            os2::Scene next;
            next.kind = os2::Scene::Kind::MainMenu;
            requestScene(next);
        }
}

// Buy mode over the lot's VM: the puck's mode buttons switch it (the live
// panel gives way to the Buy panel); --mode buy starts in it; headless,
// --buy <guid|name> picks a catalog item, --hold x,y[,dir[,level]] holds it
// there and --place x,y[,dir[,level]] buys it there (repeatable, in order),
// --grab <object id> picks up a placed object, --sell sells what is held,
// --undo undoes the last change.
void setupBuyMode(os2::Renderer& renderer, os2::ResourceIndex& index) {
    gBuy = std::make_unique<os2::BuyMode>(*gSimulation, *gObjectView, renderer, index, gameDir() / "TSData/Res");
    if (gLive)
        gLive->onMode = [](int mode) {
            if (mode == 1) {
                gBuy->enter();
                gLive->setMode(1);
            } else if (mode == 0) {
                gBuy->exit();
                releaseDayNight();
                gLive->setMode(0);
            }
        };
    gBuy->pickObject = [](simd_float3 eye, simd_float3 dir) { return gPie ? gPie->pick(eye, dir) : os2::vm::ObjectId(0); };
    gBuy->onDayNight = [] { toggleDayNight(); };
    if (gAudio)
        gBuy->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
    if (startMode().value_or("") == "buy") {
        gBuy->enter();
        if (gLive)
            gLive->setMode(1);
    }
}

// Fence rails and posts (os2::lotFencePieces): one model per CRES, an
// instance per piece; called again after Build mode changed the fences
// (instances are reused, extra ones hidden). Returns the pieces placed.
// Pieces above `level` (the lot's numbering) are left out (see WallView::visible).
size_t placeFencePieces(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                        const os2::PatternCatalog& patterns, std::vector<std::string>* warnings,
                        int level) {
    auto& models = gFenceModels;
    auto& instances = gFenceInstances;
    const auto pieces = os2::lotFencePieces(lot, patterns, warnings);
    size_t used = 0;
    for (const auto& piece : pieces) {
        // (Not the next-story rule of a level shown with the one below:
        // railings stand on decks over any ground, buildLotModel keeps them.)
        if (!os2::WallView::visible(lot, level, piece.transform[12], piece.transform[13], piece.transform[14]))
            continue;
        auto it = models.find(piece.model);
        if (it == models.end()) {
            int id = -1;
            try {
                id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
            } catch (const std::exception& e) {
                if (warnings)
                    warnings->push_back(piece.model + ": " + e.what());
                else // (a rebuild after Build mode: nobody prints the warnings)
                    os2::logOnce("fence piece", piece.model, e);
            }
            it = models.emplace(piece.model, id).first;
        }
        if (it->second < 0)
            continue;
        simd_float4x4 at;
        std::memcpy(&at, piece.transform.data(), sizeof at);
        if (used < instances.size()) {
            // An instance this placed holds a fence's model; anything else is
            // another thing's instance (the ids of an earlier scene's renderer).
            const int held = renderer.instanceModel(instances[used]);
            if (std::none_of(models.begin(), models.end(), [&](const auto& m) { return m.second == held; }))
                std::fprintf(stderr, "opensims2: fence piece instance %d holds model %d, not a fence's\n", instances[used], held);
            renderer.setInstanceModel(instances[used], it->second), renderer.setInstanceTransform(instances[used], at);
        }
        else
            instances.push_back(renderer.addInstance(it->second, at));
        ++used;
    }
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[3] = simd_make_float4(0, 0, -1000, 1);
    for (size_t i = used; i < instances.size(); ++i)
        renderer.setInstanceTransform(instances[i], away);
    return used;
}

// The level the camera shows, whose walls follow the wall mode: Build and
// Buy mode's level, else the camera's (the selected Sim's or the one Page
// Up/Down chose); every level outside the game (a bare --lot view).
int viewLevel() {
    if (!gLevelsHidden)
        return os2::LotViewOptions::kAllLevels;
    if (gBuild && gBuild->active())
        return gWallView.shownLevel(gBuild->level() - 1);
    if (gBuy && gBuy->active())
        return gWallView.shownLevel(gBuy->level() - 1);
    return gCameraFollow ? gWallView.shownLevel(gCameraFollow->level()) : os2::LotViewOptions::kAllLevels;
}

// Whether the floor of `lotLevel` at `at` is drawn when the camera shows
// `top`: not, in a level shown with the one below, the parts a story over
// the ground (os2::storyOverGround), which the next floor up shows.
bool drawnFloor(int top, int lotLevel, simd_float3 at) {
    return lotLevel != top || gWallView.lowestCut(top) >= top || !os2::storyOverGround(*gLotShape, lotLevel, int(std::floor(at.x)), int(std::floor(at.y)));
}

// The ground or floor under the mouse ray, as Go Here takes it
// (cTSUserToolStandardAction::GroundOrWallButNotPondAtGameWindowPoint): the
// first level, from the one the camera shows down, whose floor the ray
// meets where that level has something to stand on (or a pool); the point
// and its VM level.
std::optional<std::pair<simd_float2, int>> groundUnderRay(simd_float3 eye, simd_float3 dir) {
    if (!gSimulation || !gLotShape || std::fabs(dir.z) < 1e-5f || !gSimulation->machine().world)
        return std::nullopt;
    const os2::vm::World& w = *gSimulation->machine().world;
    int top = viewLevel();
    if (top == os2::LotViewOptions::kAllLevels)
        top = gLotShape->minLevel + gLotShape->levelCount - 1;
    for (int lotLevel = top; lotLevel >= gLotShape->minLevel; --lotLevel) {
        const int level = lotLevel + 1;
        float z = gSimulation->floorHeight(level, float(w.width()) / 2, float(w.height()) / 2);
        simd_float3 hit{};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            const float t = (z - eye.z) / dir.z;
            ok = t > 0;
            hit = eye + dir * t;
            z = gSimulation->floorHeight(level, hit.x, hit.y);
        }
        const os2::vm::TilePos tile{int(std::floor(hit.x)), int(std::floor(hit.y)), level};
        if (ok && w.contains(tile) && (w.walkable(tile) || w.pool(tile)) && drawnFloor(top, lotLevel, hit))
            return std::make_pair(simd_make_float2(hit.x, hit.y), level);
    }
    return std::nullopt;
}

// Where the mouse ray first meets a floor of the levels shown (the ray's
// parameter), from the one the camera shows down: a level's floor where
// the tile has one (the ground's terrain everywhere; not the openings over
// stairs). ObjectAtGameWindowPoint stops there: nothing behind it is picked.
std::optional<float> floorUnderRay(simd_float3 eye, simd_float3 dir) {
    if (!gSimulation || !gLotShape || std::fabs(dir.z) < 1e-5f || !gSimulation->machine().world)
        return std::nullopt;
    const os2::vm::World& w = *gSimulation->machine().world;
    int top = viewLevel();
    if (top == os2::LotViewOptions::kAllLevels)
        top = gLotShape->minLevel + gLotShape->levelCount - 1;
    for (int lotLevel = top; lotLevel >= gLotShape->minLevel; --lotLevel) {
        const int level = lotLevel + 1;
        float z = gSimulation->floorHeight(level, float(w.width()) / 2, float(w.height()) / 2);
        simd_float3 hit{};
        float t = 0;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            t = (z - eye.z) / dir.z;
            ok = t > 0;
            hit = eye + dir * t;
            z = gSimulation->floorHeight(level, hit.x, hit.y);
        }
        const os2::vm::TilePos tile{int(std::floor(hit.x)), int(std::floor(hit.y)), level};
        if (ok && w.contains(tile) && w.hasFloor(tile) && drawnFloor(top, lotLevel, hit))
            return t;
    }
    return std::nullopt;
}

// A click on the ground: the selected Sim's pie menu on a go-here
// destination there (vm/go_here.hpp). False when nothing opened.
bool openGoHere(simd_float3 eye, simd_float3 dir, simd_float2 at) {
    if (!gPie || !gPie->sim())
        return false;
    const auto g = groundUnderRay(eye, dir);
    if (!g)
        return false;
    os2::vm::VM& m = gPie->vm();
    const os2::vm::ObjectId dest = os2::vm::createGoHereDestination(m, gPie->sim(), g->first.x, g->first.y, g->second);
    if (!dest)
        return false;
    if (!gPie->open(dest, at)) {
        os2::vm::releaseDestination(m, dest);
        return false;
    }
    if (const os2::vm::Object* d = m.object(dest))
        std::printf("opensims2: go here: destination %d (0x%08X%s) at (%.2f, %.2f, %d)\n", dest,
                    d->definition ? d->definition->guid() : 0u, d->definition && d->definition->fileName.find("Swim") != std::string::npos ? ", swim here" : "", d->position.x, d->position.y,
                    d->position.level);
    return true;
}

// A click that picked object `id` (cTSUserToolStandardAction::MouseEvent):
// its menu; a rug's merges a go-here destination at the floor under the
// mouse (vm::objectClick). Any other object without interactions only
// shows its tooltip, as the binary does (no Go Here under a tree's canopy).
void openClicked(os2::vm::ObjectId id, simd_float3 eye, simd_float3 dir, simd_float2 at) {
    if (!gPie || !gPie->sim())
        return;
    os2::vm::VM& m = gPie->vm();
    if (os2::vm::isGoHereRug(m, id)) {
        std::optional<os2::vm::Position> ground;
        if (const auto g = groundUnderRay(eye, dir))
            ground = os2::vm::Position{g->first.x, g->first.y, g->second};
        bool has = false;
        for (const auto& e : os2::vm::pieMenu(m, gPie->sim(), id))
            has = has || (e.available() && !e.debug);
        const auto menu = os2::vm::objectClick(m, gPie->sim(), id, ground, has);
        if (menu.object && gPie->open(menu.object, at, menu.destination)) {
            std::printf("opensims2: rug %d%s\n", id, menu.destination ? " with a go-here destination" : "");
            return;
        }
        for (const os2::vm::ObjectId d : {menu.object, menu.destination})
            if (d && d != id)
                os2::vm::releaseDestination(m, d);
        return;
    }
    if (!gPie->open(id, at))
        std::printf("opensims2: object %d: no interactions available (its tooltip)\n", id);
}

// Whether an object hangs on a wall the lot model cut down: a cut wall of
// its level within 0.6 tiles, across its front (perpendicular to its +Y).
bool onCutWall(const os2::Xform& at) {
    if (gLotCache.cut.empty() || !gLotShape)
        return false;
    const os2::Vec3 f = at.rotate({0, 1, 0});
    for (const auto& c : gLotCache.cut) {
        const int level = int(c[4]);
        const float floorZ = gLotShape->elevationAt(level, std::clamp(at.t[0], 0.0f, float(gLotShape->width)),
                                                    std::clamp(at.t[1], 0.0f, float(gLotShape->height)));
        if (at.t[2] < floorZ - 0.2f || at.t[2] > floorZ + 2.5f)
            continue;
        float dx = c[2] - c[0], dy = c[3] - c[1];
        const float len = std::hypot(dx, dy);
        if (len < 1e-4f)
            continue;
        dx /= len, dy /= len;
        if (std::abs(dx * f[0] + dy * f[1]) > 0.3f)
            continue;
        const float s = std::clamp((at.t[0] - c[0]) * dx + (at.t[1] - c[1]) * dy, 0.0f, len);
        if (std::hypot(c[0] + dx * s - at.t[0], c[1] + dy * s - at.t[1]) <= 0.6f)
            return true;
    }
    return false;
}

os2::LotViewOptions wantedLotView() {
    if (cinematicPlaying())
        return cinematicLotView();
    std::array<float, 2> look{gLotView.viewDirection};
    if (gCamera) {
        const simd_float3 d = gCamera->pivot() - gCamera->eye();
        look = {d.x, d.y};
    }
    os2::LotViewOptions v = gWallView.options(look, viewLevel());
    // cutawayForSelectedSim: the selected Sim on the level shown.
    if (v.walls == os2::LotViewOptions::Walls::Cutaway && gCamera && gSimulation && gLive &&
        v.level != os2::LotViewOptions::kAllLevels) {
        const os2::vm::VM& m = gSimulation->machine();
        const int16_t id = m.globals[os2::vm::global::SelectedPersonId];
        if (const auto it = m.objects().find(os2::vm::ObjectId(id)); id > 0 && it != m.objects().end() &&
                                                                      it->second.position.inWorld() &&
                                                                      it->second.position.level - 1 == v.level) {
            const simd_float3 e = gCamera->eye();
            v.simLine = std::array<float, 4>{e.x, e.y, it->second.position.x, it->second.position.y};
        }
    }
    return v;
}

// Keeps the lot drawn for the current view (called every frame and before
// headless shots): rebuilds the lot model when the wall mode, the roof, the
// level or the cutaway side of the walls changed (only the geometry: the
// cache keeps the materials and holes, and the light-map atlas is the same
// for every view), hides the Sims above the level shown and keeps their pick
// boxes where they stand.
// The model part runs before the objects are placed (what hangs on cut
// walls hides), the Sims part (`sims`) after they are posed.
// Whether the Sim of object `id` is in the lot's world: it, or what holds
// it (a carried baby, a passenger in a car), stands in it. A Sim at work
// rode off in its carpool, which the car then took out of the world with
// the Sim still in its slot (live QA 15: the plumbob hung at the road's end).
bool simInTheWorld(int16_t id) {
    if (!gSimulation)
        return true;
    os2::vm::VM& m = gSimulation->machine();
    const os2::vm::Object* o = m.object(id);
    for (int depth = 0; o && depth < 8; ++depth) {
        if (o->position.inWorld())
            return true;
        const int16_t c = o->data[os2::vm::objdata::ContainerId];
        if (!c || c == o->id)
            return false;
        o = m.object(c);
    }
    return false;
}

void updateLotView(os2::Renderer& renderer, bool sims) {
    if (!gLotShape || gLotInstance < 0 || gLotModel < 0 || !gLotPatterns || !gIndex)
        return;
    const os2::LotViewOptions want = wantedLotView();
    if (!sims && !os2::WallView::sameCut(gLotView, want)) {
        const CFTimeInterval start = CACurrentMediaTime();
        os2::LotLightMapLayout layout;
        const os2::ObjectModel model = os2::buildLotModel(*gLotShape, *gIndex, *gLotPatterns, want, &layout, &gLotCache);
        const CFTimeInterval built = CACurrentMediaTime();
        renderer.replaceObject(gLotModel, model, *gIndex);
        if (want.level != gLotView.level)
            placeFencePieces(renderer, *gIndex, *gLotShape, *gLotPatterns, nullptr, want.level);
        // The atlas does not depend on the view; if it did change (it should
        // not), the light maps follow it.
        if (gLightMapLayout && (layout.walls.size() != gLightMapLayout->walls.size() || layout.height != gLightMapLayout->height)) {
            gLightMapLayout = std::move(layout);
            if (gLighting)
                gLighting->setLightMapLayout(*gLightMapLayout);
        }
        gLotView = want;
        static const bool perf = std::getenv("OS2_PERF") != nullptr;
        if (perf || !gCamera)
            std::printf("opensims2: lot view: walls %s, level %d, roof %s (%.1f ms build, %.1f ms upload)\n",
                        want.walls == os2::LotViewOptions::Walls::Up        ? "up"
                        : want.walls == os2::LotViewOptions::Walls::Cutaway ? "cutaway"
                                                                            : "down",
                        want.level == os2::LotViewOptions::kAllLevels ? -1 : want.level, want.roof ? "on" : "off",
                        (built - start) * 1000, (CACurrentMediaTime() - built) * 1000);
    }
    if (gLive)
        gLive->setWallMode(gWallView.mode());
    if (!sims)
        return;
    // Sims above the level shown: out of sight, and not picked.
    auto& instances = gSimInstances;
    simd_float4x4 away = matrix_identity_float4x4;
    away.columns[3] = simd_make_float4(0, 0, -1000, 1);
    for (const auto& s : gLotSims) {
        auto it = instances.find(s.actor);
        if (it == instances.end())
            it = instances.emplace(s.actor, renderer.instanceOf(s.actor->model)).first;
        // A carried Sim is shown where its carrier stands (its belly-high
        // placement would count as the level above).
        const auto carrier = gCarriers.find(s.actor);
        const os2::Vec3 p = carrier != gCarriers.end() ? carrier->second->position
                            : s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                             s.actor->transform->t[2]}
                                                 : s.actor->position;
        bool shown = os2::WallView::visible(*gLotShape, want.level, p[0], p[1], p[2], want.lowestCut);
        // A Sim the VM removed (a carpool or bus driver whose vehicle left)
        // or sent out of the world (at work, gone home) is not drawn: its
        // actor kept its last pose where it was (live QA 15: the drivers
        // stayed sitting in the street after each carpool).
        if (!simInTheWorld(s.id))
            shown = false;
        if (it->second >= 0)
            renderer.setInstanceTransform(it->second, shown ? matrix_identity_float4x4 : away);
        if (!gPie)
            continue;
        // The pick box: the posed bones' extent, a little wider.
        simd_float3 lo = simd_float3(1e9f), hi = simd_float3(-1e9f);
        for (const auto& b : s.actor->bones)
            lo = simd_min(lo, simd_make_float3(b.t[0], b.t[1], b.t[2])), hi = simd_max(hi, simd_make_float3(b.t[0], b.t[1], b.t[2]));
        if (s.actor->bones.empty()) // not posed yet (headless clicks): a standing adult's box
            lo = simd_make_float3(p[0] - 0.15f, p[1] - 0.15f, p[2]), hi = simd_make_float3(p[0] + 0.15f, p[1] + 0.15f, p[2] + 1.7f);
        if (!shown || hi.z < -100) {
            gPie->setMovingBox(os2::vm::ObjectId(s.id), simd_float3(0), simd_float3(0));
            continue;
        }
        lo -= simd_make_float3(0.15f, 0.15f, 0.05f), hi += simd_make_float3(0.15f, 0.15f, 0.15f);
        // Picked by its skinned triangles as drawn (the box is the quick
        // test); before its first pose (headless clicks) by the box.
        static const bool boxes = std::getenv("OS2_PICK_BOXES") != nullptr;
        static const bool simBoxes = std::getenv("OS2_PICK_SIM_BOXES") != nullptr; // only the Sims by box, to compare
        const bool posed = !s.actor->bones.empty() && !s.actor->posed.parts.empty() && !boxes && !simBoxes;
        gPie->setMovingBox(os2::vm::ObjectId(s.id), lo, hi, posed ? &s.actor->posed : nullptr);
    }
    // Objects the VM made after the lot loaded (the bills a mailman leaves,
    // dishes, newspapers, things bought): picked by their model's triangles
    // where the VM view draws them, as the lot's own objects are (they had no
    // pick box, so a click on the bills found nothing: live QA 15).
    if (gPie && gObjectView && gIndex) {
        auto& made = gMadePickBoxes; // (a new lot: none made yet; a new host can reuse the old one's address)
        auto& meshes = gGameData.pickMeshes; // by CRES (null: none; the game's models: for the app's life)
        std::set<int16_t> drawnNow;
        gObjectView->forEachDrawn([&](int16_t id, int, os2::SimActor*) { drawnNow.insert(id); });
        for (auto it = made.begin(); it != made.end();) {
            const bool same = drawnNow.count(it->first) && gObjectView->cresOf(it->first) == it->second;
            if (same) {
                ++it;
                continue;
            }
            gPie->removePickBox(os2::vm::ObjectId(it->first));
            it = made.erase(it);
        }
        for (const int16_t id : drawnNow) {
            if (made.count(id) || gPie->hasPickBox(os2::vm::ObjectId(id)))
                continue;
            const std::string cres = gObjectView->cresOf(id);
            if (cres.empty())
                continue;
            auto m = meshes.find(cres);
            if (m == meshes.end()) {
                std::shared_ptr<const os2::PickMesh> mesh;
                try {
                    mesh = os2::makePickMesh(os2::loadObjectModel(*gIndex, cres, {}), gIndex.get());
                } catch (const std::exception& err) {
                    os2::logOnce("pick mesh", cres, err);
                }
                m = meshes.emplace(cres, std::move(mesh)).first;
            }
            if (!m->second || m->second->corners.empty())
                continue;
            gPie->addFollowingPickBox(os2::vm::ObjectId(id), m->second->lo, m->second->hi, m->second);
            if (std::getenv("OS2_DEBUG_PICK"))
                std::fprintf(stderr, "pick: box made for object %d (%s): %s\n", id, cres.c_str(), gPie->hasPickBox(os2::vm::ObjectId(id)) ? "yes" : "no");
            if (gPie->hasPickBox(os2::vm::ObjectId(id)))
                made[id] = cres;
        }
    }
    // Objects the VM animated (a door open, a chair pulled out): picked by
    // their skinned triangles as drawn, not by their static model's rest pose.
    static const bool staticObjects = std::getenv("OS2_PICK_STATIC_OBJECTS") != nullptr; // the old picking, to compare
    if (gPie && !staticObjects)
        for (const auto& [id, actor] : gObjectActors) {
            if (!actor || actor->posed.parts.empty() || (gObjectView && !gObjectView->shown(id))) {
                gPie->setPosedObject(os2::vm::ObjectId(id), simd_float3(0), simd_float3(0), nullptr);
                continue;
            }
            simd_float3 lo = simd_float3(1e9f), hi = simd_float3(-1e9f);
            for (const auto& part : actor->posed.parts)
                for (const auto& v : part.mesh.positions)
                    lo = simd_min(lo, simd_make_float3(v[0], v[1], v[2])), hi = simd_max(hi, simd_make_float3(v[0], v[1], v[2]));
            if (lo.x > hi.x)
                continue;
            gPie->setPosedObject(os2::vm::ObjectId(id), lo - 0.01f, hi + 0.01f, &actor->posed);
        }
}

// Home / End and the puck's wall fan.
void setWallMode(int mode) {
    gWallView.setMode(mode);
    if (gLive)
        gLive->setWallMode(gWallView.mode());
}

// Redraws the lot after Build mode changed it: the model (walls cut away
// as seen from the camera now, holes for the doors and windows), and the
// light-map atlas and light maps.
void rebuildLotModel(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape || gLotInstance < 0 || !gLotPatterns)
        return;
    gWallView.study(*gLotShape);
    const os2::LotViewOptions view = wantedLotView();
    gLotCache.geometryChanged();
    gLightMapLayout.emplace();
    const os2::ObjectModel model =
        os2::buildLotModel(*gLotShape, index, *gLotPatterns, view, &*gLightMapLayout, &gLotCache);
    renderer.replaceObject(gLotModel, model, index);
    renderer.setGroundClip(os2::lotSkirtBottom(*gLotShape)); // the terrain may have been lowered
    gLotView = view;
    placeFencePieces(renderer, index, *gLotShape, *gLotPatterns, nullptr, view.level);
    if (gLighting) {
        gLighting->setLightMapLayout(*gLightMapLayout);
    } else {
        const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
        if (const auto* state = lighting.state(argument(@"--lighting").value_or("day"))) {
            const os2::RgbaImage lm =
                os2::computeLotLightMap(*gLightMapLayout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            renderer.setLightMap(0, lm);
            renderer.setLightMap(1, lm);
        }
    }
}

// "Alternar Dia/Noite" of Buy and Build mode (0xCC607035 / 0xAA): the lot
// lit as at night or by day, whatever the clock says, until the mode is left.
void toggleDayNight() {
    if (!gLighting)
        return;
    const std::string next = gLighting->stateName() == "night" ? "day" : "night";
    gLighting->forceState(next); // the next update() blends to it
    std::printf("opensims2: lighting: %s (day/night toggle)\n", next.c_str());
}

// Leaving Buy or Build mode: the clock's lighting again (or --lighting's).
void releaseDayNight() {
    if (gLighting)
        gLighting->forceState(argument(@"--lighting").value_or(""));
}

// Build mode over the lot's VM: the puck's Build button (mode 2) or --mode
// build; headless steps in BuildMode::runSteps (build_mode.cpp).
void setupBuildMode(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape)
        return;
    gBuild = std::make_unique<os2::BuildMode>(*gSimulation, *gObjectView, renderer, index, gameDir() / "TSData/Res",
                                              *gLotShape);
    gBuild->onGeometryChanged = [&renderer, &index]() { rebuildLotModel(renderer, index); };
    gBuild->pickObject = [](simd_float3 eye, simd_float3 dir) { return gPie ? gPie->pick(eye, dir) : os2::vm::ObjectId(0); };
    gBuild->onDayNight = [] { toggleDayNight(); };
    gBuild->renderThumb = [&renderer, &index](const std::vector<os2::ThumbnailPart>& parts) {
        return os2::renderPartsThumbnail(renderer, index, parts, 94);
    };
    if (gAudio) { // the ui_build_* sounds and the terrain brush's loops
        gBuild->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
        gBuild->startLoop = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            o.loop = true;
            return uint32_t(gAudio->sound().play(name, o));
        };
        gBuild->stopLoop = [](uint32_t h) { gAudio->sound().stop(os2::SoundSystem::Handle(h)); };
    }
    if (gLive) {
        auto previous = gLive->onMode;
        gLive->onMode = [previous](int mode) {
            if (mode == 2) {
                if (gBuy)
                    gBuy->exit();
                gBuild->enter();
                gLive->setMode(2);
                return;
            }
            gBuild->exit();
            if (previous)
                previous(mode);
        };
    }
    if (startMode().value_or("") == "build") {
        gBuild->enter();
        if (gLive)
            gLive->setMode(2);
    }
}

void runBuildSteps() {
    if (!gBuild || !gBuild->active())
        return;
    std::vector<std::string> args;
    for (NSString* a in sceneArgs())
        args.push_back(a.UTF8String);
    gBuild->runSteps(args);
}

// The --buy/--hold/--place/--grab/--sell/--undo steps of a headless run, in order.
void runBuySteps() {
    if (!gBuy || !gBuy->active())
        return;
    NSArray<NSString*>* args = sceneArgs();
    for (NSUInteger i = 1; i < args.count; ++i) {
        const std::string a = args[i].UTF8String;
        const std::string v = i + 1 < args.count ? args[i + 1].UTF8String : "";
        int x = 0, y = 0, dir = 2, level = 0;
        if (a == "--buy") {
            const os2::CatalogEntry* e = gBuy->catalog().find(v);
            if (!e)
                throw std::runtime_error("--buy: no catalog item \"" + v + "\"");
            gBuy->select(*e);
        } else if ((a == "--hold" || a == "--place") && std::sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &dir, &level) >= 2) {
            if (level > 0)
                gBuy->setLevel(level);
            if (a == "--hold")
                gBuy->holdAt(x, y, dir);
            else
                gBuy->placeAt(x, y, dir);
        } else if (float fx = 0, fy = 0; a == "--buy-drag" && std::sscanf(v.c_str(), "%d,%d,%f,%f", &x, &y, &fx, &fy) == 4) {
            // --buy-drag x,y,fx,fy: the press on tile x,y, a drag to the ground point fx,fy, the release.
            if (gBuy->pressAt(x, y)) {
                gBuy->aimAt(simd_make_float2(fx, fy));
                gBuy->mouseUp();
            }
        } else if (a == "--buy-cancel") { // Esc
            gBuy->cancel();
        } else if (a == "--grab") {
            gBuy->grab(os2::vm::ObjectId(std::stoi(v)));
        } else if (int id = 0; a == "--grab-drag" && std::sscanf(v.c_str(), "%d,%d,%d", &id, &x, &y) == 3) {
            gBuy->grabDrag(os2::vm::ObjectId(id), x, y); // --grab-drag id,x,y: press on it, drag to x,y, release
        } else if (a == "--eyedrop") {
            gBuy->eyedrop(os2::vm::ObjectId(std::stoi(v)));
        } else if (a == "--design") {
            // --design <id>[,row:option...][,accept|cancel]: the Design tool
            // on an object (the dialog by the middle of the screen), swatches
            // picked, then Accept or Cancel.
            std::vector<std::string> f;
            for (size_t k = 0, e; k <= v.size(); k = e + 1) {
                e = v.find(',', k);
                if (e == std::string::npos)
                    e = v.size();
                f.push_back(v.substr(k, e - k));
            }
            gBuy->setTool(os2::BuyMode::Tool::Design);
            // A name instead of an ID: the first object whose file name has it (a part with a model).
            os2::vm::ObjectId target = 0;
            if (!f[0].empty() && !std::isdigit(static_cast<unsigned char>(f[0][0]))) {
                for (const auto& [id, o] : gSimulation->machine().objects())
                    if (o.definition && std::string(o.definition->fileName).find(f[0]) != std::string::npos && !o.person) {
                        target = id;
                        break;
                    }
            } else {
                target = os2::vm::ObjectId(std::stoi(f[0]));
            }
            if (gBuy->design(target, simd_make_float2(560, 420)))
                for (size_t k = 1; k < f.size(); ++k) {
                    int row = 0, option = 0;
                    if (f[k] == "accept")
                        gBuy->designTool().accept();
                    else if (f[k] == "cancel")
                        gBuy->designTool().cancel();
                    else if (std::sscanf(f[k].c_str(), "%d:%d", &row, &option) == 2)
                        gBuy->designTool().choose(row, size_t(option));
                }
        } else if (a == "--day-night") {
            if (gBuy->onDayNight)
                gBuy->onDayNight();
        } else if (a == "--sell") {
            gBuy->deleteHeld();
        } else if (a == "--undo") {
            gBuy->undo();
        }
    }
    gBuy->update();
}

// A community lot's loading screen shows the catalog image of one of the
// lot's objects (docs/cinematics-mac.md §2.1: bit 0x20 of OBJD field 0x5E,
// the function subsort), when the screen asked for it.
void offerCommunityLotImage(os2::ResourceIndex& index, const os2::Lot& lot) {
    if (!os2::loadingIconWanted())
        return;
    try {
        os2::Catalog catalog = os2::Catalog::load(index);
        catalog.setThumbnailPackage(gameDir() / "TSData/Res/UserData/Thumbnails/ObjectThumbnails.package");
        for (const auto& o : lot.objects) {
            std::string name = o.definition;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, "_objt") == 0)
                name.resize(name.size() - 5);
            const os2::CatalogEntry* e = catalog.find(name);
            if (!e || !(e->functionSubSort & 0x20))
                continue;
            if (const os2::RgbaImage* img = catalog.thumbnail(*e)) {
                os2::offerLoadingIcon(*img);
                return;
            }
        }
    } catch (const std::exception& err) {
        os2::logOnce("loading screen icon", "catalog", err);
    }
}

// Loads --lot into the renderer in place of the test floor. Returns the middle
// of the lot at ground level, for the camera.
// --walls up|cutaway|down picks the wall mode; cutaway uses `viewDirection`.
std::optional<simd_float3> loadLot(os2::Renderer& renderer, os2::ResourceIndex& index, simd_float2 viewDirection) {
    const auto query = lotQuery();
    if (!query)
        return std::nullopt;
    CFTimeInterval start = CACurrentMediaTime();
    gLotFile = lotPath(*query);
    os2::DbpfPackage pkg(lotPath(*query));
    perfLap("before the lot");
    os2::Lot lot = os2::Lot::load(pkg);
    perfLap("Lot::load");
    const size_t leftovers = os2::vm::dropLeftoverCObjects(lot, pkg, index); // stale copies the package keeps
    std::printf("opensims2: %zu leftover cObjects not drawn\n", leftovers);
    perfLap("leftover cObjects");
    const os2::PatternCatalog patterns(index, gameDir() / "TSData/Res");
    perfLap("PatternCatalog");
    offerCommunityLotImage(index, lot);
    // The game enters a lot with walls cutaway and the levels above the
    // camera's hidden (--ui live, --mode); a bare --lot view shows every
    // level with walls up and the roof. --walls down|cutaway|up|roof picks
    // the game's wall mode 0-3 (up shows the roof too unless --roof off;
    // roof is mode 3, the only one with the roofs).
    gLevelsHidden = liveUiRequested() || startMode().has_value();
    const std::string walls = argument(@"--walls").value_or(gLevelsHidden ? "cutaway" : "up");
    const bool roofOff = argument(@"--roof").value_or("on") == "off";
    if (walls == "cutaway")
        gWallView.setMode(1);
    else if (walls == "down")
        gWallView.setMode(0);
    else if (walls == "up" || walls == "roof")
        gWallView.setMode(roofOff ? 2 : 3);
    else
        throw std::runtime_error("--walls: expected down, cutaway, up or roof");
    gWallView.study(lot);
    gLotCache = {};
    const os2::LotViewOptions view = gWallView.options(
        {viewDirection.x, viewDirection.y}, gLevelsHidden ? gWallView.shownLevel(0) : os2::LotViewOptions::kAllLevels);
    gLightMapLayout.emplace();
    const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view, &*gLightMapLayout, &gLotCache);
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("buildLotModel");
    renderer.hideTestScene();
    gLotModel = renderer.addObject(model, index);
    gLotInstance = renderer.addInstance(gLotModel, matrix_identity_float4x4);
    gLotShape = std::make_unique<os2::Lot>(lot);
    // Nothing under the lot's earth skirt is drawn (lot_model.hpp).
    renderer.setGroundClip(os2::lotSkirtBottom(lot));
    gLotPatterns = std::make_unique<os2::PatternCatalog>(patterns);
    gLotView = view;
    perfLap("lot model to the GPU");
    for (const auto& w : lot.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    const size_t objects = loadLotObjects(renderer, index, lot, *query);
    perfLap("loadLotObjects");
    const size_t sims = loadLotSims(renderer, index, lot, pkg, lotPath(*query));
    perfLap("loadLotSims");
    NSArray<NSString*>* args = sceneArgs();
    startAudio(index, lot);
    perfLap("startAudio");
    if (argument(@"--run-vm") || argument(@"--pie") || argument(@"--click") || [args containsObject:@"--vm"] ||
        liveUiRequested())
        startSimulation(renderer, index, lot, pkg, lotPath(*query));
    perfLap("startSimulation");
    // Without the VM (no lamps): the light maps of the --lighting state.
    if (!gLighting) {
        const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
        if (const auto* state = lighting.state(argument(@"--lighting").value_or("day"))) {
            const os2::RgbaImage lm = os2::computeLotLightMap(*gLightMapLayout, *state, {}, os2::LightMapGlobals::from(lighting)).image;
            if (const char* d = std::getenv("OS2_LIGHTMAP_DUMP")) // the atlas, as LotLighting writes it
                os2::writePng(d, lm);
            renderer.setLightMap(0, lm);
            renderer.setLightMap(1, lm);
            auto global = [&](const char* name, float fallback) {
                auto it = lighting.globals.find(os2::lowerAscii(name));
                return it == lighting.globals.end() ? fallback : std::stof(it->second);
            };
            gStaticShadowLight = os2::simShadowLight(*state, global("hmSunLightTransitionZone", 1), global("shadowStrengthLimit", 1));
        }
    }
    // Fence rails and posts: one model per CRES, an instance per piece.
    std::vector<std::string> fenceWarnings;
    const size_t fences = placeFencePieces(renderer, index, lot, patterns, &fenceWarnings, view.level);
    for (const auto& w : fenceWarnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("fences");
    std::printf("opensims2: lot %s: %ux%u tiles, levels %d..%d, %zu walls, %zu paints (%zu parts), %zu roofs, "
                "%zu fence pieces, %zu of %zu objects and %zu Sims placed in %.2f s\n",
                query->c_str(), lot.width, lot.height, lot.minLevel, lot.minLevel + lot.levelCount - 1,
                lot.walls.size(), lot.paints.size(), model.parts.size(), lot.roofs.size(), fences, objects,
                lot.objects.size(), sims, CACurrentMediaTime() - start);
    if (gCamera)
        gCameraFollow = std::make_unique<os2::LotCameraFollow>(*gCamera, lot);
    const int cx = int(lot.width / 2), cy = int(lot.height / 2);
    return simd_make_float3(float(lot.width) / 2, float(lot.height) / 2,
                            lot.elevation(os2::LotCameraFollow::startLevel(lot), cx, cy));
}

// Loads the objects into the renderer on a grid, four per row, three tiles
// apart, starting at tile (2, -3). Returns the middle of the grid, for the camera.
simd_float3 loadObjects(os2::Renderer& renderer, os2::ResourceIndex& index) {
    CFTimeInterval start = CACurrentMediaTime();
    int slot = 0;
    for (const auto& query : objectQueries()) {
        try {
            std::string match;
            const auto model = os2::loadObjectModel(index, os2::resolveObjectCres(index, query, &match));
            for (const auto& w : model.warnings)
                std::fprintf(stderr, "opensims2: %s: %s\n", model.name.c_str(), w.c_str());
            const int id = renderer.addObject(model, index);
            simd_float4x4 at = matrix_identity_float4x4;
            at.columns[3] = simd_make_float4(2.5f + 3.0f * float(slot % 4), -2.5f - 3.0f * float(slot / 4), 0, 1);
            ++slot;
            renderer.addInstance(id, at);
            std::printf("opensims2: %s (%zu parts)\n", match.c_str(), model.parts.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: object %s: %s\n", query.c_str(), e.what());
        }
    }
    if (slot)
        std::printf("opensims2: loaded in %.2f s\n", CACurrentMediaTime() - start);
    std::fflush(stdout);
    const int cols = std::clamp(slot, 1, 4), rows = (std::max(slot, 1) + 3) / 4;
    return simd_make_float3(2.5f + 1.5f * float(cols - 1), -2.5f - 1.5f * float(rows - 1), 0);
}

os2::Skeleton loadSkeleton(os2::ResourceIndex& index, const std::string& cres) {
    auto loc = index.findByName(cres + "_cres", kTypeCres);
    if (!loc)
        throw std::runtime_error("skeleton not found: " + cres);
    const auto bytes = index.read(*loc);
    return os2::Skeleton::fromCres(os2::Rcol(bytes));
}

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
        // (the wall view: updateLotView rebuilds the lot model for it)
        if (wholeHouse && gWallView.walls() != os2::LotViewOptions::Walls::Up) {
            gCinematicForced.walls = gWallView.mode();
            gWallView.setMode(3);
        }
        if (wholeHouse && gCameraFollow) {
            gCinematicForced.level = gCameraFollow->level();
            gCameraFollow->setLevel(gCameraFollow->highestLevel());
        }
        (void)renderer;
    }
    if (gSimulation) { // held for the whole scene (the speed buttons wait)
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
    return gWallView.options(look, viewLevel());
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

// Poses every actor at `seconds` and hands the vertices to the renderer.
// The Sims first: objects in their hands follow their bones (gObjectView),
// then the objects' actors at the places that gives.
// OS2_DEBUG_JUMPS=1 (test aid): each Sim's drawn pelvis checked frame to
// frame (app/jump_detector.hpp); jumps over a tile the VM gave no reason
// for, back-and-forth jumps and repeated VM snaps are printed as they
// happen and summed up at exit. OS2_WATCH_OBJECT=<id|name>: where the VM
// has those objects and where they are drawn, as that changes.
// A car, bus or truck: its passengers move with its own animation (test aids below).
bool isVehicle(const os2::vm::Object* o) {
    if (!o || !o->definition)
        return false;
    const std::string& n = o->definition->fileName;
    return n.starts_with("Car ") || n.find("Vehicle") != std::string::npos || n.find("Bus") != std::string::npos;
}

void sampleDrawnPlaces(double scene) {
    if (!gSimulation)
        return;
    static const bool jumps = std::getenv("OS2_DEBUG_JUMPS") != nullptr;
    static const char* watch = std::getenv("OS2_WATCH_OBJECT");
    os2::vm::VM& m = gSimulation->machine();
    // OS2_THEN_AT="<tick>:<object>:<entry>[;<tick>:...]" (test aid, the seat
    // battery): at that VM tick the selected Sim queues the English pie entry
    // of an object found by ID or name (as --vm-queue), or with the object
    // "here" a Go Here to <entry> = x,y[,level] (to get up, change seats,
    // climb out of the pool).
    if (static const char* then = std::getenv("OS2_THEN_AT"); then && gPie) {
        size_t& done = gScene.thenAtDone; // (ticks of this scene's VM)
        std::vector<std::string> items;
        for (std::string rest = then; !rest.empty();) {
            const size_t semi = rest.find(';');
            items.push_back(rest.substr(0, semi));
            rest = semi == std::string::npos ? "" : rest.substr(semi + 1);
        }
        while (done < items.size()) {
            const std::string& it = items[done];
            const size_t c1 = it.find(':'), c2 = c1 == std::string::npos ? c1 : it.find(':', c1 + 1);
            if (c2 == std::string::npos || gSimulation->machine().ticks < std::stoull(it.substr(0, c1)))
                break;
            ++done;
            os2::vm::VM& vm = gSimulation->machine();
            const std::string what = it.substr(c1 + 1, c2 - c1 - 1), entry = it.substr(c2 + 1);
            const os2::vm::ObjectId sim = gPie->sim();
            bool queued = false;
            if (what == "here") {
                float x = 0, y = 0;
                int level = 1;
                std::sscanf(entry.c_str(), "%f,%f,%d", &x, &y, &level);
                const os2::vm::ObjectId dest = sim ? os2::vm::createGoHereDestination(vm, sim, x, y, level) : 0;
                if (dest)
                    for (const auto& e : os2::vm::pieMenu(vm, sim, dest))
                        if (!queued && e.available() && !e.debug)
                            if (auto q = os2::vm::interactionFromPie(vm, e)) {
                                q->pushedBy = sim;
                                os2::vm::enqueue(vm, *vm.object(sim), *q);
                                queued = true;
                            }
                if (dest)
                    os2::vm::releaseDestination(vm, dest);
            } else {
                os2::vm::ObjectId target = 0;
                for (const auto& [id, o] : vm.objects())
                    if (!target && !o.person && o.definition &&
                        (what == std::to_string(id) || strcasestr(o.definition->fileName.c_str(), what.c_str())))
                        target = id;
                os2::vm::PieOptions opt;
                opt.language = os2::lang::EnglishUS;
                if (target && sim)
                    for (const auto& e : os2::vm::pieMenu(vm, sim, target, opt))
                        if (!queued && strcasecmp(e.name.c_str(), entry.c_str()) == 0)
                            if (auto q = os2::vm::interactionFromPie(vm, e)) {
                                os2::vm::addAction(vm, *vm.object(sim), *q);
                                queued = true;
                            }
            }
            std::string offered;
            if (!queued && what != "here")
                for (const auto& [id, o] : vm.objects())
                    if (!o.person && o.definition && strcasestr(o.definition->fileName.c_str(), what.c_str()) && offered.empty()) {
                        os2::vm::PieOptions opt;
                        opt.language = os2::lang::EnglishUS;
                        offered = " (object " + std::to_string(id) + " offers:";
                        for (const auto& e : os2::vm::pieMenu(vm, sim, id, opt))
                            offered += " '" + e.name + "'";
                        offered += ")";
                    }
            std::printf("opensims2: tick %llu: then %s:%s %s%s\n", (unsigned long long)vm.ticks, what.c_str(), entry.c_str(),
                        queued ? "queued" : "not queued", offered.c_str());
        }
    }
    if (jumps) {
        static os2::JumpDetector detector;
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                std::printf("opensims2: jumps: %d drawn jumps without a VM reason (%d back and forth), %d Sims snapped over and over\n",
                            detector.drawnJumps(), detector.backAndForth(), detector.repeatedSnaps());
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, detector.forgetSims();
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person)
                continue;
            os2::JumpDetector::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            const os2::SimActor& a = *s.actor;
            int b = a.skin.skeleton.find("pelvis");
            if (b < 0)
                b = a.skin.skeleton.find("root_trans");
            const os2::Vec3 p = b >= 0 && size_t(b) < a.bones.size() ? a.bones[size_t(b)].t : a.placement().t;
            x.drawn[0] = p[0], x.drawn[1] = p[1], x.drawn[2] = p[2];
            x.vm[0] = o->position.x, x.vm[1] = o->position.y, x.level = o->position.level;
            x.walking = o->route.has_value();
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            x.container = c == s.id ? 0 : c;
            if (const os2::vm::Object* h = x.container ? m.object(x.container) : nullptr)
                x.holder[0] = h->position.x, x.holder[1] = h->position.y;
            x.seat[0] = x.holder[0], x.seat[1] = x.holder[1];
            if (x.container && gObjectView)
                if (const auto at = gObjectView->slotPlacement(s.id, scene))
                    x.seat[0] = at->t[0], x.seat[1] = at->t[1];
            x.inWorld = simInTheWorld(s.id) && !gCarriedSims.count(s.actor) && !isVehicle(m.object(x.container));
            x.animation = o->animation.name;
            // OS2_TRACE_DRAWN=<Sim id>: that Sim's drawn pelvis and VM place each frame.
            static const int traced = std::getenv("OS2_TRACE_DRAWN") ? std::atoi(std::getenv("OS2_TRACE_DRAWN")) : 0;
            if (traced == s.id)
                std::printf("opensims2: drawn: tick %llu %.3f s Sim %d pelvis (%.2f %.2f %.2f) placement (%.2f %.2f %.2f) vm (%.2f %.2f L%d dir %d)%s in %d '%s'\n",
                            (unsigned long long)m.ticks, scene, s.id, p[0], p[1], p[2], a.position[0], a.position[1], a.position[2],
                            o->position.x, o->position.y, int(o->position.level), o->data[os2::vm::objdata::Direction],
                            o->route ? " walking" : "", x.container, x.animation.c_str());
            for (const auto& e : detector.add(x))
                std::printf("opensims2: %s\n", os2::JumpDetector::describe(e).c_str());
        }
    }
    // OS2_DEBUG_GLIDE=1 (test aid, app/glide_detector.hpp): Sims whose drawn
    // pelvis moves on while their legs hold still (gliding with no walk),
    // printed as each episode starts with what the Sim was doing (route,
    // locomotion clip, Animate Sim, overlays, the layers that play, the
    // interaction and the tree), and counted at exit by what played.
    static const bool glide = std::getenv("OS2_DEBUG_GLIDE") != nullptr;
    if (glide) {
        static os2::GlideDetector detector;
        static std::map<std::string, int> byKind;
        static bool registered = false;
        if (!registered) {
            registered = true;
            if (const char* r = std::getenv("OS2_GLIDE_LEGS")) // the feet's range threshold (tiles)
                detector.legRange = float(std::atof(r));
            std::atexit([] {
                std::printf("opensims2: glides: %d episodes of Sims gliding without moving their legs, %.1f s in all\n",
                            detector.episodes(), detector.seconds());
                for (const auto& [k, n] : byKind)
                    std::printf("opensims2: glides:   %3d  %s\n", n, k.c_str());
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, detector.forgetSims();
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person)
                continue;
            os2::SimActor& a = *s.actor;
            const os2::Skeleton& sk = a.skin.skeleton;
            const int pb = sk.find("pelvis"), lb = sk.find("l_foot"), rb = sk.find("r_foot");
            if (pb < 0 || lb < 0 || rb < 0 || a.bones.size() != sk.bones.size())
                continue;
            os2::GlideDetector::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            const os2::Vec3 p = a.bones[size_t(pb)].t;
            x.pelvis[0] = p[0], x.pelvis[1] = p[1];
            const os2::Xform toModel = a.placement().inverse();
            const os2::Vec3 pm = toModel.apply(p), lf = toModel.apply(a.bones[size_t(lb)].t),
                            rf = toModel.apply(a.bones[size_t(rb)].t);
            for (int i = 0; i < 3; ++i)
                x.lfoot[i] = lf[i] - pm[i], x.rfoot[i] = rf[i] - pm[i];
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            x.inWorld = simInTheWorld(s.id) && !gCarriedSims.count(s.actor) && (c == 0 || c == s.id);
            for (const auto& e : detector.add(x)) {
                std::string overlays;
                for (const auto& ov : o->animation.overlays)
                    overlays += (overlays.empty() ? "" : ", ") + ov.name + " L" + std::to_string(ov.level);
                std::string route = "no route";
                std::string kind = "no route";
                if (o->route) {
                    const auto& L = o->route->loco;
                    static const char* kinds[] = {"none", "start", "walk", "stop"};
                    char buf[300];
                    std::snprintf(buf, sizeof buf, "route%s%s loco %s%s '%s' climb '%s' points %zu tree '%s'",
                                  o->route->waiting ? " waiting" : "", o->route->inPortal ? " in portal" : "",
                                  kinds[std::min<int>(L.kind, 3)], L.moving ? " moving" : "", L.animation.c_str(),
                                  o->route->climb.animation.c_str(), o->route->points.size(),
                                  o->route->tree ? o->route->tree->fileName.c_str() : "");
                    route = buf;
                    kind = std::string("route, loco ") + kinds[std::min<int>(L.kind, 3)] +
                           (L.moving ? " moving" : "") + (o->route->waiting ? " waiting" : "") +
                           (L.animation.empty() ? ", no clip" : "");
                }
                if (!o->animation.name.empty())
                    kind += ", Animate Sim " + o->animation.name;
                const auto th = m.threads.find(s.id);
                std::string tree = th != m.threads.end() && !th->second.stack.empty() && th->second.stack.back().behavior
                                       ? th->second.stack.back().behavior->fileName
                                       : "";
                std::string interaction;
                if (o->activeInteraction >= 0 && size_t(o->activeInteraction) < o->queue.size())
                    interaction = o->queue[size_t(o->activeInteraction)].name;
                std::printf("opensims2: glide: tick %llu %.2f s Sim %d (%s, age %d) moved %.2f tiles in %.1f s, feet range %.3f at "
                            "(%.2f %.2f); %s; Animate Sim '%s' L%d; overlays [%s]; interaction '%s'; tree '%s'; layers %s\n",
                            (unsigned long long)e.tick, e.time, e.id, s.character.filename().string().c_str(),
                            o->person->data[os2::vm::persondata::PersonAge], e.distance, detector.window, e.legs, x.pelvis[0],
                            x.pelvis[1], route.c_str(), o->animation.name.c_str(), o->animation.level, overlays.c_str(),
                            interaction.c_str(), tree.c_str(), a.player.describe(scene).c_str());
                ++byKind[kind];
            }
        }
    }
    // OS2_DEBUG_POSES=1 (test aid): poses that look broken, as they start
    // and with a count at exit: a seated Sim's pelvis far from its seat
    // bone (out of a get-in or get-out), a hand far behind the shoulders.
    static const bool poses = std::getenv("OS2_DEBUG_POSES") != nullptr;
    if (poses && gObjectView) {
        struct Count {
            int seat = 0, behind = 0, frames = 0;
            double seatMax = 0, behindMax = 0;
        };
        static std::map<std::string, Count> counts;
        auto& flagged = gScene.posesFlagged; // (Sim, kind) currently flagged (counts: the whole run, at exit)
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                int seat = 0, behind = 0;
                for (const auto& [k, c] : counts)
                    seat += c.seat, behind += c.behind;
                std::printf("opensims2: poses: %d seated frames off the seat, %d frames with a hand behind the back\n", seat, behind);
                for (const auto& [k, c] : counts)
                    if (c.seat || c.behind)
                        std::printf("opensims2: poses:   %-50s seat %d (max %.2f) behind %d (max %.2f) of %d frames\n", k.c_str(),
                                    c.seat, c.seatMax, c.behind, c.behindMax, c.frames);
                std::fflush(stdout);
            });
        }
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person || !simInTheWorld(s.id) || gCarriedSims.count(s.actor))
                continue;
            const os2::SimActor& a = *s.actor;
            auto bone = [&](const char* n) -> std::optional<os2::Vec3> {
                const int b = a.skin.skeleton.find(n);
                if (b < 0 || size_t(b) >= a.bones.size())
                    return std::nullopt;
                return a.bones[size_t(b)].t;
            };
            const auto pelvis = bone("pelvis"), rt = bone("r_thigh"), lt = bone("l_thigh");
            const auto rc = bone("r_clavicle"), lc = bone("l_clavicle"), rh = bone("r_hand"), lh = bone("l_hand");
            if (!pelvis || !rt || !lt || !rc || !lc || !rh || !lh)
                continue;
            // What it does, for the counts: Animate Sim's, else the carry/overlays.
            std::string what = o->animation.name;
            if (what.empty())
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("blink") == std::string::npos && ov.name.find("idle") == std::string::npos)
                        what = ov.name;
            if (what.empty())
                what = o->route ? "(walking)" : "(idle)";
            Count& c = counts[what];
            ++c.frames;
            auto flag = [&](const char* kind, bool on, double value) {
                bool& f = flagged[{s.id, kind}];
                if (on && !f)
                {
                    const os2::vm::Object* in = m.object(o->data[os2::vm::objdata::ContainerId]);
                    std::printf("opensims2: pose: tick %llu Sim %d %s %.2f in '%s' at (%.2f %.2f) container %d %s\n",
                                (unsigned long long)m.ticks, s.id, kind, value, what.c_str(), (*pelvis)[0], (*pelvis)[1],
                                o->data[os2::vm::objdata::ContainerId],
                                in && in != o && in->definition ? in->definition->fileName.c_str() : "");
                }
                f = on;
            };
            // Seated: pelvis over the seat (out of the get-ins and get-outs).
            const int16_t c0 = o->data[os2::vm::objdata::ContainerId];
            const os2::vm::Object* co = c0 && c0 != s.id ? m.object(c0) : nullptr;
            const bool moving = o->animation.name.find("getIn") != std::string::npos ||
                                o->animation.name.find("getOut") != std::string::npos ||
                                o->animation.name.find("GetIn") != std::string::npos ||
                                o->animation.name.find("GetOut") != std::string::npos ||
                                o->animation.name.find("-start") != std::string::npos || // (a piano's sit-down)
                                o->animation.name.find("-stop") != std::string::npos;
            // (Not on a stair's steps or in a car: their own motion.)
            const bool ownMotion = co && co->definition && (co->definition->fileName.find("Stair") != std::string::npos ||
                                                            isVehicle(co));
            if (co && !co->person && !moving && !ownMotion)
                if (const auto seatAt = gObjectView->slotPlacement(s.id, scene)) {
                    const double d = std::hypot((*pelvis)[0] - seatAt->t[0], (*pelvis)[1] - seatAt->t[1]);
                    c.seatMax = std::max(c.seatMax, d);
                    c.seat += d > 0.35;
                    flag("pelvis off the seat", d > 0.35, d);
                    // And the seat itself near its object (a bone in the wrong frame).
                    if (co->position.inWorld()) {
                        const double far = std::hypot((*pelvis)[0] - co->position.x, (*pelvis)[1] - co->position.y);
                        c.seat += far > 1.2;
                        flag("pelvis far from its seat's object", far > 1.2, far);
                    }
                }
            // A carry pose (an overlay or Animate Sim "...-carry"/"grasp") with
            // nothing in the hands (no object in the Sim's slots).
            {
                std::string carry;
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("carry") != std::string::npos || ov.name.find("grasp") != std::string::npos)
                        carry = ov.name;
                bool holds = false;
                if (!carry.empty())
                    for (const auto& [oid, ob] : m.objects())
                        if (oid != s.id && ob.data[os2::vm::objdata::ContainerId] == s.id)
                            holds = true;
                const bool bad = !carry.empty() && !holds;
                if (bad)
                    ++counts["carry " + carry].seat;
                flag("carry pose with empty hands", bad, 0);
            }
            // A hand behind the shoulders' plane (the body's forward from the hips).
            const double lx = (*lt)[0] - (*rt)[0], ly = (*lt)[1] - (*rt)[1];
            const double ln = std::hypot(lx, ly);
            if (ln > 1e-3) {
                const double fx = ly / ln, fy = -lx / ln; // up x left: forward (the sink's scrub has the hands ahead)
                auto behind = [&](const os2::Vec3& h, const os2::Vec3& sh) { return -((h[0] - sh[0]) * fx + (h[1] - sh[1]) * fy); };
                const double b = std::max(behind(*rh, *rc), behind(*lh, *lc));
                c.behindMax = std::max(c.behindMax, b);
                const bool bad = b > 0.3 && !o->route; // (a walk swings the arms back 0.25)
                c.behind += bad;
                flag("hand behind the back", bad, b);
            }
        }
    }
    // OS2_DEBUG_SEATS=1 (test aid, app/seat_check.hpp; tests/seat_battery.sh):
    // each Sim's drawn body against its seat, its IK goals, the tables and
    // chairs around it, and its carry; problems printed as they start and
    // summed up at exit.
    static const bool seats = std::getenv("OS2_DEBUG_SEATS") != nullptr;
    if (seats && gObjectView) {
        static os2::SeatCheck check;
        static bool registered = false;
        if (!registered) {
            registered = true;
            std::atexit([] {
                std::printf("opensims2: seats: %d problems: %s\n", check.episodes(), check.summary().c_str());
                for (const auto& [k, c] : check.counts())
                    if (c.episodes)
                        std::printf("opensims2: seats:   worst %s\n", c.example.c_str());
                for (const auto& [name, r] : check.seats())
                    std::printf("opensims2: seats:   pelvis in the slot of %s: x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f (%d frames)\n",
                                name.c_str(), r.lo[0], r.hi[0], r.lo[1], r.hi[1], r.lo[2], r.hi[2], r.frames);
                std::fflush(stdout);
            });
        }
        static uint64_t generation = gSceneGeneration; // (a new scene: forget the last one's Sims)
        if (generation != gSceneGeneration)
            generation = gSceneGeneration, check.forgetSims();
        // The tables, counters and seats (their models' boxes where they are drawn now).
        std::vector<os2::SeatCheck::Box> boxes;
        for (const auto& b : gPickBoxes) {
            const int16_t id = int16_t(b.instance & 0xFFFF);
            const os2::vm::Object* ob = m.object(id);
            if (!ob || ob->person || !ob->definition)
                continue;
            const std::string& n = ob->definition->fileName;
            const bool table = n.find("Table") != std::string::npos || n.find("Counter") != std::string::npos;
            const bool seat = n.find("Chair") != std::string::npos || n.find("Stool") != std::string::npos ||
                              n.find("Sofa") != std::string::npos || n.find("Loveseat") != std::string::npos ||
                              n.find("Bench") != std::string::npos;
            if (!table && !seat)
                continue;
            const auto at = gObjectView->placement(id);
            if (!at || !gObjectView->shown(id))
                continue;
            os2::SeatCheck::Box x;
            x.id = id, x.name = n, x.at = *at, x.table = table && !seat;
            x.lo = {b.lo.x, b.lo.y, b.lo.z}, x.hi = {b.hi.x, b.hi.y, b.hi.z};
            boxes.push_back(std::move(x));
        }
        // OS2_DEBUG_SEATS=<Sim id> (over 1): that Sim only.
        static const int only = std::atoi(std::getenv("OS2_DEBUG_SEATS"));
        for (const auto& s : gLotSims) {
            const os2::vm::Object* o = m.object(s.id);
            if (!o || !o->person || !simInTheWorld(s.id) || gCarriedSims.count(s.actor) || (only > 1 && s.id != only))
                continue;
            const os2::SimActor& a = *s.actor;
            auto bone = [&](const char* n) -> std::optional<os2::Vec3> {
                const int b = a.skin.skeleton.find(n);
                if (b < 0 || size_t(b) >= a.bones.size())
                    return std::nullopt;
                return a.bones[size_t(b)].t;
            };
            const auto pelvis = bone("pelvis");
            if (!pelvis)
                continue;
            os2::SeatCheck::Sample x;
            x.id = s.id, x.time = scene, x.tick = m.ticks;
            x.pelvis = *pelvis;
            for (const char* n : {"spine1", "l_calf", "r_calf"})
                if (const auto p = bone(n))
                    x.body.push_back({n, *p});
            for (const char* n : {"l_foot", "r_foot"})
                if (const auto p = bone(n))
                    x.feet.push_back(*p);
            x.what = o->animation.name;
            if (x.what.empty())
                for (const auto& ov : o->animation.overlays)
                    if (ov.name.find("blink") == std::string::npos)
                        x.what = ov.name;
            x.walking = o->route.has_value();
            const std::string& an = o->animation.name;
            for (const char* k : {"getIn", "getOut", "GetIn", "GetOut", "-start", "-stop", "-in-", "-out-", "standUp", "sitDown", "Transition", "trans-"})
                x.transitional = x.transitional || an.find(k) != std::string::npos;
            const int16_t c = o->data[os2::vm::objdata::ContainerId];
            const os2::vm::Object* co = c && c != s.id ? m.object(c) : nullptr;
            if (co && !co->person && co->definition && co->definition->fileName.find("Stair") == std::string::npos && !isVehicle(co))
                if (const auto at = gObjectView->slotPlacement(s.id, scene)) {
                    x.container = c, x.containerName = co->definition->fileName, x.seat = *at;
                    x.lying = x.containerName.find("Tub") != std::string::npos || x.containerName.find("Bed") != std::string::npos;
                    // (Some trees walk a contained Sim: the bed's, the bath's.)
                }
            // (The hands: the standing idle's foot goals under a dance are not a seat's business.)
            for (const auto& k : a.player.lastIkMisses())
                if (k.bone.find("hand") != std::string::npos && k.distance > x.ikMiss)
                    x.ikMiss = k.distance, x.ikChain = k.bone + " of " + k.animation;
            for (const auto& ov : o->animation.overlays)
                if (ov.name.find("carry") != std::string::npos || ov.name.find("grasp") != std::string::npos) {
                    bool holds = false;
                    for (const auto& [oid, ob] : m.objects())
                        holds = holds || (oid != s.id && ob.data[os2::vm::objdata::ContainerId] == s.id);
                    x.carryEmpty = !holds;
                }
            // (The other parts of a multi-tile seat are the same seat.)
            const int16_t seatGroup = co ? int16_t(os2::vm::multiTileGroup(*co)) : 0;
            std::vector<os2::SeatCheck::Box> near;
            for (const auto& b : boxes)
                if (const os2::vm::Object* bo = m.object(b.id); seatGroup && bo && int16_t(os2::vm::multiTileGroup(*bo)) == seatGroup)
                    continue;
                else if (std::hypot(b.at.t[0] - x.pelvis[0], b.at.t[1] - x.pelvis[1]) < 3 && std::fabs(b.at.t[2] - a.position[2]) < 1.5f)
                    near.push_back(b);
            for (const auto& e : check.add(x, near))
                std::printf("opensims2: seat check: %s\n", e.c_str());
        }
    }
    // OS2_WATCH_BONE=<object id>:<bone>: that object's actor bone (world) each frame.
    if (const char* wb = std::getenv("OS2_WATCH_BONE")) {
        const std::string spec = wb;
        const int oid = std::atoi(spec.c_str());
        const std::string bone = spec.substr(spec.find(':') + 1);
        for (auto& [id, a] : gObjectActors)
            if (id == oid && a) {
                const int b = a->skin.skeleton.find(bone);
                if (b >= 0 && size_t(b) < a->bones.size())
                    std::printf("opensims2: bone: tick %llu %.3f %d %s (%.3f %.3f %.3f) transform %s\n", (unsigned long long)m.ticks,
                                scene, oid, bone.c_str(), a->bones[size_t(b)].t[0], a->bones[size_t(b)].t[1],
                                a->bones[size_t(b)].t[2], a->transform ? "set" : "unset");
            }
    }
    if (watch && gObjectView) {
        auto& last = gScene.watchedObjects; // (by object ID: per scene)
        for (const auto& [id, o] : m.objects()) {
            if (o.person || !o.definition ||
                (std::to_string(id) != watch && o.definition->fileName.find(watch) == std::string::npos))
                continue;
            const auto at = gObjectView->placement(int16_t(id));
            char buf[400];
            std::snprintf(buf, sizeof buf, "container %d slot %d at %.1f %.1f level %d; drawn %s (%.2f %.2f %.2f)",
                          o.data[os2::vm::objdata::ContainerId], o.data[os2::vm::objdata::SlotNumber], o.position.x,
                          o.position.y, o.position.level, at ? "at" : "nowhere", at ? at->t[0] : 0, at ? at->t[1] : 0,
                          at ? at->t[2] : 0);
            std::string s = buf;
            const bool moved = last[id].substr(0, last[id].find(';')) != s.substr(0, s.find(';'));
            if (moved)
                std::printf("opensims2: tick %llu: object %d (%s) %s\n", (unsigned long long)m.ticks, id,
                            o.definition->fileName.c_str(), s.c_str());
            last[id] = s;
        }
    }
}

// OS2_POSE_SHOT=<stem> (test aid): with --screenshot, each Sim (or the
// --character / --sim actor) also drawn alone from its front and its left
// side at eye level, as <stem>-<id>.png, with its posture measured: the
// lean of the pelvis-to-head line from upright (+ forward), the head's
// pitch (+ up) and each arm's angle out from hanging straight down.
void writePoseSheets(os2::Renderer& renderer, const std::string& stem) {
    std::vector<std::pair<int, os2::SimActor*>> sims;
    for (const auto& s : gLotSims)
        sims.push_back({s.id, s.actor});
    if (sims.empty())
        for (auto& a : gActors)
            if (a && a->skin.skeleton.find("pelvis") >= 0)
                sims.push_back({int(sims.size()), a.get()});
    for (auto [id, actor] : sims) {
        const os2::Skeleton& sk = actor->skin.skeleton;
        if (actor->bones.size() != sk.bones.size() || actor->model < 0)
            continue;
        const os2::Xform toModel = actor->placement().inverse();
        auto at = [&](const char* n) -> std::optional<os2::Vec3> {
            const int b = sk.find(n);
            return b < 0 ? std::nullopt : std::optional(toModel.apply(actor->bones[size_t(b)].t));
        };
        constexpr float kDeg = 180.0f / float(M_PI);
        std::string metrics;
        const auto pelvis = at("pelvis"), head = at("head");
        char buf[160];
        if (pelvis && head) {
            const float dy = (*head)[1] - (*pelvis)[1], dz = (*head)[2] - (*pelvis)[2], dx = (*head)[0] - (*pelvis)[0];
            std::snprintf(buf, sizeof buf, "lean %+.0f fwd %+.0f side", std::atan2(dy, dz) * kDeg, std::atan2(-dx, dz) * kDeg);
            metrics += buf;
        }
        if (const auto lf = at("l_foot"), rf = at("r_foot"); lf && rf && head) { // the whole body's tilt (feet to head)
            const float dx = (*head)[0] - ((*lf)[0] + (*rf)[0]) / 2, dy = (*head)[1] - ((*lf)[1] + (*rf)[1]) / 2,
                        dz = (*head)[2] - ((*lf)[2] + (*rf)[2]) / 2;
            std::snprintf(buf, sizeof buf, ", tilt %+.0f fwd %+.0f side", std::atan2(dy, dz) * kDeg, std::atan2(-dx, dz) * kDeg);
            metrics += buf;
        }
        if (const int hb = sk.find("head"); hb >= 0) {
            const auto rest = sk.world(sk.restPose());
            const os2::Vec3 fwd = os2::Xform{conjugate(rest[size_t(hb)].q), {0, 0, 0}}.rotate({0, 1, 0});
            const os2::Vec3 face = toModel.rotate(actor->bones[size_t(hb)].rotate(fwd));
            std::snprintf(buf, sizeof buf, ", head pitch %+.0f", std::asin(std::clamp(face[2], -1.0f, 1.0f)) * kDeg);
            metrics += buf;
        }
        for (const char* side : {"l", "r"}) {
            const auto sh = at((std::string(side) + "_upperarm").c_str()), hand = at((std::string(side) + "_hand").c_str());
            if (!sh || !hand)
                continue;
            const os2::Vec3 d{(*hand)[0] - (*sh)[0], (*hand)[1] - (*sh)[1], (*hand)[2] - (*sh)[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            std::snprintf(buf, sizeof buf, ", %s arm %.0f (fwd %+.2f out %+.2f)", side, std::acos(std::clamp(-d[2] / len, -1.0f, 1.0f)) * kDeg,
                          d[1], side[0] == 'l' ? d[0] : -d[0]);
            metrics += buf;
        }
        for (const char* side : {"l", "r"}) { // each knee's bend (0: straight)
            const auto th = at((std::string(side) + "_thigh").c_str()), ca = at((std::string(side) + "_calf").c_str()),
                       ft = at((std::string(side) + "_foot").c_str());
            if (!th || !ca || !ft)
                continue;
            const os2::Vec3 u{(*ca)[0] - (*th)[0], (*ca)[1] - (*th)[1], (*ca)[2] - (*th)[2]};
            const os2::Vec3 v{(*ft)[0] - (*ca)[0], (*ft)[1] - (*ca)[1], (*ft)[2] - (*ca)[2]};
            const float c = (u[0] * v[0] + u[1] * v[1] + u[2] * v[2]) /
                            std::max(1e-6f, std::sqrt((u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) * (v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
            std::snprintf(buf, sizeof buf, ", %s knee %.0f", side, std::acos(std::clamp(c, -1.0f, 1.0f)) * kDeg);
            metrics += buf;
        }
        std::string playing;
        if (gSimulation)
            if (const os2::vm::Object* o = gSimulation->machine().object(int16_t(id))) {
                playing = o->animation.name.empty() ? (o->route ? "(walking)" : "(idle)") : o->animation.name;
                for (const auto& ov : o->animation.overlays)
                    playing += " + " + ov.name;
                if (const auto target = gSimulation->lookTarget(*o)) {
                    char look[96];
                    std::snprintf(look, sizeof look, "; looks at %d (%.1f %.1f %.2f)", o->lookAt->target, (*target)[0], (*target)[1],
                                  (*target)[2]);
                    playing += look;
                }
            }
        std::printf("opensims2: pose shot: %s-%d: Sim %d %s; %s; layers %s\n", fs::path(stem).filename().c_str(), id, id, metrics.c_str(), playing.c_str(),
                    actor->player.describe(gSceneNow).c_str());
        // The Sim alone, far from the lot: its instance moved there for the shot.
        const int inst = renderer.instanceOf(actor->model);
        if (inst < 0)
            continue;
        const simd_float4x4 was = renderer.instanceTransform(inst);
        const simd_float3 away = simd_make_float3(-5000, -5000, 0);
        const simd_float3 p = simd_make_float3(actor->position[0], actor->position[1], actor->position[2]);
        simd_float4x4 moved = matrix_identity_float4x4;
        moved.columns[3] = simd_make_float4(away - p, 1);
        renderer.setInstanceTransform(inst, moved);
        const uint32_t w = 320, h = 560;
        os2::RgbaImage sheet;
        sheet.width = w * 2, sheet.height = h;
        sheet.pixels.assign(size_t(sheet.width) * h * 4, 0);
        const float heading = actor->transform ? 0.0f : actor->heading;
        for (int view = 0; view < 2; ++view) {
            // The Sim faces +Y turned by its heading; front, then its left side.
            const float a = heading + (view ? float(M_PI) / 2 : 0);
            const simd_float3 dir = simd_make_float3(-std::sin(a), std::cos(a), 0);
            const simd_float3 target = away + simd_make_float3(0, 0, 0.85f);
            const float fov = 30.0f * float(M_PI) / 180.0f, dist = 4.6f;
            const simd_float3 e = target + dir * dist;
            const simd_float3 f = simd_normalize(target - e);
            const simd_float3 sv = simd_normalize(simd_cross(f, simd_make_float3(0, 0, 1)));
            const simd_float3 u = simd_cross(sv, f);
            os2::FrameView fv;
            fv.view = simd_matrix(simd_make_float4(sv.x, u.x, -f.x, 0), simd_make_float4(sv.y, u.y, -f.y, 0),
                                  simd_make_float4(sv.z, u.z, -f.z, 0),
                                  simd_make_float4(-simd_dot(sv, e), -simd_dot(u, e), simd_dot(f, e), 1));
            const float n = 1, fa = 20, ys = 1.0f / std::tan(fov * 0.5f), zs = fa / (n - fa);
            fv.projection = simd_matrix(simd_make_float4(ys * float(h) / float(w), 0, 0, 0), simd_make_float4(0, ys, 0, 0),
                                        simd_make_float4(0, 0, zs, -1), simd_make_float4(0, 0, zs * n, 0));
            const os2::Renderer::Overlay overlay = renderer.overlay();
            renderer.setOverlay({});
            const os2::RgbaImage img = renderer.capture(w, h, fv, simd_make_float4(0.55f, 0.62f, 0.68f, 1));
            renderer.setOverlay(overlay);
            for (uint32_t y = 0; y < h && img.pixels.size() >= size_t(w) * h * 4; ++y)
                std::memcpy(&sheet.pixels[(size_t(y) * sheet.width + view * w) * 4], &img.pixels[size_t(y) * w * 4], w * 4);
        }
        renderer.setInstanceTransform(inst, was);
        os2::writePng(stem + "-" + std::to_string(id) + ".png", sheet);
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

// A Sim's actor from its character file, standing at `position` facing
// `heading`, its idle started `phase` seconds early; added to gLotSims.
bool spawnSimActor(os2::Renderer& renderer, os2::ResourceIndex& index, const fs::path& file, int16_t id,
                   simd_float3 position, float heading, double phase, const std::string& label) {
    const std::string idle = argument(@"--sim-anim").value_or("a-idle-neutral-armsSide-wtC-breatheLoop");
    const std::string stem = file.stem().string();
    try {
        perfLap("  loadSimParts");
        auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, simParts(index), file));
        perfLap("  loadCharacterSim");
        for (const auto& w : actor->skin.model.warnings)
            std::fprintf(stderr, "opensims2: %s: %s\n", stem.c_str(), w.c_str());
        actor->position = {position.x, position.y, position.z};
        actor->heading = heading;
        // Start the idles at different moments so the Sims do not breathe in step.
        // Children and toddlers have their own skeletons and idles.
        std::string anim = idle;
        if (!argument(@"--sim-anim")) {
            const std::string sk = actor->skin.skeleton.name;
            if (sk.size() > 2 && std::tolower(static_cast<unsigned char>(sk[0])) == 'c')
                anim = "c-idle-neutral-armsSide-wtC-breatheLoop50";
            else if (sk.size() > 2 && std::tolower(static_cast<unsigned char>(sk[0])) == 'p')
                anim = "p-idle-neutral-lookAround";
        }
        actor->cue(-phase, actor->animation(index, anim));
        perfLap("  Sim animation");
        actor->model = renderer.addObject(actor->skin.model, index, true);
        renderer.addInstance(actor->model, matrix_identity_float4x4);
        perfLap("  Sim to the GPU");
        std::printf("opensims2: Sim %s (%s) at (%.2f, %.2f, %.2f), %zu parts\n", stem.c_str(), label.c_str(), position.x,
                    position.y, position.z, actor->skin.model.parts.size());
        gLotSims.push_back({actor.get(), id, file, anim});
        gActors.push_back(std::move(actor));
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: Sim %s: %s\n", label.c_str(), e.what());
        return false;
    }
}

// The VM runs the lot (--vm, --pie, --click, the live UI): the arriving
// Sims of a family moving in come from its "reset lot" broadcast
// (LotSimulation), not from formats/lot_arrivals.
bool lotVmWillRun() {
    return argument(@"--run-vm") || argument(@"--pie") || argument(@"--click") ||
           [sceneArgs() containsObject:@"--vm"] || liveUiRequested();
}

size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile) {
    os2::LotObjects saved = os2::LotObjects::load(pkg);
    // A family that just moved in: its Sims arrive (formats/lot_arrivals.hpp)
    // when no VM runs; with the VM the game's tree brings them.
    const os2::LotArrivals arrivals = os2::addArrivingSims(saved, lot, lotFile, &index, !lotVmWillRun());
    std::set<uint32_t> inWorld; // object IDs of the Sims standing on the lot
    for (const auto& st : saved.objects)
        if (st.person && st.x >= 0)
            inWorld.insert(uint32_t(uint16_t(st.id)));
    size_t placed = 0;
    std::vector<os2::Lot::Object> people = lot.objects;
    people.insert(people.end(), arrivals.sims.begin(), arrivals.sims.end());
    for (const auto& o : people) {
        if (!o.person || !inWorld.count(o.instance & 0xFFFF))
            continue;
        const std::string stem = o.definition.substr(0, o.definition.find(" - "));
        const fs::path file = lotFile.parent_path().parent_path() / "Characters" / (stem + ".package");
        if (spawnSimActor(renderer, index, file, int16_t(o.instance & 0xFFFF),
                          simd_make_float3(o.position[0], o.position[1], o.position[2]),
                          2 * std::atan2(o.rotation[2], o.rotation[3]), 0.37 * double(placed), o.definition))
            ++placed;
    }
    return placed;
}

// --sim [body,hair,tone]: an adult Sim assembled from catalog parts; or
// --skinned: the listed CRES on the adult skeleton. Placed at tile (2.5, -2.5).
std::optional<simd_float3> loadSkinned(os2::Renderer& renderer, os2::ResourceIndex& index) {
    const auto list = argument(@"--skinned");
    const auto sim = argument(@"--sim");
    const auto character = argument(@"--character");
    if (!list && !sim && !character)
        return std::nullopt;
    os2::SkinnedModel skin;
    if (character) {
        skin = os2::loadCharacterSim(index, simParts(index), characterPath(*character));
    } else if (sim) {
        os2::SimLook look;
        std::vector<std::string> fields;
        for (size_t start = 0; start <= sim->size();) {
            size_t end = std::min(sim->find(',', start), sim->size());
            fields.push_back(sim->substr(start, end - start));
            start = end + 1;
        }
        if (fields.size() > 0 && !fields[0].empty() && fields[0] != "default")
            look.body = fields[0];
        if (fields.size() > 1 && !fields[1].empty())
            look.hair = fields[1];
        if (fields.size() > 2 && !fields[2].empty())
            look.skinTone = std::stoi(fields[2]);
        look.prefix = look.body.substr(0, 2);
        skin = os2::loadSim(index, simParts(index), look);
    } else {
        skin = os2::SkinnedModel(loadSkeleton(index, "auskel"));
    }
    auto actor = std::make_unique<os2::SimActor>(std::move(skin));
    for (size_t start = 0; list && start <= list->size();) {
        size_t end = list->find(',', start);
        if (end == std::string::npos)
            end = list->size();
        const std::string cres = list->substr(start, end - start);
        start = end + 1;
        if (cres.empty())
            continue;
        actor->skin.add(os2::loadObjectModel(index, cres));
    }
    for (const auto& w : actor->skin.model.warnings)
        std::fprintf(stderr, "opensims2: %s\n", w.c_str());
    actor->position = {2.5f, -2.5f, 0};
    std::string playing;
    if (auto h = argument(@"--heading")) // degrees about Z
        actor->heading = std::stof(*h) * 3.14159265f / 180;
    // The idle and walk of the skeleton's age (child cuskel, toddler puskel,
    // baby buskel; auskel for the rest); --walk-anim names another walk.
    const char age = char(std::tolower(static_cast<unsigned char>(actor->skin.skeleton.name.empty() ? 'a' : actor->skin.skeleton.name[0])));
    const char* ageIdle = age == 'c'   ? "c-idle-neutral-armsSide-wtC-breatheLoop50"
                          : age == 'p' ? "p-idle-standing-loopBreathe"
                          : age == 'b' ? "b-idle-carryBaby-breathe"
                                       : "a-idle-neutral-armsSide-wtC-breatheLoop";
    const char* ageWalk = age == 'c' ? "c-walk-alt0-normal" : age == 'p' ? "p-walk" : "a-male-walk-alt0-normal";
    if (auto walk = argument(@"--walk")) {
        // Idle, then walk straight ahead for <seconds>, then idle again;
        // the walk's locomotion velocity moves the Sim.
        const double seconds = std::stod(*walk);
        const auto& idle = actor->animation(index, argument(@"--anim").value_or(ageIdle));
        const auto& walkAnim = actor->animation(index, argument(@"--walk-anim").value_or(ageWalk));
        actor->cue(0, idle);
        actor->cue(0.5, walkAnim, {.fade = 0.3f});
        actor->cue(0.5 + seconds, idle, {.fade = 0.3f});
        actor->rootMotion = true;
        playing = "idle, walk, idle";
    } else if (auto name = argument(@"--anim"); name || character || sim) {
        actor->cue(0, actor->animation(index, name.value_or(ageIdle)));
        playing = actor->animations.back()->resource.fileName;
    }
    // --overlay <anim>[@bone]: a second animation over the bones under <bone>
    // (default spine0), with its own priority.
    if (auto overlay = argument(@"--overlay")) {
        std::string name = *overlay, root = "spine0";
        if (auto at = name.find('@'); at != std::string::npos)
            root = name.substr(at + 1), name.resize(at);
        const auto& anim = actor->animation(index, name);
        actor->cue(0, anim, {.fade = 0, .mask = os2::boneMask(actor->skin.skeleton, root), .priority = 100});
        playing += " + " + anim.resource.fileName + "@" + root;
    }
    actor->model = renderer.addObject(actor->skin.model, index, true);
    renderer.addInstance(actor->model, matrix_identity_float4x4); // placement is in the vertices
    std::printf("opensims2: skinned %s (%zu parts, %zu bones)%s%s\n", actor->skin.model.name.c_str(), actor->skin.model.parts.size(),
                actor->skin.skeleton.bones.size(), playing.empty() ? "" : ", playing ", playing.c_str());
    gActors.push_back(std::move(actor));
    return simd_make_float3(2.5f, -2.5f, 0.9f);
}

std::unique_ptr<os2::UiLayer>& gUi = gScene.ui;

// --ui <instance-hex>: lays out that UI script and hands it to the renderer as
// an overlay. Returns the canvas size, for the screenshot.
std::optional<std::pair<uint32_t, uint32_t>> loadUi(os2::Renderer& renderer, os2::ResourceIndex& index, MTKView* view) {
    const auto id = argument(@"--ui");
    if (!id || *id == "live")
        return std::nullopt;
    const fs::path res = gameDir() / "TSData/Res";
    os2::UiResources ui(index, res);
    auto script = ui.script(uint32_t(std::stoul(*id, nullptr, 16)));
    if (!script)
        throw std::runtime_error("no UI script " + *id);
    os2::UiLayoutOptions opt;
    if (auto show = argument(@"--ui-show"))
        for (const auto& w : os2::uiList("(" + *show + ")"))
            opt.show.insert(uint32_t(std::stoul(w, nullptr, 16)));
    os2::FontLibrary fonts(res / "UI/Fonts");
    const os2::UiScreen screen = os2::layoutUi(*script, ui, fonts, opt);
    for (const auto& w : screen.warnings)
        std::fprintf(stderr, "opensims2: ui %s: %s\n", id->c_str(), w.c_str());
    gUi = std::make_unique<os2::UiLayer>((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                         uint32_t(view.depthStencilPixelFormat));
    gUi->setScreen(screen);
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) { gUi->encode(enc, w, h); });
    std::printf("opensims2: ui %s: %zu windows, %zu drawn, %zu texts, %zu quads\n", id->c_str(), screen.windows,
                screen.drawn, screen.texts, screen.quads.size());
    return std::make_pair(uint32_t(screen.width), uint32_t(screen.height));
}

// Lighting state from the game's Lighting.txt: --lighting <name>, else "day".
void applyLighting(os2::Renderer& renderer) {
    const std::string name = argument(@"--lighting").value_or("day");
    const fs::path path = gameDir() / kLightingConfig;
    const os2::LightingConfig cfg = os2::loadLightingConfig(path);
    const os2::LightingState* state = cfg.state(name);
    if (!state)
        throw std::runtime_error("no lighting state \"" + name + "\" in " + path.string());
    renderer.setLighting(*state);
}

// Virtual key codes (layout independent).
enum Key : unsigned short {
    kA = 0, kS = 1, kD = 2, kF = 3, kZ = 6, kX = 7, kQ = 12, kW = 13, kE = 14, kR = 15,
    kEquals = 24, kMinus = 27, kComma = 43, kPeriod = 47, kPadPlus = 69, kPadMinus = 78,
    kEscape = 53, kSpace = 49, kHome = 115, kEnd = 119, kPageUp = 116, kPageDown = 121, kLeft = 123, kRight = 124, kDown = 125, kUp = 126,
    kF1 = 122, kF2 = 120, kF3 = 99, kF4 = 118, kF5 = 96, kReturn = 36, kEnter = 76, kP = 35, k1 = 18, k2 = 19, k3 = 20, kY = 16, kBackspace = 51, kForwardDelete = 117,
};

// Camera position slot of the keys 4-9 (-1: another key).
int cameraSlot(unsigned short key) {
    switch (key) {
    case 21: return 0; case 23: return 1; case 22: return 2; case 26: return 3; case 28: return 4; case 25: return 5;
    default: return -1;
    }
}

// --- Game flow (app/game_flow.hpp) -------------------------------------------

bool headless() { return argument(@"--screenshot").has_value(); }

// Headless: a scene without a clock of its own (the menu, the
// neighborhood, CAS, a lot without VM ticks) sounds for --sound-dwell
// seconds (5) before it is left, so that --sound-mix has its music.
void dwellHeadlessAudio() {
    if (!gGameAudio || gGameAudio->hasDevice() || gGameAudio->sceneSeconds() > 0)
        return;
    gGameAudio->dwell(std::stod(argument(@"--sound-dwell").value_or("5")));
}

// Headless, at exit: the last scene's dwell, the music log and the mix.
void finishHeadlessAudio() {
    if (!gGameAudio || gGameAudio->hasDevice())
        return;
    dwellHeadlessAudio();
    std::string log;
    for (const auto& m : gGameAudio->musicLog())
        log += " " + m;
    std::printf("opensims2: music:%s; playing %s\n", log.empty() ? " none" : log.c_str(),
                gGameAudio->nowPlaying().empty() ? "nothing" : gGameAudio->nowPlaying().c_str());
    std::string names; // every sound played by name in the run (UI clicks, voices, ...)
    for (const auto& [name, n] : gGameAudio->sound().playedNames)
        if (!name.starts_with("7B1ACFCD-")) // (effect tracks by key)
            names += " " + name + (n > 1 ? " (" + std::to_string(n) + ")" : "");
    if (!names.empty() && (std::getenv("OS2_SOUND_LOG") || gGameAudio->sound().playedNames.size() <= 24))
        std::printf("opensims2: sounds played in the run:%s\n", names.c_str());
    if (auto mix = argument(@"--sound-mix"); mix && gGameAudio->unwritten())
        gGameAudio->writeMix(*mix);
    std::fflush(stdout);
}

// The game's sound across scenes (unless --no-audio), made on the main thread.
void startGameAudio() {
    if (gGameAudio || [sceneArgs() containsObject:@"--no-audio"])
        return;
    os2::GameAudio::Options o;
    o.muted = [sceneArgs() containsObject:@"--mute"];
    o.device = !headless();
    gGameAudio = std::make_unique<os2::GameAudio>(gameDir() / "TSData/Res", o);
    perfLap("GameAudio");
    if (headless())
        std::atexit([] { finishHeadlessAudio(); });
}

// Leaves the current scene for `next` once the current event is handled:
// at once in a headless run (between steps), on the next turn of the run
// loop with a window.
void requestScene(const os2::Scene& next) {
    gPendingScene = next;
    if (!headless() && gOnSceneRequest)
        gOnSceneRequest();
}

void requestQuit() {
    gQuitRequested = true;
    if (!headless())
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
}

// A neighborhood folder for --hood: a folder path as given, else the user's
// copy of the shipped neighborhood of that name (made on first use).
fs::path hoodFolder(const std::string& query) {
    std::error_code ec;
    if (fs::is_directory(query, ec))
        return query;
    if (!gUser)
        throw std::runtime_error("no user folder for neighborhood " + query);
    return gUser->neighborhood(query);
}

// The options' group volumes and mutes on the game's mixer.
void applyAudioSettings() {
    if (!gGameAudio)
        return;
    const os2::SoundGroup groups[4] = {os2::SoundGroup::Vox, os2::SoundGroup::Sfx, os2::SoundGroup::Music,
                                       os2::SoundGroup::Ambience};
    for (size_t g = 0; g < 4; ++g)
        gGameAudio->mixer().setGroupVolume(groups[g], gSettings.muted[g] ? 0.0f : gSettings.volume[g]);
}

// Saves the running lot and its neighborhood to the user's folder (app/game_save.hpp).
bool saveGameNow() {
    if (!gSimulation || gLotFile.empty()) {
        std::printf("opensims2: save: no lot to save\n");
        return false;
    }
    // What the buy or build tool holds is not on the lot: a new object never
    // bought goes, a picked-up one goes back where it was (else the ghost
    // was saved as a placed object: a chair refused for "not enough funds"
    // came back on the lawn of N001_Lot83 after Salvar -> Vizinhança; QA 14).
    if (gBuy && gBuy->held())
        gBuy->cancel();
    if (gBuild && gBuild->active())
        gBuild->cancel();
    const CFTimeInterval start = CACurrentMediaTime();
    const os2::GameSaveResult r = os2::saveGame(gSimulation->machine(), gLotFile, gameDir(), os2::sessionCharactersDir());
    for (const auto& l : r.lines)
        std::printf("opensims2: save: %s\n", l.c_str());
    if (r.ok)
        std::printf("opensims2: saved %s (%.2f s)\n", gLotFile.string().c_str(), CACurrentMediaTime() - start);
    else
        std::fprintf(stderr, "opensims2: save failed: %s\n", r.error.c_str());
    std::fflush(stdout);
    return r.ok;
}

// The "Salvar?" questions (UIText STR# 0x99: 3/4 leaving for the
// neighborhood, 5/6 quitting with 24-26 as buttons; Sim/Não/Cancelar from
// STR# 0x8E), put up as a dialog of no object that the pie menu draws and
// that pauses the lot like the trees' dialogs.
constexpr os2::vm::ObjectId kFlowDialog = -2;
FlowDialog& gFlowDialog = gScene.flowDialog;

std::string uiText(uint32_t instance, uint32_t index, const char* fallback) {
    try {
        if (!gTexts && gIndex)
            gTexts = std::make_unique<os2::UiResources>(*gIndex, gameDir() / "TSData/Res");
        if (gTexts)
            if (auto t = gTexts->string(os2::UiStringRef{0x7fa7dc7c, instance, index}); t && !t->empty())
                return *t;
    } catch (const std::exception& err) {
        os2::logOnce("UI text", std::to_string(instance) + ":" + std::to_string(index), err);
    }
    return fallback;
}

void openFlowDialog(FlowDialog kind) {
    if (!gSimulation || gSimulation->machine().dialogs.count(kFlowDialog))
        return;
    os2::vm::Dialog d;
    d.me = kFlowDialog;
    d.blocking = true;
    d.icon = 1;
    if (kind == FlowDialog::Leave) {
        d.title = uiText(0x99, 3, "Save?");
        d.message = uiText(0x99, 4, "Do you want to save your game before exiting to the Neighborhood?");
        d.buttons = {uiText(0x8E, 2, "Yes"), uiText(0x8E, 3, "No"), uiText(0x8E, 1, "Cancel")};
    } else {
        d.title = uiText(0x99, 5, "Quit?");
        d.message = uiText(0x99, 6, "Do you want to save before quitting?");
        d.buttons = {uiText(0x99, 24, "Save & Quit"), uiText(0x99, 25, "Quit without Saving"),
                     uiText(0x99, 26, "Don't Quit")};
    }
    gSimulation->machine().dialogs[kFlowDialog] = std::move(d);
    gFlowDialog = kind;
    std::printf("opensims2: dialog \"%s\" open\n", gSimulation->machine().dialogs[kFlowDialog].title.c_str());
    std::fflush(stdout);
}

// Acts on the answer to the open "Salvar?" dialog, if it has one.
void pollFlowDialog() {
    if (gFlowDialog == FlowDialog::None || !gSimulation)
        return;
    auto& dialogs = gSimulation->machine().dialogs;
    auto it = dialogs.find(kFlowDialog);
    if (it == dialogs.end()) {
        gFlowDialog = FlowDialog::None;
        return;
    }
    const int answer = it->second.answer;
    if (answer < 0)
        return;
    dialogs.erase(it);
    const FlowDialog kind = gFlowDialog;
    gFlowDialog = FlowDialog::None;
    if (answer == 2)
        return; // cancel: stay
    if (answer == 0 && !saveGameNow())
        return; // the save failed: stay on the lot
    if (kind == FlowDialog::Quit) {
        requestQuit();
        return;
    }
    const os2::Scene here = os2::sceneOf(processArguments());
    os2::Scene next;
    next.kind = os2::Scene::Kind::Neighborhood;
    next.hood = here.hood;
    requestScene(next);
}

// Leaving the lot as the game does (ExitLotBegin): asks whether to save,
// then goes back to the neighborhood. The puck's options Neighborhood button
// and Esc call it (a leave-lot button can too).
void leaveLot() {
    if (os2::sceneOf(processArguments()).kind != os2::Scene::Kind::Lot)
        return;
    openFlowDialog(FlowDialog::Leave);
}

// The option windows of the live panel's options (app/options_panel.hpp).
void setupOptions(os2::ResourceIndex& index) {
    if (!gLive)
        return;
    gOptions = std::make_unique<os2::OptionsPanel>(index, gameDir() / "TSData/Res", gSettings);
    gOptions->onChanged = [](const os2::GameSettings& s) {
        gSettings = s;
        if (gUser)
            s.save(*gUser);
        gSettings.applyGraphics(); // for what loads next
        applyAudioSettings();
    };
    if (gAudio)
        gOptions->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
}

// The options panel's Sair on a lot: asks whether to save, then quits.
void quitFromLot() {
    if (gSimulation)
        openFlowDialog(FlowDialog::Quit);
    else
        requestQuit();
}

// A click on an open option window (the options panel itself is the live
// panel's); the window closes with the panel.
bool pressOptions(simd_float2 p, float w, float h) {
    if (!gOptions)
        return false;
    if (gLive && !gLive->optionsOpen() && gOptions->active())
        gOptions->closeWindow();
    if (!gOptions->active())
        return false;
    gOptions->setCanvas(int(w), int(h));
    // Whatever the window covers is its own: no click goes on to the panels or the world.
    return gOptions->press(p) != os2::OptionsPanel::Action::None || gOptions->covers(p);
}

// A click on the main menu (canvas pixels).
void pressMainMenu(simd_float2 p, float w, float h) {
    if (!gMenu)
        return;
    gMenu->setCanvas(int(w), int(h));
    const os2::Scene here = os2::sceneOf(processArguments());
    os2::Scene next;
    switch (gMenu->press(p)) {
    case os2::MainMenu::Action::Tutorial:
        // PlayEpisode: a fresh Tutorial neighborhood (ResetTutorialNeighborhood)
        // and straight into the episode's lot.
        if (const os2::TutorialEpisode* ep = gMenu->chosenEpisode(); ep && gUser) {
            std::printf("opensims2: main menu: tutorial %d \"%s\": %s lot %u\n", ep->number, ep->title.c_str(),
                        ep->neighborhood.c_str(), ep->lot);
            try {
                os2::resetTutorialNeighborhood(gUser->shippedNeighborhoodsDir() / ep->neighborhood,
                                               gUser->neighborhoodsDir() / ep->neighborhood);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: tutorial: %s\n", e.what());
            }
            next.kind = os2::Scene::Kind::Lot;
            next.hood = ep->neighborhood;
            next.lot = ep->lot;
            requestScene(next);
        }
        break;
    case os2::MainMenu::Action::Play:
        std::printf("opensims2: main menu: play %s\n", gMenu->chosen().c_str());
        next.kind = os2::Scene::Kind::Neighborhood;
        next.hood = gMenu->chosen();
        requestScene(next);
        break;
    case os2::MainMenu::Action::Close:
        next.kind = os2::Scene::Kind::Neighborhood;
        next.hood = here.returnHood;
        requestScene(next);
        break;
    case os2::MainMenu::Action::Quit:
        std::printf("opensims2: main menu: quit\n");
        requestQuit();
        break;
    default: break;
    }
    std::fflush(stdout);
}

// Runs the lot's VM `ticks` ticks headless (a --wait step), with the
// particle effects the VM starts and stops as the drawn frames run them.
void runTicksHeadless(long ticks, os2::Renderer* renderer) {
    if (!gSimulation || ticks <= 0)
        return;
    const uint64_t target = gSimulation->ticks() + uint64_t(ticks);
    // Paused with a tutorial running: its controller goes on (pollTutorial),
    // so a wait is that many ticks of real time.
    const bool pausedTutorial = gTutorial && gSimulation->machine().globals[os2::vm::global::Paused];
    for (long guard = 0; gSimulation->ticks() < target && guard < (pausedTutorial ? ticks * 30 / 23 + 1 : ticks * 40 + 100);
         ++guard) {
        gHeadlessClock += 1.0 / 30;
        const double scene = gSimulation->update(gHeadlessClock);
        if (gLighting && gSimulation->ticks() % 30 == 0)
            gLighting->update(scene);
        if (gLive)
            gLive->tick(gHeadlessClock);
        if (renderer)
            updateEffects(*renderer, scene);
        pollTutorial(gHeadlessClock);
        if (gAudio) {
            updateAudio(gHeadlessClock);
            gAudio->renderOffline(gHeadlessClock);
        }
    }
    std::printf("opensims2: waited to VM tick %llu\n", (unsigned long long)gSimulation->ticks());
    std::fflush(stdout);
}

// --- The game's mouse cursors (app/game_cursors.hpp) -------------------------

std::array<NSCursor*, size_t(os2::GameCursor::Count)> gCursors{};
bool gCursorsLoaded = false;

NSCursor* gameCursor(os2::GameCursor which) {
    if (!gCursorsLoaded) {
        gCursorsLoaded = true;
        const fs::path dir = gameDir() / "TSData/Res/UI/Cursors";
        for (size_t i = 0; i < gCursors.size(); ++i) {
            const fs::path f = os2::findCursorFile(dir, os2::gameCursorName(os2::GameCursor(i)));
            std::ifstream in(f, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto img = os2::decodeCursor(bytes);
            if (!img)
                continue;
            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                            pixelsWide:img->width
                                                                            pixelsHigh:img->height
                                                                         bitsPerSample:8
                                                                       samplesPerPixel:4
                                                                              hasAlpha:YES
                                                                              isPlanar:NO
                                                                        colorSpaceName:NSDeviceRGBColorSpace
                                                                          bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                           bytesPerRow:img->width * 4
                                                                          bitsPerPixel:32];
            std::memcpy(rep.bitmapData, img->rgba.data(), img->rgba.size());
            NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(img->width, img->height)];
            [image addRepresentation:rep];
            gCursors[i] = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(img->hotX, img->hotY)];
        }
    }
    NSCursor* c = gCursors[size_t(which)];
    return c ? c : [NSCursor arrowCursor];
}

// What the view knows of the mouse for the cursor (the camera's state).
struct CursorExtras {
    bool inside = false;
    bool lotScene = false;       // a lot or neighborhood scene (the camera scrolls at the edges)
    int scrollX = 0, scrollY = 0; // the window's edge scrolling
    bool panning = false;        // the right button drags the camera
    os2::vm::ObjectId liveHover = 0; // the object under the mouse in Live mode (0: none)
    std::function<void(os2::CursorContext&)> hood; // fills the neighborhood's part
};

// Whether the selected Sim has something to do with an object (the pie
// menu's entries), for the Live cursor; kept for half a second per object so
// a mouse move does not run the check trees each time.
bool objectHasInteractions(os2::vm::ObjectId id) {
    os2::vm::ObjectId& lastId = gScene.interactionsAsked.id; // (an object ID: per scene)
    double& lastAt = gScene.interactionsAsked.at;
    bool& lastResult = gScene.interactionsAsked.result;
    const double now = CACurrentMediaTime();
    if (id == lastId && now - lastAt < 0.5)
        return lastResult;
    lastId = id, lastAt = now, lastResult = false;
    if (!gPie || !gPie->sim())
        return false;
    for (const auto& e : os2::vm::pieMenu(gPie->vm(), gPie->sim(), id))
        if (e.available() && !e.debug) {
            lastResult = true;
            break;
        }
    return lastResult;
}

// The cursor for what is under canvas point p (the game's arrow over the UI,
// Buy mode's hands over the lot, the Build tools', the neighborhood mover's,
// the camera's edge scrolling: docs/camera-ui-mac.md 2.7).
void refreshGameCursor(simd_float2 p, const CursorExtras& x) {
    if (!x.inside) {
        [[NSCursor arrowCursor] set];
        return;
    }
    using Ctx = os2::CursorContext;
    Ctx c;
    c.loading = gSceneLoading;
    c.mode = gBuy && gBuy->active() ? Ctx::Mode::Buy : gBuild && gBuild->active() ? Ctx::Mode::Build
             : gHood               ? Ctx::Mode::Hood : gLive ? Ctx::Mode::Live : Ctx::Mode::Other;
    if (!gSceneLoading) {
        if (gTutorial) {
            const auto r = gTutorial->panelRect();
            c.overUi |= p.x >= r[0] && p.x < r[2] && p.y >= r[1] && p.y < r[3];
        }
        c.overUi |= gPie && (gPie->isOpen() || gPie->dialogOpen());
        c.overUi |= gLive && gLive->overPanel(p);
        if (gBuy && gBuy->active()) {
            c.overUi |= gBuy->overPanel(p);
            c.buyTool = Ctx::BuyTool(int(gBuy->tool()));
            c.holding = gBuy->held() != 0;
            c.pressing = gBuy->pressing();
            c.turned = gBuy->turned();
            c.overObject = gBuy->overObject();
        } else if (gBuild && gBuild->active()) {
            using Tool = os2::BuildMode::Tool;
            using Op = os2::vm::BuildEditor::TerrainOp;
            c.overUi |= gBuild->overPanel(p);
            switch (gBuild->tool()) {
            case Tool::Design: c.buildTool = Ctx::BuildTool::Design; break;
            case Tool::Railing: c.buildTool = Ctx::BuildTool::Railing; break;
            case Tool::Roof: c.buildTool = Ctx::BuildTool::Roof; break;
            case Tool::Object: c.buildTool = Ctx::BuildTool::Object; break;
            case Tool::Terrain:
                c.buildTool = gBuild->terrainOp() == Op::Raise   ? Ctx::BuildTool::Raise
                              : gBuild->terrainOp() == Op::Lower ? Ctx::BuildTool::Lower
                              : gBuild->terrainOp() == Op::Flatten ? Ctx::BuildTool::Level : Ctx::BuildTool::Other;
                break;
            default: break;
            }
            c.holding = gBuild->holdingObject();
            c.buildDragging = gBuild->dragging();
        } else if (gHood && x.hood) {
            x.hood(c);
        } else if (gLive && !c.overUi && x.liveHover) {
            using Hover = Ctx::LiveHover;
            const bool sim = std::any_of(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == x.liveHover; });
            c.liveHover = sim ? Hover::Person : objectHasInteractions(x.liveHover) ? Hover::Object : Hover::ObjectUnavailable;
        }
        if (x.lotScene && !c.overUi) {
            c.scrollX = x.scrollX, c.scrollY = x.scrollY;
            c.panning = x.panning;
        }
    }
    [gameCursor(os2::chooseCursor(c)) set];
}

} // namespace os2app

using namespace os2app;

// MTKView that keeps the camera and the keys held down.
@interface OS2View : MTKView
- (os2::LotCamera*)camera;
- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera;
- (void)applyHeldKeys:(double)dt;
- (void)holdKey:(unsigned short)key seconds:(double)seconds;
- (void)refreshCursor:(NSEvent*)e;
@end

@implementation OS2View {
    std::unique_ptr<os2::LotCamera> _camera;
    std::set<unsigned short> _held;
    os2::CameraMouseInput _mouse;
    bool _rotating; // right button with Control or Option: rotate/tilt instead of scroll
    bool _leftRotating; // Control + left on the lot (trackpads): rotate/tilt
    bool _leftRotated;  // ... and it moved (else the release is a plain click)
    float _leftMoved;   // points dragged since that press
    NSEvent* _leftPress; // that press, replayed as a click if it never moved
}

- (os2::LotCamera*)camera {
    return _camera.get();
}

- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera {
    _camera = std::move(camera);
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (bool)held:(unsigned short)a orKey:(unsigned short)b {
    return _held.count(a) || _held.count(b);
}

- (void)applyHeldKeys:(double)dt {
    _camera->setViewport(float(self.bounds.size.width), float(self.bounds.size.height));
    if (gCas)
        return; // the CAS camera only follows the Sim and the zoom buttons
    // Shift: "Move ... Faster" (twice the speed).
    const float fast = (NSEvent.modifierFlags & NSEventModifierFlagShift) ? 2.0f : 1.0f;
    float right = float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]);
    float forward = float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]);
    right *= fast, forward *= fast;
    if (self.window.isKeyWindow) {
        _mouse.config().edgeScroll = !argument(@"--no-edge-scroll") && gSettings.edgeScrolling;
        const auto [mr, mf] = _mouse.panAxes(float(self.bounds.size.width), float(self.bounds.size.height));
        right = std::clamp(right + mr, -fast, fast);
        forward = std::clamp(forward + mf, -fast, fast);
    }
    _camera->pan(right, forward, dt);
    // Right button: the anchor's offset x 0.25 as a move request, per 50 ms frame.
    const auto [dragX, dragY] = _mouse.dragMove();
    _camera->movePixels(dragX * float(dt) * 20.0f, dragY * float(dt) * 20.0f);
    _camera->zoomHeld(int(_held.count(kEquals)) - int(_held.count(kMinus)), dt);
    _camera->rotateHeld(int(_held.count(kPeriod)) - int(_held.count(kComma)), dt);
}

// Headless (--key hold-<key>:<seconds>): the key held that long, frame by
// frame (50 ms) as the draw loop applies held keys, the camera smoothed.
- (void)holdKey:(unsigned short)key seconds:(double)seconds {
    _held.insert(key);
    for (double t = 0; t < seconds; t += 0.05) {
        [self applyHeldKeys:0.05];
        _camera->update(0.05);
    }
    _held.erase(key);
    for (int k = 0; k < 40; ++k) // the smoothing settles
        _camera->update(0.05);
}

- (void)keyDown:(NSEvent*)e {
    if (gSceneLoading) { // the scene is the worker's; Esc, Space or Return skip the intro, not the loading screen
        if (os2::moviePlaying() && !os2::loadingScreenPlaying() && !e.isARepeat &&
            (e.keyCode == kEscape || e.keyCode == kReturn || e.keyCode == kEnter || e.keyCode == 49)) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (key %d)\n", int(e.keyCode));
        }
        return;
    }
    if (gCas) { // Create-a-Sim takes every key (names, undo, turning the Sim)
        std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        // The case typed is kept: a letter that arrives lower case with Shift (or Caps Lock) down
        // is made capital (synthesized key events carry the flag but not always the capital).
        const bool shifted = (e.modifierFlags & NSEventModifierFlagShift) != 0;
        const bool caps = (e.modifierFlags & NSEventModifierFlagCapsLock) != 0;
        if (typed.size() == 1 && typed[0] >= 'a' && typed[0] <= 'z' && (shifted != caps) &&
            !(e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
            typed[0] = char(typed[0] - 'a' + 'A');
        gCas->keyDown(e.keyCode, typed, e.modifierFlags & NSEventModifierFlagControl);
        return;
    }
    if (os2::moviePlaying()) { // Esc, Space or Return skips a movie
        if (!e.isARepeat && (e.keyCode == kEscape || e.keyCode == kReturn || e.keyCode == kEnter || e.keyCode == 49)) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (key %d)\n", int(e.keyCode));
        }
        return;
    }
    // A dialog the trees put up takes the keys (text, Return, Esc).
    if (gPie && gPie->dialogOpen()) {
        const std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        gPie->dialogKey(e.keyCode, typed);
        return;
    }
    // The neighborhood's place-lot dialog takes the keys (the lot's name).
    if (gHood && (gHood->ui().bins().textEntry() || gHood->ui().storyEditing())) {
        const std::string typed = e.characters ? std::string(e.characters.UTF8String) : std::string();
        gHood->typeKey(e.keyCode, typed);
        return;
    }
    _held.insert(e.keyCode);
    if (e.isARepeat)
        return;
    // Any key (Shift sends no keyDown) cancels the pie menu; Esc that did so does nothing more.
    const bool pieWasOpen = gPie && gPie->isOpen();
    if (pieWasOpen)
        gPie->close();
    if (e.keyCode == kEscape && pieWasOpen)
        return;
    // Esc closes the options panel (and its window) before leaving the lot.
    if (e.keyCode == kEscape && !gHood && escapeOptions())
        return;
    if (const int slot = cameraSlot(e.keyCode); slot >= 0) {
        if (e.modifierFlags & NSEventModifierFlagControl)
            _camera->savePosition(slot);
        else
            _camera->restorePosition(slot, !(e.modifierFlags & NSEventModifierFlagShift));
        return;
    }
    // Build mode: Ctrl-Z/Ctrl-Y undo and redo, , and . turn a door or window.
    if (gBuild && gBuild->active()) {
        const bool control = e.modifierFlags & NSEventModifierFlagControl;
        if (control && e.keyCode == kZ) { gBuild->undo(); return; }
        if (control && e.keyCode == kY) { gBuild->redo(); return; }
        if (gBuild->tool() == os2::BuildMode::Tool::Object && (e.keyCode == kComma || e.keyCode == kPeriod)) {
            gBuild->rotate(e.keyCode == kPeriod ? 2 : -2);
            _held.erase(e.keyCode);
            return;
        }
    }
    // Buy mode: , and . turn what is held, Delete sells it, Ctrl-Z/Ctrl-Y undo and redo.
    if (gBuy && gBuy->active()) {
        const bool control = e.modifierFlags & NSEventModifierFlagControl;
        if (control && e.keyCode == kZ) { gBuy->undo(); return; }
        if (control && e.keyCode == kY) { gBuy->redo(); return; }
        if (gBuy->held() && (e.keyCode == kComma || e.keyCode == kPeriod)) {
            gBuy->rotate(e.keyCode == kPeriod ? 2 : -2);
            _held.erase(e.keyCode); // not the camera's
            return;
        }
        if (e.keyCode == kBackspace || e.keyCode == kForwardDelete) { gBuy->deleteHeld(); return; }
    }
    // The neighborhood puck's accelerators: F1 families, F2 lots, F5 options.
    if (gHood && (e.keyCode == kF1 || e.keyCode == kF2 || e.keyCode == kF3 || e.keyCode == kF5)) {
        gHood->ui().togglePanel(e.keyCode == kF1   ? os2::HoodBins::Panel::Families
                                : e.keyCode == kF2 ? os2::HoodBins::Panel::Lots
                                : e.keyCode == kF3 ? os2::HoodBins::Panel::Decorations
                                                   : os2::HoodBins::Panel::Options);
        return;
    }
    if (gHood && e.keyCode == kF4) { // "História da Vizinhança - F4"
        gHood->ui().toggleStory();
        return;
    }
    // Ctrl-Z / Ctrl-Y (or Command): the Decorations panel's Undo and Redo.
    if (gHood && gHood->ui().bins().panel() == os2::HoodBins::Panel::Decorations && (e.keyCode == kZ || e.keyCode == kY) &&
        (e.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand))) {
        gHood->undoDecoration(e.keyCode == kY);
        return;
    }
    // The neighborhood's lot tools: , and . turn the lot being placed.
    if (gHood && gHood->placingLot() && (e.keyCode == kComma || e.keyCode == kPeriod)) {
        gHood->turnLot(e.keyCode == kPeriod ? 1 : -1);
        _held.erase(e.keyCode);
        return;
    }
    switch (e.keyCode) {
    case kSpace: selectNextFamilySim(); break;
    case kPageUp: changeFloor(1); break;
    // "Paredes ... - Origem/Fim": Home and End step through the wall modes.
    case kHome: gWallView.cycle(1); break;
    case kEnd: gWallView.cycle(-1); break;
    case kPageDown: changeFloor(-1); break;
    case kZ: case kPadPlus: _camera->zoomBy(1); break;
    case kX: case kPadMinus: _camera->zoomBy(-1); break;
    case kReturn: case kEnter:
        if (gCameraFollow && gSimulation && gPie)
            if (auto p = gCameraFollow->simPoint(gSimulation->machine(), gPie->sim()))
                _camera->moveTo(*p);
        break;
    case kEscape:
        if (gBuild && gBuild->active() && gBuild->tool() != os2::BuildMode::Tool::None) {
            gBuild->cancel();
            break;
        }
        if (gBuy && gBuy->active() && gBuy->held()) { // drops what Buy mode holds
            gBuy->cancel();
            break;
        }
        if (gHood) {
            gHood->escape();
            break;
        }
        // A lot entered from the neighborhood: back to it, asking whether to
        // save (unless the pie menu is open).
        if (gHoodLot && !(gPie && (gPie->isOpen() || gPie->dialogOpen()))) {
            leaveLot();
            break;
        }
        if (gPie) gPie->close();
        break;
    // The speed buttons' accelerators (their tips: "Pausa - P", "Velocidade Normal - 1", ...).
    case kP: if (gLive) gLive->togglePause(); break;
    case k1: case k2: case k3: if (gLive) gLive->setSpeed(e.keyCode - k1 + 2); break;
    default: break;
    }
}

- (void)keyUp:(NSEvent*)e {
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->keyUp(e.keyCode);
    _held.erase(e.keyCode);
}

- (void)flagsChanged:(NSEvent*)e {
    // A key released while Cmd was down never sends keyUp.
    if (e.modifierFlags & NSEventModifierFlagCommand)
        _held.clear();
}

- (void)scrollWheel:(NSEvent*)e {
    if (gSceneLoading)
        return;
    _camera->wheel(_mouse.wheelNotches(e.scrollingDeltaY, e.hasPreciseScrollingDeltas));
}

// The game's rotate request: pixels dragged (only the dominant axis counts).
- (void)dragRotate:(NSEvent*)e {
    if (gSceneLoading)
        return;
    _camera->rotateRequest(float(e.deltaX), float(e.deltaY));
}

// The mouse position for edge and anchor scrolling.
- (void)trackMouse:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    _mouse.mouseAt(float(p.x), float(self.bounds.size.height - p.y), NSPointInRect(p, self.bounds));
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* a in self.trackingAreas)
        [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                           NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
}

- (void)mouseExited:(NSEvent*)e {
    [self trackMouse:e];
    _mouse.mouseAt(-1, -1, false);
    refreshGameCursor({}, CursorExtras{});
}

- (void)cursorUpdate:(NSEvent*)e {
    [self refreshCursor:e];
}

// The game's cursor for the state after this event.
- (void)refreshCursor:(NSEvent*)e {
    const NSPoint q = [self convertPoint:e.locationInWindow fromView:nil];
    const simd_float2 p = [self canvasPoint:e];
    CursorExtras x;
    x.inside = NSPointInRect(q, self.bounds);
    x.lotScene = _camera && !gCas && (gLive || gBuy || gBuild || gHood);
    if (x.inside && x.lotScene) {
        const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
        if (self.window.isKeyWindow && !gSceneLoading) {
            _mouse.config().edgeScroll = !argument(@"--no-edge-scroll") && gSettings.edgeScrolling;
            const auto [mr, mf] = _mouse.panAxes(w, h);
            x.scrollX = int(mr), x.scrollY = int(mf);
        }
        x.panning = _mouse.anchored() && !_rotating;
        if (gPie && gLive && !(gBuy && gBuy->active()) && !(gBuild && gBuild->active()) && !gHood && !gSceneLoading)
            x.liveHover = gPie->pick(_camera->eye(), [self rayAt:p]);
        if (gHood)
            x.hood = [&](os2::CursorContext& c) { gHood->fillCursor(c, *_camera, p, w, h); };
    }
    refreshGameCursor(p, x);
}

- (void)rightMouseDown:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading)
        return;
    if (gPie && gPie->isOpen()) { // the right button cancels the pie menu
        gPie->close();
        return;
    }
    if (gLive && gLive->press([self canvasPoint:e], true))
        return;
    // Control (the game's "CTRL + botão DIREITO") or Option: rotate and tilt.
    _rotating = os2::CameraMouseInput::rotatePress(os2::CameraMouseInput::Button::Right,
                                                   e.modifierFlags & NSEventModifierFlagControl,
                                                   e.modifierFlags & NSEventModifierFlagOption);
    if (!_rotating && gPie && !(gBuy && gBuy->active()) && !(gBuild && gBuild->active())) // a right click on a Sim of the family selects it
        selectFamilySim(gPie->pick(_camera->eye(), [self rayAt:[self canvasPoint:e]]));
    if (_rotating)
        [self holdCursor:YES];
    else
        _mouse.beginAnchor();
}

- (void)rightMouseUp:(NSEvent*)e {
    [self trackMouse:e];
    _mouse.endAnchor();
    if (_rotating)
        [self holdCursor:NO];
    _rotating = false;
}

// While rotating the cursor stays where it was clicked (the game warps it
// back after each move: an endless drag); the deltas keep coming.
- (void)holdCursor:(BOOL)hold {
    CGAssociateMouseAndMouseCursorPosition(hold ? false : true);
}

- (void)otherMouseDown:(NSEvent*)e {
    [self holdCursor:YES];
}

- (void)otherMouseUp:(NSEvent*)e {
    [self holdCursor:NO];
}

// Mouse position in view points, origin top left (the pie menu's canvas).
- (simd_float2)canvasPoint:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return simd_make_float2(float(p.x), float(self.bounds.size.height - p.y));
}

// Left click (with --vm): on an open pie menu picks an entry (or closes it);
// else casts a ray from the camera through the mouse and opens the menu of
// the object hit.
- (void)mouseDown:(NSEvent*)e {
    struct Refresh { // the cursor after the press, whichever way it returns
        OS2View* view; NSEvent* event;
        ~Refresh() { [view refreshCursor:event]; }
    } refresh{self, e};
    if (gSceneLoading) { // a click skips the intro, not the loading screen
        if (os2::moviePlaying() && !os2::loadingScreenPlaying()) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (click)\n");
        }
        return;
    }
    if (gCas) {
        gCas->setCanvas(int(self.bounds.size.width), int(self.bounds.size.height));
        gCas->mouseDown([self canvasPoint:e]);
        return;
    }
    if (os2::moviePlaying()) { // a click skips a movie
        os2::skipMovie();
        std::printf("opensims2: movie skipped (click)\n");
        return;
    }
    if (gMenu) {
        pressMainMenu([self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
        return;
    }
    if (gHood) {
        gHood->mouseDown(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
        return;
    }
    if (!gPie)
        return;
    const simd_float2 p = [self canvasPoint:e];
    if (gPie->dialogOpen() && gPie->click(p)) {
        pollFlowDialog();
        return;
    }
    if (gTutorial) { // the tutorial's window (top right)
        gTutorial->setCanvas(int(self.bounds.size.width), int(self.bounds.size.height));
        if (gTutorial->press(p))
            return;
    }
    if (gPie->isOpen() && gPie->press(p))
        return;
    if (pressOptions(p, float(self.bounds.size.width), float(self.bounds.size.height))) // its body is over all
        return;
    if (gLive && gLive->overModeButton(p) && gLive->press(p)) // the puck's modes win over the panels
        return;
    // The options panel floats over Buy/Build's panels: it must take its clicks first
    // (their backdrops would swallow Salvar/Sair and the like).
    if (gLive && gLive->optionsOpen() && gLive->press(p))
        return;
    const bool optionsOver = gLive && gLive->optionsOpen(); // Buy/Build's panels are hidden under it
    if (!optionsOver && gBuy && gBuy->active() && gBuy->press(p))
        return;
    if (!optionsOver && gBuild && gBuild->active() && gBuild->press(p))
        return;
    if (gLive && gLive->press(p))
        return;
    if (pressOptions(p, float(self.bounds.size.width), float(self.bounds.size.height))) // under the puck
        return;
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = _camera->eye();
    if (gBuy && gBuy->active()) { // the object tool, not the pie menu (a drag turns what is held, the release drops it)
        gBuy->mouseDown(eye, simd_normalize(q.xyz / q.w - eye), p);
        return;
    }
    if (gBuild && gBuild->active()) { // Build mode's tools
        gBuild->mouseDown(eye, simd_normalize(q.xyz / q.w - eye), e.modifierFlags & NSEventModifierFlagControl,
                          e.modifierFlags & NSEventModifierFlagShift);
        return;
    }
    // Control + left on the lot: the rotate/tilt tool (a trackpad's or a
    // one-button mouse's Ctrl-click is the right button); a press that
    // never moves is still a click, replayed on release.
    if (!_leftPress && os2::CameraMouseInput::rotatePress(os2::CameraMouseInput::Button::Left,
                                                          e.modifierFlags & NSEventModifierFlagControl, false)) {
        _leftRotating = true;
        _leftRotated = false;
        _leftMoved = 0;
        _leftPress = e;
        [self holdCursor:YES];
        return;
    }
    const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
    if (id)
        openClicked(id, eye, simd_normalize(q.xyz / q.w - eye), p);
    if (!id) // the ground: Go Here
        openGoHere(eye, simd_normalize(q.xyz / q.w - eye), p);
    std::fflush(stdout);
}

// The pie menu picks on release (a drag from the opening press is a gesture).
- (void)mouseUp:(NSEvent*)e {
    if (_leftRotating) {
        _leftRotating = false;
        [self holdCursor:NO];
        NSEvent* press = _leftPress;
        if (!_leftRotated && !gSceneLoading)
            [self mouseDown:press]; // _leftPress set: a plain click now
        _leftPress = nil;
        return;
    }
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->mouseUp([self canvasPoint:e]);
    if (gPie && gPie->isOpen())
        gPie->release([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseUp(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    if (gBuy && gBuy->active())
        gBuy->mouseUp();
    [self refreshCursor:e];
    if (gHood)
        gHood->mouseUp(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
}

// The mouse ray (world space) through a canvas point.
- (simd_float3)rayAt:(simd_float2)p {
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    return simd_normalize(q.xyz / q.w - _camera->eye());
}

- (void)mouseMoved:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading) {
        [self refreshCursor:e];
        return;
    }
    if (gHood)
        gHood->mouseMoved(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
    if (gPie)
        gPie->hover([self canvasPoint:e]);
    if (gLive)
        gLive->hover([self canvasPoint:e]);
    if (gBuy && gBuy->active()) {
        const simd_float2 p = [self canvasPoint:e];
        gBuy->hover(p);
        const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
        const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
        const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
        gBuy->pointAt(_camera->eye(), simd_normalize(q.xyz / q.w - _camera->eye()));
    }
    if (gBuild && gBuild->active()) {
        gBuild->hover([self canvasPoint:e]);
        gBuild->pointAt(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    }
    [self refreshCursor:e];
}

- (void)mouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading)
        return;
    if (_leftRotating) {
        _leftMoved += std::fabs(float(e.deltaX)) + std::fabs(float(e.deltaY));
        _leftRotated = _leftMoved > 3; // a click's jitter is still a click
        [self dragRotate:e];
        return;
    }
    if (gCas)
        gCas->mouseDragged([self canvasPoint:e]);
    if (gPie)
        gPie->hover([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseDrag(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    if (gBuy && gBuy->active())
        gBuy->mouseDrag(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
    [self refreshCursor:e];
    if (gHood)
        gHood->mouseDragged(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
}

- (void)rightMouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (_rotating)
        [self dragRotate:e];
}

- (void)otherMouseDragged:(NSEvent*)e {
    [self dragRotate:e];
}
@end

@interface OS2ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view;
- (os2::Renderer*)renderer;
- (void)resetRenderer:(MTKView*)view;
- (void)restartClock;
@end

@implementation OS2ViewDelegate {
    std::unique_ptr<os2::Renderer> _renderer;
    CFTimeInterval _last;
    CFTimeInterval _start;
}

- (instancetype)initWithView:(MTKView*)view {
    if ((self = [super init])) {
        _renderer = std::make_unique<os2::Renderer>((__bridge MTL::Device*)view.device,
                                                    uint32_t(view.colorPixelFormat),
                                                    uint32_t(view.depthStencilPixelFormat));
        _last = _start = CACurrentMediaTime();
    }
    return self;
}

- (os2::Renderer*)renderer {
    return _renderer.get();
}

// A new, empty renderer for the next scene (the old one waits for its frames
// in flight); the scene clock starts again.
- (void)resetRenderer:(MTKView*)view {
    _renderer.reset();
    _renderer = std::make_unique<os2::Renderer>((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                                uint32_t(view.depthStencilPixelFormat));
    _last = _start = CACurrentMediaTime();
}

// The scene clock starts when the scene is shown (after its load).
- (void)restartClock {
    _last = _start = CACurrentMediaTime();
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
}

- (void)drawInMTKView:(MTKView*)mtkView {
    OS2View* view = (OS2View*)mtkView;
    CFTimeInterval now = CACurrentMediaTime();
    struct FramePerf { // OS2_PERF=1: CPU time of a frame (not the wait for the drawable), over 120 frames
        CFTimeInterval began, waited = 0, total = 0, worst = 0;
        int count = 0;
    };
    static FramePerf perf;
    perf.began = now;
    double dt = std::min(now - _last, 0.1); // no huge steps after a stall
    _last = now;

    // A scene loading on the worker (app/scene_loader.hpp): only the loading
    // screen (or the intro) is drawn; the scene's objects are the worker's.
    if (gSceneLoading) {
        if (gPollLoad)
            gPollLoad();
        if (gGameAudio) { // the music goes on behind the loading screen
            gGameAudio->setHeld(os2::moviePlaying());
            gGameAudio->update(now - _start);
        }
        MTLRenderPassDescriptor* pass = mtkView.currentRenderPassDescriptor;
        id<CAMetalDrawable> drawable = mtkView.currentDrawable;
        os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable);
        return;
    }

    if (!view.camera) { // no scene (--movie): only a movie
        MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
        id<CAMetalDrawable> drawable = view.currentDrawable;
        os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable);
        return;
    }
    [view applyHeldKeys:dt];
    pollFlowDialog();
    if (gOptions && gLive && !gLive->optionsOpen() && gOptions->active())
        gOptions->closeWindow(); // the options panel closed
    if (gLive)
        gLive->tick(now - _start);
    followSelectedSim();
    view.camera->update(dt);
    CGSize size = view.drawableSize;
    float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
    gLastFrameW = uint32_t(std::max(1.0, size.width)), gLastFrameH = uint32_t(std::max(1.0, size.height));
    poseActors(*_renderer, now - _start);
    if (gGameAudio) { // (on a lot, the lot's audio updates it)
        gGameAudio->setHeld(os2::moviePlaying());
        if (!gAudio)
            gGameAudio->update(now - _start);
    }
    const os2::FrameView frame = cinematicView({view.camera->view(), view.camera->projection(aspect)}, now - _start, aspect);
    if (gCas)
        gCas->update(now - _start);
    if (gBuy)
        gBuy->update();
    if (gBuild)
        gBuild->update();
    if (view.bounds.size.width > 0)
        gUiScale = float(size.width / view.bounds.size.width);

    const CFTimeInterval beforeDrawable = CACurrentMediaTime();
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    perf.waited = CACurrentMediaTime() - beforeDrawable;
    // A fullscreen movie (app/movie_host.hpp) in place of the scene.
    if (os2::movieHostDraw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable))
        return;
    _renderer->setReflections(reflectionsOn());
    _renderer->draw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable, frame);
    if (const char* shot = liveShotFile(); shot && gLiveShotWanted) {
        gLiveShotWanted = 0;
        const MTLClearColor c = view.clearColor;
        os2::writePng(shot, _renderer->capture(uint32_t(gLastFrameW), uint32_t(gLastFrameH), frame,
                                               simd_make_float4(float(c.red), float(c.green), float(c.blue), 1)));
        std::printf("opensims2: live shot %s\n", shot);
        std::fflush(stdout);
    }
    if (perfOn()) {
        const CFTimeInterval spent = CACurrentMediaTime() - perf.began - perf.waited;
        perf.total += spent, perf.worst = std::max(perf.worst, spent);
        if (++perf.count == 120) {
            std::printf("opensims2: perf: 120 frames, CPU %.2f ms average, %.2f ms worst\n", perf.total / 120 * 1000,
                        perf.worst * 1000);
            std::fflush(stdout);
            perf.total = perf.worst = 0, perf.count = 0;
        }
    }
}
@end

@interface OS2AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation OS2AppDelegate {
    NSWindow* _window;
    OS2View* _view;
    OS2ViewDelegate* _viewDelegate; // MTKView holds its delegate weakly
    bool _introPlayed;
    int _flowShot;
    // The scene being loaded (prepareScene -> finishScene).
    os2::Scene _scene;
    bool _cas;
    std::optional<std::pair<uint32_t, uint32_t>> _uiCanvas; // the lot's --ui canvas
    simd_float3 _pivot;                                     // the lot's camera pivot
    std::vector<fs::path> _menuDirs;                        // the main menu's neighborhoods
    os2::LoadingScreen _loadingScreen;
    bool _loadCompletionQueued;
    CFTimeInterval _loadStart;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    perfLap("process start");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "opensims2: no Metal device\n");
        [NSApp terminate:nil];
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"opensims2";

    OS2View* view = [[OS2View alloc] initWithFrame:frame device:device];
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm; // gamma space, like the game
    view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    view.preferredFramesPerSecond = 60;
    _view = view;

    try {
        _viewDelegate = [[OS2ViewDelegate alloc] initWithView:view];
        applyLighting(*_viewDelegate.renderer);
        os2::movieHostInit((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                           uint32_t(view.depthStencilPixelFormat),
                           {gameDir() / "TSData/Res", ![sceneArgs() containsObject:@"--no-audio"]});
        // --movie <name>: play one movie in the window and quit when it ends.
        if (auto movie = argument(@"--movie")) {
            os2::playMovie(*movie, [] { [NSApp terminate:nil]; });
            view.delegate = _viewDelegate;
            _window.contentView = view;
            [_window center];
            [_window makeKeyAndOrderFront:nil];
            [_window makeFirstResponder:view];
            [NSApp activateIgnoringOtherApps:YES];
            return;
        }
        // --intro: the start-up movies (EA logo, intro) before the scene.
        if ([sceneArgs() containsObject:@"--intro"])
            os2::playMovies(os2::introMovies());
        // The start-up movies before the first main menu (EA logo, then the
        // intro; skippable), unless --intro already started them.
        gIntroMovies = [] {
            if (!os2::moviePlaying())
                os2::playMovies(os2::introMovies());
        };
        __weak OS2AppDelegate* weakSelf = self;
        gOnSceneRequest = [weakSelf] {
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf switchScene];
            });
        };
        gPollLoad = [weakSelf] { [weakSelf pollSceneLoad]; };
        // Headless, the scenes load in order on this thread; with a window
        // the first one loads behind the loading screen (below).
        startGameAudio(); // (on this thread: the scenes' loads only use it)
        if (headless()) {
            [self ensureGameState];
            [self runScenes];
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: %s\n", e.what());
        if (argument(@"--screenshot"))
            std::exit(1); // no one to show an alert to
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"opensims2 could not start";
        alert.informativeText = [NSString stringWithUTF8String:e.what()];
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    view.delegate = _viewDelegate;
    _window.contentView = view;
    [_window center];
    _window.acceptsMouseMovedEvents = YES;
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
    [self beginSceneLoad];
}

// Quitting while a scene loads: the worker cannot be stopped, so the process
// ends at once (nothing is being saved: saves run on the main thread).
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    if (gLoadJob.active()) {
        std::fflush(stdout);
        std::_Exit(0);
    }
    return NSTerminateNow;
}

// The game's resources and the user's folder, made once (with a window, on
// the loading thread of the first scene).
- (void)ensureGameState {
    if (!gIndex) {
        gIndex = std::make_unique<os2::ResourceIndex>(gameDir() / "TSData/Res");
        perfLap("ResourceIndex");
    }
    if (!gUser) {
        // The user's folder (app/user_data.hpp): --user-dir, $OS2_USER_DIR,
        // else ~/Library/Application Support/opensims2.
        fs::path user = os2::defaultUserDir();
        if (const char* env = std::getenv("OS2_USER_DIR"))
            user = env;
        if (auto u = argument(@"--user-dir"))
            user = *u;
        gUser = std::make_unique<os2::UserData>(gameDir(), user);
        gSettings = os2::GameSettings::load(*gUser);
        gSettings.applyGraphics();
    }
}

// --- Loading behind the loading screen (app/scene_loader.hpp) ---------------

// A lot opened by flags (--hood N --hood-lot id) has no click behind it: its
// loading screen's case comes from the neighborhood's lot info.
static void fillLotEntryFromFlags(const os2::Scene& scene) {
    if (scene.kind == os2::Scene::Kind::Neighborhood && !scene.hood.empty()) {
        // The neighborhood's own loading screen: its name, the Tutorial's, or
        // (back from Create-a-Sim) the new family (docs/loading-mac.md).
        try {
            const os2::hood::Neighborhood hood = os2::hood::Neighborhood::load(hoodFolder(scene.hood), false);
            gLotEntry = {};
            gLotEntry.hoodName = hood.name;
            gLotEntry.tutorial = hood.prefix == "Tutorial";
            std::optional<uint32_t> created = gCreatedFamily; // --loading-family N: test aid (case 6)
            if (auto a = argument(@"--loading-family"))
                created = uint32_t(std::stoul(*a));
            if (created)
                if (const auto* f = hood.family(*created)) {
                    gLotEntry.withFamily = true, gLotEntry.familyId = f->id, gLotEntry.familyName = f->name;
                    gLotEntry.familyThumbs =
                        hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package");
                }
        } catch (const std::exception& err) {
            os2::logOnce("loading screen", scene.hood, err);
        }
        return;
    }
    if (scene.kind != os2::Scene::Kind::Lot || gLotEntry.known || scene.hood.empty())
        return;
    try {
        const fs::path dir = hoodFolder(scene.hood);
        const os2::hood::Neighborhood hood = os2::hood::Neighborhood::load(dir, false);
        const os2::hood::Lot* lot = hood.lot(scene.lot);
        if (!lot)
            return;
        gLotEntry = {true, lot->info.type, !lot->families.empty(), lot->name, "", 0,
                     hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package")};
        if (!lot->families.empty()) {
            gLotEntry.familyId = lot->families.front();
            if (const auto* f = hood.family(gLotEntry.familyId))
                gLotEntry.familyName = f->name;
        }
    } catch (const std::exception& err) {
        os2::logOnce("loading screen", scene.hood, err);
    }
}

// With a window: starts loading the scene the arguments pick on the worker,
// and shows its loading screen (after the intro movies, when they play).
// --loading-hold <seconds>: test aid, the worker waits that long after the
// load, so the loading screen can be looked at.
- (void)beginSceneLoad {
    const os2::Scene scene = os2::sceneOf(processArguments());
    const bool cas = [sceneArgs() containsObject:@"--cas"];
    if (scene.kind == os2::Scene::Kind::MainMenu && !cas && !_introPlayed) {
        _introPlayed = true;
        if (gIntroMovies)
            gIntroMovies();
    }
    fillLotEntryFromFlags(scene);
    _loadingScreen = os2::loadingScreenFor(gameDir() / "TSData/Res", scene, cas, gLotEntry);
    gLotEntry = {};
    os2::loading::begin();
    gSceneLoading = true;
    _loadCompletionQueued = false;
    _loadStart = CACurrentMediaTime();
    [self showLoadingScreen];
    const double hold = std::stod(argument(@"--loading-hold").value_or("0"));
    gLoadJob.start([self, hold] {
        @autoreleasepool {
            [self prepareScene];
        }
        if (hold > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(hold));
    });
}

// The loading screen, unless another movie (the intro) is playing.
- (void)showLoadingScreen {
    if (os2::moviePlaying())
        return;
    os2::playLoadingScreen(_loadingScreen, [] { return os2::loading::shown(); });
}

// Each frame while loading (from the view's draw): the loading screen once
// the intro is over, and the end of the load handed to the run loop.
- (void)pollSceneLoad {
    if (!gSceneLoading)
        return;
    if (!os2::moviePlaying())
        [self showLoadingScreen];
    if (!gLoadJob.done() || _loadCompletionQueued)
        return;
    _loadCompletionQueued = true;
    __weak OS2AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf completeSceneLoad];
    });
}

// The worker is done: the main thread's part of the scene (AppKit, the view,
// the headless steps), then frames and input go to the scene again.
- (void)completeSceneLoad {
    std::exception_ptr error = gLoadJob.collect();
    const CFTimeInterval worked = CACurrentMediaTime() - _loadStart;
    gSceneLoading = false;
    os2::loading::end();
    os2::finishLoadingScreen(); // plays out quickly over the first frames
    try {
        if (error)
            std::rethrow_exception(error);
        const CFTimeInterval mainStart = CACurrentMediaTime();
        [self finishScene];
        if (perfOn())
            std::printf("opensims2: perf: scene loaded in %.1f ms (worker), then %.1f ms on the main thread\n",
                        worked * 1000, (CACurrentMediaTime() - mainStart) * 1000);
        std::fflush(stdout);
    } catch (const std::exception& e) {
        [self sceneFailed:e.what()];
        return;
    }
    [_viewDelegate restartClock];
    [_window makeFirstResponder:_view];
}

// A scene that could not load: an alert, then the main menu (or quit, when
// the main menu itself failed).
- (void)sceneFailed:(const char*)what {
    std::fprintf(stderr, "opensims2: %s\n", what);
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = gIndex ? @"opensims2 could not load the scene" : @"opensims2 could not start";
    alert.informativeText = [NSString stringWithUTF8String:what];
    [alert runModal];
    if (!gIndex || !gUser || (_scene.kind == os2::Scene::Kind::MainMenu && !_cas)) {
        [NSApp terminate:nil];
        return;
    }
    os2::Scene menu;
    menu.kind = os2::Scene::Kind::MainMenu;
    gPendingScene = menu;
    [self applyPendingScene];
    [self beginSceneLoad];
}

// Headless --loading-shot <file.png> [--loading-at <fraction>]: the first
// scene loads on the worker as with a window, and the loading screen is
// saved once the load has reported that much progress (default 0.5).
- (void)loadSceneWithLoadingShot {
    const os2::Scene scene = os2::sceneOf(processArguments());
    const bool cas = [sceneArgs() containsObject:@"--cas"];
    os2::loading::begin();
    fillLotEntryFromFlags(scene);
    os2::playLoadingScreen(os2::loadingScreenFor(gameDir() / "TSData/Res", scene, cas, gLotEntry),
                           [] { return os2::loading::shown(); });
    const float at = std::stof(argument(@"--loading-at").value_or("0.5"));
    const CFTimeInterval start = CACurrentMediaTime();
    gLoadJob.start([self] {
        @autoreleasepool {
            [self prepareScene];
        }
    });
    while (!gLoadJob.done() && os2::loading::reported() < at)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const double elapsed = CACurrentMediaTime() - start;
    const float shown = os2::loading::shown();
    const os2::RgbaImage img = os2::movieHostCapture(1280, 720, elapsed);
    const std::string file = *argument(@"--loading-shot");
    os2::writePng(file, img);
    std::printf("opensims2: wrote loading screen %s (%.2f s into the load, progress %.0f%%)\n", file.c_str(), elapsed,
                shown * 100);
    std::exception_ptr error = gLoadJob.collect();
    os2::loading::end();
    os2::finishLoadingScreen();
    if (error)
        std::rethrow_exception(error);
    std::printf("opensims2: scene loaded on the worker in %.2f s\n", CACurrentMediaTime() - start);
    [self finishScene];
}

// Loads the scene the arguments pick. Headless (--screenshot) each scene runs
// its steps; while one asks for another scene, that one is loaded in turn,
// and the last saves the screenshot and exits.
- (void)runScenes {
    for (bool first = true;; first = false) {
        if (first && argument(@"--loading-shot"))
            [self loadSceneWithLoadingShot];
        else
            [self loadScene];
        if (!gPendingScene || !headless())
            return;
        if (auto shot = argument(@"--screenshot"); shot && [sceneArgs() containsObject:@"--flow-shots"])
            [self captureScene:os2::numberedShot(*shot, ++_flowShot)];
        [self applyPendingScene];
    }
}

// A scene loads in two parts (docs/loading-mac.md): prepareScene builds its
// data, models, textures and VM, and may run on the worker (it touches no
// AppKit object: window titles and the view's colours wait for the other part);
// finishScene does the rest on the main thread (the view, the UI layers'
// attachment, the headless steps and screenshot). Headless both run here.
- (void)loadScene {
    [self prepareScene];
    [self finishScene];
}

- (void)prepareScene {
    [self ensureGameState];
    _scene = os2::sceneOf(processArguments());
    _cas = [sceneArgs() containsObject:@"--cas"]; // Create-a-Sim (app/cas_mode.hpp)
    gStepsUsed = 0;
    applyLighting(*_viewDelegate.renderer);
    if (_cas) {
        [self prepareCas:_view];
        return;
    }
    switch (_scene.kind) {
    case os2::Scene::Kind::MainMenu: [self prepareMainMenu:_scene]; break;
    case os2::Scene::Kind::Neighborhood: [self prepareHood:_view query:hoodFolder(_scene.hood).string()]; break;
    case os2::Scene::Kind::Lot:
        gHoodLot = os2::hoodLotPackage(gameDir() / "TSData/Res", hoodFolder(_scene.hood).string(), _scene.lot).string();
        gLotVacant = false;
        try {
            const auto hood = os2::hood::Neighborhood::load(hoodFolder(_scene.hood), false);
            if (const auto* l = hood.lot(_scene.lot))
                gLotVacant = l->families.empty();
        } catch (const std::exception& err) {
            os2::logOnce("lot vacancy", _scene.hood, err);
        }
        [self prepareLot];
        break;
    case os2::Scene::Kind::Other: [self prepareLot]; break;
    case os2::Scene::Kind::Cas: break; // handled above (_cas)
    }
}

- (void)finishScene {
    _view.clearColor = MTLClearColorMake(0.18, 0.22, 0.28, 1.0);
    applyAudioSettings(); // the options' volumes (the settings load with the first scene)
    // The scene's music state, as the game's music manager switches it
    // (app/lot_audio.hpp GameAudio): the opening music in the main menu,
    // NHood in the neighborhood (afresh, unless the opening music fades
    // into it), CAS in Create-a-Sim, the lot's (stereos only) on a lot.
    if (gGameAudio) {
        gGameAudio->setHeld(os2::moviePlaying()); // (the intro may still be playing)
        if (_cas) {
            gGameAudio->setMusic("CAS");
        } else if (_scene.kind == os2::Scene::Kind::MainMenu) {
            gGameAudio->setMusic("Splash");
        } else if (_scene.kind == os2::Scene::Kind::Neighborhood) {
            if (gGameAudio->music() != "Splash")
                gGameAudio->stopAllMusic();
            gGameAudio->setMusic("NHood");
        } else {
            gGameAudio->setMusic(argument(@"--music").value_or(""));
        }
        gGameAudio->setNeighborhoodAmbience(!_cas && _scene.kind == os2::Scene::Kind::Neighborhood);
    }
    if (_cas) {
        [self finishCas:_view];
        std::fflush(stdout);
        return;
    }
    switch (_scene.kind) {
    case os2::Scene::Kind::MainMenu: [self finishMainMenu:_scene]; break;
    case os2::Scene::Kind::Neighborhood: [self finishHood:_view]; break;
    case os2::Scene::Kind::Lot:
    case os2::Scene::Kind::Other: [self finishLot]; break;
    case os2::Scene::Kind::Cas: break;
    }
    std::fflush(stdout);
}

// Unloads the scene and sets the arguments of the one asked for (with the
// steps not used yet, headless).
- (void)applyPendingScene {
    const os2::Scene next = *gPendingScene;
    const size_t used = gStepsUsed;
    gPendingScene.reset();
    const std::vector<std::string> args = os2::sceneArguments(processArguments(), next, headless() ? used : 0);
    const CFTimeInterval start = CACurrentMediaTime();
    dwellHeadlessAudio(); // (headless: the scene's music heard for a while)
    unloadScene();
    if (gGameAudio)
        gGameAudio->beginScene();
    setSceneArguments(args);
    [_viewDelegate resetRenderer:_view];
    std::string line;
    for (size_t i = 1; i < args.size(); ++i)
        line += " " + args[i];
    std::printf("opensims2: scene switch (%.2f s to unload):%s\n", CACurrentMediaTime() - start, line.c_str());
    std::fflush(stdout);
}

// With a window: the scene asked for, in place of this one, loaded behind
// its loading screen.
- (void)switchScene {
    if (!gPendingScene || gSceneLoading)
        return;
    [self applyPendingScene];
    [self beginSceneLoad];
}

// Renders the current scene offscreen at 1280x720 and saves it.
- (void)captureScene:(const std::string&)file {
    const uint32_t w = 1280, h = 720;
    _view.camera->setViewport(float(w), float(h));
    _view.camera->snap();
    if (gSimulation)
        poseActors(*_viewDelegate.renderer, gHeadlessClock);
    else
        updateLotView(*_viewDelegate.renderer, false);
    const os2::FrameView fv{_view.camera->view(), _view.camera->projection(float(w) / float(h))};
    const MTLClearColor c = _view.clearColor;
    _viewDelegate.renderer->setReflections(reflectionsOn());
    const auto img = _viewDelegate.renderer->capture(w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), 1));
    os2::writePng(file, img);
    std::printf("opensims2: wrote %s\n", file.c_str());
    std::fflush(stdout);
}

// --menu (or no scene flag): the main menu, which chooses the neighborhood
// (app/main_menu.hpp). Headless: --menu-select <i> opens the preview of
// neighborhood i, and the steps click it.
- (void)prepareMainMenu:(const os2::Scene&)scene {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    renderer.hideTestScene();
    [_view setCamera:std::make_unique<os2::LotCamera>(loadLiveCamera())];
    gCamera = _view.camera;
    std::vector<fs::path> dirs;
    for (const auto& prefix : gUser->shipped())
        if (prefix != "Tutorial")
            dirs.push_back(gUser->neighborhoodForReading(prefix));
    gMenu = std::make_unique<os2::MainMenu>(*gIndex, gameDir() / "TSData/Res", os2::readMainMenuHoods(dirs),
                                            scene.fromNeighborhood);
    perfLap("menu: scripts");
    gMenu->setEpisodes(os2::loadTutorialEpisodes(
        gameDir() / "TSData/Res", gUser && gUser->hasCopy("Tutorial") ? gUser->neighborhoodsDir() / "Tutorial" : fs::path()));
    _menuDirs = dirs;
}

- (void)finishMainMenu:(const os2::Scene&)scene {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    if (gGameAudio) // the buttons' click sounds
        gMenu->setSoundPlayer([](const std::string& name) { gGameAudio->playUi(name); });
    const std::vector<fs::path>& dirs = _menuDirs;
    gMenu->attach((__bridge MTL::Device*)_view.device, uint32_t(_view.colorPixelFormat),
                  uint32_t(_view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gMenu)
            gMenu->encode(enc, w, h, gUiScale);
    });
    _view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    _window.title = @"opensims2";
    const float w = 1280, h = 720;
    gMenu->setCanvas(int(w), int(h));
    for (size_t i = 0; i < dirs.size(); ++i) {
        const auto r = gMenu->thumbnailRect(int(i));
        std::printf("opensims2: main menu: %s at (%.0f, %.0f)-(%.0f, %.0f)\n", dirs[i].filename().string().c_str(),
                    r[0], r[1], r[2], r[3]);
    }
    if (!gMenu->ok())
        std::fprintf(stderr, "opensims2: main menu: scripts missing\n");
    if (!headless())
        return;
    if (auto sel = argument(@"--menu-select"))
        gMenu->preview(std::stoi(*sel));
    // --menu-learn <i>: "Aprenda a Jogar" open with episode i selected.
    if (auto ep = argument(@"--menu-learn")) {
        gMenu->openLearn(std::stoi(*ep));
        for (int i = 0;; ++i) {
            const auto r = gMenu->episodeRect(i);
            if (r[2] <= r[0] && i > 8)
                break;
            if (r[2] > r[0])
                std::printf("opensims2: main menu: episode %d at (%.0f, %.0f)-(%.0f, %.0f)\n", i, r[0], r[1], r[2], r[3]);
        }
    }
    const auto steps = os2::flowSteps(processArguments());
    for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
        gStepsUsed = k + 1;
        if (steps[k].kind == os2::FlowStep::Kind::Click)
            pressMainMenu(simd_make_float2(steps[k].x, steps[k].y), w, h);
    }
    if (gPendingScene)
        return;
    [self captureScene:*argument(@"--screenshot")];
    std::exit(0);
}

// A lot (--lot, or --hood <N> --hood-lot <id>), or the test objects and Sims.
- (void)prepareLot {
    OS2View* view = _view;
    os2::CameraDef liveCamera = loadLiveCamera();
    // --camera-scale <k>: every zoom distance times k (close-ups of Sims below the closest level).
    if (auto k = argument(@"--camera-scale"))
        for (float& d : liveCamera.distances)
            d *= std::stof(*k);
    [view setCamera:std::make_unique<os2::LotCamera>(liveCamera)];
    gCamera = view.camera;
    os2::ResourceIndex& index = *gIndex;
    { // (indented as it was inside the start-up's try block)
        const simd_float3 objects = loadObjects(*_viewDelegate.renderer, index);
        perfLap("loadObjects");
        const auto skinned = loadSkinned(*_viewDelegate.renderer, index);
        perfLap("loadSkinned");
        const auto uiCanvas = loadUi(*_viewDelegate.renderer, index, view);
        perfLap("loadUi");
        // --zoom <levels> / --orientation <steps>: start the camera that many steps from its default.
        if (auto z = argument(@"--zoom"))
            view.camera->zoomBy(std::stoi(*z));
        if (auto o = argument(@"--orientation"))
            view.camera->rotateBy(std::stoi(*o));
        // --tilt <degrees>: the pitch moved by that much, inside the zoom
        // level's range (negative: lower, toward a side view).
        if (auto t = argument(@"--tilt"))
            view.camera->tilt(std::stof(*t));
        view.camera->snap();
        // The lot's cutaway walls follow the starting view direction.
        const simd_float3 look = view.camera->pivot() - view.camera->eye();
        simd_float3 pivot = loadLot(*_viewDelegate.renderer, index, simd_make_float2(look.x, look.y))
                                .value_or(skinned.value_or(objects));
        // --pivot <x,y[,z]>: look at that point instead (tiles; at the same
        // height, or z: the floor the camera is on follows it).
        float px, py, pz;
        if (auto p = argument(@"--pivot")) {
            const int n = std::sscanf(p->c_str(), "%f,%f,%f", &px, &py, &pz);
            if (n >= 2)
                pivot.x = px, pivot.y = py;
            if (n == 3)
                pivot.z = pz;
        }
        view.camera->setPivot(pivot);
        view.camera->snap();
        _uiCanvas = uiCanvas;
        _pivot = pivot;
    }
}

- (void)finishLot {
    OS2View* view = _view;
    const auto uiCanvas = _uiCanvas;
    const simd_float3 pivot = _pivot;
    { // (indented as it was inside the start-up's try block)
        // --pie <object> [--pie-select N]: open an object's pie menu for the
        // selected Sim (--sim-id <id|name>) and queue entry N; --click x,y
        // (repeatable) clicks like the mouse. Before --run-vm's ticks.
        const uint32_t shotW = uiCanvas ? uiCanvas->first : 1280, shotH = uiCanvas ? uiCanvas->second : 720;
        if (gPie) {
            gPie->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                         uint32_t(view.depthStencilPixelFormat));
            if (gLive)
                gLive->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                              uint32_t(view.depthStencilPixelFormat));
            if (gBuy)
                gBuy->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                             uint32_t(view.depthStencilPixelFormat));
            if (gBuild)
                gBuild->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                               uint32_t(view.depthStencilPixelFormat));
            if (gOptions)
                gOptions->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                 uint32_t(view.depthStencilPixelFormat));
            if (gTutorial)
                gTutorial->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                                  uint32_t(view.depthStencilPixelFormat));
            _viewDelegate.renderer->setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
                if (gUi)
                    gUi->encode(enc, w, h);
                const bool optionsOver = gLive && gLive->optionsOpen(); // the options replace Buy/Build's panel
                if (gBuy && !optionsOver) // under the puck
                    gBuy->encode(enc, w, h, gUiScale);
                if (gBuild && !optionsOver)
                    gBuild->encode(enc, w, h, gUiScale);
                if (gOptions) // under the puck, as Buy mode's panel (its tab rises behind the puck)
                    gOptions->encode(enc, w, h, gUiScale);
                if (gLive)
                    gLive->encode(enc, w, h, gUiScale);
                if (gTutorial) // the tutorial's window and arrow over the panels
                    gTutorial->encode(enc, w, h, gUiScale);
                gPie->encode(enc, w, h, gUiScale);
            });
            const bool headless = argument(@"--screenshot").has_value();
            const float cw = headless ? float(shotW) : float(view.bounds.size.width);
            const float ch = headless ? float(shotH) : float(view.bounds.size.height);
            if (gTutorial && headless) { // the tutorial presses and points at controls laid out at the shot's size
                gLive->setCanvas(int(cw), int(ch));
                gTutorial->setCanvas(int(cw), int(ch));
                pollTutorial(gHeadlessClock);
            }
            if (auto q = argument(@"--pie")) {
                os2::vm::ObjectId id = os2::vm::findInteractionObject(gPie->vm(), *q);
                if (!id && !q->empty()) // a Sim by its character name (N001_User00005)
                    id = os2::vm::findSim(gPie->vm(), *q);
                auto at = id ? gPie->anchor(id) : std::nullopt;
                if (id && !at) // a Sim: its pick box comes with the first frame; until then where it stands, chest high
                    for (const auto& s : gLotSims)
                        if (os2::vm::ObjectId(s.id) == id && s.actor) {
                            const os2::Vec3 sp = s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                                                s.actor->transform->t[2]}
                                                                    : s.actor->position;
                            at = simd_make_float3(sp[0], sp[1], sp[2] + 1.2f);
                        }
                if (!at)
                    throw std::runtime_error("--pie: no drawn object \"" + *q + "\" with interactions");
                if (!argument(@"--pivot")) {
                    view.camera->setPivot(simd_make_float3(at->x, at->y, pivot.z));
                    view.camera->snap();
                }
                const simd_float4 c = simd_mul(simd_mul(view.camera->projection(cw / ch), view.camera->view()),
                                               simd_make_float4(at->x, at->y, at->z, 1));
                const simd_float2 p = simd_make_float2((c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
                if (!gPie->open(id, p))
                    std::printf("opensims2: object %d: no interactions available\n", id);
                for (size_t i = 0; i < gPie->entries().size(); ++i)
                    std::printf("opensims2: pie [%zu] %s\n", i, gPie->entries()[i].name.c_str());
                if (auto sel = argument(@"--pie-select"); sel && !gPie->select(std::stoul(*sel)))
                    throw std::runtime_error("--pie-select: no entry " + *sel);
                // With --run-vm the menu closes after the pick, as a click does; else it stays, highlighted.
                if (argument(@"--pie-select") && argument(@"--run-vm"))
                    gPie->close();
            }
            // The --click / --right-click / --wait steps, in order (app/game_flow.hpp):
            // a step that leaves the lot leaves the rest to the next scene.
            const auto steps = os2::flowSteps(processArguments());
            followSelectedSim(); // the camera on the Sim's floor first, as the first frame does (a floor button then holds)
            if (!steps.empty()) // and there already, as the frames before a click leave it (the clicks pick what the shot shows)
                view.camera->snap();
            for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
                gStepsUsed = k + 1;
                const os2::FlowStep& st = steps[k];
                if (st.kind == os2::FlowStep::Kind::Wait) {
                    runTicksHeadless(st.ticks, _viewDelegate.renderer);
                    pollFlowDialog();
                    continue;
                }
                if (st.kind == os2::FlowStep::Kind::Key) { // --key space
                    if (st.text.rfind("hold-", 0) == 0) { // --key hold-<left|right|up|down|a|d|w|s|comma|period>:<seconds>
                        static const std::map<std::string, unsigned short> kKeys{
                            {"left", kLeft}, {"right", kRight}, {"up", kUp}, {"down", kDown}, {"a", kA},
                            {"d", kD},       {"w", kW},         {"s", kS},   {"comma", kComma}, {"period", kPeriod}};
                        const size_t colon = st.text.find(':');
                        const std::string name = st.text.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
                        const double secs = colon == std::string::npos ? 0.5 : std::atof(st.text.c_str() + colon + 1);
                        if (auto k = kKeys.find(name); k != kKeys.end()) {
                            const simd_float3 p0 = view.camera->pivot();
                            const float y0 = view.camera->yaw();
                            [view holdKey:k->second seconds:secs];
                            const simd_float3 p1 = view.camera->pivot();
                            std::printf("opensims2: key %s held %.2f s: pivot (%.2f, %.2f) -> (%.2f, %.2f), yaw %.1f -> %.1f\n",
                                        name.c_str(), secs, p0.x, p0.y, p1.x, p1.y, y0, view.camera->yaw());
                        }
                        continue;
                    }
                    if (st.text == "space")
                        selectNextFamilySim();
                    if (st.text.rfind("walls-", 0) == 0) // --key walls-<0..3>: the puck's wall fan (down, cutaway, up, roof)
                        setWallMode(std::atoi(st.text.c_str() + 6));
                    if (st.text == "pageup" || st.text == "pagedown") { // as keyDown: the floor shown
                        changeFloor(st.text == "pageup" ? 1 : -1);
                        if (gCamera) // the camera on that floor now, as the frames before a click would leave it
                            gCamera->snap();
                        std::printf("opensims2: key %s: level shown %d\n", st.text.c_str(), viewLevel());
                    }
                    if (st.text == "escape") { // as keyDown: a pie menu or an option window first, else leave the lot
                        const bool pieWasOpen = gPie && gPie->isOpen();
                        if (pieWasOpen)
                            gPie->close();
                        std::printf("opensims2: key escape (pie %s, option window %d)\n", pieWasOpen ? "closed" : "was not open",
                                    gOptions ? gOptions->window() : 0);
                        if (pieWasOpen)
                            continue;
                        if (escapeOptions())
                            continue;
                        if (gHoodLot && !(gPie && gPie->dialogOpen()))
                            leaveLot();
                    }
                    continue;
                }
                if (st.kind == os2::FlowStep::Kind::Drag || st.kind == os2::FlowStep::Kind::Hover)
                    continue; // after the build steps (below)
                if (st.kind == os2::FlowStep::Kind::TutorialAuto) { // --tutorial-auto <turns>
                    // Each turn: a click on what the arrow points at or Next;
                    // else what a step waiting on the Sim waits for (a Go
                    // Here, an object bought, a job: vm::tutorialAutoAct);
                    // else time passes (the first step shows after some
                    // ticks, a skill grows). Stops after ~3000 idle ticks.
                    int idle = 0;
                    // The trees' own dialogs (the job offers) get their first
                    // button, as s2pkg tutorial --auto, unless --dialog-answer says.
                    int& autoAnswer = gPie->vm().autoDialogAnswer;
                    const int answerBefore = autoAnswer;
                    if (autoAnswer < 0)
                        autoAnswer = 0;
                    for (long turn = 0; gTutorial && turn < st.ticks && !gPendingScene && idle < 50; ++turn) {
                        if (st.entry > 0 && gPie->vm().tutorial.progress.visible && gPie->vm().tutorial.progress.step >= st.entry)
                            break; // --tutorial-auto <turns>:<step>
                        gTutorial->setCanvas(int(cw), int(ch));
                        gLive->setCanvas(int(cw), int(ch));
                        if (gBuy)
                            gBuy->setCanvas(int(cw), int(ch));
                        if (gBuild)
                            gBuild->setCanvas(int(cw), int(ch));
                        pollTutorial(gHeadlessClock);
                        if (gTutorial->autoClick()) {
                            idle = 0;
                            runTicksHeadless(40);
                            continue;
                        }
                        if (idle % 10 == 1)
                            if (const std::string done = os2::vm::tutorialAutoAct(gPie->vm(), gIndex.get()); !done.empty())
                                std::printf("opensims2: tutorial: %s\n", done.c_str());
                        ++idle;
                        runTicksHeadless(60);
                    }
                    autoAnswer = answerBefore;
                    continue;
                }
                if (st.kind == os2::FlowStep::Kind::Pie) { // --pie-step <object>:<entry index or name>
                    const os2::vm::ObjectId id = os2::vm::findInteractionObject(gPie->vm(), st.object);
                    gPie->setCanvas(int(cw), int(ch));
                    if (!id || !gPie->open(id, simd_make_float2(cw / 2, ch / 2))) {
                        std::printf("opensims2: pie step: no pie menu on \"%s\"\n", st.object.c_str());
                        continue;
                    }
                    if (st.text == "open") { // --pie-step <object>:open leaves the menu open for the shot
                        auto at = gPie->anchor(id);
                        if (!at) // a Sim: where it is drawn, chest high (as --pie)
                            for (const auto& s : gLotSims)
                                if (os2::vm::ObjectId(s.id) == id && s.actor) {
                                    const os2::Vec3 sp = s.actor->transform ? os2::Vec3{s.actor->transform->t[0], s.actor->transform->t[1],
                                                                                        s.actor->transform->t[2]}
                                                                            : s.actor->position;
                                    at = simd_make_float3(sp[0], sp[1], sp[2] + 0.6f);
                                }
                        if (at) { // over the object, the camera on it (as --pie)
                            view.camera->setPivot(simd_make_float3(at->x, at->y, view.camera->pivot().z));
                            view.camera->snap();
                            const simd_float4 c = simd_mul(simd_mul(view.camera->projection(cw / ch), view.camera->view()),
                                                           simd_make_float4(at->x, at->y, at->z, 1));
                            gPie->close();
                            gPie->open(id, simd_make_float2((c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch));
                        }
                        for (size_t e = 0; e < gPie->entries().size(); ++e)
                            std::printf("opensims2: pie step: [%zu] %s\n", e, gPie->entries()[e].name.c_str());
                        continue;
                    }
                    bool picked = false;
                    for (size_t e = 0; e < gPie->entries().size() && !picked; ++e)
                        if (st.entry >= 0 ? e == size_t(st.entry) : gPie->entries()[e].name.find(st.text) != std::string::npos) {
                            std::printf("opensims2: pie step: %s on object %d\n", gPie->entries()[e].name.c_str(), id);
                            picked = gPie->select(e);
                        }
                    if (!picked)
                        for (size_t e = 0; e < gPie->entries().size(); ++e)
                            std::printf("opensims2: pie step: [%zu] %s (not \"%s\")\n", e, gPie->entries()[e].name.c_str(), st.text.c_str());
                    gPie->close();
                    continue;
                }
                const float clickX = st.x, clickY = st.y;
                // --right-click x,y: the right button (the live panel's thumbnails follow a Sim).
                const bool right = st.kind == os2::FlowStep::Kind::RightClick;
                const simd_float2 p = simd_make_float2(clickX, clickY);
                gPie->setCanvas(int(cw), int(ch));
                if (gTutorial && !right) { // the tutorial's window (top right) first
                    gTutorial->setCanvas(int(cw), int(ch));
                    gLive->setCanvas(int(cw), int(ch)); // (its click trees find the controls laid out)
                    if (gBuy)
                        gBuy->setCanvas(int(cw), int(ch));
                    if (gBuild)
                        gBuild->setCanvas(int(cw), int(ch));
                    if (gTutorial->press(p))
                        continue;
                }
                if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p)) {
                    pollFlowDialog();
                    continue;
                }
                if (!right && pressOptions(p, cw, ch)) // the option windows float over everything
                    continue;
                if (gLive && !right) {
                    gLive->setCanvas(int(cw), int(ch));
                    if (gLive->overModeButton(p) && gLive->press(p)) {
                        gLive->screen(int(cw), int(ch)); // no frame lays it out again before the next step
                        continue;
                    }
                }
                if (gLive && !right && gLive->optionsOpen() && gLive->press(p)) { // over Buy/Build's panels
                    gLive->screen(int(cw), int(ch));
                    continue;
                }
                if (gBuy && gBuy->active() && !(gLive && gLive->optionsOpen())) {
                    gBuy->setCanvas(int(cw), int(ch));
                    if (gBuy->press(p))
                        continue;
                }
                if (gBuild && gBuild->active() && !(gLive && gLive->optionsOpen())) {
                    gBuild->setCanvas(int(cw), int(ch));
                    if (gBuild->press(p))
                        continue;
                }
                if (gLive) {
                    gLive->setCanvas(int(cw), int(ch));
                    const bool onPanel = gLive->press(p, right);
                    if (right && !onPanel && gPie) { // a right click on a Sim of the family selects it
                        const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
                        const simd_float4 q =
                            simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / cw - 1, 1 - 2 * p.y / ch, 0.5f, 1));
                        updateLotView(*_viewDelegate.renderer, true);
                        selectFamilySim(gPie->pick(view.camera->eye(), simd_normalize(q.xyz / q.w - view.camera->eye())));
                    }
                    if (onPanel || right) {
                        gLive->screen(int(cw), int(ch)); // no frame lays it out again before the next step
                        if (gOptions && !gLive->optionsOpen())
                            gOptions->closeWindow();
                        continue;
                    }
                }
                if (pressOptions(p, cw, ch)) // under the puck
                    continue;
                const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
                const simd_float4 q =
                    simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / cw - 1, 1 - 2 * p.y / ch, 0.5f, 1));
                const simd_float3 eye = view.camera->eye();
                if (gBuy && gBuy->active()) { // the object tools, not the pie menu
                    gBuy->click(eye, simd_normalize(q.xyz / q.w - eye), p);
                    continue;
                }
                // The view as a frame would leave it (what the cut walls hide), the Sims posed where they stand.
                if (gSimulation)
                    poseActors(*_viewDelegate.renderer, gHeadlessClock);
                updateLotView(*_viewDelegate.renderer, false);
                if (gObjectView)
                    gObjectView->update();
                updateLotView(*_viewDelegate.renderer, true);
                keepSelectedSimHome();
                const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
                const os2::vm::Object* o = gPie->vm().object(id);
                std::printf("opensims2: click (%g, %g) picked object %d (%s)\n", clickX, clickY, id,
                            o && o->definition ? o->definition->fileName.c_str() : "nothing");
                if (std::getenv("OS2_DEBUG_PICK"))
                    std::printf("opensims2: click eye (%.3f, %.3f, %.3f) canvas %gx%g\n", eye.x, eye.y, eye.z, cw, ch);
                if (id)
                    openClicked(id, eye, simd_normalize(q.xyz / q.w - eye), p);
                if (!id) // the ground: Go Here
                    openGoHere(eye, simd_normalize(q.xyz / q.w - eye), p);
                for (size_t e = 0; e < gPie->entries().size(); ++e)
                    std::printf("opensims2: pie [%zu] %s\n", e, gPie->entries()[e].name.c_str());
            }
            // OS2_PICK_MAP=<png>: what a click would pick at every 4th pixel (OS2_PICK_MAP_STEP), an
            // object a colour (a look at the picking against the picture).
            if (const char* mapFile = std::getenv("OS2_PICK_MAP")) {
                if (gSimulation) // the Sims posed as drawn (their triangles are picked)
                    poseActors(*_viewDelegate.renderer, gHeadlessClock);
                updateLotView(*_viewDelegate.renderer, false);
                if (gObjectView)
                    gObjectView->update();
                updateLotView(*_viewDelegate.renderer, true);
                const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
                const simd_float4x4 inv = simd_inverse(m);
                const simd_float3 eye = view.camera->eye();
                os2::RgbaImage img;
                img.width = uint32_t(cw), img.height = uint32_t(ch);
                img.pixels.assign(size_t(img.width) * img.height * 4, 0);
                const CFTimeInterval t0 = CACurrentMediaTime();
                size_t rays = 0;
                std::map<int, int> findIds;
                if (const char* f = std::getenv("OS2_PICK_FIND"))
                    for (std::string_view v{f}; !v.empty();) {
                        const size_t c = v.find(',');
                        findIds[std::atoi(std::string(v.substr(0, c)).c_str())] = 0;
                        v = c == std::string_view::npos ? std::string_view{} : v.substr(c + 1);
                    }
                const uint32_t step = std::getenv("OS2_PICK_MAP_STEP") ? uint32_t(std::max(1, std::atoi(std::getenv("OS2_PICK_MAP_STEP")))) : 4;
                for (uint32_t y = 0; y < img.height; y += step)
                    for (uint32_t x = 0; x < img.width; x += step) {
                        const simd_float4 q = simd_mul(inv, simd_make_float4(2 * float(x) / cw - 1, 1 - 2 * float(y) / ch, 0.5f, 1));
                        const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
                        ++rays;
                        if (!id)
                            continue;
                        if (findIds.count(int(id)) && findIds[int(id)]++ == 0) // OS2_PICK_FIND=id,id: where each is picked first
                            std::printf("opensims2: pick map: object %d at (%u, %u)\n", id, x, y);
                        const uint32_t h = uint32_t(id) * 2654435761u;
                        for (uint32_t dy = 0; dy < step && y + dy < img.height; ++dy)
                            for (uint32_t dx = 0; dx < step && x + dx < img.width; ++dx) {
                                uint8_t* px = &img.pixels[(size_t(y + dy) * img.width + x + dx) * 4];
                                px[0] = uint8_t(h >> 24), px[1] = uint8_t(h >> 16), px[2] = uint8_t(h >> 8), px[3] = 255;
                            }
                    }
                std::printf("opensims2: pick map: eye (%.3f, %.3f, %.3f) canvas %gx%g\n", eye.x, eye.y, eye.z, cw, ch);
                for (const auto& [fid, n] : findIds) // where each one's origin projects
                    if (gObjectView)
                        if (const auto at = gObjectView->placement(int16_t(fid))) {
                            const simd_float4 c = simd_mul(m, simd_make_float4(at->t[0], at->t[1], at->t[2], 1));
                            std::printf("opensims2: pick map: object %d origin (%.2f, %.2f, %.2f) on screen (%.1f, %.1f)\n", fid, at->t[0], at->t[1], at->t[2],
                                        (c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
                        }
                std::printf("opensims2: pick map: %zu rays, %.3f ms each\n", rays, (CACurrentMediaTime() - t0) * 1000 / double(rays));
                os2::writePng(mapFile, img);
            }
            // --ui-hover x,y: the mouse over the live panel (a queue icon's name).
            float hx, hy;
            if (auto h = argument(@"--ui-hover"); gLive && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gLive->setCanvas(int(cw), int(ch));
                gLive->hover(simd_make_float2(hx, hy));
            }
            runBuySteps();
            runBuildSteps();
            // --drag / --hover: Build mode's mouse on the lot, after its tool is chosen.
            for (const os2::FlowStep& st : os2::flowSteps(processArguments()))
                [&] {
                    if (st.kind != os2::FlowStep::Kind::Drag && st.kind != os2::FlowStep::Kind::Hover)
                        return;
                    // --drag x0,y0,x1,y1[,ctrl|hold] / --hover x,y: Build mode's mouse on the lot.
                    if (!gBuild || !gBuild->active())
                        return;
                    const simd_float4x4 m = simd_mul(view.camera->projection(cw / ch), view.camera->view());
                    const simd_float3 eye = view.camera->eye();
                    auto ray = [&](float x, float y) {
                        const simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * x / cw - 1, 1 - 2 * y / ch, 0.5f, 1));
                        return simd_normalize(q.xyz / q.w - eye);
                    };
                    auto where = [&](const char* what) {
                        if (auto p = gBuild->pointer()) {
                            std::printf("opensims2: build mouse %s at lot (%.2f, %.2f)\n", what, p->x, p->y);
                            if (std::getenv("OS2_DEBUG_PICK")) {
                                const simd_float3 e = view.camera->eye();
                                std::printf("opensims2: eye (%.3f, %.3f, %.3f) pivot (%.3f, %.3f, %.3f)\n", e.x, e.y, e.z,
                                            view.camera->pivot().x, view.camera->pivot().y, view.camera->pivot().z);
                                for (simd_float3 w : {simd_make_float3(p->x, p->y, gBuild->pickHeight(*p))}) { // where the cursor is drawn
                                    const simd_float4 c = simd_mul(m, simd_make_float4(w, 1));
                                    std::printf("opensims2: world (%.1f, %.1f, %.1f) on screen (%.1f, %.1f)\n", w.x, w.y, w.z,
                                                (c.x / c.w + 1) / 2 * cw, (1 - c.y / c.w) / 2 * ch);
                                }
                            }
                        }
                    };
                    gBuild->setCanvas(int(cw), int(ch));
                    if (st.kind == os2::FlowStep::Kind::Hover) {
                        gBuild->pointAt(eye, ray(st.x, st.y));
                        where("over");
                    } else {
                        gBuild->mouseDown(eye, ray(st.x, st.y), st.control, false);
                        where("down");
                        gBuild->mouseDrag(eye, ray(st.x1, st.y1));
                        where("dragged");
                        if (!st.hold) {
                            gBuild->mouseUp(eye, ray(st.x1, st.y1));
                            std::printf("opensims2: build drag: %s\n", gBuild->message().c_str());
                        }
                    }
                    gBuild->update();
                    return;
                }();
            if (auto h = argument(@"--ui-hover"); gBuy && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gBuy->setCanvas(int(cw), int(ch));
                gBuy->hover(simd_make_float2(hx, hy));
            }
            if (auto h = argument(@"--ui-hover"); gBuild && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gBuild->setCanvas(int(cw), int(ch));
                gBuild->hover(simd_make_float2(hx, hy));
            }
            std::fflush(stdout);
        }
        attachLetterbox(*_viewDelegate.renderer, (__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                        uint32_t(view.depthStencilPixelFormat));
        // --cinematic <scene>[@id[,id...]][:mirror][:house]: a cinematic with those actors (in the CINE's
        // order; default: the lot's Sims for its Sim actors), started at once.
        if (auto cine = argument(@"--cinematic")) {
            std::string spec = *cine;
            bool mirror = false, house = false;
            if (spec.ends_with(":house")) // "Show Entire House" (flag 0x08)
                house = true, spec.resize(spec.size() - 6);
            if (spec.ends_with(":mirror"))
                mirror = true, spec.resize(spec.size() - 7);
            const auto at = spec.find('@');
            const std::string scene = spec.substr(0, at);
            std::vector<int16_t> actors;
            if (at != std::string::npos) {
                std::stringstream ids(spec.substr(at + 1));
                for (std::string id; std::getline(ids, id, ',');)
                    actors.push_back(int16_t(std::stoi(id)));
            } else if (gIndex) {
                try {
                    const os2::CinematicCamera probe(*gIndex, scene);
                    size_t next = 0;
                    for (const auto& a : probe.scene().actors) {
                        const bool sim = a.sgObjectName.find("skel") != std::string::npos ||
                                         a.sgObjectName.rfind("sim", 0) == 0;
                        int16_t id = sim && !a.autoCreate && next < gLotSims.size() ? gLotSims[next++].id : 0;
                        // An object actor ("mailbox::1"): a lot object of that model.
                        if (!sim && !a.autoCreate) {
                            const std::string model = a.sgObjectName.substr(0, a.sgObjectName.find(':'));
                            for (const auto& [oid, p] : gPlacedObjects)
                                if (!id && !p.contained && !model.empty() &&
                                    strncasecmp(p.cres.c_str(), model.c_str(), model.size()) == 0)
                                    id = oid;
                        }
                        actors.push_back(id);
                    }
                } catch (const std::exception& err) {
                    os2::logOnce("cinematic actors", scene, err);
                }
            }
            startCinematic(*_viewDelegate.renderer, scene, actors, gLotSims.empty() ? 0 : gLotSims.front().id, mirror, true,
                           std::numeric_limits<double>::infinity(), true, house); // starts at the first update
        }
        if (gPendingScene)
            return; // headless: the steps left the lot (runScenes loads the next scene)
        if (const char* want = std::getenv("OS2_LIST_OBJECTS"); want && gSimulation) // test aid: objects by name, where they are
            for (const auto& [id, o] : gSimulation->machine().objects())
                if (o.definition && o.definition->fileName.find(want) != std::string::npos)
                    std::printf("opensims2: object %d (%s) at %.1f %.1f level %d, container %d\n", id,
                                o.definition->fileName.c_str(), o.position.x, o.position.y, o.position.level,
                                o.data[os2::vm::objdata::ContainerId]);
        if (std::getenv("OS2_LIST_QUEUES") && gSimulation) // test aid: each Sim's queue
            for (const auto& [id, o] : gSimulation->machine().objects())
                if (o.person) {
                    std::printf("opensims2: Sim %d queue (active %d):", id, o.activeInteraction);
                    for (const auto& q : o.queue)
                        std::printf(" [%s obj %d prio %d%s]", q.name.c_str(), q.object, q.priority, q.cancelled ? " cancelled" : "");
                    std::printf("\n");
                }
        // --screenshot <file.png>: render one frame offscreen, save it and quit.
        if (auto shot = argument(@"--screenshot")) {
            const double base = gHeadlessClock; // after the --wait steps
            // --run-vm <ticks>: the moment after that many VM ticks; --frames
            // <n>: n shots spread up to it (out-1.png ... out-n.png).
            const auto runVm = argument(@"--run-vm");
            const double time = std::stod(argument(@"--time").value_or(
                runVm ? std::to_string(std::stod(*runVm) / os2::LotSimulation::kTicksPerSecond) : std::string("0")));
            const int frames = std::max(1, std::stoi(argument(@"--frames").value_or("1")));
            poseActors(*_viewDelegate.renderer, base);
            std::vector<simd_float2> dialogClicks;
            size_t dialogClick = 0;
            std::string dialogTyped;
            NSArray<NSString*>* shotArgs = sceneArgs();
            for (NSUInteger i = 1; i + 1 < shotArgs.count; ++i) {
                float x, y;
                if ([shotArgs[i] isEqualToString:@"--dialog-click"] && std::sscanf(shotArgs[i + 1].UTF8String, "%f,%f", &x, &y) == 2)
                    dialogClicks.push_back(simd_make_float2(x, y));
                else if ([shotArgs[i] isEqualToString:@"--dialog-click"] && [shotArgs[i + 1] isEqualToString:@"return"])
                    dialogClicks.push_back(simd_make_float2(-1, -1)); // the Return key
                else if ([shotArgs[i] isEqualToString:@"--dialog-click"] && [shotArgs[i + 1] isEqualToString:@"escape"])
                    dialogClicks.push_back(simd_make_float2(-2, -2)); // the Esc key
                if ([shotArgs[i] isEqualToString:@"--dialog-type"])
                    dialogTyped = shotArgs[i + 1].UTF8String;
            }
            uint32_t dialogTypedFor = 0;
            double perfSim = 0, perfCapture = 0, perfWorst = 0;
            // --frames-from <seconds>: the shots spread from that moment instead of the start
            // (the VM still runs from the start: a scene late in a long run).
            const double from = std::clamp(std::stod(argument(@"--frames-from").value_or("0")), 0.0, time);
            double previous = 0;
            for (int f = 1; f <= frames; ++f) {
                const CFTimeInterval frameStart = CACurrentMediaTime();
                const double at = frames > 1 && from > 0 ? from + (time - from) * (f - 1) / (frames - 1) : time * f / frames;
                // The VM in small steps, so that the Sims' turning is followed.
                for (double t = previous + 1.0 / 30; gSimulation && t < at; t += 1.0 / 30) {
                    const double scene = gSimulation->update(base + t); // the scene clock (game speed)
                    adoptVmSims(*_viewDelegate.renderer);            // a Sim the VM just made (its actor for the scene)
                    updateCinematic(*_viewDelegate.renderer, scene); // a scene starts when the tree asks
                    // The lights follow the VM clock as in a live run (the
                    // day/night switch and its crossfade start when the clock gets there).
                    if (gLighting && gSimulation->ticks() % 30 == 0)
                        gLighting->update(scene);
                    if (gLive)
                        gLive->tick(base + t);
                    // The Sims' bones at this step, so that balloons and other
                    // effects on their bones follow the animations between shots.
                    for (auto& sim : gLotSims)
                        sim.actor->update(scene);
                    for (os2::SimActor* a : simsReachingLaterSims())
                        a->update(scene);
                    // The objects too, as a live frame does (poseActors): a car
                    // driving in, a chair pulled out, placed where the VM view
                    // puts them, so that the seats on them are not left where
                    // the last shot had them (the maid's seat stayed at the
                    // road's end while the car came in).
                    for (auto& [id, a] : gObjectActors)
                        if (a)
                            a->update(scene);
                    // OS2_DEBUG_LAYERS=1: each game hour, the object actor with
                    // the most animation layers (the held ones merge: AnimPlayer).
                    if (std::getenv("OS2_DEBUG_LAYERS") && gSimulation && gSimulation->ticks() % 1800 == 0) {
                        uint64_t& lastHour = gScene.layersReportedAt;
                        if (gSimulation->ticks() != lastHour) {
                            lastHour = gSimulation->ticks();
                            int most = -1;
                            size_t count = 0, total = 0;
                            for (const auto& [id, a] : gObjectActors)
                                if (a) {
                                    total += a->player.layerCount();
                                    if (a->player.layerCount() > count || most < 0) {
                                        count = a->player.layerCount();
                                        most = id;
                                    }
                                }
                            std::printf("opensims2: layers: tick %llu most %zu (object %d), %zu in %zu object actors\n",
                                        (unsigned long long)lastHour, count, most, total, gObjectActors.size());
                        }
                    }
                    if (gObjectView)
                        gObjectView->update();
                    sampleDrawnPlaces(scene);
                    // OS2_DEBUG_DOORS=1: the doors' leaves (r_door_rot, l_door_rot
                    // about Z, degrees) as they turn by 10 degrees or come back closed.
                    if (std::getenv("OS2_DEBUG_DOORS")) {
                        auto& last = gScene.doorLeaves; // (by object ID: per scene)
                        for (auto& [id, actor] : gObjectActors) {
                            if (!actor)
                                continue;
                            const int r = actor->skin.skeleton.find("r_door_rot"), l = actor->skin.skeleton.find("l_door_rot");
                            if (r < 0)
                                continue;
                            // OS2_DEBUG_DOORS=2: as drawn (the last update()'s bones, the
                            // leaf's turn from its parent), without starting the cues here.
                            static const bool drawn = std::string(std::getenv("OS2_DEBUG_DOORS")) == "2";
                            std::vector<os2::Xform> pose;
                            if (drawn) {
                                if (actor->bones.size() != actor->skin.skeleton.bones.size())
                                    continue;
                                pose = actor->bones;
                                for (size_t b = 0; b < pose.size(); ++b)
                                    if (const int parent = actor->skin.skeleton.bones[b].parent; parent >= 0)
                                        pose[b] = actor->bones[size_t(parent)].inverse().then(actor->bones[b]);
                            } else {
                                actor->startCues(scene);
                                pose = actor->player.pose(scene);
                            }
                            auto ang = [&](int b) { return b < 0 ? 0.0f : 2 * std::atan2(pose[size_t(b)].q.z, pose[size_t(b)].q.w) * 57.2958f; };
                            const std::pair<float, float> a{ang(r), ang(l)};
                            auto& prev = last[id];
                            if (std::abs(prev.first - a.first) > 10 || std::abs(prev.second - a.second) > 10 || (std::abs(a.first) < 1 && std::abs(prev.first) >= 1)) {
                                std::fprintf(stderr, "door %d at %.2f: r %.0f l %.0f\n", id, scene, a.first, a.second);
                                prev = a;
                            }
                        }
                    }
                    updateEffects(*_viewDelegate.renderer, scene); // effects the VM starts and stops between shots
                    // --dialog-click x,y (repeatable): clicks, in turn, each dialog that opens.
                    // --dialog-type <text>: typed, key by key, into the text box of a dialog that opens.
                    if (gPie && gPie->dialogOpen() && gPie->dialogHasTextBox() && !dialogTyped.empty() &&
                        dialogTypedFor != gPie->dialog()->id) {
                        dialogTypedFor = gPie->dialog()->id;
                        for (size_t k = 0; k < dialogTyped.size();) {
                            size_t n = 1;
                            while (k + n < dialogTyped.size() && (uint8_t(dialogTyped[k + n]) & 0xC0) == 0x80)
                                ++n;
                            gPie->dialogKey(0, dialogTyped.substr(k, n));
                            k += n;
                        }
                    }
                    if (gPie && gPie->dialogOpen() && dialogClick < dialogClicks.size()) {
                        gPie->setCanvas(int(shotW), int(shotH));
                        const simd_float2 c = dialogClicks[dialogClick++];
                        if (c.x == -1)
                            gPie->dialogKey(36, {});
                        else if (c.x == -2)
                            gPie->dialogKey(53, {});
                        else
                            gPie->click(c);
                    }
                    if (gAudio) {
                        updateAudio(base + t);
                        gAudio->renderOffline(base + t);
                    }
                }
                const CFTimeInterval simmed = CACurrentMediaTime();
                // --track-sim-id <id|name>: each shot follows and centres on that Sim
                // instead of the selected one (a scene another Sim plays).
                const auto trackOther = argument(@"--track-sim-id");
                const os2::vm::ObjectId tracked =
                    trackOther && gSimulation ? os2::vm::findSim(gSimulation->machine(), *trackOther) : 0;
                if (tracked && gCameraFollow)
                    gCameraFollow->follow(gSimulation->machine(), tracked);
                else
                    followSelectedSim();
                // --track-sim: each shot centred on the selected Sim (close-ups of poses).
                if (([shotArgs containsObject:@"--track-sim"] && gPie) || tracked)
                    for (const auto& s : gLotSims)
                        if (s.id == (tracked ? tracked : gPie->sim()))
                            view.camera->setPivot(simd_make_float3(s.actor->position[0], s.actor->position[1], s.actor->position[2]));
                view.camera->setViewport(float(shotW), float(shotH));
                view.camera->snap();
                poseActors(*_viewDelegate.renderer, base + at);
                previous = at;
                _viewDelegate.renderer->setTime(at); // texture animations
                const uint32_t w = uiCanvas ? uiCanvas->first : 1280, h = uiCanvas ? uiCanvas->second : 720;
                gLastFrameW = w, gLastFrameH = h;
                const os2::FrameView fv = cinematicView({view.camera->view(), view.camera->projection(float(w) / float(h))}, at,
                                                        float(w) / float(h));
                MTLClearColor c = view.clearColor;
                _viewDelegate.renderer->setReflections(reflectionsOn());
                auto img = _viewDelegate.renderer->capture(
                    w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), float(c.alpha)));
                std::string file = *shot;
                if (frames > 1) {
                    const auto dot = file.rfind('.');
                    file = file.substr(0, dot) + "-" + std::to_string(f) + (dot == std::string::npos ? "" : file.substr(dot));
                }
                const CFTimeInterval captured = CACurrentMediaTime();
                os2::writePng(file, img);
                if (const char* poseShot = std::getenv("OS2_POSE_SHOT"))
                    writePoseSheets(*_viewDelegate.renderer, frames > 1 ? poseShot + ("-f" + std::to_string(f)) : poseShot);
                perfSim += simmed - frameStart, perfCapture += captured - simmed;
                perfWorst = std::max(perfWorst, captured - frameStart);
                std::printf("opensims2: wrote %s (%.2f s%s)\n", file.c_str(), at,
                            gSimulation ? (", " + std::to_string(gSimulation->ticks()) + " VM ticks").c_str() : "");
            }
            if (perfOn())
                std::printf("opensims2: perf: %d frames: VM+audio steps %.1f ms, capture %.1f ms per frame on average; "
                            "worst frame %.1f ms\n",
                            frames, perfSim / frames * 1000, perfCapture / frames * 1000, perfWorst * 1000);
            if (gAudio) {
                gAudio->renderOffline(base + time);
                std::printf("opensims2: sound: %s\n", gAudio->summary().c_str());
                if (auto mix = argument(@"--sound-mix"))
                    gAudio->writeMix(*mix);
            }
            std::fflush(stdout);
            std::exit(0);
        }
    }
}

// --hood <N>: the neighborhood view (app/hood_mode.hpp); with --screenshot
// runs the steps (clicks on lots and the dialog), then renders one frame and quits.
- (void)prepareHood:(OS2View*)view query:(const std::string&)query {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    gHood = std::make_unique<os2::HoodMode>(renderer, *gIndex, gameDir(), query, processArguments());
    for (const auto& line : gHood->view().report())
        std::printf("opensims2: %s\n", line.c_str());
    // Entering a lot and the main menu switch scenes in place (app/game_flow.hpp).
    gHood->onEnterLot = [](const os2::HoodLot& lot) {
        const auto& hood = gHood->view().neighborhood();
        gLotEntry = {true, lot.type, lot.family != 0, lot.name, lot.familyName, lot.family,
                     hood.package.parent_path() / "Thumbnails" / (hood.prefix + "_FamilyThumbnails.package")}; // its loading screen
        os2::Scene next;
        next.kind = os2::Scene::Kind::Lot;
        next.hood = os2::sceneOf(processArguments()).hood;
        next.lot = lot.id;
        requestScene(next);
    };
    // "Create New Family": Create-a-Sim for this neighborhood, then back here
    // with the new family selected in the bin.
    gHood->onCreateFamily = [](const fs::path& folder) {
        os2::Scene next;
        next.kind = os2::Scene::Kind::Cas;
        next.hood = folder.string();
        next.returnHood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    // A lot placed, moved or binned: the neighborhood view built again,
    // seen from where the camera was.
    gHood->onReload = [] {
        if (gCamera)
            gHoodReloadPose = gCamera->pose();
        os2::Scene next;
        next.kind = os2::Scene::Kind::Neighborhood;
        next.hood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    gHood->onQuit = [] { requestQuit(); };
    gHood->onMainMenu = [] {
        os2::Scene next;
        next.kind = os2::Scene::Kind::MainMenu;
        next.fromNeighborhood = true;
        next.returnHood = os2::sceneOf(processArguments()).hood;
        requestScene(next);
    };
    [view setCamera:gHood->makeCamera()];
    gCamera = view.camera;
    if (auto z = argument(@"--zoom"))
        view.camera->zoomBy(std::stoi(*z));
    if (auto o = argument(@"--orientation"))
        view.camera->rotateBy(std::stoi(*o));
    view.camera->snap();
    if (gHoodReloadPose) {
        view.camera->setPose(*gHoodReloadPose);
        gHoodReloadPose.reset();
    }
}

- (void)finishHood:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    if (gGameAudio) // the buttons' click sounds
        gHood->ui().playSound = [](const std::string& name) { gGameAudio->playUi(name); };
    // Head shots for the characters whose files have none (app/head_shot.hpp).
    gHood->ui().renderHead = [rp = &renderer](const fs::path& file) {
        return os2::renderHeadShot(*rp, *gIndex, simParts(*gIndex), file, 128, gameDir() / "TSData/Res");
    };
    // The Decorations catalog's pictures: effects have UI images, the rest are rendered.
    gHood->ui().renderDecorThumb = [rp = &renderer](const os2::hood::DecorProduct& p) -> std::optional<os2::RgbaImage> {
        const int model = gHood->view().decorationModel(p);
        if (model < 0)
            return std::nullopt;
        return os2::renderModelThumbnail(*rp, model, gHood->view().decorationBounds(p), 94);
    };
    gHood->ui().setForceRenderHeads(argument(@"--hood-render-heads").has_value());
    // The Options panel's option windows (audio, game, graphics, camera, credits).
    gHood->ui().options().setSettings(gSettings);
    gHood->ui().options().onChanged = [](const os2::GameSettings& s) {
        gSettings = s;
        if (gUser)
            s.save(*gUser);
        gSettings.applyGraphics(); // for what loads next
        applyAudioSettings();
        if (gHood) // Shadows: the terrain's light map is rebuilt now (the object shadow planes follow the option when drawn)
            gHood->view().refreshLightMap();
    };
    if (gGameAudio)
        gHood->ui().options().playSound = [](const std::string& name) { gGameAudio->playUi(name); };
    view.clearColor = MTLClearColorMake(0.42, 0.55, 0.72, 1.0);
    gHood->ui().attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                       uint32_t(view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gHood)
            gHood->ui().encode(enc, w, h, gUiScale);
    });
    _window.title = [NSString stringWithUTF8String:("opensims2 - " + gHood->view().displayName()).c_str()];
    if (gCreatedFamily) {
        gHood->familyCreated(*gCreatedFamily);
        gCreatedFamily.reset();
    }
    std::fflush(stdout);
    if (auto shot = argument(@"--screenshot")) {
        const uint32_t w = 1280, h = 720;
        view.camera->setViewport(float(w), float(h));
        gHood->scriptedView(*view.camera, float(w), float(h));
        view.camera->snap();
        // Where the lots are on the 1280x720 canvas, for click steps.
        const simd_float4x4 m = simd_mul(view.camera->projection(float(w) / float(h)), view.camera->view());
        for (const auto& lot : gHood->view().lots()) {
            const simd_float3 c = gHood->view().lotCentre(lot);
            const simd_float4 q = simd_mul(m, simd_make_float4(c, 1));
            const float x = (q.x / q.w + 1) / 2 * float(w), y = (1 - q.y / q.w) / 2 * float(h);
            if (q.w > 0 && x >= 0 && x < float(w) && y >= 0 && y < float(h))
                std::printf("opensims2: lot %u \"%s\" at (%.0f, %.0f)\n", lot.id, lot.name.c_str(), x, y);
        }
        const auto steps = os2::flowSteps(processArguments());
        for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
            gStepsUsed = k + 1;
            if (steps[k].kind == os2::FlowStep::Kind::Click)
            {
                gHood->mouseDown(*view.camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
                gHood->mouseUp(*view.camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
            }
        }
        if (gPendingScene)
            return;
        [self captureScene:*shot];
        std::exit(0);
    }
}

// --cas: Create-a-Sim (app/cas_mode.hpp); with --screenshot renders one frame and quits.
- (void)prepareCas:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    const os2::LightingConfig lighting = os2::loadLightingConfig(gameDir() / kLightingConfig);
    if (const auto* state = lighting.state(argument(@"--lighting").value_or("CAS"))) {
        renderer.setLighting(*state); // Lighting.txt's "CAS" state unless --lighting names another
        if (!argument(@"--lighting")) {
            // The CAS state leaves the Sim to the studio lamps of GlobalLots/CAS!.package
            // (app/cas_studio, which replaces these light sets once it loads); without the
            // studio (--cas-no-studio) its boom light (casStudio_lightingFloorStudioBoom_light,
            // intensity 1.25, colour (1, .95, .85)) stands in as a directional light from the
            // camera's side, above (inferred placement).
            auto set = os2::Renderer::outdoorLights(*state, true);
            set.lights.push_back({simd_normalize(simd_make_float3(0.35f, 1.0f, 0.9f)), simd_make_float3(1.0f, 0.95f, 0.85f) * 1.25f * 0.6f});
            renderer.setLightSets({set, set});
        }
    }
    // Families are saved into the user's neighborhoods (app/user_data.hpp) unless --cas-out says otherwise.
    std::vector<std::string> casArgs = processArguments();
    if (gUser && !argument(@"--cas-out")) {
        casArgs.push_back("--cas-out");
        casArgs.push_back(gUser->neighborhoodsDir().string());
    }
    gCas = std::make_unique<os2::CasMode>(renderer, *gIndex, gameDir(), casArgs);
    gCas->playSound = [](const std::string& name) { // the buttons' click sounds
        if (gGameAudio)
            gGameAudio->playUi(name);
    };
    // Opened from a neighborhood (Scene::Kind::Cas): Accept saves the family
    // there and goes back with it selected; Exit goes back without one.
    if (const os2::Scene sc = os2::sceneOf(processArguments()); sc.kind == os2::Scene::Kind::Cas && !sc.returnHood.empty()) {
        const std::string ret = sc.returnHood;
        auto back = [ret] {
            os2::Scene next;
            next.kind = os2::Scene::Kind::Neighborhood;
            next.hood = ret;
            requestScene(next);
        };
        gCas->onFamilySaved = [back](uint32_t family) {
            gCreatedFamily = family;
            back();
        };
        gCas->onExit = back;
    }
    [view setCamera:gCas->makeCamera()];
    gCamera = view.camera;
    gCas->setCamera(view.camera);
}

- (void)finishCas:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    view.clearColor = MTLClearColorMake(0.33, 0.36, 0.45, 1.0);
    gCas->attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                 uint32_t(view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        if (gCas)
            gCas->encode(enc, w, h, gUiScale);
    });
    if (auto shot = argument(@"--screenshot")) {
        uint32_t w = 1024, h = 768; // --cas-size WxH for another window shape
        if (auto sz = argument(@"--cas-size"))
            std::sscanf(sz->c_str(), "%ux%u", &w, &h);
        gCas->setCanvas(int(w), int(h));
        gCas->runScripted(processArguments());
        if (gPendingScene)
            return; // the steps went back to the neighborhood: the next scene takes the shot
        view.camera->setViewport(float(w), float(h));
        view.camera->snap();
        gCas->update(0.5);
        view.camera->snap();
        for (const auto& line : gCas->report())
            std::printf("opensims2: %s\n", line.c_str());
        const os2::FrameView fv{view.camera->view(), view.camera->projection(float(w) / float(h))};
        MTLClearColor c = view.clearColor;
        gUiScale = 1;
        renderer.setReflections(reflectionsOn());
        const auto img = renderer.capture(w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), 1));
        os2::writePng(*shot, img);
        std::printf("opensims2: wrote %s\n", shot->c_str());
        std::fflush(stdout);
        std::exit(0);
    }
    gCas->runScripted(processArguments());
    _window.title = [NSString stringWithUTF8String:gCas->title().c_str()];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
    return YES;
}

// Cmd-Q, the window closed or the game flow's Quit (not while a scene loads:
// applicationShouldTerminate ends the process then): the scene and the
// app's caches go here, before exit() runs any static's destructor.
- (void)applicationWillTerminate:(NSNotification*)note {
    releaseAppState();
}
@end

static void buildMenu() {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit opensims2" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
}

// --help / -h: the command line's options (the file's header comment has the
// details); prints and exits without opening a window.
static void printUsage() {
    std::puts(
        "usage: opensims2 [options]\n"
        "\n"
        "scenes (default: the main menu; the game data is found by --data <dir>, OS2_DATA or a folder asked for):\n"
        "  --hood <N001>            a neighborhood      --hood-lot <n>   enter one of its lots\n"
        "  --lot <N001_Lot82|file>  a lot's terrain, walls, objects        --cas    Create-a-Sim\n"
        "  --sim / --character / --skinned / --anim   a Sim, its animation  --movie <name> / --intro  movies\n"
        "  --play-sound <name>      play a sound and exit (--sound-index, --sound-wav <out.wav>)\n"
        "view:\n"
        "  --ui <instance-hex|live> a UI script (live: the game's panel)   --ui-show <id,...>  --ui-hover x,y\n"
        "  --vm                     run the lot's objects in the SimAntics VM   --run-vm <ticks>\n"
        "  --mode <live|buy|build>  --walls <up|cutaway|down>  --lighting <state>  --zoom --pivot --heading\n"
        "  --no-audio --mute --no-edge-scroll --reflections on|off --camera-scale <s> --user-dir <dir>\n"
        "headless (no window):\n"
        "  --screenshot <out.png>   render a frame (--frames <n>, --time <s>) and exit\n"
        "  --click x,y              click the canvas (repeatable; --right-click, --key <name>, --wait <s>)\n"
        "  --pie <object> [--pie-select N]   --dialog-answer <n> / --dialog-click x,y|return|escape\n"
        "  --loading-shot <png> [--loading-at <f>]   a scene's loading screen\n"
        "  -h, --help               this text\n");
}

// An app opened from the Finder or the Dock starts with 256 open files
// (launchd's soft limit; a terminal gives far more). The resource index keeps
// up to 128 packages open, the Sim parts' parallel read a descriptor per
// package it reads (the base game's and every neighborhood's character
// files), plus the scene's own files, Metal and audio: past 256 a package
// could not be reopened (DbpfError on the loading worker, std::terminate) on
// the first lot entered. Raised to the hard limit (at most OPEN_MAX).
void raiseOpenFileLimit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    const rlim_t want = std::min<rlim_t>(rl.rlim_max, OPEN_MAX);
    if (rl.rlim_cur >= want)
        return;
    rl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        std::fprintf(stderr, "opensims2: could not raise the open file limit (%s)\n", std::strerror(errno));
}

int main() {
    raiseOpenFileLimit();
    liveShotFile(); // (installs its SIGUSR1 handler before anything can send one)
    // Quitting tears the scene down as a scene switch does (in gScene's
    // order, while the app's globals it uses are still there), then frees
    // the app's caches of the game's data, before the namespace-scope
    // statics go: registered after them, it runs first (releaseAppState).
    std::atexit([] { releaseAppState(); });
    @autoreleasepool {
        for (NSString* a in NSProcessInfo.processInfo.arguments)
            if ([a isEqualToString:@"--help"] || [a isEqualToString:@"-h"]) {
                printUsage();
                return 0;
            }
        // The character folders of earlier sessions that did not exit
        // (crashed or killed) go (app/game_save.hpp); this one's is removed
        // at exit.
        {
            std::error_code ec;
            if (const size_t n = os2::removeStaleSessionCharacters(std::filesystem::temp_directory_path(ec)))
                std::printf("opensims2: removed %zu character folders of earlier sessions\n", n);
        }
        // --movie <name> --screenshot out.png [--frames n] [--time s] [--sound-wav mix.wav]: headless frames.
        if (auto movie = argument(@"--movie"); movie && argument(@"--screenshot")) {
            try {
                return os2::movieScreenshots(gameDir() / "TSData/Res", *movie, *argument(@"--screenshot"),
                                             std::stoi(argument(@"--frames").value_or("1")),
                                             std::stod(argument(@"--time").value_or("0")), 1280, 720,
                                             argument(@"--sound-wav").value_or(""));
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "error: %s\n", ex.what());
                return 1;
            }
        }
        if (auto sound = argument(@"--play-sound")) {
            try {
                const auto index = argument(@"--sound-index");
                return os2::playSoundCommand(gameDir() / "TSData/Res", *sound, index ? std::stoul(*index) : 0,
                                             argument(@"--sound-wav").value_or(""));
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "error: %s\n", ex.what());
                return 1;
            }
        }
        // No data found and a window is wanted: ask for the folder (remembered).
        if (!argument(@"--screenshot") && !findGameDir())
            os2::askForGameData(kCameraConfig);
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        buildMenu();
        OS2AppDelegate* delegate = [[OS2AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
