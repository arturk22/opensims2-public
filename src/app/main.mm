// macOS shell: an AppKit window whose MTKView drives os2::Renderer every frame
// and feeds input to the lot camera. The renderer itself is plain C++ on
// metal-cpp; this file only bridges the Objective-C objects across.
//
// Camera controls: WASD/arrows pan, Q/E or ,/. rotate, Z/X, +/- or the wheel
// zoom, R/F or Page Up/Down tilt, Home resets; with the mouse as in the game
// (app/camera_input.hpp): hold the right button and move away from where it
// went down to scroll, hold the middle button (or Option + right) to rotate
// and tilt, the mouse at the window's edge scrolls (--no-edge-scroll). --lighting <state> picks a lighting state from Lighting.txt
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
// background 0da36c7d). With the VM running, a left click on an object opens
// its pie menu for the selected Sim (--sim-id) and picking an entry queues it
// (Esc closes it); headless: --pie <object> [--pie-select N] and --click x,y
// (repeatable), which also start the VM (app/pie_menu.hpp). --vm runs the lot's objects in the SimAntics VM while it
// is shown, with the Sims following it (walking, animations); --vm-walk
// <sim-id>,<object-id>[,<anim>] makes a Sim walk to an object (then play entry
// <anim> of the global animation list); --run-vm <ticks> runs that many ticks
// (30 a second) before a --screenshot, and --frames <n> saves n screenshots
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
#include "formats/ui_resources.hpp"
#include "formats/ui_screen.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "render/camera.hpp"
#include "render/renderer.hpp"
#include "render/ui_layer.hpp"
#include "app/pie_menu.hpp"
#include "lot_audio.hpp"
#include "camera_input.hpp"
#include "lot_vm.hpp"
#include "vm_objects.hpp"

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

// The extracted game (data/game): --data <dir>, else $OS2_DATA, else the first
// data/game found walking up from the executable (the repo when run from build/).
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
    return std::nullopt;
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

// Value of `--name <value>`, if given.
std::optional<std::string> argument(NSString* name) {
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:name])
            return std::string(args[i + 1].UTF8String);
    return std::nullopt;
}

// Objects to show: every --object <CRES name | 0xGUID | OBJD name>, else the
// dining chair (unless a lot is shown).
std::vector<std::string> objectQueries() {
    std::vector<std::string> out;
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    for (NSUInteger i = 1; i + 1 < args.count; ++i)
        if ([args[i] isEqualToString:@"--object"])
            out.emplace_back(args[i + 1].UTF8String);
    if (out.empty() && !argument(@"--lot") && !argument(@"--skinned") && !argument(@"--sim") && !argument(@"--character"))
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
    for (const auto& [id, p] : gPlacedObjects) {
        std::optional<os2::Xform> saved;
        if (!p.contained)
            saved = os2::Xform{os2::Quat{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]},
                               {p.position[0], p.position[1], p.position[2]}};
        gObjectView->addPlaced(id, p.definition, p.cres, p.instance, saved);
    }
    for (const auto& s : gLotSims)
        gObjectView->addSim(s.id, s.actor);
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
                const os2::ObjectModel model = os2::loadObjectModel(index, p.cres, p.materials);
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
    // --object-anim <id>,<animation>: test aid, an object plays an animation
    // as if the VM's Animate Object had started it.
    if (auto anim = argument(@"--object-anim")) {
        const auto comma = anim->find(',');
        if (comma == std::string::npos)
            throw std::runtime_error("--object-anim: expected <object-id>,<animation>");
        gSimulation->onObjectAnimation(int16_t(std::stoi(anim->substr(0, comma))), anim->substr(comma + 1), 0);
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
    const os2::vm::Object* sim = gPie->vm().object(gPie->sim());
    std::printf("opensims2: pie menus for Sim %d (%s)\n", gPie->sim(),
                sim && sim->definition ? sim->definition->fileName.c_str() : "none");
}

// Loads --lot into the renderer in place of the test floor. Returns the middle
// of the lot at ground level, for the camera.
// --walls up|cutaway|down picks the wall mode; cutaway uses `viewDirection`.
std::optional<simd_float3> loadLot(os2::Renderer& renderer, os2::ResourceIndex& index, simd_float2 viewDirection) {
    const auto query = argument(@"--lot");
    if (!query)
        return std::nullopt;
    CFTimeInterval start = CACurrentMediaTime();
    os2::DbpfPackage pkg(lotPath(*query));
    const os2::Lot lot = os2::Lot::load(pkg);
    const os2::PatternCatalog patterns(index, gameDir() / "TSData/Res");
    os2::LotViewOptions view;
    view.viewDirection = {viewDirection.x, viewDirection.y};
    const std::string walls = argument(@"--walls").value_or("up");
    if (walls == "cutaway")
        view.walls = os2::LotViewOptions::Walls::Cutaway;
    else if (walls == "down")
        view.walls = os2::LotViewOptions::Walls::Down;
    else if (walls != "up")
        throw std::runtime_error("--walls: expected up, cutaway or down");
    const os2::ObjectModel model = os2::buildLotModel(lot, index, patterns, view);
    for (const auto& w : model.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    renderer.hideTestScene();
    renderer.addInstance(renderer.addObject(model, index), matrix_identity_float4x4);
    for (const auto& w : lot.warnings)
        std::fprintf(stderr, "opensims2: %s: %s\n", query->c_str(), w.c_str());
    const size_t objects = loadLotObjects(renderer, index, lot, *query);
    const size_t sims = loadLotSims(renderer, index, lot, pkg, lotPath(*query));
    NSArray<NSString*>* args = NSProcessInfo.processInfo.arguments;
    startAudio(index, lot);
    if (argument(@"--run-vm") || argument(@"--pie") || argument(@"--click") || [args containsObject:@"--vm"])
        startSimulation(renderer, index, lot, pkg, lotPath(*query));
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
    std::printf("opensims2: lot %s: %ux%u tiles, levels %d..%d, %zu walls, %zu paints (%zu parts), %zu roofs, "
                "%zu fence pieces, %zu of %zu objects and %zu Sims placed in %.2f s\n",
                query->c_str(), lot.width, lot.height, lot.minLevel, lot.minLevel + lot.levelCount - 1,
                lot.walls.size(), lot.paints.size(), model.parts.size(), lot.roofs.size(), fences.size(), objects,
                lot.objects.size(), sims, CACurrentMediaTime() - start);
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
// posed mesh, less the height the mesh is modelled at; the gap is a guess). Green, its material's colour: the
// mood colours are not done.
constexpr float kPlumbobGap = 0.2f, kPlumbobSpinSeconds = 2.316f;

void placePlumbob(os2::Renderer& renderer, double seconds) {
    if (gPlumbob < 0 || !gPie)
        return;
    simd_float4x4 at = matrix_identity_float4x4;
    at.columns[3] = simd_make_float4(0, 0, -1000, 1); // hidden
    for (const auto& s : gLotSims) {
        if (s.id != gPie->sim())
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
        gSimulation->update(seconds);
    auto isObject = [](const os2::SimActor* a) {
        for (const auto& [id, o] : gObjectActors)
            if (o == a)
                return true;
        return false;
    };
    for (auto& a : gActors)
        if (!isObject(a.get()))
            a->update(seconds);
    if (gObjectView)
        gObjectView->update();
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
            auto actor = std::make_unique<os2::SimActor>(os2::loadCharacterSim(index, parts, file));
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
            actor->model = renderer.addObject(actor->skin.model, index, true);
            renderer.addInstance(actor->model, matrix_identity_float4x4);
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
    if (!id)
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
};

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
    float right = float([self held:kD orKey:kRight]) - float([self held:kA orKey:kLeft]);
    float forward = float([self held:kW orKey:kUp]) - float([self held:kS orKey:kDown]);
    if (self.window.isKeyWindow) {
        _mouse.config().edgeScroll = !argument(@"--no-edge-scroll");
        const auto [mr, mf] = _mouse.panAxes(float(self.bounds.size.width), float(self.bounds.size.height));
        right = std::clamp(right + mr, -1.0f, 1.0f);
        forward = std::clamp(forward + mf, -1.0f, 1.0f);
    }
    _camera->pan(right, forward, dt);
    float tilt = float([self held:kR orKey:kPageUp]) - float([self held:kF orKey:kPageDown]);
    if (tilt != 0)
        _camera->tilt(tilt * 40.0f * float(dt));
}

- (void)keyDown:(NSEvent*)e {
    _held.insert(e.keyCode);
    if (e.isARepeat)
        return;
    switch (e.keyCode) {
    case kQ: case kComma: _camera->rotateBy(-1); break;
    case kE: case kPeriod: _camera->rotateBy(1); break;
    case kZ: case kEquals: case kPadPlus: _camera->zoomBy(1); break;
    case kX: case kMinus: case kPadMinus: _camera->zoomBy(-1); break;
    case kHome: _camera->reset(); break;
    case kEscape: if (gPie) gPie->close(); break;
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
    if (const int steps = _mouse.wheel(e.scrollingDeltaY, e.hasPreciseScrollingDeltas))
        _camera->zoomBy(steps);
}

- (void)dragRotate:(NSEvent*)e {
    const auto [yaw, tilt] = _mouse.rotateDrag(float(e.deltaX), float(e.deltaY));
    _camera->freeRotate(yaw);
    _camera->tilt(tilt);
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
    _rotating = (e.modifierFlags & NSEventModifierFlagOption) != 0;
    if (!_rotating)
        _mouse.beginAnchor();
}

- (void)rightMouseUp:(NSEvent*)e {
    [self trackMouse:e];
    _mouse.endAnchor();
    _rotating = false;
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
    if (!gPie)
        return;
    const simd_float2 p = [self canvasPoint:e];
    if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p))
        return;
    const float w = float(self.bounds.size.width), h = float(self.bounds.size.height);
    const simd_float4x4 m = simd_mul(_camera->projection(w / h), _camera->view());
    simd_float4 q = simd_mul(simd_inverse(m), simd_make_float4(2 * p.x / w - 1, 1 - 2 * p.y / h, 0.5f, 1));
    const simd_float3 eye = _camera->eye();
    const os2::vm::ObjectId id = gPie->pick(eye, simd_normalize(q.xyz / q.w - eye));
    if (id && !gPie->open(id, p))
        std::printf("opensims2: object %d: no interactions available\n", id);
    std::fflush(stdout);
}

- (void)mouseMoved:(NSEvent*)e {
    [self trackMouse:e];
    if (gPie)
        gPie->hover([self canvasPoint:e]);
}

- (void)mouseDragged:(NSEvent*)e {
    [self trackMouse:e];
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
    double dt = std::min(now - _last, 0.1); // no huge steps after a stall
    _last = now;

    [view applyHeldKeys:dt];
    view.camera->update(dt);
    CGSize size = view.drawableSize;
    float aspect = size.height > 0 ? float(size.width / size.height) : 1.0f;
    os2::FrameView frame{view.camera->view(), view.camera->projection(aspect)};
    poseActors(*_renderer, now - _start);
    if (view.bounds.size.width > 0)
        gUiScale = float(size.width / view.bounds.size.width);

    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    _renderer->draw((__bridge MTL::RenderPassDescriptor*)pass, (__bridge CA::MetalDrawable*)drawable, frame);
}
@end

@interface OS2AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation OS2AppDelegate {
    NSWindow* _window;
    OS2ViewDelegate* _viewDelegate; // MTKView holds its delegate weakly
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
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
        os2::ResourceIndex& index = *gIndex;
        const simd_float3 objects = loadObjects(*_viewDelegate.renderer, index);
        const auto skinned = loadSkinned(*_viewDelegate.renderer, index);
        const auto uiCanvas = loadUi(*_viewDelegate.renderer, index, view);
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
            _viewDelegate.renderer->setOverlay([](MTL::RenderCommandEncoder* enc, uint32_t w, uint32_t h) {
                if (gUi)
                    gUi->encode(enc, w, h);
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
                if (![args[i] isEqualToString:@"--click"] ||
                    std::sscanf(args[i + 1].UTF8String, "%f,%f", &clickX, &clickY) != 2)
                    continue;
                const simd_float2 p = simd_make_float2(clickX, clickY);
                if ((gPie->isOpen() || gPie->dialogOpen()) && gPie->click(p))
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
                for (size_t k = 0; k < gPie->entries().size(); ++k)
                    std::printf("opensims2: pie [%zu] %s\n", k, gPie->entries()[k].name.c_str());
            }
            std::fflush(stdout);
        }
        // --screenshot <file.png>: render one frame offscreen, save it and quit.
        if (auto shot = argument(@"--screenshot")) {
            // --run-vm <ticks>: the moment after that many VM ticks; --frames
            // <n>: n shots spread up to it (out-1.png ... out-n.png).
            const auto runVm = argument(@"--run-vm");
            const double time = std::stod(argument(@"--time").value_or(
                runVm ? std::to_string(std::stod(*runVm) / 30.0) : std::string("0")));
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
            for (int f = 1; f <= frames; ++f) {
                const double at = time * f / frames;
                // The VM in small steps, so that the Sims' turning is followed.
                for (double t = time * (f - 1) / frames + 1.0 / 30; gSimulation && t < at; t += 1.0 / 30) {
                    gSimulation->update(t);
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
                os2::writePng(file, img);
                std::printf("opensims2: wrote %s (%.2f s%s)\n", file.c_str(), at,
                            gSimulation ? (", " + std::to_string(gSimulation->ticks()) + " VM ticks").c_str() : "");
            }
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
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        buildMenu();
        OS2AppDelegate* delegate = [[OS2AppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
