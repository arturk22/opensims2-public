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
// --dialog-click x,y (repeatable) clicks each dialog in turn.
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
#include "app/loading_screen.hpp"
#include "app/scene_loader.hpp"
#include "app/tv_broadcast.hpp"
#include "formats/neighborhood.hpp"
#include "formats/ui_script.hpp"
#include "vm/wants.hpp"
#include "app/movie_host.hpp"
#include "render/movie_layer.hpp"
#include "render/screen_fx_layer.hpp"
#include "vm/engine.hpp"
#include "audio/play_sound.hpp"
#include "formats/camera_config.hpp"
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
#include "app/build_mode.hpp"
#include "app/data_location.hpp"
#include "app/game_flow.hpp"
#include "app/game_save.hpp"
#include "app/hood_mode.hpp"
#include "formats/lot_arrivals.hpp"
#include "app/main_menu.hpp"
#include "app/options_panel.hpp"
#include "app/user_data.hpp"
#include "app/lot_effects.hpp"
#include "app/cas_mode.hpp"
#include "app/live_ui.hpp"
#include "app/pie_menu.hpp"
#include "lot_audio.hpp"
#include "anim_snaps.hpp"
#include "camera_follow.hpp"
#include "camera_input.hpp"
#include "lot_vm.hpp"
#include "vm_objects.hpp"
#include "lot_lighting.hpp"
#include "door_portals.hpp"
#include "vm/lot.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <strings.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

constexpr const char* kCameraConfig = "TSData/Res/UserData/Cameras/cameras.txt";
constexpr const char* kLightingConfig = "TSData/Res/Lights/Lighting.txt";
constexpr uint32_t kTypeCres = 0xE519C933, kTypeObjd = 0x4F424A44;

// The pie menu over the lot's VM (app/pie_menu.hpp), with the pick boxes of
// the drawn lot objects (collected before the VM exists).
std::unique_ptr<os2::PieMenuHost> gPie;
// --ui live: the game's live-mode panel over the same VM (app/live_ui.hpp).
std::unique_ptr<os2::LiveUi> gLive;
bool liveUiRequested();
// Buy mode (app/buy_mode.hpp): the puck's Buy button, or --mode buy.
std::unique_ptr<os2::BuyMode> gBuy;
void setupBuyMode(os2::Renderer& renderer, os2::ResourceIndex& index); // below setupLiveUi
// Build mode (app/build_mode.hpp): the puck's Build button, or --mode build.
// It changes gLotShape (the lot as drawn); rebuildLotModel redraws it.
std::unique_ptr<os2::BuildMode> gBuild;
std::unique_ptr<os2::Lot> gLotShape;
std::unique_ptr<os2::PatternCatalog> gLotPatterns;
os2::LotViewOptions gLotView;
int gLotInstance = -1;
void setupBuildMode(os2::Renderer& renderer, os2::ResourceIndex& index);
struct PickBox {
    uint32_t instance;
    simd_float3 lo, hi;
    simd_float4x4 at;
};
std::vector<PickBox> gPickBoxes;
// The lot's placed objects by object ID (the cObject instance's low 16
// bits): what is needed to pose one when the VM animates it.
struct PlacedObject {
    std::string cres;
    std::map<std::string, std::string> materials;
    std::array<float, 3> position;
    std::array<float, 4> rotation;
    std::string definition; // the cObject's OBJD file name ("Newspaper_objt")
    int instance = -1; // renderer instance of its static model
    bool contained = false; // saved in a slot (at (0, 0)): hidden until the VM places it
};
std::map<int16_t, PlacedObject> gPlacedObjects;
// Skinned models posed every frame by their animation players.
std::vector<std::unique_ptr<os2::SimActor>> gActors;
std::map<int16_t, os2::SimActor*> gObjectActors; // objects the VM has animated (in gActors)
std::unique_ptr<os2::CinematicPlayer> gCinematic; // the cinematic prepared or playing (see updateCinematic)
std::unique_ptr<os2::TvBroadcast> gTv;             // TV channels on the lot's screens
void cinematicActorAnimates(int16_t id);
float gUiScale = 1; // drawable pixels per view point
// --hood: the neighborhood view (app/hood_mode.hpp) in place of a lot.
std::unique_ptr<os2::HoodMode> gHood;
std::unique_ptr<os2::CasMode> gCas; // --cas: Create-a-Sim (app/cas_mode.hpp)
// --hood <N> --hood-lot <id>: that lot's package, shown as --lot is.
std::optional<std::string> gHoodLot;
std::vector<std::string> processArguments();
// The game flow (app/game_flow.hpp): the user's folder, the main menu, the
// puck's options, the scene asked for next and the steps used so far.
std::unique_ptr<os2::UserData> gUser;
std::unique_ptr<os2::MainMenu> gMenu;
std::unique_ptr<os2::OptionsPanel> gOptions;
os2::GameSettings gSettings;
std::optional<os2::Scene> gPendingScene;
// A family Create-a-Sim just wrote into the neighborhood: selected in the
// family bin once the neighborhood is back (HoodMode::familyCreated).
std::optional<uint32_t> gCreatedFamily;
bool gQuitRequested = false;
size_t gStepsUsed = 0;    // --click/--wait steps this scene has taken (headless)
double gHeadlessClock = 0; // the caller's clock of headless VM runs (--wait, --run-vm)
fs::path gLotFile;        // the package the running lot came from (saved back to)
std::function<void()> gOnSceneRequest; // the app delegate's switch (interactive)
// Hook for the intro movies (the video agent): called once, before the
// first main menu is shown; null plays nothing.
std::function<void()> gIntroMovies;
// Scene loading off the main thread (app/scene_loader.hpp, docs/loading-mac.md).
// While gSceneLoading, a worker builds the next scene (the globals below,
// the new renderer, the view's camera) and the main thread only draws the
// loading screen: frames and input leave the scene alone.
os2::BackgroundJob gLoadJob;
bool gSceneLoading = false;                    // main thread
std::function<void()> gPollLoad;               // the app delegate's check, each frame while loading
os2::LotEntry gLotEntry;                       // the lot being entered (its loading screen)

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

// OS2_PERF=1: timings of the load phases and of the frames on stdout
// (docs/performance.md); nothing else changes.
bool perfOn() {
    static const bool on = std::getenv("OS2_PERF") != nullptr;
    return on;
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
        if (o.model.empty() || o.model == "nullModel" || o.model == "noModel" || o.person)
            continue;
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
            } catch (const std::exception& e) {
                std::fprintf(stderr, "opensims2: %s: %s (%s): %s\n", lotName.c_str(), o.model.c_str(),
                             o.definition.c_str(), e.what());
            }
            it = models.emplace(key, id).first;
        }
        if (it->second < 0)
            continue;
        const auto& q = o.rotation;
        simd_float4x4 at = simd_matrix4x4(simd_quaternion(q[0], q[1], q[2], q[3]));
        at.columns[3] = simd_make_float4(o.position[0], o.position[1], o.position[2], 1);
        if (contained)
            at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
        const int instance = renderer.addInstance(it->second, at);
        // Stale cObjects share IDs with live ones: the first (or one in the
        // world) keeps the ID; app/vm_objects.cpp checks it against the VM.
        const int16_t id = int16_t(o.instance & 0xFFFF);
        if (!gPlacedObjects.count(id) || (gPlacedObjects[id].contained && !contained))
            gPlacedObjects[id] = {cresOf(o), o.materials, o.position, o.rotation, o.definition, instance, contained};
        if (contained)
            continue;
        gPickBoxes.push_back({o.instance, bounds[key].first, bounds[key].second, at});
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
size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile);

// The game's resources. The VM, the pie menus and the sound keep references to
// it, so it lives as long as the app (not just while the window is set up).
std::unique_ptr<os2::ResourceIndex> gIndex;

// Every Sim part of the game (GZPS with a 3IDR), read once for the app's
// life: every lot entry and outfit change uses them. On the loading thread
// or the main thread, never both at once (the main thread reads it only
// between loads).
const std::vector<os2::SimPart>& simParts(os2::ResourceIndex& index) {
    static std::vector<os2::SimPart> parts;
    static const os2::ResourceIndex* of = nullptr;
    if (of != &index) {
        parts = os2::loadSimParts(index);
        of = &index;
    }
    return parts;
}

// The lot's objects in the VM (--vm, --run-vm), driving the lot's Sims.
std::unique_ptr<os2::LotSimulation> gSimulation;
// The lot's objects drawn where the VM puts them (app/vm_objects.hpp).
std::unique_ptr<os2::VmObjectView> gObjectView;
std::unique_ptr<os2::AnimSnapView> gSnaps; // models the animations' snap events put in the Sims' hands
// The lot's lights from the VM: the clock's day/night, rooms, lamps (app/lot_lighting.hpp).
std::unique_ptr<os2::LotLighting> gLighting;
std::optional<os2::LotLightMapLayout> gLightMapLayout; // the lot's light-map atlas
// Doors opening as the Sims walk through (app/door_portals.hpp).
std::unique_ptr<os2::DoorPortals> gDoors;
struct LotSim {
    os2::SimActor* actor;
    int16_t id; // object ID (the cObject's instance)
    fs::path character;
    std::string idle;
};
std::vector<LotSim> gLotSims;
int gPlumbob = -1; // renderer instance of the plumbob over the selected Sim (placePlumbob)
float gPlumbobBottom = 0; // lowest point of its mesh (modelled about 1.24 over the origin)

// Sound (app/lot_audio.hpp): gGameAudio lives across scenes (the mixer on
// the device, the mode music: NHood, CAS, Buy, Build); gAudio is the lot's.
// --no-audio leaves it out, --mute keeps it silent; headless runs
// (--screenshot) open no device, and --sound-mix <out.wav> saves what would
// have played (scenes without a clock add --sound-dwell seconds, 5 by
// default). --music <category> plays music in live mode ("Build", "Nhood",
// or a radio station: "pop", "rnb", ...).
std::unique_ptr<os2::GameAudio> gGameAudio;
std::unique_ptr<os2::LotAudio> gAudio;
os2::LotCamera* gCamera = nullptr;
std::unique_ptr<os2::LotCameraFollow> gCameraFollow; // --lot: pivot held to the lot, on the Sim's floor

// Page Up / Page Down and the puck's floor buttons ("Subir um Andar (Page
// Up)", "Descer um Andar (Page Down)"): in Buy mode the floor objects go on
// (the camera follows it), else the floor the camera shows.
void changeFloor(int delta) {
    if (gBuy && gBuy->active()) {
        if (gBuy->changeLevel(delta) && gCameraFollow)
            gCameraFollow->setLevel(gBuy->level() - 1);
    } else if (gCameraFollow) {
        gCameraFollow->setLevel(gCameraFollow->level() + delta);
    }
}

void setupPieMenu(os2::ResourceIndex& index); // below loadLot
void applyAudioSettings();                     // game flow, below
void setupOptions(os2::ResourceIndex& index);  // game flow, below
void leaveLot();                               // game flow, below
void quitFromLot();                            // game flow, below
bool saveGameNow();                            // game flow, below
void setupLiveUi(os2::ResourceIndex& index);  // below setupPieMenu
std::optional<os2::RgbaImage> simPortrait(int16_t id);
std::optional<os2::RgbaImage> neighborPortrait(int16_t neighborId);

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
    gSimulation->onSimAnimation = [](int16_t id, os2::SimActor& actor, const os2::Animation* anim, double at) {
        gSnaps->simAnimation(id, actor, anim, at);
        if (anim)
            cinematicActorAnimates(id);
    };
    for (const auto& [id, p] : gPlacedObjects) {
        std::optional<os2::Xform> saved;
        if (!p.contained)
            saved = os2::Xform{os2::Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]},
                               {p.position[0], p.position[1], p.position[2]}};
        gObjectView->addPlaced(id, p.definition, p.cres, p.instance, saved, p.materials);
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
    // Objects the VM animates (doors, fridges, phones, ...) swap their static
    // model for a skinned one of the same CRES, posed by the animation.
    gSimulation->onObjectAnimation = [&renderer, &index](int16_t id, const std::string& name, double at) {
        if (gSnaps)
            gSnaps->objectAnimation(id, name, at);
        if (std::getenv("OS2_DEBUG_VM"))
            std::fprintf(stderr, "opensims2: object %d starts %s (%s)\n", id, name.c_str(),
                         gPlacedObjects.count(id) ? "placed" : "not placed");
        auto it = gObjectActors.find(id);
        if (it == gObjectActors.end()) {
            const auto placed = gPlacedObjects.find(id);
            if (placed == gPlacedObjects.end())
                return; // no model in the world
            const PlacedObject& p = placed->second;
            try {
                const os2::ObjectModel model = os2::loadObjectModel(index, p.cres, gObjectView->materialsOf(id));
                os2::SkinnedModel skin(model.skeleton);
                skin.add(model);
                auto actor = std::make_unique<os2::SimActor>(std::move(skin));
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
                return;
            }
        }
        if (!it->second)
            return;
        try {
            it->second->cue(at, it->second->animation(index, name), {.fade = 0.1f, .loop = false});
            if (std::getenv("OS2_DEBUG_VM"))
                std::fprintf(stderr, "opensims2: object %d plays %s\n", id, name.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: object %d: %s: %s\n", id, name.c_str(), e.what());
        }
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
    // --motives <n>: test aid, every Sim's motives at n (instead of the saved ones).
    if (auto m = argument(@"--motives"))
        os2::vm::setAllMotives(gSimulation->machine(), int16_t(std::stoi(*m)));
    // Pie menus over the same VM.
    gPie = std::make_unique<os2::PieMenuHost>(gSimulation->machine(), gameDir() / "TSData/Res/UI/Fonts",
                                              argument(@"--sim-id").value_or(""));
    for (const auto& b : gPickBoxes)
        gPie->addPickBox(b.instance, b.lo, b.hi, b.at);
    perfLap("  VM hooks");
    setupPieMenu(index);
    perfLap("  pie menu");
    if (liveUiRequested())
        setupLiveUi(index);
    perfLap("  live UI");
    if (liveUiRequested() || argument(@"--mode")) {
        setupBuyMode(renderer, index);
        perfLap("  Buy mode");
        setupBuildMode(renderer, index);
        perfLap("  Build mode");
    }
    if (liveUiRequested())
        setupOptions(index); // below: the puck's Options mode
    // --dialog-answer <n>: every dialog the trees put up gets button n at once.
    if (auto a = argument(@"--dialog-answer"))
        gSimulation->machine().autoDialogAnswer = std::stoi(*a);
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
    static std::map<uint32_t, fs::path> byGuid;
    static fs::path scanned;
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
                } catch (const std::exception&) {
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
    } catch (const std::exception&) {
    }
    return std::nullopt;
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
    gLive->onQuit = [] { quitFromLot(); };
    gLive->onSave = [] { saveGameNow(); };
    gLive->onOptionWindow = [](int window) {
        if (gOptions)
            gOptions->toggleWindow(window);
    };
    gLive->onFloor = [](int delta) { changeFloor(delta); };
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
                    } catch (const std::exception&) {
                    }
            const auto it = byGuid->find(guid);
            if (it == byGuid->end())
                return std::nullopt;
            try {
                os2::DbpfPackage character(it->second);
                for (const auto& e : character.entries())
                    if (e.tgi.type == os2::kTypeUiImage && e.tgi.instance < 0x100)
                        return os2::decodeUiImage(character.read(e));
            } catch (const std::exception&) {
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
                gLive->setMode(0);
            }
        };
    if (gAudio)
        gBuy->playSound = [](const std::string& name) {
            os2::SoundSystem::PlayOptions o;
            o.group = os2::SoundGroup::Ui;
            gAudio->sound().play(name, o);
        };
    if (argument(@"--mode").value_or("") == "buy") {
        gBuy->enter();
        if (gLive)
            gLive->setMode(1);
    }
}

// Fence rails and posts (os2::lotFencePieces): one model per CRES, an
// instance per piece; called again after Build mode changed the fences
// (instances are reused, extra ones hidden). Returns the pieces placed.
size_t placeFencePieces(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                        const os2::PatternCatalog& patterns, std::vector<std::string>* warnings) {
    static std::map<std::string, int> models;
    static std::vector<int> instances;
    const auto pieces = os2::lotFencePieces(lot, patterns, warnings);
    size_t used = 0;
    for (const auto& piece : pieces) {
        auto it = models.find(piece.model);
        if (it == models.end()) {
            int id = -1;
            try {
                id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
            } catch (const std::exception& e) {
                if (warnings)
                    warnings->push_back(piece.model + ": " + e.what());
            }
            it = models.emplace(piece.model, id).first;
        }
        if (it->second < 0)
            continue;
        simd_float4x4 at;
        std::memcpy(&at, piece.transform.data(), sizeof at);
        if (used < instances.size())
            renderer.setInstanceModel(instances[used], it->second), renderer.setInstanceTransform(instances[used], at);
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

// Redraws the lot after Build mode changed it: the model (walls cut away
// as seen from the camera now, holes for the doors and windows), and the
// light-map atlas and light maps.
void rebuildLotModel(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape || gLotInstance < 0 || !gLotPatterns)
        return;
    os2::LotViewOptions view = gLotView;
    if (gCamera) {
        const simd_float3 d = gCamera->pivot() - gCamera->eye();
        if (std::hypot(d.x, d.y) > 1e-3f)
            view.viewDirection = {d.x, d.y};
    }
    gLightMapLayout.emplace();
    const os2::ObjectModel model = os2::buildLotModel(*gLotShape, index, *gLotPatterns, view, &*gLightMapLayout);
    renderer.setInstanceModel(gLotInstance, renderer.addObject(model, index));
    placeFencePieces(renderer, index, *gLotShape, *gLotPatterns, nullptr);
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

// Build mode over the lot's VM: the puck's Build button (mode 2) or --mode
// build; headless steps in BuildMode::runSteps (build_mode.cpp).
void setupBuildMode(os2::Renderer& renderer, os2::ResourceIndex& index) {
    if (!gLotShape)
        return;
    gBuild = std::make_unique<os2::BuildMode>(*gSimulation, *gObjectView, renderer, index, gameDir() / "TSData/Res",
                                              *gLotShape);
    gBuild->onGeometryChanged = [&renderer, &index]() { rebuildLotModel(renderer, index); };
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
    if (argument(@"--mode").value_or("") == "build") {
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
        } else if (a == "--grab") {
            gBuy->grab(os2::vm::ObjectId(std::stoi(v)));
        } else if (a == "--sell") {
            gBuy->deleteHeld();
        } else if (a == "--undo") {
            gBuy->undo();
        }
    }
    gBuy->update();
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
    os2::LotViewOptions view;
    view.viewDirection = {viewDirection.x, viewDirection.y};
    const std::string walls = argument(@"--walls").value_or("up");
    if (walls == "cutaway")
        view.walls = os2::LotViewOptions::Walls::Cutaway;
    else if (walls == "down")
        view.walls = os2::LotViewOptions::Walls::Down;
    else if (walls != "up")
        throw std::runtime_error("--walls: expected up, cutaway or down");
    gLightMapLayout.emplace();
    const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view, &*gLightMapLayout);
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("buildLotModel");
    renderer.hideTestScene();
    gLotInstance = renderer.addInstance(renderer.addObject(model, index), matrix_identity_float4x4);
    gLotShape = std::make_unique<os2::Lot>(lot);
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
            renderer.setLightMap(0, lm);
            renderer.setLightMap(1, lm);
        }
    }
    // Fence rails and posts: one model per CRES, an instance per piece.
    std::vector<std::string> fenceWarnings;
    const size_t fences = placeFencePieces(renderer, index, lot, patterns, &fenceWarnings);
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
    return simd_make_float3(float(lot.width) / 2, float(lot.height) / 2, lot.elevation(0, cx, cy));
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
bool gVmPlumbob = false;           // the VM's PlumbBob is over the selected Sim
std::set<int16_t> gHiddenPlumbobs; // the game's PlumbBobs the app hides (its own stands in)
std::optional<os2::Xform> gAppPlumbobAt; // where the app's plumbob is drawn, if it is
std::map<std::string, int> gPlumbobModels; // plumbob model per core material (renderer ids)
std::string gPlumbobMaterial;      // the app plumbob's core material ("" = the model's)
void choosePlumbob(os2::Renderer& renderer) {
    if (!gSimulation || !gObjectView || !gPie)
        return;
    gVmPlumbob = false;
    std::string material;
    for (const auto& [id, o] : gSimulation->machine().objects()) {
        if (o.person || !o.definition || o.definition->fileName != "PlumbBob")
            continue;
        const bool over = gObjectView->hasModel(id) && gObjectView->outermostContainer(id) == gPie->sim();
        gObjectView->setHidden(id, !over || gVmPlumbob);
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

// The lot's particle effects (app/lot_effects.hpp): what the VM starts, and
// --effect <name>[@<object id>[:<bone index>] | @x,y,z] (repeatable) by hand.
std::unique_ptr<os2::LotEffects> gEffects;
bool gEffectsFailed = false;
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
        if (s.id != gPie->sim() || gVmPlumbob)
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
os2::ScreenFxLayer::Params gScreenFxParams;
size_t gCinematicsSeen = 0;
bool gCinematicForce = false;      // start at the next update (--cinematic)
double gCinematicPrepared = -1;    // scene clock when it was prepared (waits for an actor's animation)
double gCinematicLast = 0;         // scene clock of the last update
double gCrossfadeAt = -1e9;        // scene clock of the last snapshot
double gSceneNow = 0;              // the scene clock of the last poseActors
std::optional<os2::FrameView> gLastFrameView; // what was drawn last (snapshots)
uint32_t gLastFrameW = 1280, gLastFrameH = 720;
constexpr double kCrossfadeSeconds = 2.0; // crossfadeshort/standard: screen mode 4, length 2, strength 1 -> 0
constexpr double kCinematicWait = 1.0;    // without "Start Animations Now": the longest wait for an actor

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
    if (!gSimulation)
        return std::nullopt;
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
    const os2::RgbaImage img = renderer.capture(gLastFrameW, gLastFrameH, *gLastFrameView, simd_make_float4(0, 0, 0, 1));
    gScreenFxParams = keep;
    gScreenFx->setSnapshot(img);
    gCrossfadeAt = now;
}

void startCinematic(os2::Renderer& renderer, const std::string& scene, std::vector<int16_t> actors, int16_t origin,
                    bool mirror, bool now, bool guessSims = false) {
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
        auto player = std::make_unique<os2::CinematicPlayer>(renderer, *gIndex, std::move(host), scene, std::move(actors),
                                                             origin, mirror);
        std::printf("opensims2: cinematic %s prepared (%.2f s, camera %s%s%s)\n", player->camera().scene().name.c_str(),
                    player->camera().duration(), player->camera().scene().cameraAnimation.c_str(),
                    mirror ? ", mirrored" : "", now ? "" : ", waits for its actors");
        player->setGuessSimAnimations(guessSims);
        gCinematic = std::move(player);
        gCinematicPrepared = -1;
        gCinematicForce = now;
    } catch (const std::exception& e) {
        std::printf("opensims2: cinematic %s: %s\n", scene.c_str(), e.what());
    }
    std::fflush(stdout);
}

// An actor of the prepared scene starts an animation: the scene starts with it.
void cinematicActorAnimates(int16_t id) {
    if (!gCinematic || gCinematic->started())
        return;
    for (int16_t a : gCinematic->actors())
        if (a == id) {
            gCinematicForce = true;
            return;
        }
}

// Starts the cinematics the VM asked for since the last call; runs and ends the one playing.
void updateCinematic(os2::Renderer& renderer, double now) {
    gSceneNow = now;
    auto& requests = os2::vm::cinematicRequests();
    if (gCinematicsSeen > requests.size())
        gCinematicsSeen = 0;
    for (; gCinematicsSeen < requests.size(); ++gCinematicsSeen) {
        const auto& r = requests[gCinematicsSeen];
        if (r.scene.empty())
            continue;
        std::vector<int16_t> actors(r.actors.begin(), r.actors.end());
        startCinematic(renderer, r.scene, actors, r.me, r.mirrored(), r.startNow());
    }
    if (gLighting && !gLighting->extraLights)
        gLighting->extraLights = [](int16_t id) {
            std::vector<os2::LotLighting::ExtraLight> out;
            if (gCinematic)
                for (const auto& l : gCinematic->lightsFor(id))
                    out.push_back({l.position, l.color, l.falloff, l.range});
            return out;
        };
    if (gCinematic && !gCinematic->started()) {
        if (gCinematicPrepared < 0)
            gCinematicPrepared = now;
        if (gCinematicForce || now - gCinematicPrepared >= kCinematicWait) {
            gCinematicLast = now - 1e-3;
            if (gCinematic->start(now)) {
                std::printf("opensims2: cinematic %s starts at %.2f s\n", gCinematic->camera().scene().name.c_str(), now);
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
        } else {
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
        if (previous)
            previous(enc, w, h);
    });
}

// Poses every actor at `seconds` and hands the vertices to the renderer.
// The Sims first: objects in their hands follow their bones (gObjectView),
// then the objects' actors at the places that gives.
void poseActors(os2::Renderer& renderer, double seconds) {
    if (gSimulation)
        seconds = gSimulation->update(seconds); // the scene clock (game speed)
    updateCinematic(renderer, seconds);
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
    for (auto& a : gActors)
        if (!isObject(a.get()))
            a->update(seconds);
    if (gSnaps)
        gSnaps->update(seconds);
    choosePlumbob(renderer);
    if (gObjectView)
        gObjectView->update();
    if (gLighting)
        gLighting->update(seconds);
    for (auto& a : gActors) {
        if (isObject(a.get()))
            a->update(seconds);
        renderer.updateObject(a->model, a->posed);
    }
    placePlumbob(renderer, seconds);
    if (gTv)
        gTv->update(seconds);
    updateEffects(renderer, seconds);
    updateAudio(seconds);
}

size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile) {
    os2::LotObjects saved = os2::LotObjects::load(pkg);
    // A family that just moved in: its Sims arrive (formats/lot_arrivals.hpp).
    const os2::LotArrivals arrivals = os2::addArrivingSims(saved, lot, lotFile);
    std::set<uint32_t> inWorld; // object IDs of the Sims standing on the lot
    for (const auto& st : saved.objects)
        if (st.person && st.x >= 0)
            inWorld.insert(uint32_t(uint16_t(st.id)));
    const std::vector<os2::SimPart>* parts = nullptr;
    const std::string idle = argument(@"--sim-anim").value_or("a-idle-neutral-armsSide-wtC-breatheLoop");
    size_t placed = 0;
    std::vector<os2::Lot::Object> people = lot.objects;
    people.insert(people.end(), arrivals.sims.begin(), arrivals.sims.end());
    for (const auto& o : people) {
        if (!o.person || !inWorld.count(o.instance & 0xFFFF))
            continue;
        const std::string stem = o.definition.substr(0, o.definition.find(" - "));
        try {
            const fs::path file = lotFile.parent_path().parent_path() / "Characters" / (stem + ".package");
            if (!parts)
                parts = &simParts(index);
            perfLap("  loadSimParts");
            auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, *parts, file));
            perfLap("  loadCharacterSim");
            for (const auto& w : actor->skin.model.warnings)
                std::fprintf(stderr, "opensims2: %s: %s\n", stem.c_str(), w.c_str());
            actor->position = {o.position[0], o.position[1], o.position[2]};
            actor->heading = 2 * std::atan2(o.rotation[2], o.rotation[3]);
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
            actor->cue(-0.37 * double(placed), actor->animation(index, anim));
            perfLap("  Sim animation");
            actor->model = renderer.addObject(actor->skin.model, index, true);
            renderer.addInstance(actor->model, matrix_identity_float4x4);
            perfLap("  Sim to the GPU");
            std::printf("opensims2: Sim %s (%s) at (%.2f, %.2f, %.2f), %zu parts\n", stem.c_str(),
                        o.definition.c_str(), o.position[0], o.position[1], o.position[2],
                        actor->skin.model.parts.size());
            gLotSims.push_back({actor.get(), int16_t(o.instance & 0xFFFF), file, anim});
            gActors.push_back(std::move(actor));
            ++placed;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opensims2: Sim %s: %s\n", o.definition.c_str(), e.what());
        }
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

std::unique_ptr<os2::UiLayer> gUi;

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
    kEscape = 53, kHome = 115, kPageUp = 116, kPageDown = 121, kLeft = 123, kRight = 124, kDown = 125, kUp = 126,
    kReturn = 36, kEnter = 76, kP = 35, k1 = 18, k2 = 19, k3 = 20, kY = 16, kBackspace = 51, kForwardDelete = 117,
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
    const CFTimeInterval start = CACurrentMediaTime();
    const os2::GameSaveResult r = os2::saveGame(gSimulation->machine(), gLotFile, gameDir());
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
enum class FlowDialog { None, Leave, Quit };
FlowDialog gFlowDialog = FlowDialog::None;
std::unique_ptr<os2::UiResources> gTexts;

std::string uiText(uint32_t instance, uint32_t index, const char* fallback) {
    try {
        if (!gTexts && gIndex)
            gTexts = std::make_unique<os2::UiResources>(*gIndex, gameDir() / "TSData/Res");
        if (gTexts)
            if (auto t = gTexts->string(os2::UiStringRef{0x7fa7dc7c, instance, index}); t && !t->empty())
                return *t;
    } catch (const std::exception&) {
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
    return gOptions->press(p) != os2::OptionsPanel::Action::None;
}

// A click on the main menu (canvas pixels).
void pressMainMenu(simd_float2 p, float w, float h) {
    if (!gMenu)
        return;
    gMenu->setCanvas(int(w), int(h));
    const os2::Scene here = os2::sceneOf(processArguments());
    os2::Scene next;
    switch (gMenu->press(p)) {
    case os2::MainMenu::Action::Play:
    case os2::MainMenu::Action::Tutorial:
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

// Runs the lot's VM `ticks` ticks headless (a --wait step).
void runTicksHeadless(long ticks) {
    if (!gSimulation || ticks <= 0)
        return;
    const uint64_t target = gSimulation->ticks() + uint64_t(ticks);
    for (long guard = 0; gSimulation->ticks() < target && guard < ticks * 40 + 100; ++guard) {
        gHeadlessClock += 1.0 / 30;
        const double scene = gSimulation->update(gHeadlessClock);
        if (gLighting && gSimulation->ticks() % 30 == 0)
            gLighting->update(scene);
        if (gLive)
            gLive->tick(gHeadlessClock);
        if (gAudio) {
            updateAudio(gHeadlessClock);
            gAudio->renderOffline(gHeadlessClock);
        }
    }
    std::printf("opensims2: waited to VM tick %llu\n", (unsigned long long)gSimulation->ticks());
    std::fflush(stdout);
}

// Everything a scene loaded, so that the next one starts clean (the
// renderer is replaced by the caller).
void unloadScene() {
    gFlowDialog = FlowDialog::None;
    gCinematic.reset(); // (the VM's request list stays counted: gCinematicsSeen)
    gScreenFxParams = {};
    gCrossfadeAt = -1e9;
    gBuild.reset();
    gBuy.reset();
    gLive.reset();
    gPie.reset();
    gOptions.reset();
    gMenu.reset();
    gHood.reset();
    gCas.reset();
    gDoors.reset();
    gLighting.reset();
    gSnaps.reset();
    gObjectView.reset();
    gTv.reset();
    gSimulation.reset();
    gAudio.reset();
    gCameraFollow.reset();
    gUi.reset();
    gObjectActors.clear();
    gLotSims.clear();
    gActors.clear();
    gPlacedObjects.clear();
    gPickBoxes.clear();
    gPlumbob = -1;
    gPlumbobBottom = 0;
    gVmPlumbob = false;
    gHiddenPlumbobs.clear();
    gAppPlumbobAt.reset();
    gPlumbobMaterial.clear();
    gPlumbobModels.clear();
    gLightMapLayout.reset();
    gLotShape.reset();
    gLotPatterns.reset();
    gLotView = {};
    gLotInstance = -1;
    gHoodLot.reset();
    gLotFile.clear();
    gStepsUsed = 0;
    gHeadlessClock = 0;
    gCamera = nullptr;
}

} // namespace

// MTKView that keeps the camera and the keys held down.
@interface OS2View : MTKView
- (os2::LotCamera*)camera;
- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera;
- (void)applyHeldKeys:(double)dt;
@end

@implementation OS2View {
    std::unique_ptr<os2::LotCamera> _camera;
    std::set<unsigned short> _held;
    os2::CameraMouseInput _mouse;
    bool _rotating; // right button with Option: rotate instead of scroll
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
        gCas->keyDown(e.keyCode, e.characters ? std::string(e.characters.UTF8String) : std::string(),
                      e.modifierFlags & NSEventModifierFlagControl);
        return;
    }
    if (os2::moviePlaying()) { // Esc, Space or Return skips a movie
        if (!e.isARepeat && (e.keyCode == kEscape || e.keyCode == kReturn || e.keyCode == kEnter || e.keyCode == 49)) {
            os2::skipMovie();
            std::printf("opensims2: movie skipped (key %d)\n", int(e.keyCode));
        }
        return;
    }
    _held.insert(e.keyCode);
    if (e.isARepeat)
        return;
    if (gPie && gPie->isOpen()) // any key (Shift sends no keyDown) cancels the pie menu
        gPie->close();
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
    switch (e.keyCode) {
    case kPageUp: changeFloor(1); break;
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
                                   options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                           NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
}

- (void)mouseExited:(NSEvent*)e {
    [self trackMouse:e];
    _mouse.mouseAt(-1, -1, false);
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
    _rotating = (e.modifierFlags & NSEventModifierFlagOption) != 0;
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
    if (gPie->isOpen() && gPie->press(p))
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
    if (gBuy && gBuy->active()) { // the object tool, not the pie menu
        gBuy->click(eye, simd_normalize(q.xyz / q.w - eye));
        return;
    }
    if (gBuild && gBuild->active()) { // Build mode's tools
        gBuild->mouseDown(eye, simd_normalize(q.xyz / q.w - eye), e.modifierFlags & NSEventModifierFlagControl,
                          e.modifierFlags & NSEventModifierFlagShift);
        return;
    }
    const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
    if (id && !gPie->open(id, p))
        std::printf("opensims2: object %d: no interactions available\n", id);
    std::fflush(stdout);
}

// The pie menu picks on release (a drag from the opening press is a gesture).
- (void)mouseUp:(NSEvent*)e {
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->mouseUp([self canvasPoint:e]);
    if (gPie && gPie->isOpen())
        gPie->release([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseUp(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
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
    if (gSceneLoading)
        return;
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
}

- (void)mouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (gSceneLoading)
        return;
    if (gCas)
        gCas->mouseDragged([self canvasPoint:e]);
    if (gPie)
        gPie->hover([self canvasPoint:e]);
    if (gBuild && gBuild->active())
        gBuild->mouseDrag(_camera->eye(), [self rayAt:[self canvasPoint:e]]);
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
    _renderer->draw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable, frame);
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
    }
}

// --- Loading behind the loading screen (app/scene_loader.hpp) ---------------

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
    const os2::FrameView fv{_view.camera->view(), _view.camera->projection(float(w) / float(h))};
    const MTLClearColor c = _view.clearColor;
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
        view.camera->snap();
        // The lot's cutaway walls follow the starting view direction.
        const simd_float3 look = view.camera->pivot() - view.camera->eye();
        simd_float3 pivot = loadLot(*_viewDelegate.renderer, index, simd_make_float2(look.x, look.y))
                                .value_or(skinned.value_or(objects));
        // --pivot <x,y>: look at that point instead (tiles, at the same height).
        float px, py;
        if (auto p = argument(@"--pivot"); p && std::sscanf(p->c_str(), "%f,%f", &px, &py) == 2)
            pivot.x = px, pivot.y = py;
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
            _viewDelegate.renderer->setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
                if (gUi)
                    gUi->encode(enc, w, h);
                const bool optionsOver = gLive && gLive->optionsOpen(); // the options replace Buy/Build's panel
                if (gBuy && !optionsOver) // under the puck
                    gBuy->encode(enc, w, h, gUiScale);
                if (gBuild && !optionsOver)
                    gBuild->encode(enc, w, h, gUiScale);
                if (gOptions) // under the puck, as Buy mode's panel
                    gOptions->encode(enc, w, h, gUiScale);
                if (gLive)
                    gLive->encode(enc, w, h, gUiScale);
                gPie->encode(enc, w, h, gUiScale);
            });
            const bool headless = argument(@"--screenshot").has_value();
            const float cw = headless ? float(shotW) : float(view.bounds.size.width);
            const float ch = headless ? float(shotH) : float(view.bounds.size.height);
            if (auto q = argument(@"--pie")) {
                const os2::vm::ObjectId id = os2::vm::findInteractionObject(gPie->vm(), *q);
                const auto at = id ? gPie->anchor(id) : std::nullopt;
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
            for (size_t k = 0; k < steps.size() && !gPendingScene && !gQuitRequested; ++k) {
                gStepsUsed = k + 1;
                const os2::FlowStep& st = steps[k];
                if (st.kind == os2::FlowStep::Kind::Wait) {
                    runTicksHeadless(st.ticks);
                    pollFlowDialog();
                    continue;
                }
                const float clickX = st.x, clickY = st.y;
                // --right-click x,y: the right button (the live panel's thumbnails follow a Sim).
                const bool right = st.kind == os2::FlowStep::Kind::RightClick;
                const simd_float2 p = simd_make_float2(clickX, clickY);
                gPie->setCanvas(int(cw), int(ch));
                if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p)) {
                    pollFlowDialog();
                    continue;
                }
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
                    if (gLive->press(p, right) || right) {
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
                const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
                const os2::vm::Object* o = gPie->vm().object(id);
                std::printf("opensims2: click (%g, %g) picked object %d (%s)\n", clickX, clickY, id,
                            o && o->definition ? o->definition->fileName.c_str() : "nothing");
                if (id && !gPie->open(id, p))
                    std::printf("opensims2: object %d: no interactions available\n", id);
                for (size_t e = 0; e < gPie->entries().size(); ++e)
                    std::printf("opensims2: pie [%zu] %s\n", e, gPie->entries()[e].name.c_str());
            }
            // --ui-hover x,y: the mouse over the live panel (a queue icon's name).
            float hx, hy;
            if (auto h = argument(@"--ui-hover"); gLive && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gLive->setCanvas(int(cw), int(ch));
                gLive->hover(simd_make_float2(hx, hy));
            }
            runBuySteps();
            runBuildSteps();
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
        // --cinematic <scene>[@id[,id...]][:mirror]: a cinematic with those actors (in the CINE's
        // order; default: the lot's Sims for its Sim actors), started at once.
        if (auto cine = argument(@"--cinematic")) {
            std::string spec = *cine;
            bool mirror = false;
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
                } catch (const std::exception&) {
                }
            }
            startCinematic(*_viewDelegate.renderer, scene, actors, gLotSims.empty() ? 0 : gLotSims.front().id, mirror, true,
                           true);
        }
        if (gPendingScene)
            return; // headless: the steps left the lot (runScenes loads the next scene)
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
            NSArray<NSString*>* shotArgs = sceneArgs();
            for (NSUInteger i = 1; i + 1 < shotArgs.count; ++i) {
                float x, y;
                if ([shotArgs[i] isEqualToString:@"--dialog-click"] && std::sscanf(shotArgs[i + 1].UTF8String, "%f,%f", &x, &y) == 2)
                    dialogClicks.push_back(simd_make_float2(x, y));
            }
            double perfSim = 0, perfCapture = 0, perfWorst = 0;
            for (int f = 1; f <= frames; ++f) {
                const CFTimeInterval frameStart = CACurrentMediaTime();
                const double at = time * f / frames;
                // The VM in small steps, so that the Sims' turning is followed.
                for (double t = time * (f - 1) / frames + 1.0 / 30; gSimulation && t < at; t += 1.0 / 30) {
                    const double scene = gSimulation->update(base + t); // the scene clock (game speed)
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
                    updateEffects(*_viewDelegate.renderer, scene); // effects the VM starts and stops between shots
                    // --dialog-click x,y (repeatable): clicks, in turn, each dialog that opens.
                    if (gPie && gPie->dialogOpen() && dialogClick < dialogClicks.size()) {
                        gPie->setCanvas(int(shotW), int(shotH));
                        gPie->click(dialogClicks[dialogClick++]);
                    }
                    if (gAudio) {
                        updateAudio(base + t);
                        gAudio->renderOffline(base + t);
                    }
                }
                const CFTimeInterval simmed = CACurrentMediaTime();
                followSelectedSim();
                // --track-sim: each shot centred on the selected Sim (close-ups of poses).
                if ([shotArgs containsObject:@"--track-sim"] && gPie)
                    for (const auto& s : gLotSims)
                        if (s.id == gPie->sim())
                            view.camera->setPivot(simd_make_float3(s.actor->position[0], s.actor->position[1], s.actor->position[2]));
                view.camera->setViewport(float(shotW), float(shotH));
                view.camera->snap();
                poseActors(*_viewDelegate.renderer, base + at);
                _viewDelegate.renderer->setTime(at); // texture animations
                const uint32_t w = uiCanvas ? uiCanvas->first : 1280, h = uiCanvas ? uiCanvas->second : 720;
                gLastFrameW = w, gLastFrameH = h;
                const os2::FrameView fv = cinematicView({view.camera->view(), view.camera->projection(float(w) / float(h))}, at,
                                                        float(w) / float(h));
                MTLClearColor c = view.clearColor;
                auto img = _viewDelegate.renderer->capture(
                    w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), float(c.alpha)));
                std::string file = *shot;
                if (frames > 1) {
                    const auto dot = file.rfind('.');
                    file = file.substr(0, dot) + "-" + std::to_string(f) + (dot == std::string::npos ? "" : file.substr(dot));
                }
                const CFTimeInterval captured = CACurrentMediaTime();
                os2::writePng(file, img);
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
        gLotEntry = {true, lot.type, lot.family != 0}; // its loading screen
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
}

- (void)finishHood:(OS2View*)view {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    if (gGameAudio) // the buttons' click sounds
        gHood->ui().playSound = [](const std::string& name) { gGameAudio->playUi(name); };
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
                gHood->mouseDown(*view.camera, simd_make_float2(steps[k].x, steps[k].y), float(w), float(h));
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

int main() {
    @autoreleasepool {
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
