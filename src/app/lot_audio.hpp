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

private:
    Lot lot_;
    std::map<uint16_t, std::string> surfaces_; // floor pattern reference -> surface
    std::string terrain_ = "grass";
    uint16_t room(int lotLevel, int x, int y, int quarter) const;
};

// The shoe type name ("leath", ...) of a character's everyday outfit: the
// "shoe" value of its parts (GZPS), through the shoetypes property set.
std::string characterShoe(ResourceIndex& index, const Character& character, const SoundSystem& sound);

class LotAudio {
public:
    struct Options {
        bool muted = false;       // mixer runs silent
        bool device = true;       // open the output device (off for headless runs)
        std::string music;        // background music category ("Build", "Nhood", ...), "" none
    };
    LotAudio(const std::filesystem::path& resDir, ResourceIndex& index, const Lot& lot, const Options& options);
    ~LotAudio();

    SoundSystem& sound() { return *sound_; }
    AudioMixer& mixer() { return mixer_; }

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
    // buffer that writeMix saves as a WAV (--sound-mix), to check the mix.
    void renderOffline(double seconds);
    bool writeMix(const std::string& path) const;

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

    AudioMixer mixer_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<SoundSystem> sound_;
    std::unique_ptr<AmbiencePlayer> ambience_;
    std::unique_ptr<MusicPlayer> music_;
    std::map<int16_t, std::unique_ptr<MusicPlayer>> stereos_;
    SurfaceMap surfaces_;
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
    std::vector<int16_t> mix_;
    uint64_t mixFrames_ = 0;
    float lastTimeOfDay_ = 0;
    size_t requests_ = 0, animEvents_ = 0, unresolved_ = 0;
};

} // namespace os2
