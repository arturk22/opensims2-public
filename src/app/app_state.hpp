// The app shell's shared state and functions (internal: only the shell's
// files include it). The shell was one file, main.mm, until round 26; its
// code now lives by area, each file's functions in namespace os2app:
//   main.mm             main(), the app delegate (scene loading and switching,
//                       the loading screen) and the view's delegate (frames)
//   app_state.cpp       SceneState gScene and the app's globals, unload/release
//   app_args.mm         the game's folder, the scene's arguments and flags
//   app_view.mm/.h      OS2View: keys, mouse, the game's cursors
//   app_delegate.h      the app delegate's state, shared with its scene parts
//   scene_setup.mm      the main menu, neighborhood and Create-a-Sim scenes
//   scene_lot.mm        the lot scene: its UI, headless steps and screenshots
//   lot_loading.mm      the lot, objects, Sims, --sim/--anim, --ui, --lighting
//   lot_vm_setup.mm     the lot's VM and audio, every VM hook, the VM test aids
//   lot_view.mm         floors, levels, mouse rays, cut walls, the lot model
//   lot_actors.cpp      the Sims the VM makes, the actors posed each frame
//   live_setup.mm       pie menu, live panel, tutorial, Sim selection, Buy/Build
//   live_dialogs.cpp    portraits, thumbnails, wardrobe, rack, vanity
//   plumbob_effects.mm  the plumbob, the censor, the particle effects
//   cinematic_host.cpp  the cinematics' host (camera, state, letterbox)
//   flow_host.mm        game flow: audio, scene requests, save, dialogs, options
//   debug_probes.cpp    the OS2_DEBUG_* / OS2_THEN_AT test aids, pose sheets
#pragma once

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#endif

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

// --- The scene's state ---------------------------------------------------------
//
// Everything that belongs to the scene being shown (a lot, a neighborhood,
// Create-a-Sim, the main menu) lives in one object, gScene: the modes and
// views over the scene's VM and renderer, the renderer ids, and every cache
// keyed by what only means something in one scene (object IDs, Sim IDs,
// family numbers, the lot's files). resetSceneState() replaces it with a
// fresh one in ONE place: unloadScene on every switch (lot to neighborhood,
// neighborhood to neighborhood, lot to lot) and an atexit handler on quit.
// The defaulted move assignment resets the members in their order here, so
// that order is the teardown order (the modes and views before the VM they
// hold, the effects before the renderer's next scene: round 22's crash).
// The globals the rest of the shell uses (gBuild, gLotSims, ...) are
// references into it. What outlives scenes (the resource index, the game's
// audio, the user's settings) stays outside, and the caches keyed by the
// game's read-only data (GUIDs, resource names, file paths of TSData) are
// in gGameData (app/game_data_caches.hpp). On quit releaseAppState() frees
// both, the scene first.
// A new per-scene global or function static goes in here, never beside it
// (round 23, docs/game-flow-mac.md §5; OS2_DEBUG_SCENE_STATE=1 prints its sizes
// around each unload).

struct PickBox {
    bool contained = false; // saved in a slot: the box follows where the VM draws it
    uint32_t instance;
    simd_float3 lo, hi;
    simd_float4x4 at;
    std::shared_ptr<const os2::PickMesh> mesh; // its model's triangles (app/pie_menu.hpp)
};
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
    std::vector<os2::Xform> pose; // its model's local pose as saved (a chair pushed in), empty: the rest pose
};
struct LotSim {
    os2::SimActor* actor;
    int16_t id; // object ID (the cObject's instance)
    fs::path character;
    std::string idle;
};
// The "Salvar?" questions put up by the game flow (see uiText's caller).
enum class FlowDialog { None, Leave, Quit };
// What a scene forces while it plays (the binary's Start/Stop,
// docs/cinematics-mac.md §6): the game speed (here normal and unpaused,
// inferred: a paused game would freeze the scene) and, with "Show Entire
// House" (flag 0x08; movein_cine always), the walls up and the top floor.
// Put back when the scene ends.
struct CinematicForced {
    bool active = false;
    int16_t speed = 0, paused = 0;
    std::optional<int> walls; // the wall mode before
    std::optional<int> level;                       // the camera's floor before
    // The scene's targets for the cutaway (AddTargetLocation): its actors
    // where they stood when it started, x, y and the lot's level.
    std::vector<std::array<float, 3>> targets;
};

struct SceneState {
    // (The order is the teardown order: see above.)
    FlowDialog flowDialog = FlowDialog::None;
    // (The VM's cinematic request list stays counted: gCinematicSerialSeen is the app's.)
    std::unique_ptr<os2::CinematicPlayer> cinematic;
    std::optional<double> cinematicStartAt;
    CinematicForced cinematicForced;
    std::map<int16_t, double> simAnimationAt;
    std::map<int16_t, fs::path> bornCharacters;
    std::set<int16_t> simsLookedAt;
    os2::ScreenFxLayer::Params screenFxParams;
    double crossfadeAt = -1e9;
    std::unique_ptr<os2::BuildMode> build;
    std::unique_ptr<os2::BuyMode> buy;
    std::unique_ptr<os2::TutorialOverlay> tutorial;
    std::unique_ptr<os2::LiveUi> live;
    std::unique_ptr<os2::PieMenuHost> pie;
    std::unique_ptr<os2::OptionsPanel> options;
    std::unique_ptr<os2::MainMenu> menu;
    std::unique_ptr<os2::HoodMode> hood;
    std::unique_ptr<os2::CasMode> cas;
    std::unique_ptr<os2::DoorPortals> doors;
    std::unique_ptr<os2::LotLighting> lighting;
    std::unique_ptr<os2::AnimSnapView> snaps;
    std::unique_ptr<os2::VmObjectView> objectView;
    std::unique_ptr<os2::TvBroadcast> tv;
    // The effects draw through the scene's renderer (its scene hook), which
    // the next scene replaces: kept, they hooked into the freed renderer and
    // crashed on the exit after a scene switch (~LotEffects, SIGBUS/SIGSEGV).
    std::unique_ptr<os2::LotEffects> effects;
    bool effectsFailed = false;
    std::unique_ptr<os2::LotSimulation> simulation;
    std::unique_ptr<os2::LotAudio> audio;
    std::unique_ptr<os2::LotCameraFollow> cameraFollow;
    std::unique_ptr<os2::UiLayer> ui;
    std::map<int16_t, os2::SimActor*> objectActors;
    std::function<os2::SimActor*(int16_t)> objectActor; // the actor animating an object (made on first use; captures the scene's renderer)
    std::vector<LotSim> lotSims;
    std::vector<std::unique_ptr<os2::SimActor>> actors;
    std::map<int16_t, PlacedObject> placedObjects;
    // The renderer's ids (round 20: kept across scenes, they named the next
    // scene's models and instances).
    std::map<std::string, int> fenceModels;
    std::vector<int> fenceInstances;
    std::map<const os2::SimActor*, int> simInstances;
    std::map<std::pair<int16_t, int16_t>, std::unique_ptr<os2::RgbaImage>> dialogThumbnails;
    std::map<int16_t, std::string> madePickBoxes;
    std::vector<PickBox> pickBoxes;
    int plumbob = -1;
    float plumbobBottom = 0;
    bool vmPlumbob = false;
    std::set<int16_t> hiddenPlumbobs;
    std::optional<os2::Xform> appPlumbobAt;
    std::string plumbobMaterial;
    std::map<std::string, int> plumbobModels;
    std::optional<os2::LotLightMapLayout> lightMapLayout;
    std::unique_ptr<os2::Lot> lotShape;
    std::unique_ptr<os2::PatternCatalog> lotPatterns;
    os2::LotViewOptions lotView;
    std::vector<bool> lotCutaway; // the walls lotView's cutaway cut (os2::lotCutawayWalls)
    // cWallCutawayManager's selected Sim (SetSelectedSimLocation, docs/ui-live-mac.md
    // 8d): where it was last seen on the level shown, and the point the cutaway
    // uses, taken again only when the Sim changes room (tile outdoors) or the
    // camera or the view changes.
    struct CutawaySim {
        bool stored = false;
        int level = 0;
        float x = 0, y = 0;
        std::optional<std::array<float, 3>> used;
        std::array<float, 6> camera{};
        int mode = -1, viewLevel = 0;
    } cutawaySim;
    // cWallCutawayManager's room memory (cutawayMemorySize, 3): the last
    // rooms with a cinematic's target, oldest first (Expose). Kept from one
    // scene to the next (only the message 0x2B4D18FB clears it in the game;
    // here a new lot, inferred).
    std::vector<uint16_t> cutawayRooms;
    int lotInstance = -1;
    int lotModel = -1;
    os2::LotModelCache lotCache;
    std::optional<std::string> hoodLot;
    bool lotVacant = false;
    fs::path lotFile;
    size_t stepsUsed = 0;
    double headlessClock = 0;
    os2::LotCamera* camera = nullptr;

    // Globals that unloadScene did not reset (they leaked into the next scene).
    size_t tutorialRequestsSeen = 0;
    bool levelsHidden = false;
    std::optional<os2::SimShadowLight> staticShadowLight;
    std::set<const os2::SimActor*> carriedSims;
    std::map<const os2::SimActor*, const os2::SimActor*> carriers;
    fs::path lotCharactersDir;
    double cinematicPrepared = -1;
    double cinematicLast = 0;
    bool cinematicWholeHouse = false;
    double sceneNow = 0;
    std::optional<os2::FrameView> lastFrameView;

    // Caches that were function statics (keyed by Sim ID, object ID, family
    // number or the neighborhood's files, so they meant something else, or
    // were stale, in the next scene: round 20's leftovers).
    std::map<int16_t, os2::cas::CharacterSpec> mirrorEdits; // the vanity's look being edited, per Sim ID
    std::set<int16_t> wardrobeFilled; // families whose missing clothing bin familyBin filled
    std::map<uint32_t, fs::path> portraitFiles; // neighborPortrait: character file by OBJD GUID
    fs::path portraitFilesDir;                  // ... of this Characters folder
    std::map<fs::path, std::map<uint32_t, fs::path>> characterFiles; // characterFileOf: folder -> GUID -> file
    std::map<uint32_t, std::optional<fs::path>> embeddedCharacters;  // unique NPCs' characters written, by GUID
    std::map<int16_t, std::string> censorReports; // OS2_DEBUG_CENSOR: the last report per Sim
    size_t thenAtDone = 0;                         // OS2_THEN_AT: the items done
    std::map<std::pair<int, std::string>, bool> posesFlagged; // OS2_DEBUG_POSES: (Sim, kind) currently flagged
    std::map<int, std::string> watchedObjects;     // OS2_WATCH_OBJECT: the last line per object
    uint64_t layersReportedAt = ~0ull;             // OS2_DEBUG_LAYERS: the tick last reported
    std::map<int, std::pair<float, float>> doorLeaves; // OS2_DEBUG_DOORS: the leaves' last angles per object
    struct InteractionsAsked {                     // objectHasInteractions' last answer
        os2::vm::ObjectId id = 0;
        double at = -1;
        bool result = false;
    } interactionsAsked;

    // The sizes of the scene's containers (OS2_DEBUG_SCENE_STATE).
    std::string sizes() const;
};
extern SceneState gScene;
// Counts the scenes torn down: run-long tools (the debug detectors) forget
// the last scene's Sims when it changes.
extern uint64_t gSceneGeneration;

// Replaces the scene's state with a fresh one (see above).
void resetSceneState();

// The app's caches of the game's read-only data (CAS catalog, thumbnails,
// pick meshes, Sim parts, ...): app/game_data_caches.hpp.
extern os2::GameDataCaches& gGameData;

constexpr const char* kCameraConfig = "TSData/Res/UserData/Cameras/cameras.txt";
constexpr const char* kLightingConfig = "TSData/Res/Lights/Lighting.txt";
constexpr uint32_t kTypeCres = 0xE519C933, kTypeObjd = 0x4F424A44;

// --- The globals (app_state.cpp: their comments) ----------------------------

extern std::unique_ptr<os2::PieMenuHost>& gPie;
extern std::unique_ptr<os2::LiveUi>& gLive;
extern std::unique_ptr<os2::TutorialOverlay>& gTutorial; // over a lot of the Tutorial neighborhood (app/tutorial.hpp)
extern size_t& gTutorialRequestsSeen;
extern std::unique_ptr<os2::BuyMode>& gBuy;
extern std::unique_ptr<os2::BuildMode>& gBuild;
extern std::unique_ptr<os2::Lot>& gLotShape;
extern std::unique_ptr<os2::PatternCatalog>& gLotPatterns;
extern os2::LotViewOptions& gLotView; // the view the lot model was last built for
extern int& gLotInstance;
extern int& gLotModel;           // the renderer model of gLotInstance (replaced on rebuilds)
extern os2::WallView gWallView;
extern os2::LotModelCache& gLotCache;
extern bool& gLevelsHidden; // levels above the camera's are hidden (in the game: --ui live, --mode)
extern std::vector<PickBox>& gPickBoxes;
extern std::map<int16_t, PlacedObject>& gPlacedObjects;
extern std::map<std::string, int>& gFenceModels; // fence CRES -> model id (-1: failed)
extern std::vector<int>& gFenceInstances;        // instances placeFencePieces places, reused when it runs again
extern std::map<const os2::SimActor*, int>& gSimInstances; // a Sim's actor -> its instance (hidden above the level shown)
extern std::map<std::pair<int16_t, int16_t>, std::unique_ptr<os2::RgbaImage>>& gDialogThumbnails; // (object, neighbor) -> picture
extern std::map<int16_t, std::string>& gMadePickBoxes; // objects made in play: id -> CRES their pick box was made for
extern std::vector<std::unique_ptr<os2::SimActor>>& gActors;
extern std::map<int16_t, os2::SimActor*>& gObjectActors; // objects the VM has animated (in gActors)
extern std::unique_ptr<os2::CinematicPlayer>& gCinematic; // the cinematic prepared or playing (see updateCinematic)
extern std::unique_ptr<os2::TvBroadcast>& gTv;             // TV channels on the lot's screens
extern float gUiScale; // drawable pixels per view point
extern std::unique_ptr<os2::HoodMode>& gHood;
extern std::optional<os2::LotCamera::Pose> gHoodReloadPose; // the camera across a neighborhood reload
extern std::unique_ptr<os2::CasMode>& gCas; // --cas: Create-a-Sim (app/cas_mode.hpp)
extern std::optional<std::string>& gHoodLot;
extern bool& gLotVacant;
extern std::unique_ptr<os2::UserData> gUser;
extern std::unique_ptr<os2::MainMenu>& gMenu;
extern std::unique_ptr<os2::OptionsPanel>& gOptions;
extern os2::GameSettings gSettings;
extern std::optional<os2::Scene> gPendingScene;
extern std::optional<uint32_t> gCreatedFamily;
extern bool gQuitRequested;
extern size_t& gStepsUsed;    // --click/--wait steps this scene has taken (headless)
extern double& gHeadlessClock; // the caller's clock of headless VM runs (--wait, --run-vm)
extern fs::path& gLotFile;        // the package the running lot came from (saved back to)
extern std::function<void()> gOnSceneRequest; // the app delegate's switch (interactive)
extern std::function<void()> gIntroMovies;
extern os2::BackgroundJob gLoadJob;
extern bool gSceneLoading;                    // main thread
extern std::function<void()> gPollLoad;               // the app delegate's check, each frame while loading
extern os2::LotEntry gLotEntry;                       // the lot being entered (its loading screen)
extern std::unique_ptr<os2::ResourceIndex> gIndex;
extern std::unique_ptr<os2::LotSimulation>& gSimulation;
extern std::unique_ptr<os2::VmObjectView>& gObjectView;
extern std::unique_ptr<os2::AnimSnapView>& gSnaps; // models the animations' snap events put in the Sims' hands
extern std::unique_ptr<os2::LotLighting>& gLighting;
extern std::optional<os2::LotLightMapLayout>& gLightMapLayout; // the lot's light-map atlas
extern std::optional<os2::SimShadowLight>& gStaticShadowLight;
extern std::unique_ptr<os2::DoorPortals>& gDoors;
extern std::vector<LotSim>& gLotSims;
extern std::set<const os2::SimActor*>& gCarriedSims;
extern std::map<const os2::SimActor*, const os2::SimActor*>& gCarriers;
extern std::unique_ptr<os2::UiResources> gTexts; // Text/*.package's strings (uiText, the VM's textTables)
extern std::map<int16_t, fs::path>& gBornCharacters; // neighbor ID -> character file written in this session
extern std::set<int16_t>& gSimsLookedAt;              // person objects already adopted or given up on
extern fs::path& gLotCharactersDir;                   // the lot's neighborhood Characters folder
extern int& gPlumbob; // renderer instance of the plumbob over the selected Sim (placePlumbob)
extern float& gPlumbobBottom; // lowest point of its mesh (modelled about 1.24 over the origin)
extern std::unique_ptr<os2::GameAudio> gGameAudio;
extern std::unique_ptr<os2::LotAudio>& gAudio;
extern os2::LotCamera*& gCamera;
extern std::unique_ptr<os2::LotCameraFollow>& gCameraFollow; // --lot: pivot held to the lot, on the Sim's floor

// --- The shell's functions, by file -----------------------------------------

// app_state.cpp: the game's Sim parts (gGameData), read once for the app's
// life; the scene's unload and the app's release on quit.
const std::vector<os2::SimPart>& simParts(os2::ResourceIndex& index);
void unloadScene();
void releaseAppState();

// app_args.mm: the game's folder, the scene's arguments and the flags read
// from them.
void setSceneArguments(const std::vector<std::string>& args);
std::optional<fs::path> findGameDir();
fs::path gameDir();
os2::CameraDef loadLiveCamera();
std::vector<std::string> processArguments();
bool reflectionsOn();
bool perfOn();
extern volatile std::sig_atomic_t gLiveShotWanted;
const char* liveShotFile();
void perfLap(const char* what);
std::optional<std::string> lotQuery();
std::vector<std::string> objectQueries();
fs::path lotPath(const std::string& query);
fs::path characterPath(const std::string& query);
bool liveUiRequested();
std::optional<std::string> startMode();

// lot_loading.mm: the lot, its objects and Sims, the test objects, the
// skinned Sim of --sim / --character / --skinned, --ui and --lighting.
size_t loadLotObjects(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                      const std::string& lotName);
void offerCommunityLotImage(os2::ResourceIndex& index, const os2::Lot& lot);
std::optional<simd_float3> loadLot(os2::Renderer& renderer, os2::ResourceIndex& index, simd_float2 viewDirection);
simd_float3 loadObjects(os2::Renderer& renderer, os2::ResourceIndex& index);
os2::Skeleton loadSkeleton(os2::ResourceIndex& index, const std::string& cres);
bool spawnSimActor(os2::Renderer& renderer, os2::ResourceIndex& index, const fs::path& file, int16_t id,
                   simd_float3 position, float heading, double phase, const std::string& label);
bool lotVmWillRun();
size_t loadLotSims(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                   const fs::path& lotFile);
std::optional<simd_float3> loadSkinned(os2::Renderer& renderer, os2::ResourceIndex& index);
extern std::unique_ptr<os2::UiLayer>& gUi;
void applyLighting(os2::Renderer& renderer);

// lot_vm_setup.mm: the lot's audio and the lot's objects in the VM (--vm,
// --run-vm, the live UI) with every hook the views need.
void startAudio(os2::ResourceIndex& index, const os2::Lot& lot);
void updateAudio(double seconds);
void startSimulation(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot, os2::DbpfPackage& pkg,
                     const fs::path& lotFile);

// lot_view.mm: the floor and level shown, the mouse ray on the lot, the cut
// walls, the lot model for the view, the fences.
void changeFloor(int delta);
void followSelectedSim();
size_t placeFencePieces(os2::Renderer& renderer, os2::ResourceIndex& index, const os2::Lot& lot,
                        const os2::PatternCatalog& patterns, std::vector<std::string>* warnings,
                        int level = os2::LotViewOptions::kAllLevels);
int viewLevel();
std::optional<std::pair<simd_float2, int>> groundUnderRay(simd_float3 eye, simd_float3 dir);
std::optional<float> floorUnderRay(simd_float3 eye, simd_float3 dir);
bool openGoHere(simd_float3 eye, simd_float3 dir, simd_float2 at);
void openClicked(os2::vm::ObjectId id, simd_float3 eye, simd_float3 dir, simd_float2 at);
bool onCutWall(const os2::Xform& at);
os2::LotViewOptions wantedLotView();
bool simInTheWorld(int16_t id);
void updateLotView(os2::Renderer& renderer, bool sims);
void setWallMode(int mode);
void rebuildLotModel(os2::Renderer& renderer, os2::ResourceIndex& index);

// live_setup.mm: the pie menu, the live panel, the tutorial, the Sim
// selection, Buy and Build mode and their headless steps.
void setupPieMenu(os2::ResourceIndex& index);
void setupLiveUi(os2::ResourceIndex& index);
void setupTutorial(os2::ResourceIndex& index);
bool selectFamilySim(os2::vm::ObjectId id);
void selectNextFamilySim();
void keepSelectedSimHome();
void pollTutorial(double seconds);
void setupBuyMode(os2::Renderer& renderer, os2::ResourceIndex& index);
void toggleDayNight();
void releaseDayNight();
void setupBuildMode(os2::Renderer& renderer, os2::ResourceIndex& index);
void runBuildSteps();
void runBuySteps();

// live_dialogs.cpp: the portraits and thumbnails, the aspiration dialog's
// goals, the wardrobe, clothing rack and vanity.
std::optional<os2::RgbaImage> simPortrait(int16_t id);
std::optional<os2::RgbaImage> neighborPortrait(int16_t neighborId);
const os2::RgbaImage* dialogThumbnail(int16_t object, int16_t neighbor);
void setupAspirationWants(os2::ResourceIndex& index);
void setupWardrobe(os2::ResourceIndex& index);

// plumbob_effects.mm: the plumbob, the censor, the particle effects.
void choosePlumbob(os2::Renderer& renderer);
void updateCensor(os2::Renderer& renderer);
void updateEffects(os2::Renderer& renderer, double seconds);
void placePlumbob(os2::Renderer& renderer, double seconds);

// cinematic_host.cpp: the cinematics the VM asks for (or --cinematic).
extern double& gSceneNow;
extern uint32_t gLastFrameW, gLastFrameH;
void forceCinematicState(os2::Renderer& renderer, bool wholeHouse);
void restoreCinematicState(os2::Renderer& renderer);
bool cinematicPlaying();
std::optional<os2::Xform> cinematicOrigin(int16_t id);
void cinematicSnapshot(os2::Renderer& renderer, double now);
os2::LotViewOptions cinematicLotView();
void startCinematic(os2::Renderer& renderer, const std::string& scene, std::vector<int16_t> actors, int16_t origin,
                    bool mirror, bool now, double preparedAt, bool guessSims = false, bool wholeHouse = false);
void cinematicActorAnimates(int16_t id, double at);
void updateCinematic(os2::Renderer& renderer, double now);
os2::FrameView cinematicView(const os2::FrameView& normal, double now, float aspect);
void attachLetterbox(os2::Renderer& renderer, MTL::Device* device, uint32_t color, uint32_t depth);

// lot_actors.cpp: the Sims the VM makes, and every actor posed each frame.
std::optional<fs::path> characterFileOf(uint32_t guid);
void adoptVmSims(os2::Renderer& renderer);
std::vector<os2::SimActor*> simsReachingLaterSims();
void poseActors(os2::Renderer& renderer, double seconds);

// debug_probes.cpp: the test aids that watch the drawn Sims (OS2_DEBUG_*,
// OS2_THEN_AT, OS2_WATCH_*) and OS2_POSE_SHOT's sheets.
void sampleDrawnPlaces(double scene);
void writePoseSheets(os2::Renderer& renderer, const std::string& stem);

// flow_host.mm: the game flow (app/game_flow.hpp): the game's audio across
// scenes, scene switches, saving, the "Salvar?" dialogs, the options, the
// main menu's clicks and the headless VM waits.
bool escapeOptions();
bool headless();
void dwellHeadlessAudio();
void finishHeadlessAudio();
void startGameAudio();
void requestScene(const os2::Scene& next);
void requestQuit();
fs::path hoodFolder(const std::string& query);
void applyAudioSettings();
bool saveGameNow();
void leaveLot();
void setupOptions(os2::ResourceIndex& index);
void quitFromLot();
bool pressOptions(simd_float2 p, float w, float h);
void pressMainMenu(simd_float2 p, float w, float h);
void pollFlowDialog();
void runTicksHeadless(long ticks, os2::Renderer* renderer = nullptr);

#ifdef __OBJC__
// app_args.mm: the arguments of the scene being shown (app/game_flow.hpp):
// the process's at start-up, replaced when the game flow switches scenes in
// place; `--name <value>`'s value.
extern NSArray<NSString*>* gSceneArgs;
NSArray<NSString*>* sceneArgs();
std::optional<std::string> argument(NSString* name);
// lot_loading.mm: --ui <instance-hex>.
std::optional<std::pair<uint32_t, uint32_t>> loadUi(os2::Renderer& renderer, os2::ResourceIndex& index, MTKView* view);
#endif

} // namespace os2app
