#pragma once

// Fullscreen movies in the app (the EA logo and intro before the main menu,
// or any .movie): drawn over everything in the window with their sound on
// an output of their own, skipped with Esc, Space, Return or a click.
//
// For game code: playMovie("intro_eng_audio", [] { ...show the main menu... }).
// The names are those findMovie (formats/movie.hpp) takes, or
// "reia:<instance-hex>[:loop]" for a Reia UI animation of ui.package
// (formats/reia.hpp; "reia:8DA3ADE7:loop" is the start-up loading screen,
// which loops until skipMovie() is called). onDone runs on
// the main thread when the movie ends or is skipped (also at once when the
// movie is missing, after a message on stderr); it may start another movie.

#include "formats/image.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace MTL {
class Device;
class RenderPassDescriptor;
} // namespace MTL
namespace CA {
class MetalDrawable;
}

namespace os2 {

// Starts a movie, replacing any playing one (whose onDone does not run).
// False when it cannot be found or read (onDone has then already run).
bool playMovie(const std::string& name, std::function<void()> onDone = {});
// Plays several in turn (the start-up sequence), then onDone.
void playMovies(std::vector<std::string> names, std::function<void()> onDone = {});
bool moviePlaying();
void skipMovie();

// A loading screen (docs/loading-mac.md): a Reia of ui.package played once,
// its clock held back by progress() (0..1, polled every frame) so that the
// progress bar drawn into the animation follows the load, with an icon
// drawn over it (fitted into iconW x iconH, centred at iconX, iconY of the
// picture: the lot loading screen's middle tile). finishLoadingScreen()
// plays the rest quickly and then ends it (onDone runs); until then
// skipMovie() leaves it alone.
struct LoadingScreen {
    uint32_t reia = 0; // instance (type 0, group kReiaGroup)
    std::optional<RgbaImage> icon;
    float iconX = 0, iconY = 0, iconW = 0, iconH = 0;
    // A title drawn as it is (not scaled), centred at titleX, titleY of the picture.
    std::optional<RgbaImage> title;
    float titleX = 0, titleY = 0;
    // Polled every frame: an icon found while loading (the community lot's
    // image); it replaces `icon`. Called from the thread that draws.
    std::function<std::optional<RgbaImage>()> lateIcon;
};
bool playLoadingScreen(LoadingScreen screen, std::function<float()> progress, std::function<void()> onDone = {});
void finishLoadingScreen();
bool loadingScreenPlaying();

// The movies the game plays before the main menu, in order.
std::vector<std::string> introMovies();

// Host side (main.mm). Init once the window's Metal view exists.
struct MovieHostOptions {
    std::filesystem::path resDir; // TSData/Res
    bool sound = true;            // open an output device for the sound
};
#ifdef _WIN32
void movieHostInit(const MovieHostOptions& options);
// Each frame: when a movie is playing, its picture (the host draws it fitted
// on black, instead of its scene), else null; runs onDone when it ends.
const RgbaImage* movieHostFrame();
#else
void movieHostInit(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat, const MovieHostOptions& options);
// Each frame: when a movie is playing, draws it into the drawable and
// returns true (the caller then draws nothing else); runs onDone when it ends.
bool movieHostDraw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable);
#endif

// Headless: moves the playing movie `seconds` on in steps of 1/30 s from its
// start and renders it at width x height (empty when nothing plays).
RgbaImage movieHostCapture(uint32_t width, uint32_t height, double seconds);

// --movie <name> --screenshot out.png [--frames n] [--time seconds]
// [--sound-wav mix.wav]: plays the movie headless with an offline mixer as
// the clock and writes n frames spread up to `seconds` (default: the whole
// movie) at width x height, printing the picture's and the sound's
// timestamps for each. Returns the exit code.
int movieScreenshots(const std::filesystem::path& resDir, const std::string& name, const std::string& out,
                     int frames, double seconds, uint32_t width, uint32_t height, const std::string& wavOut);

} // namespace os2
