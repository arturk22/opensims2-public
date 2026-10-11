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

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

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
#include "app/data_location.hpp"
#include "app/hood_mode.hpp"
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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
float gUiScale = 1; // drawable pixels per view point
// --hood: the neighborhood view (app/hood_mode.hpp) in place of a lot.
std::unique_ptr<os2::HoodMode> gHood;
// --hood <N> --hood-lot <id>: that lot's package, shown as --lot is.
std::optional<std::string> gHoodLot;
std::vector<std::string> processArguments();

// The extracted game (data/game): --data <dir>, else $OS2_DATA, else the first
// data/game found walking up from the executable (the repo when run from build/),
// else the packaged-app places (app/data_location.hpp).
std::optional<fs::path> findGameDir() {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
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
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i < args.count; ++i)
        out.emplace_back(args[i].UTF8String);
    return out;
}

// Value of `--name <value>`, if given.
std::optional<std::string> argument(NSString* name) {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
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
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
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

// Sound on the lot (app/lot_audio.hpp): --no-audio leaves it out, --mute
// keeps it silent; headless runs (--screenshot) open no device, and
// --sound-mix <out.wav> saves what would have played. --music <category>
// plays background music ("Build", "Buy", "CAS", "Nhood", or a radio
// station: "pop", "rnb", ...).
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
void setupLiveUi(os2::ResourceIndex& index);  // below setupPieMenu
std::optional<os2::RgbaImage> simPortrait(int16_t id);

// The camera shows the selected Sim's level (cCameraLive::Update).
void followSelectedSim() {
    if (gCameraFollow && gSimulation && gPie)
        gCameraFollow->follow(gSimulation->machine(), gPie->sim());
}

void startAudio(os2::ResourceIndex& index, const os2::Lot& lot) {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    if ([args containsObject:@"--no-audio"])
        return;
    os2::LotAudio::Options o;
    o.muted = [args containsObject:@"--mute"];
    o.device = !argument(@"--screenshot");
    o.music = argument(@"--music").value_or("");
    gAudio = std::make_unique<os2::LotAudio>(gameDir() / "TSData/Res", index, lot, o);
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
    gAudio->update(gSimulation ? &gSimulation->machine() : nullptr, seconds, l);
}

// --vm / --run-vm: the lot's objects in the VM, with its Sims attached to
// their actors; --vm-walk <sim>,<object>[,<anim>] starts a scripted walk.
void startSimulation(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                     const fs::path& lotFile) {
    CFTimeInterval start = CACurrentMediaTime();
    gSimulation = std::make_unique<os2::LotSimulation>(index, lot, pkg, lotFile,
                                                      [NSProcessInfo.processInfo.arguments containsObject:@"--init"]);
    for (const auto& s : gLotSims) {
        os2::DbpfPackage character(s.character);
        gSimulation->attach(*s.actor, s.id, os2::readCharacter(character, index), s.idle);
    }
    gObjectView = std::make_unique<os2::VmObjectView>(renderer, index, *gSimulation);
    gSnaps = std::make_unique<os2::AnimSnapView>(renderer, index, *gObjectView, gSimulation->machine());
    gSimulation->onSimAnimation = [](int16_t id, os2::SimActor& actor, const os2::Animation* anim, double at) {
        gSnaps->simAnimation(id, actor, anim, at);
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
    // A Sim that changes clothes gets the outfit of its character file.
    gSimulation->onOutfitChange = [&renderer, &index](int16_t id, os2::SimActor& actor, uint32_t category) {
        static std::vector<os2::SimPart> parts;
        const auto it = std::find_if(gLotSims.begin(), gLotSims.end(), [&](const LotSim& s) { return s.id == id; });
        if (it == gLotSims.end())
            return;
        try {
            if (parts.empty())
                parts = os2::loadSimParts(index);
            actor.skin = os2::loadCharacterSim(index, parts, it->character, category);
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
    setupPieMenu(index);
    if (liveUiRequested())
        setupLiveUi(index);
    if (liveUiRequested() || argument(@"--mode"))
        setupBuyMode(renderer, index);
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
    if (auto query = argument(@"--lot"))
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
    if (![NSProcessInfo.processInfo.arguments containsObject:@"--debug-queue"])
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

// The --buy/--hold/--place/--grab/--sell/--undo steps of a headless run, in order.
void runBuySteps() {
    if (!gBuy || !gBuy->active())
        return;
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
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
    renderer.addInstance(renderer.addObject(model, index), matrix_identity_float4x4);
    perfLap("lot model to the GPU");
    for (const auto& w : lot.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    const size_t objects = loadLotObjects(renderer, index, lot, *query);
    perfLap("loadLotObjects");
    const size_t sims = loadLotSims(renderer, index, lot, pkg, lotPath(*query));
    perfLap("loadLotSims");
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
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
    const auto fences = os2::lotFencePieces(lot, patterns, &fenceWarnings);
    std::map<std::string, int> fenceModels;
    for (const auto& piece : fences) {
        auto it = fenceModels.find(piece.model);
        if (it == fenceModels.end()) {
            int id = -1;
            try {
                id = renderer.addObject(os2::loadObjectModel(index, piece.model), index);
            } catch (const std::exception& e) {
                fenceWarnings.push_back(piece.model + ": " + e.what());
            }
            it = fenceModels.emplace(piece.model, id).first;
        }
        if (it->second < 0)
            continue;
        simd_float4x4 at;
        std::memcpy(&at, piece.transform.data(), sizeof at);
        renderer.addInstance(it->second, at);
    }
    for (const auto& w : fenceWarnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    perfLap("fences");
    std::printf("opensims2: lot %s: %ux%u tiles, levels %d..%d, %zu walls, %zu paints (%zu parts), %zu roofs, "
                "%zu fence pieces, %zu of %zu objects and %zu Sims placed in %.2f s\n",
                query->c_str(), lot.width, lot.height, lot.minLevel, lot.minLevel + lot.levelCount - 1,
                lot.walls.size(), lot.paints.size(), model.parts.size(), lot.roofs.size(), fences.size(), objects,
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
    static std::map<std::string, int> models;
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

void placePlumbob(os2::Renderer& renderer, double seconds) {
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
    }
    renderer.setInstanceTransform(gPlumbob, at);
}

// Poses every actor at `seconds` and hands the vertices to the renderer.
// The Sims first: objects in their hands follow their bones (gObjectView),
// then the objects' actors at the places that gives.
void poseActors(os2::Renderer& renderer, double seconds) {
    if (gSimulation)
        seconds = gSimulation->update(seconds); // the scene clock (game speed)
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
    updateAudio(seconds);
}

size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile) {
    const os2::LotObjects saved = os2::LotObjects::load(pkg);
    std::set<uint32_t> inWorld; // object IDs of the Sims standing on the lot
    for (const auto& st : saved.objects)
        if (st.person && st.x >= 0)
            inWorld.insert(uint32_t(uint16_t(st.id)));
    std::vector<os2::SimPart> parts;
    const std::string idle = argument(@"--sim-anim").value_or("a-idle-neutral-armsSide-wtC-breatheLoop");
    size_t placed = 0;
    for (const auto& o : lot.objects) {
        if (!o.person || !inWorld.count(o.instance & 0xFFFF))
            continue;
        const std::string stem = o.definition.substr(0, o.definition.find(" - "));
        try {
            const fs::path file = lotFile.parent_path().parent_path() / "Characters" / (stem + ".package");
            if (parts.empty())
                parts = os2::loadSimParts(index);
            perfLap("  loadSimParts");
            auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, parts, file));
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
        skin = os2::loadCharacterSim(index, os2::loadSimParts(index), characterPath(*character));
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
        skin = os2::loadSim(index, os2::loadSimParts(index), look);
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
    if (auto walk = argument(@"--walk")) {
        // Idle, then walk straight ahead for <seconds>, then idle again;
        // the walk's locomotion velocity moves the Sim.
        const double seconds = std::stod(*walk);
        const auto& idle = actor->animation(index, argument(@"--anim").value_or("a-idle-neutral-armsSide-wtC-breatheLoop"));
        const auto& walkAnim = actor->animation(index, "a-male-walk-alt0-normal");
        actor->cue(0, idle);
        actor->cue(0.5, walkAnim, {.fade = 0.3f});
        actor->cue(0.5 + seconds, idle, {.fade = 0.3f});
        actor->rootMotion = true;
        if (auto h = argument(@"--heading"))
            actor->heading = std::stof(*h) * 3.14159265f / 180;
        playing = "idle, walk, idle";
    } else if (auto name = argument(@"--anim")) {
        actor->cue(0, actor->animation(index, *name));
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
    // Shift: "Move ... Faster" (twice the speed).
    const float fast = (NSEvent.modifierFlags & NSEventModifierFlagShift) ? 2.0f : 1.0f;
    float right = float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]);
    float forward = float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]);
    right *= fast, forward *= fast;
    if (self.window.isKeyWindow) {
        _mouse.config().edgeScroll = !argument(@"--no-edge-scroll");
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
        if (gBuy && gBuy->active() && gBuy->held()) { // drops what Buy mode holds
            gBuy->cancel();
            break;
        }
        if (gHood) {
            gHood->escape();
            break;
        }
        // A lot entered from the neighborhood: back to it (unless the pie menu is open).
        if (auto hood = argument(@"--hood"); hood && gHoodLot && !(gPie && gPie->isOpen()))
            os2::restartWith(os2::hoodArguments(processArguments(), *hood));
        if (gPie) gPie->close();
        break;
    // The speed buttons' accelerators (their tips: "Pausa - P", "Velocidade Normal - 1", ...).
    case kP: if (gLive) gLive->togglePause(); break;
    case k1: case k2: case k3: if (gLive) gLive->setSpeed(e.keyCode - k1 + 2); break;
    default: break;
    }
}

- (void)keyUp:(NSEvent*)e {
    _held.erase(e.keyCode);
}

- (void)flagsChanged:(NSEvent*)e {
    // A key released while Cmd was down never sends keyUp.
    if (e.modifierFlags & NSEventModifierFlagCommand)
        _held.clear();
}

- (void)scrollWheel:(NSEvent*)e {
    _camera->wheel(_mouse.wheelNotches(e.scrollingDeltaY, e.hasPreciseScrollingDeltas));
}

// The game's rotate request: pixels dragged (only the dominant axis counts).
- (void)dragRotate:(NSEvent*)e {
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
    if (gHood) {
        gHood->mouseDown(*_camera, [self canvasPoint:e], float(self.bounds.size.width), float(self.bounds.size.height));
        return;
    }
    if (!gPie)
        return;
    const simd_float2 p = [self canvasPoint:e];
    if (gPie->dialogOpen() && gPie->click(p))
        return;
    if (gPie->isOpen() && gPie->press(p))
        return;
    if (gBuy && gBuy->active() && gBuy->press(p))
        return;
    if (gLive && gLive->press(p))
        return;
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = _camera->eye();
    if (gBuy && gBuy->active()) { // the object tool, not the pie menu
        gBuy->click(eye, simd_normalize(q.xyz / q.w - eye));
        return;
    }
    const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
    if (id && !gPie->open(id, p))
        std::printf("opensims2: object %d: no interactions available\n", id);
    std::fflush(stdout);
}

// The pie menu picks on release (a drag from the opening press is a gesture).
- (void)mouseUp:(NSEvent*)e {
    if (gPie && gPie->isOpen())
        gPie->release([self canvasPoint:e]);
}

- (void)mouseMoved:(NSEvent*)e {
    [self trackMouse:e];
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
}

- (void)mouseDragged:(NSEvent*)e {
    [self trackMouse:e];
    if (gPie)
        gPie->hover([self canvasPoint:e]);
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

    [view applyHeldKeys:dt];
    if (gLive)
        gLive->tick(now - _start);
    followSelectedSim();
    view.camera->update(dt);
    CGSize size = view.drawableSize;
    float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
    os2::FrameView frame{view.camera->view(), view.camera->projection(aspect)};
    poseActors(*_renderer, now - _start);
    if (gBuy)
        gBuy->update();
    if (view.bounds.size.width > 0)
        gUiScale = float(size.width / view.bounds.size.width);

    const CFTimeInterval beforeDrawable = CACurrentMediaTime();
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    perf.waited = CACurrentMediaTime() - beforeDrawable;
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
    OS2ViewDelegate* _viewDelegate; // MTKView holds its delegate weakly
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
    view.clearColor = MTLClearColorMake(0.18, 0.22, 0.28, 1.0);
    view.preferredFramesPerSecond = 60;

    try {
        [view setCamera:std::make_unique<os2::LotCamera>(loadLiveCamera())];
        gCamera = view.camera;
        _viewDelegate = [[OS2ViewDelegate alloc] initWithView:view];
        applyLighting(*_viewDelegate.renderer);
        gIndex = std::make_unique<os2::ResourceIndex>(gameDir() / "TSData/Res");
        perfLap("ResourceIndex");
        os2::ResourceIndex& index = *gIndex;
        if (auto hood = argument(@"--hood")) {
            if (auto lot = argument(@"--hood-lot")) {
                gHoodLot = os2::hoodLotPackage(gameDir() / "TSData/Res", *hood, uint32_t(std::stoul(*lot))).string();
            } else {
                [self startHood:view query:*hood];
                return;
            }
        }
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
            _viewDelegate.renderer->setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
                if (gUi)
                    gUi->encode(enc, w, h);
                if (gBuy) // under the puck
                    gBuy->encode(enc, w, h, gUiScale);
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
            NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
            for (NSUInteger i = 1; i + 1 < args.count; ++i) {
                float clickX, clickY;
                // --right-click x,y: the right button (the live panel's thumbnails follow a Sim).
                const bool right = [args[i] isEqualToString:@"--right-click"];
                if ((![args[i] isEqualToString:@"--click"] && !right) ||
                    std::sscanf(args[i + 1].UTF8String, "%f,%f", &clickX, &clickY) != 2)
                    continue;
                const simd_float2 p = simd_make_float2(clickX, clickY);
                if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p))
                    continue;
                if (gBuy && gBuy->active()) {
                    gBuy->setCanvas(int(cw), int(ch));
                    if (gBuy->press(p))
                        continue;
                }
                if (gLive) {
                    gLive->setCanvas(int(cw), int(ch));
                    if (gLive->press(p, right) || right)
                        continue;
                }
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
                for (size_t k = 0; k < gPie->entries().size(); ++k)
                    std::printf("opensims2: pie [%zu] %s\n", k, gPie->entries()[k].name.c_str());
            }
            // --ui-hover x,y: the mouse over the live panel (a queue icon's name).
            float hx, hy;
            if (auto h = argument(@"--ui-hover"); gLive && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gLive->setCanvas(int(cw), int(ch));
                gLive->hover(simd_make_float2(hx, hy));
            }
            runBuySteps();
            if (auto h = argument(@"--ui-hover"); gBuy && h && std::sscanf(h->c_str(), "%f,%f", &hx, &hy) == 2) {
                gBuy->setCanvas(int(cw), int(ch));
                gBuy->hover(simd_make_float2(hx, hy));
            }
            std::fflush(stdout);
        }
        // --screenshot <file.png>: render one frame offscreen, save it and quit.
        if (auto shot = argument(@"--screenshot")) {
            // --run-vm <ticks>: the moment after that many VM ticks; --frames
            // <n>: n shots spread up to it (out-1.png ... out-n.png).
            const auto runVm = argument(@"--run-vm");
            const double time = std::stod(argument(@"--time").value_or(
                runVm ? std::to_string(std::stod(*runVm) / os2::LotSimulation::kTicksPerSecond) : std::string("0")));
            const int frames = std::max(1, std::stoi(argument(@"--frames").value_or("1")));
            poseActors(*_viewDelegate.renderer, 0);
            std::vector<simd_float2> dialogClicks;
            size_t dialogClick = 0;
            NSArray<NSString*>* shotArgs = NSProcessInfo.processInfo.arguments;
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
                    const double scene = gSimulation->update(t); // the scene clock (game speed)
                    // The lights follow the VM clock as in a live run (the
                    // day/night switch and its crossfade start when the clock gets there).
                    if (gLighting && gSimulation->ticks() % 30 == 0)
                        gLighting->update(scene);
                    if (gLive)
                        gLive->tick(t);
                    // --dialog-click x,y (repeatable): clicks, in turn, each dialog that opens.
                    if (gPie && gPie->dialogOpen() && dialogClick < dialogClicks.size()) {
                        gPie->setCanvas(int(shotW), int(shotH));
                        gPie->click(dialogClicks[dialogClick++]);
                    }
                    if (gAudio) {
                        updateAudio(t);
                        gAudio->renderOffline(t);
                    }
                }
                const CFTimeInterval simmed = CACurrentMediaTime();
                followSelectedSim();
                view.camera->setViewport(float(shotW), float(shotH));
                view.camera->snap();
                poseActors(*_viewDelegate.renderer, at);
                _viewDelegate.renderer->setTime(at); // texture animations
                const uint32_t w = uiCanvas ? uiCanvas->first : 1280, h = uiCanvas ? uiCanvas->second : 720;
                os2::FrameView fv{view.camera->view(), view.camera->projection(float(w) / float(h))};
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
                gAudio->renderOffline(time);
                std::printf("opensims2: sound: %s\n", gAudio->summary().c_str());
                if (auto mix = argument(@"--sound-mix"))
                    gAudio->writeMix(*mix);
            }
            std::fflush(stdout);
            std::exit(0);
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
}

// --hood <N>: the neighborhood view (app/hood_mode.hpp); with --screenshot
// renders one frame and quits.
- (void)startHood:(OS2View*)view query:(const std::string&)query {
    os2::Renderer& renderer = *_viewDelegate.renderer;
    gHood = std::make_unique<os2::HoodMode>(renderer, *gIndex, gameDir(), query, processArguments());
    for (const auto& line : gHood->view().report())
        std::printf("opensims2: %s\n", line.c_str());
    [view setCamera:gHood->makeCamera()];
    gCamera = view.camera;
    view.clearColor = MTLClearColorMake(0.42, 0.55, 0.72, 1.0);
    if (auto z = argument(@"--zoom"))
        view.camera->zoomBy(std::stoi(*z));
    if (auto o = argument(@"--orientation"))
        view.camera->rotateBy(std::stoi(*o));
    view.camera->snap();
    gHood->ui().attach((__bridge MTL::Device*)view.device, uint32_t(view.colorPixelFormat),
                       uint32_t(view.depthStencilPixelFormat));
    renderer.setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
        gHood->ui().encode(enc, w, h, gUiScale);
    });
    std::fflush(stdout);
    if (auto shot = argument(@"--screenshot")) {
        const uint32_t w = 1280, h = 720;
        view.camera->setViewport(float(w), float(h));
        gHood->scriptedView(*view.camera, float(w), float(h));
        view.camera->snap();
        const os2::FrameView fv{view.camera->view(), view.camera->projection(float(w) / float(h))};
        MTLClearColor c = view.clearColor;
        const auto img = renderer.capture(w, h, fv, simd_make_float4(float(c.red), float(c.green), float(c.blue), 1));
        os2::writePng(*shot, img);
        std::printf("opensims2: wrote %s\n", shot->c_str());
        std::fflush(stdout);
        std::exit(0);
    }
    view.delegate = _viewDelegate;
    _window.contentView = view;
    _window.title = [NSString stringWithUTF8String:("opensims2 - " + gHood->view().displayName()).c_str()];
    [_window center];
    _window.acceptsMouseMovedEvents = YES;
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
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
