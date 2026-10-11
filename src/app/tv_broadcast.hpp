#pragma once

// TV channels and arcade screens (plain C++, driven from main.mm): the
// game's video broadcasts (nTSSG::cVideoBroadcastManager, Mac binary;
// docs/cinematics-mac.md §7). The broadcasts are defined by
// Res/UserData/Config/Broadcast.ini and Res/Movies/Games/INI/*.ini
// ("[tvstation_sbn] Folder=Broadcast/TvSBN, CommercialInterval=60,
// CommercialFolder=Broadcast/Commercials"; arcade "IsGames=1, File=...").
// A TV turns a channel on with Change Material: the material name is the
// broadcast's ("tvstation_sbn" in the channel object's STR# 0x88, "This is
// defined in Broadcasts.ini") on the screen's mesh group. Here such a
// material becomes a self-lit material whose texture ("movie:<channel>") is
// a dynamic renderer texture with the channel's picture: the 128x128 MAD
// movies of the folder one after another in a random order, a commercial
// every CommercialInterval seconds (inferred: between shows), or the one
// file of a games broadcast, looped. Every TV on a channel shows the same
// picture, as the binary's channels feed all their receivers.

#include "formats/audio.hpp"
#include "formats/image.hpp"
#include "formats/model.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace os2 {

class Movie;
class MovieDecoder;
class Renderer;

class TvBroadcast {
public:
    // `resDir`: TSData/Res.
    TvBroadcast(Renderer& renderer, const std::filesystem::path& resDir);
    ~TvBroadcast();

    // True if `material` names a broadcast.
    bool isChannel(const std::string& material) const;
    // Rewrites the parts of `model` whose subset has a broadcast material in
    // `materials` (subset -> material) to show the channel; registers the
    // channel's texture with the renderer first.
    void adjust(ObjectModel& model, const std::map<std::string, std::string>& materials);
    // Advances the channels in use to `seconds` (the scene clock).
    void update(double seconds);

    size_t channelsInUse() const { return live_.size(); }

    // The sound of what a channel shows (the movie's own sound track, as
    // the binary's broadcasts carry it), for the host to play from every TV
    // on the channel: the PCM, the scene time it started at, and a serial
    // that changes with each movie. Null if the channel is not in use or
    // the movie has no sound.
    struct Sound {
        std::shared_ptr<const Pcm> pcm;
        double started = 0;
        uint64_t serial = 0;
    };
    const Sound* sound(const std::string& channel) const;
    double now() const { return last_; } // the clock of the last update
    // Off: the movies' sound is not decoded (no audio).
    void setSoundWanted(bool wanted) { soundWanted_ = wanted; }

private:
    struct Definition {
        std::string folder, commercialFolder, file;
        double commercialInterval = 0;
        bool games = false;
    };
    struct Live {
        std::vector<std::filesystem::path> shows, commercials;
        std::unique_ptr<Movie> movie;
        std::unique_ptr<MovieDecoder> decoder;
        double started = 0, lastCommercial = 0;
        bool inCommercial = false;
        std::filesystem::path lastShow;
        RgbaImage picture;
        Sound sound;
    };
    void loadIni(const std::filesystem::path& file, const std::filesystem::path& moviesDir);
    void next(Live& l, const Definition& d, double now);

    Renderer& renderer_;
    std::filesystem::path movies_;
    std::map<std::string, Definition> defs_; // by lowercase name
    std::map<std::string, Live> live_;
    std::mt19937 rng_{20040914};
    double last_ = 0;
    bool soundWanted_ = true;
    uint64_t serial_ = 0;
};

} // namespace os2
