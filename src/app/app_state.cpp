// The scene's state (SceneState gScene, resetSceneState) and the app's
// globals that outlive scenes; the scene's unload and the app's release on
// quit (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

SceneState gScene;
// Counts the scenes torn down: run-long tools (the debug detectors) forget
// the last scene's Sims when it changes.
uint64_t gSceneGeneration = 0;

std::string SceneState::sizes() const {
    std::string out;
    auto add = [&out](const char* name, size_t n) {
        out += (out.empty() ? "" : " ") + std::string(name) + "=" + std::to_string(n);
    };
    add("actors", actors.size());
    add("lotSims", lotSims.size());
    add("objectActors", objectActors.size());
    add("placedObjects", placedObjects.size());
    add("pickBoxes", pickBoxes.size());
    add("fenceModels", fenceModels.size());
    add("simInstances", simInstances.size());
    add("dialogThumbnails", dialogThumbnails.size());
    add("madePickBoxes", madePickBoxes.size());
    add("simAnimationAt", simAnimationAt.size());
    add("bornCharacters", bornCharacters.size());
    add("simsLookedAt", simsLookedAt.size());
    add("mirrorEdits", mirrorEdits.size());
    add("wardrobeFilled", wardrobeFilled.size());
    add("portraitFiles", portraitFiles.size());
    add("characterFiles", characterFiles.size());
    add("embeddedCharacters", embeddedCharacters.size());
    add("censorReports", censorReports.size());
    add("posesFlagged", posesFlagged.size());
    add("watchedObjects", watchedObjects.size());
    add("doorLeaves", doorLeaves.size());
    add("simulation", simulation != nullptr);
    add("objectView", objectView != nullptr);
    add("effects", effects != nullptr);
    add("pie", pie != nullptr);
    add("hood", hood != nullptr);
    return out;
}

// Replaces the scene's state with a fresh one (see above).
void resetSceneState() {
    gScene = SceneState{};
    ++gSceneGeneration;
}

// The app's caches of the game's read-only data (CAS catalog, thumbnails,
// pick meshes, Sim parts, ...): app/game_data_caches.hpp.
os2::GameDataCaches& gGameData = os2::gameDataCaches();
// The pie menu over the lot's VM (app/pie_menu.hpp), with the pick boxes of
// the drawn lot objects (collected before the VM exists).
std::unique_ptr<os2::PieMenuHost>& gPie = gScene.pie;
// --ui live: the game's live-mode panel over the same VM (app/live_ui.hpp).
std::unique_ptr<os2::LiveUi>& gLive = gScene.live;
std::unique_ptr<os2::TutorialOverlay>& gTutorial = gScene.tutorial; // over a lot of the Tutorial neighborhood (app/tutorial.hpp)
size_t& gTutorialRequestsSeen = gScene.tutorialRequestsSeen;
// Buy mode (app/buy_mode.hpp): the puck's Buy button, or --mode buy.
std::unique_ptr<os2::BuyMode>& gBuy = gScene.buy;
// Build mode (app/build_mode.hpp): the puck's Build button, or --mode build.
// It changes gLotShape (the lot as drawn); rebuildLotModel redraws it.
std::unique_ptr<os2::BuildMode>& gBuild = gScene.build;
std::unique_ptr<os2::Lot>& gLotShape = gScene.lotShape;
std::unique_ptr<os2::PatternCatalog>& gLotPatterns = gScene.lotPatterns;
os2::LotViewOptions& gLotView = gScene.lotView; // the view the lot model was last built for
int& gLotInstance = gScene.lotInstance;
int& gLotModel = gScene.lotModel;           // the renderer model of gLotInstance (replaced on rebuilds)
// The wall and roof view (app/wall_view.hpp): the puck's wall fan, Home/End;
// updateLotView rebuilds the lot model when it changes what is cut.
os2::WallView gWallView;
os2::LotModelCache& gLotCache = gScene.lotCache;
bool& gLevelsHidden = gScene.levelsHidden; // levels above the camera's are hidden (in the game: --ui live, --mode)
std::vector<PickBox>& gPickBoxes = gScene.pickBoxes;
std::map<int16_t, PlacedObject>& gPlacedObjects = gScene.placedObjects;
// Caches of the scene's renderer ids, cleared by unloadScene: the renderer is
// replaced with each scene, and a model or instance id of the old one names
// another model or instance in the new one. They were function statics: on
// the second lot of a session the fence pieces took over the instances of
// 336 of the new lot's objects (N002_Lot3 after N002_Lot10: the grandfather
// clock drawn as a brick chimney piece where a railing stands, in front of
// the Sims: "a menina está carregando um pedaço de parede").
std::map<std::string, int>& gFenceModels = gScene.fenceModels; // fence CRES -> model id (-1: failed)
std::vector<int>& gFenceInstances = gScene.fenceInstances;        // instances placeFencePieces places, reused when it runs again
std::map<const os2::SimActor*, int>& gSimInstances = gScene.simInstances; // a Sim's actor -> its instance (hidden above the level shown)
std::map<std::pair<int16_t, int16_t>, std::unique_ptr<os2::RgbaImage>>& gDialogThumbnails = gScene.dialogThumbnails; // (object, neighbor) -> picture
std::map<int16_t, std::string>& gMadePickBoxes = gScene.madePickBoxes; // objects made in play: id -> CRES their pick box was made for
// Skinned models posed every frame by their animation players.
std::vector<std::unique_ptr<os2::SimActor>>& gActors = gScene.actors;
std::map<int16_t, os2::SimActor*>& gObjectActors = gScene.objectActors; // objects the VM has animated (in gActors)
std::unique_ptr<os2::CinematicPlayer>& gCinematic = gScene.cinematic; // the cinematic prepared or playing (see updateCinematic)
std::unique_ptr<os2::TvBroadcast>& gTv = gScene.tv;             // TV channels on the lot's screens
float gUiScale = 1; // drawable pixels per view point
// --hood: the neighborhood view (app/hood_mode.hpp) in place of a lot.
std::unique_ptr<os2::HoodMode>& gHood = gScene.hood;
std::optional<os2::LotCamera::Pose> gHoodReloadPose; // the camera across a neighborhood reload
std::unique_ptr<os2::CasMode>& gCas = gScene.cas; // --cas: Create-a-Sim (app/cas_mode.hpp)
// --hood <N> --hood-lot <id>: that lot's package, shown as --lot is.
std::optional<std::string>& gHoodLot = gScene.hoodLot;
// That lot has no family: it opens in Buy mode and Live stays off (lotStartMode).
bool& gLotVacant = gScene.lotVacant;
// The game flow (app/game_flow.hpp): the user's folder, the main menu, the
// puck's options, the scene asked for next and the steps used so far.
std::unique_ptr<os2::UserData> gUser;
std::unique_ptr<os2::MainMenu>& gMenu = gScene.menu;
std::unique_ptr<os2::OptionsPanel>& gOptions = gScene.options;
os2::GameSettings gSettings;

std::optional<os2::Scene> gPendingScene;
// A family Create-a-Sim just wrote into the neighborhood: selected in the
// family bin once the neighborhood is back (HoodMode::familyCreated).
std::optional<uint32_t> gCreatedFamily;
bool gQuitRequested = false;
size_t& gStepsUsed = gScene.stepsUsed;    // --click/--wait steps this scene has taken (headless)
double& gHeadlessClock = gScene.headlessClock; // the caller's clock of headless VM runs (--wait, --run-vm)
fs::path& gLotFile = gScene.lotFile;        // the package the running lot came from (saved back to)
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
// The game's resources. The VM, the pie menus and the sound keep references to
// it, so it lives as long as the app (not just while the window is set up).
std::unique_ptr<os2::ResourceIndex> gIndex;
// Every Sim part of the game (GZPS with a 3IDR), read once for the app's
// life (gGameData): every lot entry and outfit change uses them. On the
// loading thread or the main thread, never both at once (the main thread
// reads it only between loads).
const std::vector<os2::SimPart>& simParts(os2::ResourceIndex& index) { return gGameData.simPartsFor(index); }
// The lot's objects in the VM (--vm, --run-vm), driving the lot's Sims.
std::unique_ptr<os2::LotSimulation>& gSimulation = gScene.simulation;
// The lot's objects drawn where the VM puts them (app/vm_objects.hpp).
std::unique_ptr<os2::VmObjectView>& gObjectView = gScene.objectView;
std::unique_ptr<os2::AnimSnapView>& gSnaps = gScene.snaps; // models the animations' snap events put in the Sims' hands
// The lot's lights from the VM: the clock's day/night, rooms, lamps (app/lot_lighting.hpp).
std::unique_ptr<os2::LotLighting>& gLighting = gScene.lighting;
std::optional<os2::LotLightMapLayout>& gLightMapLayout = gScene.lightMapLayout; // the lot's light-map atlas
// Without the VM: the Sims' shadow light of the --lighting state (with the VM, LotLighting's).
std::optional<os2::SimShadowLight>& gStaticShadowLight = gScene.staticShadowLight;
// Doors opening as the Sims walk through (app/door_portals.hpp).
std::unique_ptr<os2::DoorPortals>& gDoors = gScene.doors;
std::vector<LotSim>& gLotSims = gScene.lotSims;
// Sims drawn in another Sim's slot (a carried baby), and who carries each.
std::set<const os2::SimActor*>& gCarriedSims = gScene.carriedSims;
std::map<const os2::SimActor*, const os2::SimActor*>& gCarriers = gScene.carriers;
std::unique_ptr<os2::UiResources> gTexts; // Text/*.package's strings (uiText, the VM's textTables)
// Sims the VM adds while the lot plays (adoptVmSims):
std::map<int16_t, fs::path>& gBornCharacters = gScene.bornCharacters; // neighbor ID -> character file written in this session
std::set<int16_t>& gSimsLookedAt = gScene.simsLookedAt;              // person objects already adopted or given up on
fs::path& gLotCharactersDir = gScene.lotCharactersDir;                   // the lot's neighborhood Characters folder
int& gPlumbob = gScene.plumbob; // renderer instance of the plumbob over the selected Sim (placePlumbob)
float& gPlumbobBottom = gScene.plumbobBottom; // lowest point of its mesh (modelled about 1.24 over the origin)

// Sound (app/lot_audio.hpp): gGameAudio lives across scenes (the mixer on
// the device, the mode music: NHood, CAS, Buy, Build); gAudio is the lot's.
// --no-audio leaves it out, --mute keeps it silent; headless runs
// (--screenshot) open no device, and --sound-mix <out.wav> saves what would
// have played (scenes without a clock add --sound-dwell seconds, 5 by
// default). --music <category> plays music in live mode ("Build", "Nhood",
// or a radio station: "pop", "rnb", ...).
std::unique_ptr<os2::GameAudio> gGameAudio;
std::unique_ptr<os2::LotAudio>& gAudio = gScene.audio;
os2::LotCamera*& gCamera = gScene.camera;
std::unique_ptr<os2::LotCameraFollow>& gCameraFollow = gScene.cameraFollow; // --lot: pivot held to the lot, on the Sim's floor
os2::CameraSelector& gCameraSelector = gScene.cameraSelector;

// Everything a scene loaded, so that the next one starts clean (the
// renderer is replaced by the caller). Then the app's decoded pictures go if
// they are past their cap (GameDataCaches::trim: nothing points at them
// once the scene is gone).
// OS2_DEBUG_SCENE_STATE=1 prints the scene state's sizes before and after
// (tests/scene_cycle.sh: all empty after), and the app's caches with the
// process's resident memory after.
void unloadScene() {
    static const bool debug = std::getenv("OS2_DEBUG_SCENE_STATE") != nullptr;
    if (debug)
        std::printf("opensims2: scene state before unload: %s\n", gScene.sizes().c_str());
    if (gGameAudio)
        gGameAudio->setDipping(false);
    resetSceneState();
    static const size_t cap = [] {
        const char* mb = std::getenv("OS2_PICTURE_CAP_MB"); // (tests, measurements)
        return mb ? size_t(std::strtoull(mb, nullptr, 10)) << 20 : os2::GameDataCaches::kImageCap;
    }();
    const size_t pictures = gGameData.imageBytes();
    const bool trimmed = gGameData.trim(cap);
    if (debug) {
        std::printf("opensims2: scene state after unload: %s\n", gScene.sizes().c_str());
        std::printf("opensims2: app caches after unload: %s residentMB=%zu%s\n", gGameData.sizes().c_str(),
                    os2::residentBytes() >> 20,
                    trimmed ? (" (pictures dropped: " + std::to_string(pictures >> 10) + " KB past the cap)").c_str() : "");
        std::fflush(stdout);
    }
}

// Quitting (Cmd-Q, the window closed, the game flow's Quit, the end of a
// headless run): the scene, then the app's caches of the game's data, each
// in its own order (SceneState's members, GameDataCaches::clear), while the
// resource index, the game's audio and the window's Metal device are still
// alive. With a window it runs from applicationWillTerminate:, before exit()
// destroys any static; the atexit handler main() registers first runs it
// again (then a no-op) and is the one a headless run's std::exit uses: by
// then the function statics made during the run are gone (none of them is
// used by the teardown: the log-once sets, the debug detectors, the movie
// host, the VM's caches of the game's data).
// OS2_DEBUG_SCENE_STATE=1 prints what was freed (tests/scene_cycle.sh).
void releaseAppState() {
    static const bool debug = std::getenv("OS2_DEBUG_SCENE_STATE") != nullptr;
    const bool had = gGameData.simPartsOf || gGameData.cas.catalog || gGameData.imageBytes() || gScene.simulation ||
                     gScene.hood || gScene.cas || gScene.menu;
    resetSceneState();
    if (debug && had)
        std::printf("opensims2: quit: app caches before release: %s\n", gGameData.sizes().c_str());
    gGameData.clear();
    if (debug && had) {
        std::printf("opensims2: quit: scene state after release: %s\n", gScene.sizes().c_str());
        std::printf("opensims2: quit: app caches after release: %s\n", gGameData.sizes().c_str());
        std::fflush(stdout);
    }
}

} // namespace os2app
