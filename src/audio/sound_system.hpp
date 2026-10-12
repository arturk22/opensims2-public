#pragma once

// The game-facing side of sound: sound events by name, the way the game turns
// an event into a track (aliases, modifiers such as the floor's surface type
// and the Sim's shoes, see docs/audio.md), a sample picked from the track's
// hit list, decoded once and cached, and played on an AudioMixer, optionally
// at a position in the lot (distance rolloff and pan from the listener).
// Also the ambience (layers of property sets driving loops and FSC
// sequences) and music (categories of songs) players. Plain C++.

#include "audio/mixer.hpp"
#include "audio/sound_bank.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

// Lot position in tiles (x, y) and metres up (z), as the VM keeps them.
struct SoundPos {
    float x = 0, y = 0, z = 0;
};

// What the modifiers of an event (the "events" property set: foot_step =
// "destfootstep,sourcefootstep", chair_liv_sit = "destsurfacetype", ...)
// stand for where the sound plays. Empty = unknown; the modifier is then
// dropped. Each modifier appends "_<value>" in the listed order.
struct SoundModifiers {
    std::string destSurface;   // destsurfacetype/destfootstep: the floor, or soundinfo[0] of the object
    std::string sourceSurface; // sourcesurfacetype: soundinfo[1] of the object
    std::string shoe;          // sourcefootstep: bare, leath, heel, rub, sand, slip
    std::string quality;       // objectquality: soundinfo[2] of the object (exp, chp, wdglass, ...)
    std::string vox;           // vox: age, gender and voice ("ama", "tfa", ...)
    std::string physical;      // physicalskill: ls, ms, hs
    std::string creative;      // creativeskill: a..j
};

// A layer of the ambience (property sets with a "Name", listed by the
// "LayerIds" of the ambience settings): when it is enabled (EnabledPrimitiveIds
// such as "islot,lightofday,curveisday": booleans, and values passed through
// the curve named after them), it plays a loop (AmbientLoopName) and/or an
// FSC sequence (FreshnessScoreName, the FSC text resource of that name) at a
// volume from its *VolumePrimitives (value, curve pairs).
struct AmbienceLayer {
    std::string name;
    std::vector<std::string> enabled, andPartition, notPartition, loopVolume, freshVolume;
    std::map<std::string, std::vector<float>> curves; // x0,y0,x1,y1,... piecewise linear
    std::string loopName, scoreName;
    float loopMinDistance = 0, freshMinDistance = 0;
    bool loop2d = false, fresh2d = false;
};
// Piecewise-linear curve through (x, y) pairs, clamped at the ends.
float evalCurve(const std::vector<float>& xy, float x);

// FSC ("version 7" text): a sequence of notes at a tempo; a note with
// "rand" starts a segment the player can jump to; "NONE" rests. As in
// FreeSO's FSC reader (the TSO format; the same in The Sims 2).
struct FscScore {
    float masterVolume = 1024; // 0..1024
    int tempo = 120;           // beats a minute
    struct Note {
        float volume = 1024; // 0..1024
        bool rand = false;
        int pan = 512; // 0..1024, 512 = centre
        int loop = 1;  // beats it lasts
        int prob = 0;  // in 16, 0 = always
        int pitchLow = 0, pitchHigh = 0;
        std::string file; // sample name without ".xa"; "NONE" rests
    };
    std::vector<Note> notes;
    std::vector<size_t> jumpPoints;
};
FscScore parseFsc(std::string_view text);

class SoundSystem {
public:
    // `resDir` is TSData/Res. Reads the sound property sets (aliases, event
    // modifiers, shoe types, ambience layers, music categories).
    SoundSystem(const std::filesystem::path& resDir, AudioMixer& mixer, uint32_t seed = 0x5132);
    SoundSystem(const SoundSystem&) = delete;
    SoundSystem& operator=(const SoundSystem&) = delete;

    AudioMixer& mixer() { return mixer_; }
    SoundBank& bank() { return bank_; }

    // The name an event is played under: the event plus the modifiers'
    // suffixes, through the aliases; suffixes are dropped from the end until
    // a track exists. Empty if nothing by that name exists.
    std::string resolveEvent(std::string_view event, const SoundModifiers& m = {}) const;
    bool exists(std::string_view name) const;
    // Whether a track loops: its track settings have key 0xff595ffd (every
    // looping track checked has it: lot_outside_lp, water_lp, the fire, the
    // flies, the sink's water; one-shots do not; inferred, the value looks
    // like a loop point), or its name ends in "_lp".
    bool loops(std::string_view name);
    // Modifier names of an event ("destfootstep", ...), empty if it has none.
    const std::vector<std::string>* modifiersOf(std::string_view event) const;
    // Shoe name for a GZPS "shoe" value (1 bare .. 6 slip), empty if unknown.
    std::string shoeName(uint32_t value) const;
    // A skill's suffix for a value (0..1000) by the skill ranges property set
    // ("Physical" = "0,ls,333,ms,666,hs,1000"); empty if the skill has none.
    std::string skillSuffix(std::string_view skill, int value) const;

    // A random sample of a track or sample name, decoded (cached), and the
    // group it mixes in (from the folder it came from). Null if none.
    std::shared_ptr<const Pcm> pick(std::string_view name, SoundGroup* group = nullptr);

    using Handle = AudioMixer::VoiceId;
    struct PlayOptions {
        std::optional<SoundPos> position; // none = not positional (UI, 2D ambience, music)
        bool loop = false;
        float volume = 1;
        float pan = 0; // for non-positional sounds
        std::optional<SoundGroup> group;
        double startAt = 0; // seconds into the sound
        float lopass = 0, hipass = 0; // filter cutoffs in Hz, 0 = none
    };
    // Plays a track or sample by its final name (see resolveEvent); 0 if
    // there is no such sound.
    Handle play(std::string_view name, const PlayOptions& options);
    // Plays decoded PCM the same way (music streams, tests).
    Handle playPcm(std::shared_ptr<const Pcm> pcm, const PlayOptions& options);
    void stop(Handle h);
    // Fades to silence over `seconds`, then stops.
    void fadeOut(Handle h, double seconds);
    bool playing(Handle h) const { return mixer_.playing(h); }
    void setPosition(Handle h, SoundPos p);
    void setVolume(Handle h, float volume, double fadeSeconds = 0);

    // The listener: where the camera looks (the game's primary listener sits
    // at the camera's target), with the camera's right vector for panning.
    void setListener(SoundPos at, float rightX, float rightY);
    // Recomputes the gain and pan of positional voices.
    void update();
    // Gain and pan of a source for the current listener (exposed for tests):
    // full volume within `minDistance` tiles, then (min / d)^rolloff
    // (rolloff 1.2 from the sound manager's tunables).
    std::pair<float, float> spatialize(SoundPos p, float minDistance) const;

    // Sound property sets.
    const std::vector<AmbienceLayer>& ambienceLayers() const { return layers_; }
    float lotAmbienceRadius(float zoom) const; // LotAmbienceZoomRadiusCurve
    // Songs of a music category ("pop", "Build", "Nhood", ...), shuffled
    // order is the caller's; empty if the category has none.
    std::vector<SoundBank::Location> musicTracks(std::string_view category) const;
    bool isMusicCategory(std::string_view category) const;
    // Music tunables (TrackLogic settings): default volume 0.7, fade out 3 s.
    float defaultMusicVolume() const { return musicVolume_; }
    // Non-positional music (the modes') at defaultmusicvolume times the
    // sound manager's 2dMusicVolumeScalar (0.75; inferred that it means this).
    float music2dVolume() const { return musicVolume_ * music2dScalar_; }
    double musicFadeOut() const { return musicFadeOut_; }
    // TrackLogic's dipping (cTSAudioEventHandler::ReadTuningValues): while
    // a cinematic plays (cTSSGSystem::PlayCinematicScene turns it on,
    // UpdateCinematicScene off at the scene's end; docs/audio.md) the
    // effects drop to `sfx`, the voices to `vox` and the music to `music` of
    // their volume over `start` s, and come back over `stop` s
    // (sfxdippingvolume 0.2, voxdippingvolume 0.3, musicdippingvolume 0.1,
    // dippingstarttime 0.5, dippingstoptime 5).
    struct Dipping {
        float sfx = 0.2f, vox = 0.3f, music = 0.1f;
        double start = 0.5, stop = 5;
    };
    const Dipping& dipping() const { return dipping_; }
    // The FSC score of a name ("midday"), parsed once; null if none.
    const FscScore* score(std::string_view name);
    std::shared_ptr<const Pcm> decodeSample(const SoundBank::Location& loc);

    std::mt19937& random() { return random_; }

    // Counters for logs and tests.
    size_t played = 0;
    std::map<std::string, size_t> playedNames; // by final name
    std::map<std::string, size_t> missing; // events with no sound

private:
    struct Positional {
        Handle handle;
        SoundPos pos;
        float volume;
        float minDistance;
    };
    AudioMixer& mixer_;
    SoundBank bank_;
    std::mt19937 random_;
    std::map<std::string, std::string> aliases_;
    std::map<std::string, std::vector<std::string>> modifiers_;
    std::map<uint32_t, std::string> shoes_;
    std::map<std::string, std::vector<std::string>> skillRanges_; // lowercase skill -> bound, name, bound, ...
    std::vector<AmbienceLayer> layers_;
    std::vector<float> lotRadiusCurve_{0, 20, 1, 8};
    std::vector<std::string> musicCategories_;
    float musicVolume_ = 0.7f;
    float music2dScalar_ = 0.75f;
    double musicFadeOut_ = 3.0;
    Dipping dipping_;
    float rolloff_ = 1.2f;
    std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<const Pcm>> cache_;
    std::map<std::string, std::unique_ptr<FscScore>> scores_;
    std::vector<Positional> positional_;
    SoundPos listener_;
    float rightX_ = 1, rightY_ = 0;
};

// One step of a group's dipping: `duck` (1 = none) falls to `low` over
// `d.start` s while dipped and rises back to 1 over `d.stop` s after.
float stepDuck(float duck, bool dipped, float low, const SoundSystem::Dipping& d, double dt);

// A stereo event name (docs/audio.md "Rodada 11"): "stereo_<station>" and
// the quality and commands the binary's cTSAudioMusicManager::
// _HandleStartStereoEvent reads, as "_"-separated tokens after the station:
// "cheap"/"chp" (band limited: low-pass 8500 Hz, high-pass 3500 Hz, as _Start
// does) or "expensive"/"exp" (no filter), "wall" (a wall speaker: plays what
// the stereo in reach plays), "lopass<Hz>", "hipass<Hz>" and "volume<0..100>".
// The token spelling is inferred; the base game's events only name stations
// ("stereo_pop") and "stereo_wall_speaker".
struct StereoEvent {
    std::string station; // "pop", "techno", ...; "" for a wall speaker alone
    bool wall = false;
    float volume = 1;
    float lopass = 0, hipass = 0;
};
// Nullopt if `event` is not a stereo event.
std::optional<StereoEvent> parseStereoEvent(std::string_view event);
// The cheap quality's filters: _Start's 0x2134 and 0xDAC.
constexpr float kCheapLowPass = 8500, kCheapHighPass = 3500;

// Plays the lot's ambience: each enabled layer's loop (faded to its volume)
// and FSC sequence (notes on a beat clock). The host gives it the state the
// layers' primitives read every frame.
class AmbiencePlayer {
public:
    explicit AmbiencePlayer(SoundSystem& sound) : sound_(sound) {}
    ~AmbiencePlayer();

    struct State {
        bool onLot = true;          // islot (else isneighborhood)
        float timeOfDay = 0.5f;     // lightofday/timeofday: hours / 24
        float outsideness = 1;      // share of the listening area that is outdoors
        float zoom = 0;             // 0 farthest .. 1 closest
        bool inside = false;        // isinside: the listener is in a room
        std::map<std::string, bool> partitions{{"isflora", true}}; // iswater, istombstone, ...
        bool paused = false;
    };
    // Advances by `dt` seconds with this state.
    void update(const State& state, double dt);
    // Layer volumes after the last update (name -> volume; 0 = off).
    const std::map<std::string, float>& volumes() const { return volumes_; }
    size_t notesPlayed() const { return notesPlayed_; }

private:
    struct Running {
        SoundSystem::Handle loop = 0;
        float loopVolume = -1;
        const FscScore* score = nullptr;
        size_t position = 0;
        int hold = 0; // beats left on the current note
        double clock = 0;
        bool started = false;
        bool fresh = false; // just jumped: the "rand" note under the cursor starts the segment
    };
    float primitive(const State& s, const std::string& id) const;
    float weight(const AmbienceLayer& layer, const State& s, const std::vector<std::string>& list) const;

    SoundSystem& sound_;
    std::map<std::string, Running> running_;
    std::map<std::string, float> volumes_;
    size_t notesPlayed_ = 0;
};

// Plays the songs of a music category one after another in a shuffled order
// (TrackLogic: fade out 3 s, volume 0.7), optionally from a position (a
// stereo). setCategory("") stops with a fade.
class MusicPlayer {
public:
    explicit MusicPlayer(SoundSystem& sound) : sound_(sound) {}
    ~MusicPlayer();
    // `fadeOut` (< 0: the TrackLogic's trackfadeouttime) fades the song
    // playing while the next category starts at once.
    void setCategory(std::string_view category, std::optional<SoundPos> position = std::nullopt, double fadeOut = -1);
    // Volume (times the usual one) and filters (Hz, 0 none) of the songs,
    // now and for the next ones.
    void setStyle(float volume, float lopass, float hipass);
    const std::string& category() const { return category_; }
    void update();
    // The song playing (sample name key), for logs.
    std::string nowPlaying() const;
    // Songs are decoded on a worker thread (the next one while one plays),
    // not cached; off (headless runs), at once when needed.
    void setBackgroundDecoding(bool on) { background_ = on; }
    // Paused: the song holds its place, silent, and no next one starts.
    void setPaused(bool paused);
    bool paused() const { return paused_; }

private:
    struct Decoded {
        std::shared_ptr<const Pcm> pcm;
        std::string name;
    };
    void prefetch();

    SoundSystem& sound_;
    std::string category_;
    std::optional<SoundPos> position_;
    std::vector<SoundBank::Location> queue_;
    size_t next_ = 0;
    SoundSystem::Handle voice_ = 0;
    std::string playing_;
    bool background_ = false;
    bool paused_ = false;
    float volume_ = 1, lopass_ = 0, hipass_ = 0;
    std::future<Decoded> pending_;
    std::vector<std::future<Decoded>> discarded_;
};

} // namespace os2
