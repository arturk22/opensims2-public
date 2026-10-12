#include "app/tv_broadcast.hpp"

#include "formats/movie.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace os2 {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

// A path under `base` matched without regard to case ("Games/bus" is
// "Games/BUS" on the CD).
fs::path findPath(const fs::path& base, const std::string& rel) {
    fs::path at = base;
    std::string part;
    std::string r = rel;
    std::replace(r.begin(), r.end(), '\\', '/');
    size_t from = 0;
    while (from <= r.size()) {
        const size_t slash = r.find('/', from);
        part = r.substr(from, slash == std::string::npos ? std::string::npos : slash - from);
        from = slash == std::string::npos ? r.size() + 1 : slash + 1;
        if (part.empty())
            continue;
        fs::path found;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(at, ec))
            if (lower(e.path().filename().string()) == lower(part)) {
                found = e.path();
                break;
            }
        if (found.empty())
            return {};
        at = found;
    }
    return at;
}

std::vector<fs::path> moviesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!dir.empty())
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (lower(e.path().extension().string()) == ".movie")
                out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TvBroadcast::TvBroadcast(Renderer& renderer, const fs::path& resDir) : renderer_(renderer), movies_(resDir / "Movies") {
    loadIni(resDir / "UserData/Config/Broadcast.ini", movies_);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(movies_ / "Games/INI", ec))
        if (lower(e.path().extension().string()) == ".ini")
            loadIni(e.path(), movies_);
}

TvBroadcast::~TvBroadcast() = default;

void TvBroadcast::loadIni(const fs::path& file, const fs::path&) {
    std::ifstream in(file);
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(line.substr(1, line.size() - 2));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || section.empty() || section == "broadcasts")
            continue;
        const std::string key = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        Definition& d = defs_[section];
        if (key == "folder")
            d.folder = value;
        else if (key == "commercialfolder")
            d.commercialFolder = value;
        else if (key == "commercialinterval")
            d.commercialInterval = std::atof(value.c_str());
        else if (key == "file")
            d.file = value;
        else if (key == "isgames")
            d.games = std::atoi(value.c_str()) != 0;
    }
}

bool TvBroadcast::isChannel(const std::string& material) const { return defs_.count(lower(material)) != 0; }

void TvBroadcast::next(Live& l, const Definition& d, double now) {
    // As cVideoBroadcastChannel::ChooseNextMovieKey: the shows in order (the
    // index wraps), and after each one a commercial drawn at random, not the
    // last one again; CommercialInterval is not read on that path.
    fs::path pick;
    l.program.next(rng_, pick);
    (void)d;
    (void)now;
    l.decoder.reset();
    l.movie.reset();
    if (pick.empty())
        return;
    try {
        l.movie = std::make_unique<Movie>(Movie::load(pick));
        l.decoder = std::make_unique<MovieDecoder>(*l.movie);
        l.started = now;
        l.sound = {};
        l.sound.started = now;
        l.sound.serial = ++serial_;
        if (soundWanted_ && l.movie->hasSound())
            l.sound.pcm = std::make_shared<const Pcm>(l.movie->decodeSound());
        if (std::getenv("OS2_DEBUG_TV"))
            std::fprintf(stderr, "opensims2: tv plays %s\n", pick.filename().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "opensims2: tv %s: %s\n", pick.c_str(), e.what());
        l.movie.reset();
    }
}

void TvBroadcast::adjust(ObjectModel& model, const std::map<std::string, std::string>& materials) {
    for (const auto& [subset, material] : materials) {
        const std::string channel = lower(material);
        auto d = defs_.find(channel);
        if (d == defs_.end())
            continue;
        const std::string tex = "movie:" + channel;
        auto [it, fresh] = live_.try_emplace(channel);
        if (fresh) {
            Live& l = it->second;
            if (d->second.games) {
                if (fs::path f = findPath(movies_, d->second.folder + "/" + d->second.file); !f.empty())
                    l.program.shows.push_back(f);
            } else {
                l.program.shows = moviesIn(findPath(movies_, d->second.folder));
                l.program.commercials = moviesIn(findPath(movies_, d->second.commercialFolder));
            }
            l.picture = RgbaImage{128, 128, std::vector<uint8_t>(128 * 128 * 4, 0)};
            for (size_t k = 3; k < l.picture.pixels.size(); k += 4)
                l.picture.pixels[k] = 255;
            renderer_.setDynamicTexture(tex + "_txtr", l.picture); // as materials name TXTRs
            next(l, d->second, last_);
        }
        MaterialDefinition m;
        m.materialType = "StandardMaterial";
        m.properties = {{"stdMatBaseTextureName", tex},
                        {"stdMatDiffCoef", "1,1,1,1"},
                        {"stdMatLightingEnabled", "false"}};
        model.materials[tex] = m;
        for (auto& p : model.parts)
            if (lower(p.mesh.name) == lower(subset))
                p.material = tex;
    }
}

const TvBroadcast::Sound* TvBroadcast::sound(const std::string& channel) const {
    auto it = live_.find(lower(channel));
    if (it == live_.end() || !it->second.movie || !it->second.sound.pcm)
        return nullptr;
    return &it->second.sound;
}

void TvBroadcast::update(double seconds) {
    last_ = seconds;
    for (auto& [channel, l] : live_) {
        const Definition& d = defs_[channel];
        // The next movie starts where the last one ended (several, if the
        // clock jumped: headless shots far apart).
        for (int guard = 0; guard < 64 && (!l.decoder || l.decoder->finished(seconds - l.started)); ++guard) {
            const double end = l.movie ? l.started + l.movie->duration() : seconds;
            next(l, d, std::min(end, seconds));
            if (!l.decoder)
                break;
        }
        if (!l.decoder)
            continue;
        if (std::getenv("OS2_DEBUG_TV"))
            std::fprintf(stderr, "opensims2: tv %s at %.2f s: %.2f s into the movie (frame %ld of %zu)\n", channel.c_str(),
                         seconds, seconds - l.started, l.decoder->current(), l.movie->frames().size());
        if (l.decoder->seek(seconds - l.started)) {
            l.decoder->picture(l.picture);
            renderer_.setDynamicTexture("movie:" + channel + "_txtr", l.picture);
        }
    }
}

} // namespace os2
