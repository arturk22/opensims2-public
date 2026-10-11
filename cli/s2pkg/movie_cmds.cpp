// s2pkg commands for the .movie files (EA MAD video + EA ADPCM sound).
#include "movie_cmds.hpp"

#include "formats/movie.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace os2 {

namespace {

namespace fs = std::filesystem;

// A file path, or a name looked up under <Res>.
fs::path resolve(const std::string& where, const std::string& name) {
    if (fs::is_regular_file(name))
        return name;
    const fs::path p = findMovie(where, name);
    if (p.empty())
        throw std::runtime_error("no movie \"" + name + "\" under " + where);
    return p;
}

int cmdMovieInfo(const fs::path& path) {
    const Movie m = Movie::load(path);
    std::map<char, int> kinds;
    for (const auto& f : m.frames())
        ++kinds[f.kind];
    const auto& s = m.soundHeader();
    std::printf("%s: %dx%d, %zu frames (%d MADk, %d MADm, %d MADe) at %.3f ms = %.2f s\n", path.string().c_str(),
                m.width(), m.height(), m.frames().size(), kinds['k'], kinds['m'], kinds['e'], m.frameSeconds() * 1000,
                m.duration());
    if (m.hasSound())
        std::printf("sound: EA ADPCM revision %d, %d channel(s), %d Hz, %u samples = %.2f s\n", s.revision, s.channels,
                    s.sampleRate, s.samples, s.sampleRate > 0 ? double(s.samples) / s.sampleRate : 0.0);
    return 0;
}

int cmdMovieFrame(const fs::path& path, double seconds, const std::string& out) {
    const Movie m = Movie::load(path);
    MovieDecoder d(m);
    d.seek(seconds);
    RgbaImage img;
    d.picture(img);
    writePng(out, img);
    std::printf("frame %ld (%.2f s) -> %s\n", d.current(), seconds, out.c_str());
    return 0;
}

int cmdMovieSound(const fs::path& path, const std::string& out) {
    const Pcm pcm = Movie::load(path).decodeSound();
    const auto wav = encodeWav(pcm);
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
    std::printf("%zu frames, %u Hz, %u ch, peak %d, %.1f dB RMS -> %s\n", pcm.frames(), pcm.sampleRate, pcm.channels,
                pcm.peak(), pcm.rmsDb(), out.c_str());
    return 0;
}

// Decodes every frame and the sound of every movie under <Res>/Movies and Locale/*/Movies.
int cmdMovieSweep(const fs::path& res) {
    int files = 0, bad = 0;
    size_t frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& e : fs::recursive_directory_iterator(res)) {
        if (!e.is_regular_file() || e.path().extension() != ".movie")
            continue;
        ++files;
        const Movie m = Movie::load(e.path());
        MadDecoder mad;
        int failed = 0;
        for (const auto& f : m.frames())
            failed += !mad.decode(m.chunk(f));
        frames += m.frames().size();
        const Pcm pcm = m.decodeSound();
        const double av = pcm.seconds() - m.duration();
        if (failed || std::abs(av) > 0.5) {
            ++bad;
            std::printf("%s: %d/%zu frames damaged, sound %.2f s vs video %.2f s\n", e.path().string().c_str(), failed,
                        m.frames().size(), pcm.seconds(), m.duration());
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d movies, %zu frames, %d with problems (%.1f s, %.0f frames/s)\n", files, frames, bad, secs,
                frames / secs);
    return bad ? 1 : 0;
}

} // namespace

const char* movieUsage() {
    return "       s2pkg movie-info <Res-dir> <name|file.movie>\n"
           "       s2pkg movie-frame <Res-dir> <name|file.movie> <seconds> <out.png>\n"
           "       s2pkg movie-sound <Res-dir> <name|file.movie> <out.wav>\n"
           "       s2pkg movie-sweep <Res-dir>\n";
}

int movieCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "movie-info" && argc == 4)
        return cmdMovieInfo(resolve(argv[2], argv[3]));
    if (cmd == "movie-frame" && argc == 6)
        return cmdMovieFrame(resolve(argv[2], argv[3]), std::stod(argv[4]), argv[5]);
    if (cmd == "movie-sound" && argc == 5)
        return cmdMovieSound(resolve(argv[2], argv[3]), argv[4]);
    if (cmd == "movie-sweep" && argc == 3)
        return cmdMovieSweep(argv[2]);
    return -1;
}

} // namespace os2
