#pragma once

// The app's sound on a lot: plays what the VM asks for (VM::soundRequests,
// from the objects' positions), the sound events of the Sims' animations
// (footsteps with the floor's surface and the Sim's shoes, object sounds),
// the lot's ambience (time of day from the VM's clock, outdoors share around
// the camera, zoom) and stereo music. Plain C++, driven from main.mm.

#include "audio/sound_system.hpp"
#include "formats/lot.hpp"
#include "render/particle_system.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace os2 {

struct Character;
struct SimActor;
class ResourceIndex;
namespace vm {
class VM;
struct SoundRequest;
}

// Floor surface types for footsteps: floorPatterns.txt "surfaceType" for
// named patterns, the catalog's "surfacetype" for GUID patterns, and the
// terrain's "terrainTypeSoundSuffix" ("grass", also when unset — inferred).
class SurfaceMap {
public:
    SurfaceMap(const Lot& lot, ResourceIndex& index, const std::filesystem::path& resDir);
    // Surface at a point of a lot level (the VM's level - 1), "" for none.
    std::string at(int lotLevel, float x, float y) const;
    // Share of the ground level's quarter tiles within `radius` tiles of
    // (x, y) that are outdoors (no room).
    float outsideness(float x, float y, float radius) const;
    bool indoors(int lotLevel, float x, float y) const;
    // Whether any tile within `radius` tiles of (x, y) holds water: a pool
    // (Lot::pools) or terrain under the water level (a pond). The ambience's
    // "iswater" partition, an area around the camera.
    bool waterNear(float x, float y, float radius) const;
    bool hasWater() const { return waterTiles_ != 0; }

private:
    Lot lot_;
    std::map<uint16_t, std::string> surfaces_; // floor pattern reference -> surface
    std::string terrain_ = "grass";
    uint16_t room(int lotLevel, int x, int y, int quarter) const;
    std::vector<uint8_t> water_; // per tile, width * height
    size_t waterTiles_ = 0;
};

// The shoe type name ("leath", ...) of a character's everyday outfit: the
// "shoe" value of its parts (GZPS), through the shoetypes property set.
std::string characterShoe(ResourceIndex& index, const Character& character, const SoundSystem& sound);

// The game's sound across scenes: one mixer on the output device, the sound
// system (its packages indexed once) and the music states of the binary's
// cTSAudioMusicManager (docs/audio.md, "Rodada 10"): each state has its own
// music, and switching pauses the one playing and resumes (or starts) the
// new one with no fade — "Splash" (the opening music, Music/splash: main
// menu), "NHood" (neighborhood), "CAS", "Buy", "Build", and the lot's state
// "" (live mode: only the stereos, which pause in Buy/Build mode). Leaving
// the opening music fades it over 6 s; going back to the neighborhood stops
// everything and starts its music afresh. The neighborhood view also plays
// the neighborhood ambience layers. Headless, renderOffline/dwell pull the
// mixer into a buffer that writeMix saves.
class GameAudio {
public:
    struct Options {
        bool muted = false; // mixer runs silent
        bool device = true; // open the output device (off for headless runs)
    };
    GameAudio(const std::filesystem::path& resDir, const Options& options);
    ~GameAudio();
    GameAudio(const GameAudio&) = delete;
    GameAudio& operator=(const GameAudio&) = delete;

    SoundSystem& sound() { return *sound_; }
    AudioMixer& mixer() { return mixer_; }
    const std::filesystem::path& resDir() const { return resDir_; }

    // The music state: its category ("Splash", "NHood", "CAS", "Buy",
    // "Build", a station for --music) or "" for the lot's (no music).
    // Unchanged if it is already that one.
    void setMusic(std::string_view category);
    const std::string& music() const { return wanted_; }
    // A cinematic plays (on at its start, off at its end): the effects,
    // voices and music dip (SoundSystem::Dipping).
    void setDipping(bool on) { dipped_ = on; }
    // The music's current dipping factor (1 = not dipped).
    float musicDuck() const { return duck_[2]; }
    // Stops every state's music (back in the neighborhood).
    void stopAllMusic();
    std::string nowPlaying() const;
    // A fullscreen movie plays (the intro): the mode music waits for it.
    void setHeld(bool held);
    // The neighborhood's ambience (the "isneighborhood" layers) on or off.
    void setNeighborhoodAmbience(bool on) { hoodAmbience_ = on; }
    // A UI sound by name (the UI group).
    void playUi(const std::string& name);
    // Next songs, the neighborhood ambience, 3D sound; `seconds` is any
    // clock that advances (the frame's).
    void update(double seconds);

    // A new scene: the last one's sounds stop (the music goes on) and the
    // scene clock of renderOffline restarts at 0.
    void beginScene();
    // Headless (no device): pulls the mixer up to `sceneSeconds` of the
    // current scene into the mix buffer.
    void renderOffline(double sceneSeconds);
    // Headless: lets the current scene run `seconds` more (music and
    // ambience updated as it goes), for scenes without a clock of their own.
    void dwell(double seconds);
    double sceneSeconds() const { return sceneSeconds_; }
    bool writeMix(const std::string& path);
    bool unwritten() const { return mixFrames_ != written_; }
    bool hasDevice() const { return output_ != nullptr; }
    // The music categories set so far, in order (logs).
    const std::vector<std::string>& musicLog() const { return musicLog_; }

private:
    AudioMixer mixer_;
    std::filesystem::path resDir_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<SoundSystem> sound_;
    std::map<std::string, std::unique_ptr<MusicPlayer>> players_; // by state (category)
    std::unique_ptr<AmbiencePlayer> hood_;
    std::string wanted_;
    std::vector<std::string> musicLog_;
    bool held_ = false, hoodAmbience_ = false;
    bool dipped_ = false;
    std::array<float, 3> duck_{1, 1, 1}; // the dipping factors of the effects, voices and music
    double last_ = -1;
    std::vector<int16_t> mix_;
    uint64_t mixFrames_ = 0, sceneBase_ = 0, written_ = 0;
    double sceneSeconds_ = 0;
};

class LotAudio {
public:
    struct Options {
        std::string music; // music category for live mode (--music: "Build", "Nhood", a station), "" none
    };
    LotAudio(GameAudio& game, ResourceIndex& index, const Lot& lot, const Options& options);
    ~LotAudio();

    SoundSystem& sound() { return *sound_; }
    AudioMixer& mixer() { return game_.mixer(); }
    GameAudio& game() { return game_; }
    // The live mode music (--music), "" none.
    const std::string& liveMusic() const { return liveMusic_; }

    // A Sim whose animation events play (footsteps with `shoe`).
    void attachSim(SimActor& actor, int16_t id, std::string shoe);

    struct Listener {
        float x = 0, y = 0, z = 0;     // camera pivot (tiles; z in metres)
        float rightX = 1, rightY = 0;  // camera right vector
        float zoom = 0;                // 0 farthest .. 1 closest
    };
    // Takes the VM's sound requests, fires the animation events between the
    // last call and `seconds`, updates the ambience, music and 3D sound.
    void update(vm::VM* machine, double seconds, const Listener& listener);

    // The effects' sounds due now (LotEffects::collectSounds): a sound
    // starts when it appears, follows its effect, and a looping one stops
    // (fades) when it is no longer due; one-shots play out.
    void effectSounds(const std::vector<fx::EffectSound>& due);

    // TVs showing a broadcast (TvBroadcast::sound): each plays the movie's
    // sound from the set, `offset` seconds in; a new serial (the next show)
    // starts over, and a TV left out (turned off, other channel) stops.
    struct TvSource {
        int16_t object = 0;
        uint64_t serial = 0;
        std::shared_ptr<const Pcm> pcm;
        double offset = 0;
    };
    void tvSounds(vm::VM& machine, const std::vector<TvSource>& on);

    // Headless runs (no device): pulls the mixer up to `seconds` into a
    // buffer that writeMix saves as a WAV (--sound-mix), to check the mix
    // (GameAudio's, across scenes).
    void renderOffline(double seconds) { game_.renderOffline(seconds); }
    bool writeMix(const std::string& path) { return game_.writeMix(path); }

    // For logs: requests played, events fired, ...
    std::string summary() const;

private:
    struct SimBinding {
        SimActor* actor;
        int16_t id;
        std::string shoe;
    };
    struct ObjectVoice {
        std::string event;
        SoundSystem::Handle handle;
    };
    void handle(vm::VM& machine, const vm::SoundRequest& r);
    SoundPos positionOf(vm::VM& machine, int16_t id) const;
    SoundModifiers modifiersFor(vm::VM* machine, int16_t source, const SoundPos& at, const std::string& shoe) const;

    GameAudio& game_;
    SoundSystem* sound_;
    std::string liveMusic_;
    std::unique_ptr<AmbiencePlayer> ambience_;
    struct Stereo {
        std::unique_ptr<MusicPlayer> player;
        bool wall = false;  // a wall speaker: plays what the nearest stereo plays
        StereoEvent style;  // volume and filters its event asked for
    };
    std::map<int16_t, Stereo> stereos_;
    void updateWallSpeakers(vm::VM& machine);
    SurfaceMap surfaces_;
    std::vector<std::pair<float, float>> tombstones_; // tiles of the objects in the "object:tombstone" partition
    std::map<uint32_t, bool> tombstoneGroups_;        // object group -> its soundinfo partition is a tombstone
    double tombstonesAt_ = -10;
    std::vector<SimBinding> sims_;
    std::map<int16_t, std::vector<ObjectVoice>> voices_; // per source object
    struct EffectVoice {
        SoundSystem::Handle handle = 0;
        bool loop = false;
    };
    std::map<std::pair<int, int>, EffectVoice> effectVoices_; // (effect instance, sound part)
    std::map<uint32_t, std::string> effectTracks_;            // audioId -> track key ("" none)
    size_t effectSounds_ = 0;
    struct TvVoice {
        uint64_t serial = 0;
        SoundSystem::Handle handle = 0;
    };
    std::map<int16_t, TvVoice> tvVoices_;
    size_t tvShows_ = 0;
    double last_ = 0;
    bool started_ = false;
    float lastTimeOfDay_ = 0;
    size_t requests_ = 0, animEvents_ = 0, unresolved_ = 0;
};

} // namespace os2
