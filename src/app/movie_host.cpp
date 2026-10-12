#include "app/movie_host.hpp"

#include "audio/mixer.hpp"
#include "formats/audio.hpp"
#include "formats/reia.hpp"
#include "media/movie_player.hpp"
#include "render/movie_layer.hpp"

#include <Metal/Metal.hpp>

#include <algorithm>
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

// A loading screen: a Reia played once, paced by the load's progress, with
// an icon over it.
struct LoadingClip : Clip {
    Reia reia;
    ReiaDecoder decoder;
    std::function<float()> progress;
    struct Overlay {
        RgbaImage img; // already scaled
        int left = 0, top = 0;
    };
    Overlay icon, title;
    float iconX = 0, iconY = 0, iconW = 0, iconH = 0;
    std::function<std::optional<RgbaImage>()> lateIcon;
    RgbaImage picture_;
    double t = 0, lastWall = 0, tailSpeed = 0;
    bool finishing = false, skipped = false, fresh = true;
    long shownFrame = -1;

    void setIcon(const RgbaImage& src) {
        icon = {};
        if (!src.width || !src.height)
            return;
        const float scale = std::min({1.0f, iconW / float(src.width), iconH / float(src.height)});
        RgbaImage& out = icon.img;
        out.width = std::max(1u, uint32_t(std::lround(float(src.width) * scale)));
        out.height = std::max(1u, uint32_t(std::lround(float(src.height) * scale)));
        out.pixels.resize(size_t(out.width) * out.height * 4);
        // Box filter over the source pixels each target pixel covers.
        for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x) {
                const uint32_t x0 = x * src.width / out.width, x1 = std::max(x0 + 1, (x + 1) * src.width / out.width);
                const uint32_t y0 = y * src.height / out.height, y1 = std::max(y0 + 1, (y + 1) * src.height / out.height);
                float acc[4] = {0, 0, 0, 0};
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx) {
                        const uint8_t* q = &src.pixels[(size_t(sy) * src.width + sx) * 4];
                        const float a = q[3] / 255.0f;
                        acc[0] += q[0] * a, acc[1] += q[1] * a, acc[2] += q[2] * a, acc[3] += a;
                    }
                uint8_t* d = &out.pixels[(size_t(y) * out.width + x) * 4];
                const float n = float((x1 - x0) * (y1 - y0));
                for (int c = 0; c < 3; ++c)
                    d[c] = acc[3] > 0 ? uint8_t(std::clamp(acc[c] / acc[3], 0.0f, 255.0f)) : 0; // unpremultiplied
                d[3] = uint8_t(std::clamp(acc[3] / n * 255.0f, 0.0f, 255.0f));
            }
        icon.left = int(std::lround(iconX - float(out.width) / 2));
        icon.top = int(std::lround(iconY - float(out.height) / 2));
    }

    LoadingClip(std::vector<uint8_t> bytes, LoadingScreen screen, std::function<float()> progress)
        : reia(std::move(bytes)), decoder(reia), progress(std::move(progress)), iconX(screen.iconX),
          iconY(screen.iconY), iconW(screen.iconW), iconH(screen.iconH), lateIcon(std::move(screen.lateIcon)) {
        decoder.seekFrame(0);
        if (screen.icon)
            setIcon(*screen.icon);
        if (screen.title && screen.title->width && screen.title->height) {
            title.img = *screen.title;
            title.left = int(std::lround(screen.titleX - float(title.img.width) / 2));
            title.top = int(std::lround(screen.titleY - float(title.img.height) / 2));
        }
        compose();
    }
    void blend(const Overlay& o) {
        for (uint32_t y = 0; y < o.img.height; ++y) {
            const int py = o.top + int(y);
            if (py < 0 || py >= int(picture_.height))
                continue;
            for (uint32_t x = 0; x < o.img.width; ++x) {
                const int px = o.left + int(x);
                if (px < 0 || px >= int(picture_.width))
                    continue;
                const uint8_t* s = &o.img.pixels[(size_t(y) * o.img.width + x) * 4];
                uint8_t* d = &picture_.pixels[(size_t(py) * picture_.width + size_t(px)) * 4];
                const int a = s[3];
                for (int c = 0; c < 3; ++c)
                    d[c] = uint8_t((s[c] * a + d[c] * (255 - a) + 127) / 255);
            }
        }
    }
    void compose() {
        picture_ = decoder.picture();
        blend(icon);
        blend(title);
    }
    bool update(double wall) override {
        const double dt = std::clamp(wall - lastWall, 0.0, 0.1);
        lastWall = wall;
        if (lateIcon)
            if (auto img = lateIcon()) {
                setIcon(*img);
                fresh = true;
            }
        const double end = reia.duration() - reia.frameSeconds() * 0.5;
        // While loading, the clock does not pass the progress (its bar is the
        // load's); it runs at twice the speed while far behind; finishing,
        // the rest plays at three times the speed or faster, in 0.5 s at most.
        const double cap = finishing ? end : std::min(end, double(progress ? progress() : 0) * reia.duration());
        if (finishing && tailSpeed == 0)
            tailSpeed = std::max(3.0, (end - t) / 0.5);
        const double speed = finishing ? tailSpeed : (cap - t > 0.5 ? 2 : 1);
        t = std::max(t, std::min(t + dt * speed, cap));
        decoder.seek(t);
        if (decoder.current() == shownFrame && !fresh)
            return false;
        fresh = false;
        shownFrame = decoder.current();
        compose();
        return true;
    }
    const RgbaImage& picture() const override { return picture_; }
    bool finished() const override {
        return skipped || (finishing && t >= reia.duration() - reia.frameSeconds() * 0.5);
    }
    void skip() override {
        if (finishing)
            skipped = true;
    }
    double time() const override { return t; }
    double duration() const override { return reia.duration(); }
    long frame() const override { return decoder.current(); }
    std::string describe() const override {
        char b[96];
        std::snprintf(b, sizeof b, "Reia %ux%u, %zu frames, %.2f s, loading screen%s", reia.width(), reia.height(),
                      reia.frameCount(), reia.duration(), icon.img.pixels.empty() ? "" : " with an icon");
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

bool playLoadingScreen(LoadingScreen screen, std::function<float()> progress, std::function<void()> onDone) {
    Host& h = host();
    h.player.reset();
    h.onDone = nullptr;
    char name[32];
    std::snprintf(name, sizeof name, "reia:%08X", screen.reia);
    try {
        auto bytes = readUiReia(h.options.resDir, screen.reia);
        if (!bytes)
            throw std::runtime_error("not in ui.package");
        h.player = std::make_unique<LoadingClip>(std::move(*bytes), std::move(screen), std::move(progress));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: loading screen %s: %s\n", name, e.what());
        if (onDone)
            onDone();
        return false;
    }
    h.onDone = std::move(onDone);
    h.start = std::chrono::steady_clock::now();
    std::printf("opensims2: movie %s (%s)\n", name, h.player->describe().c_str());
    std::fflush(stdout);
    if (h.layer)
        h.layer->setFrame(h.player->picture());
    return true;
}

void finishLoadingScreen() {
    if (auto* clip = dynamic_cast<LoadingClip*>(host().player.get()))
        clip->finishing = true;
}

bool loadingScreenPlaying() { return dynamic_cast<LoadingClip*>(host().player.get()) != nullptr; }

RgbaImage movieHostCapture(uint32_t width, uint32_t height, double seconds) {
    Host& h = host();
    if (!h.player || !h.layer)
        return {};
    for (double t = 1.0 / 30; t < seconds + 1e-9; t += 1.0 / 30)
        h.player->update(t);
    h.player->update(seconds);
    h.layer->setFrame(h.player->picture());
    return h.layer->capture(width, height);
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
