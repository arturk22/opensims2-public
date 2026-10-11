#include "app/movie_host.hpp"

#include "audio/mixer.hpp"
#include "formats/audio.hpp"
#include "formats/reia.hpp"
#include "media/movie_player.hpp"
#include "render/movie_layer.hpp"

#include <Metal/Metal.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>

namespace os2 {

namespace {

// The output's buffering (3 AudioQueue buffers of 1024 frames, about 2.5 of
// them queued ahead of what is heard).
constexpr double kOutputLatency = 2.5 * 1024 / 44100.0;

// What the host shows: a .movie (with its sound) or a Reia UI animation.
struct Clip {
    virtual ~Clip() = default;
    virtual bool update(double wall) = 0; // true when the picture changed
    virtual const RgbaImage& picture() const = 0;
    virtual bool finished() const = 0;
    virtual void skip() = 0;
    virtual double time() const = 0;
    virtual double duration() const = 0;
    virtual double audioTime() const { return -1; }
    virtual long frame() const = 0;
    virtual std::string describe() const = 0;
};

struct MovieClip : Clip {
    MoviePlayer player;
    MovieClip(const std::filesystem::path& p, const MoviePlayer::Options& o) : player(p, o) {}
    bool update(double wall) override { return player.update(wall); }
    const RgbaImage& picture() const override { return player.picture(); }
    bool finished() const override { return player.finished(); }
    void skip() override { player.skip(); }
    double time() const override { return player.time(); }
    double duration() const override { return player.duration(); }
    double audioTime() const override { return player.audioTime(); }
    long frame() const override { return player.frame(); }
    std::string describe() const override {
        char b[96];
        std::snprintf(b, sizeof b, "%dx%d, %zu frames, %.2f s", player.movie().width(), player.movie().height(),
                      player.movie().frames().size(), player.duration());
        return b;
    }
};

// "reia:<instance-hex>" (optionally "...:loop"): a Reia of ui.package.
struct ReiaClip : Clip {
    Reia reia;
    ReiaDecoder decoder;
    bool loop, skipped = false;
    double time_ = 0;
    ReiaClip(std::vector<uint8_t> bytes, bool loop) : reia(std::move(bytes)), decoder(reia), loop(loop) {
        decoder.seekFrame(0);
    }
    bool update(double wall) override {
        time_ = wall;
        const double t = loop && reia.duration() > 0 ? std::fmod(wall, reia.duration()) : wall;
        const long before = decoder.current();
        decoder.seek(t);
        return decoder.current() != before;
    }
    const RgbaImage& picture() const override { return decoder.picture(); }
    bool finished() const override { return skipped || (!loop && time_ >= reia.duration()); }
    void skip() override { skipped = true; }
    double time() const override { return time_; }
    double duration() const override { return reia.duration(); }
    long frame() const override { return decoder.current(); }
    std::string describe() const override {
        char b[96];
        std::snprintf(b, sizeof b, "Reia %ux%u, %zu frames, %.2f s%s", reia.width(), reia.height(), reia.frameCount(),
                      reia.duration(), loop ? ", looped" : "");
        return b;
    }
};

// Opens a clip by name: "reia:<hex>[:loop]" or a movie name (findMovie).
std::unique_ptr<Clip> openClip(const std::filesystem::path& resDir, const std::string& name,
                               const MoviePlayer::Options& o) {
    if (name.rfind("reia:", 0) == 0) {
        std::string rest = name.substr(5);
        const bool loop = rest.size() > 5 && rest.compare(rest.size() - 5, 5, ":loop") == 0;
        if (loop)
            rest.resize(rest.size() - 5);
        auto bytes = readUiReia(resDir, uint32_t(std::stoul(rest, nullptr, 16)));
        if (!bytes)
            throw std::runtime_error("no Reia " + rest + " in ui.package");
        return std::make_unique<ReiaClip>(std::move(*bytes), loop);
    }
    const auto path = findMovie(resDir, name);
    if (path.empty())
        throw std::runtime_error("not found");
    return std::make_unique<MovieClip>(path, o);
}

struct Host {
    MTL::Device* device = nullptr;
    std::unique_ptr<MovieLayer> layer;
    MovieHostOptions options;
    std::unique_ptr<AudioMixer> mixer;
    std::unique_ptr<AudioOutput> output;
    std::unique_ptr<Clip> player;
    std::function<void()> onDone;
    std::chrono::steady_clock::time_point start;
    bool pendingDone = false;
};

Host& host() {
    static Host h;
    return h;
}

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// Ends the current movie and runs its onDone (which may start another).
void finish() {
    Host& h = host();
    h.player.reset();
    auto done = std::move(h.onDone);
    h.onDone = nullptr;
    if (done)
        done();
}

} // namespace

bool playMovie(const std::string& name, std::function<void()> onDone) {
    Host& h = host();
    h.player.reset();
    h.onDone = nullptr;
    try {
        MoviePlayer::Options o;
        o.mixer = h.mixer.get();
        o.audioClock = h.output && h.output->running();
        o.outputLatency = kOutputLatency;
        h.player = openClip(h.options.resDir, name, o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: movie %s: %s\n", name.c_str(), e.what());
        if (onDone)
            onDone();
        return false;
    }
    h.onDone = std::move(onDone);
    h.start = std::chrono::steady_clock::now();
    std::printf("opensims2: movie %s (%s)\n", name.c_str(), h.player->describe().c_str());
    std::fflush(stdout);
    if (h.layer)
        h.layer->setFrame(h.player->picture());
    return true;
}

void playMovies(std::vector<std::string> names, std::function<void()> onDone) {
    if (names.empty()) {
        if (onDone)
            onDone();
        return;
    }
    const std::string first = names.front();
    names.erase(names.begin());
    // The rest when this one ends (or is skipped, or missing).
    playMovie(first, [names = std::move(names), onDone = std::move(onDone)]() mutable {
        playMovies(std::move(names), std::move(onDone));
    });
}

bool moviePlaying() { return host().player != nullptr; }

void skipMovie() {
    Host& h = host();
    if (h.player)
        h.player->skip();
}

std::vector<std::string> introMovies() {
    // The localized EA logo, then the intro (English audio only on the CD).
    return {"ealogo_audio", "intro_eng_audio"};
}

void movieHostInit(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat, const MovieHostOptions& options) {
    Host& h = host();
    h.device = device;
    h.options = options;
    h.layer = std::make_unique<MovieLayer>(device, colorFormat, depthFormat);
    if (options.sound && !h.mixer) {
        h.mixer = std::make_unique<AudioMixer>(44100);
        h.output = std::make_unique<AudioOutput>(*h.mixer);
        if (!h.output->start())
            h.output.reset();
    }
}

bool movieHostDraw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable) {
    Host& h = host();
    if (!h.player || !h.layer)
        return false;
    if (h.player->update(since(h.start)))
        h.layer->setFrame(h.player->picture());
    if (h.player->finished()) {
        std::printf("opensims2: movie ended: clock %.3f s, sound mixed %.3f s, wall %.3f s, frame %ld\n",
                    h.player->time(), h.player->audioTime(), since(h.start), h.player->frame());
        std::fflush(stdout);
        finish();
        if (!h.player)
            return false;
        h.layer->setFrame(h.player->picture());
    }
    h.layer->draw(pass, drawable);
    return true;
}

int movieScreenshots(const std::filesystem::path& resDir, const std::string& name, const std::string& out,
                     int frames, double seconds, uint32_t width, uint32_t height, const std::string& wavOut) {
    AudioMixer mixer(44100);
    MoviePlayer::Options o;
    o.mixer = &mixer;
    o.audioClock = true; // pulled below, as the device would
    std::unique_ptr<Clip> clip;
    try {
        clip = openClip(resDir, name, o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: movie \"%s\": %s\n", name.c_str(), e.what());
        return 1;
    }
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (!device) {
        std::fprintf(stderr, "opensims2: no Metal device\n");
        return 1;
    }
    MovieLayer layer(device, uint32_t(MTL::PixelFormatBGRA8Unorm), uint32_t(MTL::PixelFormatDepth32Float));
    std::printf("opensims2: movie %s: %s\n", name.c_str(), clip->describe().c_str());
    const auto* movie = dynamic_cast<MovieClip*>(clip.get());
    if (seconds <= 0)
        seconds = clip->duration() - 1e-3;
    frames = std::max(1, frames);
    std::vector<int16_t> mix;
    for (int f = 1; f <= frames; ++f) {
        const double at = seconds * f / frames;
        // Pull the mixer to `at`, as the output device would.
        const uint64_t want = uint64_t(at * mixer.sampleRate());
        while (mixer.framesRendered() < want) {
            const size_t n = size_t(std::min<uint64_t>(1024, want - mixer.framesRendered()));
            const size_t base = mix.size();
            mix.resize(base + 2 * n);
            mixer.render(mix.data() + base, n);
        }
        clip->update(at);
        layer.setFrame(clip->picture());
        const RgbaImage img = layer.capture(width, height);
        std::string file = out;
        if (frames > 1) {
            const auto dot = file.rfind('.');
            file = file.substr(0, dot) + "-" + std::to_string(f) + (dot == std::string::npos ? "" : file.substr(dot));
        }
        writePng(file, img);
        const long k = clip->frame();
        if (movie) {
            const double fs = movie->player.movie().frameSeconds();
            std::printf("opensims2: wrote %s: clock %.3f s, sound at %.3f s (%llu frames mixed), picture frame %ld "
                        "(%c, shown %.3f-%.3f s)\n",
                        file.c_str(), clip->time(), clip->audioTime(), (unsigned long long)mixer.framesRendered(), k,
                        movie->player.movie().frames()[size_t(k)].kind, k * fs, (k + 1) * fs);
        } else {
            std::printf("opensims2: wrote %s: clock %.3f s, frame %ld\n", file.c_str(), clip->time(), k);
        }
    }
    if (!wavOut.empty()) {
        Pcm pcm;
        pcm.sampleRate = mixer.sampleRate();
        pcm.channels = 2;
        pcm.samples = std::move(mix);
        const auto wav = encodeWav(pcm);
        std::ofstream(wavOut, std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
        std::printf("opensims2: wrote %s (%.2f s, peak %d)\n", wavOut.c_str(), pcm.seconds(), pcm.peak());
    }
    std::fflush(stdout);
    clip.reset();
    device->release();
    return 0;
}

} // namespace os2
