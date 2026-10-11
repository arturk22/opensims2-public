#pragma once

// Fullscreen movies in the app (the EA logo and intro before the main menu,
// or any .movie): drawn over everything in the window with their sound on
// an output of their own, skipped with Esc, Space, Return or a click.
//
// For game code: playMovie("intro_eng_audio", [] { ...show the main menu... }).
// The names are those findMovie (formats/movie.hpp) takes. onDone runs on
// the main thread when the movie ends or is skipped (also at once when the
// movie is missing, after a message on stderr); it may start another movie.

#include <filesystem>
#include <functional>
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

// The movies the game plays before the main menu, in order.
std::vector<std::string> introMovies();

// Host side (main.mm). Init once the window's Metal view exists.
struct MovieHostOptions {
    std::filesystem::path resDir; // TSData/Res
    bool sound = true;            // open an output device for the sound
};
void movieHostInit(MTL::Device* device, uint32_t colorFormat, uint32_t depthFormat, const MovieHostOptions& options);
// Each frame: when a movie is playing, draws it into the drawable and
// returns true (the caller then draws nothing else); runs onDone when it ends.
bool movieHostDraw(MTL::RenderPassDescriptor* pass, CA::MetalDrawable* drawable);

// --movie <name> --screenshot out.png [--frames n] [--time seconds]
// [--sound-wav mix.wav]: plays the movie headless with an offline mixer as
// the clock and writes n frames spread up to `seconds` (default: the whole
// movie) at width x height, printing the picture's and the sound's
// timestamps for each. Returns the exit code.
int movieScreenshots(const std::filesystem::path& resDir, const std::string& name, const std::string& out,
                     int frames, double seconds, uint32_t width, uint32_t height, const std::string& wavOut);

} // namespace os2
