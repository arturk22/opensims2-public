// The game flow's host (app/game_flow.hpp): the game's audio across scenes,
// scene switches, saving, the "Salvar?" dialogs, the options, the main
// menu's clicks and the headless VM waits (app_state.hpp).

#include "app/app_state.hpp"

namespace os2app {

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

} // namespace os2app
