#include "sound_system.hpp"

#include "audio/decode.hpp"
#include "formats/property_set.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace os2 {

namespace {

std::vector<std::string> splitList(std::string_view s, char sep = ',') {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(sep, pos);
        if (end == std::string_view::npos)
            end = s.size();
        std::string item(s.substr(pos, end - pos));
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty())
            out.push_back(item);
        pos = end + 1;
    }
    return out;
}

std::vector<float> numberList(std::string_view s) {
    std::vector<float> out;
    for (const auto& item : splitList(s))
        out.push_back(std::strtof(item.c_str(), nullptr));
    return out;
}

float numberOr(const std::map<std::string, std::string>& m, const char* key, float fallback) {
    auto it = m.find(key);
    return it == m.end() || it->second.empty() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

std::string get(const std::map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = char(std::tolower(uint8_t(c)));
    return out;
}

// The mixing group of a sound from the folder its sample came from.
SoundGroup groupOf(uint32_t resourceGroup) {
    const char* name = soundGroupName(resourceGroup);
    if (!name)
        return SoundGroup::Sfx;
    std::string_view n(name);
    if (n.starts_with("Vox"))
        return SoundGroup::Vox;
    if (n.starts_with("Music") || n.starts_with("Stings"))
        return SoundGroup::Music;
    if (n.starts_with("Ambience"))
        return SoundGroup::Ambience;
    if (n.starts_with("UI"))
        return SoundGroup::Ui;
    return SoundGroup::Sfx;
}

} // namespace

float evalCurve(const std::vector<float>& xy, float x) {
    const size_t n = xy.size() / 2;
    if (n == 0)
        return 1;
    if (x <= xy[0])
        return xy[1];
    for (size_t i = 1; i < n; ++i) {
        const float x0 = xy[2 * (i - 1)], y0 = xy[2 * (i - 1) + 1], x1 = xy[2 * i], y1 = xy[2 * i + 1];
        if (x <= x1)
            return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
    }
    return xy[2 * n - 1];
}

FscScore parseFsc(std::string_view text) {
    FscScore score;
    std::vector<std::vector<std::string>> rows;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line.starts_with("version") || line.starts_with("cells"))
            continue;
        std::vector<std::string> cols;
        size_t p = 0;
        while (p <= line.size()) {
            size_t e = line.find('\t', p);
            if (e == std::string_view::npos)
                e = line.size();
            cols.emplace_back(line.substr(p, e - p));
            p = e + 1;
        }
        if (cols.size() > 1)
            rows.push_back(std::move(cols));
    }
    auto num = [](const std::vector<std::string>& r, size_t i) {
        return i < r.size() ? std::atoi(r[i].c_str()) : 0;
    };
    if (rows.empty())
        return score;
    // Header: (empty), vol, pri, min, max, rows, cols, tempo, BPB, ...
    score.masterVolume = float(num(rows[0], 1));
    score.tempo = std::max(1, num(rows[0], 7));
    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& c = rows[r];
        if (c.size() < 20)
            continue;
        FscScore::Note note;
        note.volume = float(num(c, 1));
        note.rand = num(c, 2) != 0;
        note.pan = num(c, 3);
        note.loop = std::max(1, num(c, 10));
        note.prob = num(c, 13);
        note.pitchLow = num(c, 14);
        note.pitchHigh = num(c, 15);
        note.file = c[19];
        if (note.file.size() > 3 && lower(note.file.substr(note.file.size() - 3)) == ".xa")
            note.file.resize(note.file.size() - 3);
        if (note.rand)
            score.jumpPoints.push_back(score.notes.size());
        score.notes.push_back(std::move(note));
    }
    return score;
}

SoundSystem::SoundSystem(const std::filesystem::path& resDir, AudioMixer& mixer, uint32_t seed)
    : mixer_(mixer), bank_(resDir), random_(seed) {
    const auto aliasesKey = SoundBank::nameKey("aliases"), shoesKey = SoundBank::nameKey("shoetypes"),
               eventsKey = SoundBank::nameKey("events");
    for (const auto& loc : bank_.ofType(type::SOUND_PROPERTIES)) {
        std::map<std::string, std::string> props;
        try {
            props = readPropertySet(bank_.read(loc));
        } catch (const std::exception&) {
            continue;
        }
        const SoundBank::Key k{loc.entry->tgi.instance, loc.entry->tgi.instanceHi};
        if (k == aliasesKey) {
            for (auto& [key, value] : props)
                aliases_[lower(key)] = value;
        } else if (k == eventsKey) {
            for (auto& [key, value] : props)
                modifiers_[lower(key)] = splitList(value);
        } else if (k == shoesKey) {
            for (auto& [key, value] : props)
                shoes_[uint32_t(std::atoi(key.c_str()))] = value;
        } else if (props.count("Name")) {
            AmbienceLayer layer;
            layer.name = props["Name"];
            layer.enabled = splitList(get(props, "EnabledPrimitiveIds"));
            layer.andPartition = splitList(get(props, "ANDPartitionPrimitiveIds"));
            layer.notPartition = splitList(get(props, "NOTPartitionPrimitiveIds"));
            layer.loopVolume = splitList(get(props, "LoopVolumePrimitives"));
            layer.freshVolume = splitList(get(props, "FreshnessVolumePrimitives"));
            layer.loopName = get(props, "AmbientLoopName");
            layer.scoreName = get(props, "FreshnessScoreName");
            layer.loopMinDistance = numberOr(props, "LoopMinDistance", 5);
            layer.freshMinDistance = numberOr(props, "FreshnessMinDistance", 5);
            layer.loop2d = lower(get(props, "LoopSpatialMode")) == "2d";
            layer.fresh2d = lower(get(props, "FreshnessSpatialMode")) == "2d";
            for (auto& [key, value] : props)
                if (lower(key).find("curve") != std::string::npos)
                    layer.curves[lower(key)] = numberList(value);
            layers_.push_back(std::move(layer));
        } else if (props.count("Physical") && props.count("Creative")) {
            for (auto& [key, value] : props)
                skillRanges_[lower(key)] = splitList(value);
        } else if (props.count("LotAmbienceZoomRadiusCurve")) {
            lotRadiusCurve_ = numberList(props["LotAmbienceZoomRadiusCurve"]);
        } else if (props.count("Nhood") && props.count("Build")) {
            for (auto& [key, value] : props)
                musicCategories_.push_back(value);
        } else if (props.count("defaultmusicvolume")) {
            musicVolume_ = numberOr(props, "defaultmusicvolume", 0.7f);
            musicFadeOut_ = numberOr(props, "trackfadeouttime", 3000) / 1000.0;
        } else if (props.count("rolloff")) {
            rolloff_ = numberOr(props, "rolloff", 1.2f);
        }
    }
    std::sort(layers_.begin(), layers_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
}

bool SoundSystem::exists(std::string_view name) const {
    const auto k = SoundBank::nameKey(name);
    return bank_.hitList(k) || bank_.sample(k);
}

const std::vector<std::string>* SoundSystem::modifiersOf(std::string_view event) const {
    auto it = modifiers_.find(lower(event));
    return it == modifiers_.end() ? nullptr : &it->second;
}

std::string SoundSystem::shoeName(uint32_t value) const {
    auto it = shoes_.find(value);
    return it == shoes_.end() ? std::string() : it->second;
}

std::string SoundSystem::skillSuffix(std::string_view skill, int value) const {
    auto it = skillRanges_.find(lower(skill));
    if (it == skillRanges_.end() || it->second.size() < 3)
        return {};
    // "0,low,10,medium,20,high,30": 0-9 low, 10-19 medium, ...; outside, the extremes.
    const auto& r = it->second;
    std::string out = r[1];
    for (size_t i = 1; i + 1 < r.size(); i += 2)
        if (value >= std::atoi(r[i - 1].c_str()))
            out = r[i];
    return out;
}

std::string SoundSystem::resolveEvent(std::string_view event, const SoundModifiers& m) const {
    const std::string base(event);
    std::vector<std::string> suffixes;
    // Voice events ("vox_soc_agree_hiA") take the speaker's voice suffix
    // last, after their listed modifiers ("vox_exmach_pec_stressA" =
    // physicalskill -> "..._hs_ama"): the tracks are named so; the events
    // property set lists "vox" only for some cinematic ones (inferred).
    std::vector<std::string> mods;
    if (const auto* listed = modifiersOf(base))
        mods = *listed;
    if (lower(base).starts_with("vox") && std::find(mods.begin(), mods.end(), "vox") == mods.end())
        mods.push_back("vox");
    bool voxLast = false;
    {
        for (const auto& mod : mods) {
            const std::string* v = nullptr;
            if (mod == "destsurfacetype" || mod == "destfootstep")
                v = &m.destSurface;
            else if (mod == "sourcesurfacetype")
                v = &m.sourceSurface;
            else if (mod == "sourcefootstep")
                v = &m.shoe;
            else if (mod == "objectquality")
                v = &m.quality;
            else if (mod == "vox")
                v = &m.vox;
            else if (mod == "physicalskill")
                v = &m.physical;
            else if (mod == "creativeskill")
                v = &m.creative;
            if (!v || v->empty())
                break;
            suffixes.push_back(*v);
            voxLast = mod == "vox";
        }
    }
    // A voice the game has no recording of (the ghosts' "g") falls back to
    // the first voice of that age and gender.
    if (voxLast && !suffixes.back().empty() && suffixes.back().back() != 'a' && suffixes.back().size() > 1) {
        std::string name = base;
        for (const auto& sfx : suffixes)
            name += "_" + sfx;
        if (!exists(name) && !aliases_.count(lower(name)))
            suffixes.back().back() = 'a';
    }
    // The longest name that exists: all the suffixes, then fewer. An alias
    // redirects the composed name ("foot_run_cpet_leath" -> "foot_run_cpet_rub",
    // and a bare "foot_step" -> "foot_step_cpet_bare").
    for (size_t n = suffixes.size() + 1; n-- > 0;) {
        std::string name = base;
        for (size_t i = 0; i < n; ++i)
            name += "_" + suffixes[i];
        if (auto it = aliases_.find(lower(name)); it != aliases_.end() && exists(it->second))
            return it->second;
        if (exists(name))
            return name;
    }
    return {};
}

std::shared_ptr<const Pcm> SoundSystem::decodeSample(const SoundBank::Location& loc) {
    const std::pair<uint32_t, uint32_t> key{loc.entry->tgi.instance, loc.entry->tgi.instanceHi};
    if (auto it = cache_.find(key); it != cache_.end())
        return it->second;
    std::shared_ptr<const Pcm> pcm;
    try {
        pcm = std::make_shared<const Pcm>(bank_.decode(loc));
    } catch (const std::exception&) {
        pcm = nullptr;
    }
    cache_[key] = pcm;
    return pcm;
}

std::shared_ptr<const Pcm> SoundSystem::pick(std::string_view name, SoundGroup* group) {
    const auto samples = bank_.resolve(name);
    if (samples.empty())
        return nullptr;
    // The game's choice among a hit list's entries is not decoded (HIT
    // SmartChoose?); uniformly random here.
    std::uniform_int_distribution<size_t> pickOne(0, samples.size() - 1);
    const auto& loc = samples[pickOne(random_)];
    if (group)
        *group = groupOf(loc.entry->tgi.group);
    return decodeSample(loc);
}

SoundSystem::Handle SoundSystem::play(std::string_view name, const PlayOptions& o) {
    SoundGroup group = SoundGroup::Sfx;
    auto pcm = pick(name, &group);
    if (!pcm) {
        ++missing[std::string(name)];
        return 0;
    }
    PlayOptions with = o;
    if (!with.group)
        with.group = group;
    const Handle h = playPcm(std::move(pcm), with);
    if (h)
        ++playedNames[std::string(name)];
    return h;
}

SoundSystem::Handle SoundSystem::playPcm(std::shared_ptr<const Pcm> pcm, const PlayOptions& o) {
    AudioMixer::Params p;
    p.group = o.group.value_or(SoundGroup::Sfx);
    p.loop = o.loop;
    p.volume = o.volume;
    p.pan = o.pan;
    float minDistance = 5;
    if (o.position) {
        auto [gain, pan] = spatialize(*o.position, minDistance);
        p.volume = o.volume * gain;
        p.pan = pan;
    }
    const Handle h = mixer_.play(std::move(pcm), p);
    if (h) {
        ++played;
        if (o.position)
            positional_.push_back({h, *o.position, o.volume, minDistance});
    }
    return h;
}

void SoundSystem::stop(Handle h) {
    mixer_.stop(h);
    std::erase_if(positional_, [&](const Positional& p) { return p.handle == h; });
}

void SoundSystem::fadeOut(Handle h, double seconds) {
    std::erase_if(positional_, [&](const Positional& p) { return p.handle == h; });
    mixer_.fade(h, 0, seconds, true);
}

void SoundSystem::setPosition(Handle h, SoundPos pos) {
    for (auto& p : positional_)
        if (p.handle == h)
            p.pos = pos;
}

void SoundSystem::setVolume(Handle h, float volume, double fadeSeconds) {
    for (auto& p : positional_)
        if (p.handle == h) {
            p.volume = volume;
            mixer_.fade(h, volume * spatialize(p.pos, p.minDistance).first, fadeSeconds);
            return;
        }
    mixer_.fade(h, volume, fadeSeconds);
}

void SoundSystem::setListener(SoundPos at, float rightX, float rightY) {
    listener_ = at;
    const float len = std::hypot(rightX, rightY);
    if (len > 1e-6f)
        rightX_ = rightX / len, rightY_ = rightY / len;
}

std::pair<float, float> SoundSystem::spatialize(SoundPos p, float minDistance) const {
    const float dx = p.x - listener_.x, dy = p.y - listener_.y, dz = p.z - listener_.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float gain = d <= minDistance ? 1.0f : std::pow(minDistance / d, rolloff_);
    const float h = std::hypot(dx, dy);
    // Pan by the side of the camera the source is on, softened near the listener.
    const float side = h > 1e-4f ? (dx * rightX_ + dy * rightY_) / h : 0.0f;
    const float pan = side * std::min(1.0f, h / std::max(minDistance, 1.0f)) * 0.8f;
    return {gain, pan};
}

void SoundSystem::update() {
    std::erase_if(positional_, [&](const Positional& p) { return !mixer_.playing(p.handle); });
    for (const auto& p : positional_) {
        auto [gain, pan] = spatialize(p.pos, p.minDistance);
        mixer_.fade(p.handle, p.volume * gain, 0.05);
        mixer_.setPan(p.handle, pan);
    }
}

float SoundSystem::lotAmbienceRadius(float zoom) const { return evalCurve(lotRadiusCurve_, zoom); }

bool SoundSystem::isMusicCategory(std::string_view category) const {
    const std::string c = lower(category);
    return std::any_of(musicCategories_.begin(), musicCategories_.end(), [&](const auto& m) { return lower(m) == c; });
}

std::vector<SoundBank::Location> SoundSystem::musicTracks(std::string_view category) const {
    std::vector<SoundBank::Location> out;
    const std::string folder = "music/" + lower(category);
    for (const auto& loc : bank_.samples()) {
        const char* g = soundGroupName(loc.entry->tgi.group);
        if (g && lower(g) == folder)
            out.push_back(loc);
    }
    return out;
}

const FscScore* SoundSystem::score(std::string_view name) {
    auto it = scores_.find(std::string(name));
    if (it != scores_.end())
        return it->second.get();
    std::unique_ptr<FscScore> s;
    if (auto loc = bank_.sample(SoundBank::nameKey(name))) {
        const auto bytes = bank_.read(*loc);
        if (detectAudioFormat(bytes) == AudioFormat::Text) {
            s = std::make_unique<FscScore>(
                parseFsc(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
            if (s->notes.empty())
                s.reset();
        }
    }
    return (scores_[std::string(name)] = std::move(s)).get();
}

// --- AmbiencePlayer ---

AmbiencePlayer::~AmbiencePlayer() {
    for (auto& [name, r] : running_)
        if (r.loop)
            sound_.stop(r.loop);
}

float AmbiencePlayer::primitive(const State& s, const std::string& id) const {
    if (id == "islot")
        return s.onLot;
    if (id == "isneighborhood")
        return !s.onLot;
    if (id == "lightofday" || id == "timeofday")
        return s.timeOfDay;
    if (id == "outsideness")
        return s.outsideness;
    if (id == "zoom")
        return s.zoom;
    if (id == "isinside")
        return s.inside;
    if (id == "isoutside")
        return !s.inside;
    if (id == "islivemode")
        return 1;
    if (id == "ispaused")
        return s.paused;
    auto it = s.partitions.find(id);
    return it != s.partitions.end() && it->second;
}

// Product over a primitive list: a curve name maps the value before it
// through the curve, other names multiply in their value.
float AmbiencePlayer::weight(const AmbienceLayer& layer, const State& s, const std::vector<std::string>& list) const {
    float w = 1;
    std::optional<float> pending; // a value waiting to see whether a curve follows
    for (const auto& id : list) {
        if (auto curve = layer.curves.find(id); curve != layer.curves.end()) {
            w *= evalCurve(curve->second, pending.value_or(1));
            pending.reset();
            continue;
        }
        if (pending)
            w *= *pending, pending.reset();
        const float v = primitive(s, id);
        if (id.rfind("is", 0) == 0)
            w *= v; // booleans gate at once
        else
            pending = v;
    }
    if (pending)
        w *= *pending;
    return w;
}

void AmbiencePlayer::update(const State& s, double dt) {
    for (const auto& layer : sound_.ambienceLayers()) {
        float on = weight(layer, s, layer.enabled);
        for (const auto& p : layer.andPartition)
            on *= primitive(s, p);
        // Partitions are areas of the world in the game (flora, water, indoors
        // around the listener); here they are lot-wide flags, except that
        // "not inside" keeps a layer while any of the area heard is outdoors.
        for (const auto& p : layer.notPartition)
            if (p == "isinside" ? s.outsideness <= 0 : primitive(s, p) > 0)
                on = 0;
        Running& r = running_[layer.name];
        const float loopVolume = on * weight(layer, s, layer.loopVolume);
        const float freshVolume = on * weight(layer, s, layer.freshVolume);
        volumes_[layer.name] = std::max(layer.loopName.empty() ? 0.0f : loopVolume,
                                        layer.scoreName.empty() ? 0.0f : freshVolume);
        // The loop: started when audible, faded (LoopVolumeLerpSteps), stopped when off.
        if (!layer.loopName.empty()) {
            if (loopVolume > 0.001f && !r.loop) {
                r.loop = sound_.play(layer.loopName, {.loop = true, .volume = 0, .group = SoundGroup::Ambience});
                r.loopVolume = -1;
            }
            if (r.loop && std::fabs(loopVolume - r.loopVolume) > 0.005f) {
                sound_.setVolume(r.loop, loopVolume, 1.0);
                r.loopVolume = loopVolume;
            }
            if (r.loop && loopVolume <= 0.001f && r.loopVolume <= 0.001f && !sound_.playing(r.loop))
                r.loop = 0;
        }
        // The FSC sequence: one note per beat.
        if (!layer.scoreName.empty() && freshVolume > 0.001f && !s.paused) {
            if (!r.score)
                r.score = sound_.score(layer.scoreName);
            const FscScore* sc = r.score;
            if (!sc)
                continue;
            auto jump = [&] {
                r.fresh = true;
                if (sc->jumpPoints.empty())
                    r.position = 0;
                else
                    r.position = sc->jumpPoints[std::uniform_int_distribution<size_t>(
                        0, sc->jumpPoints.size() - 1)(sound_.random())];
            };
            if (!r.started) {
                jump();
                r.started = r.fresh = true;
            }
            const double beat = 60.0 / sc->tempo;
            r.clock += dt;
            while (r.clock >= beat) {
                r.clock -= beat;
                if (r.hold > 0) {
                    --r.hold;
                    continue;
                }
                // A segment runs until the next one starts (a "rand" note) or
                // the score ends; then the player jumps to a random segment.
                if (r.position >= sc->notes.size() || (sc->notes[r.position].rand && !r.fresh))
                    jump();
                r.fresh = false;
                const auto* note = &sc->notes[r.position++];
                if (note->file != "NONE" && !note->file.empty()) {
                    const bool play =
                        note->prob <= 0 || int(std::uniform_int_distribution<int>(0, 15)(sound_.random())) < note->prob;
                    if (play) {
                        const float v = note->volume / 1024.0f * sc->masterVolume / 1024.0f * freshVolume;
                        if (sound_.play(note->file, {.volume = v, .pan = note->pan / 512.0f - 1,
                                                     .group = SoundGroup::Ambience}))
                            ++notesPlayed_;
                    }
                }
                r.hold = note->loop - 1;
            }
        } else {
            r.clock = 0;
        }
    }
}

// --- MusicPlayer ---

MusicPlayer::~MusicPlayer() {
    if (voice_)
        sound_.stop(voice_);
}

void MusicPlayer::setCategory(std::string_view category, std::optional<SoundPos> position) {
    if (category == category_ && position_.has_value() == position.has_value()) {
        if (position && voice_)
            sound_.setPosition(voice_, *position);
        position_ = position;
        return;
    }
    if (voice_)
        sound_.fadeOut(voice_, sound_.musicFadeOut());
    voice_ = 0;
    playing_.clear();
    category_ = category;
    position_ = position;
    queue_ = category_.empty() ? std::vector<SoundBank::Location>{} : sound_.musicTracks(category_);
    std::shuffle(queue_.begin(), queue_.end(), sound_.random());
    next_ = 0;
    update();
}

void MusicPlayer::update() {
    if (queue_.empty() || (voice_ && sound_.playing(voice_)))
        return;
    const auto& loc = queue_[next_++ % queue_.size()];
    auto pcm = sound_.decodeSample(loc);
    if (!pcm)
        return;
    voice_ = sound_.playPcm(pcm, {.position = position_, .volume = sound_.defaultMusicVolume(),
                                  .group = SoundGroup::Music});
    char buf[24];
    std::snprintf(buf, sizeof buf, "%08X-%08X", loc.entry->tgi.instanceHi, loc.entry->tgi.instance);
    playing_ = buf;
}

std::string MusicPlayer::nowPlaying() const { return playing_; }

} // namespace os2
