#include "lot_audio.hpp"

#include "formats/config_text.hpp"
#include "formats/property_set.hpp"
#include "formats/resindex.hpp"
#include "formats/sim_actor.hpp"
#include "formats/sim_character.hpp"
#include "vm/sound.hpp"
#include "vm/vm.hpp"

#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace os2 {

namespace {

constexpr uint32_t kTypeCatalogPattern = 0xCCA8E925, kTypeGzps = 0xEBCF3E27;
constexpr float kStoryHeight = 3.0f;

std::optional<uint32_t> parseGuid(const std::string& s) {
    if (s.empty() || s.size() > 8 || !std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)); }))
        return std::nullopt;
    return uint32_t(std::stoul(s, nullptr, 16));
}

// floorPatterns.txt: floorPattern "name" ... surfaceType "wood".
std::map<std::string, std::string> scriptSurfaces(const std::filesystem::path& path) {
    std::map<std::string, std::string> out;
    if (!std::filesystem::exists(path))
        return out;
    std::string current;
    forEachConfigLine(readTextFile(path), [&](std::string_view line) {
        const auto t = tokenizeConfigLine(line);
        if (t.size() < 2 || t[0].kind != ConfigToken::Word || t[1].kind != ConfigToken::Word)
            return;
        if (t[0].word == "floorPattern")
            current = lowerAscii(t[1].word);
        else if (lowerAscii(t[0].word) == "surfacetype" && !current.empty())
            out[current] = t[1].word;
    });
    return out;
}

} // namespace

SurfaceMap::SurfaceMap(const Lot& lot, ResourceIndex& index, const std::filesystem::path& resDir) : lot_(lot) {
    const auto named = scriptSurfaces(resDir / "Catalog/Scripts/floorPatterns.txt");
    std::map<uint32_t, std::string> byGuid;
    for (const auto& loc : index.ofType(kTypeCatalogPattern)) {
        try {
            const auto bytes = index.read(loc);
            const auto props =
                parsePropertySet(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            auto g = props.find("guid"), st = props.find("surfacetype");
            if (g != props.end() && st != props.end() && !st->second.empty())
                byGuid[uint32_t(std::stoul(g->second))] = st->second;
        } catch (const std::exception&) {
        }
    }
    for (const auto& [ref, name] : lot.floorPatterns) {
        if (auto it = named.find(lowerAscii(name)); it != named.end())
            surfaces_[ref] = it->second;
        else if (auto guid = parseGuid(name))
            if (auto g = byGuid.find(*guid); g != byGuid.end())
                surfaces_[ref] = g->second;
    }
}

uint16_t SurfaceMap::room(int lotLevel, int x, int y, int quarter) const {
    if (!lot_.hasLevel(lotLevel) || x < 0 || y < 0 || uint32_t(x) >= lot_.width || uint32_t(y) >= lot_.height ||
        lot_.rooms.empty())
        return 0;
    return lot_.rooms[(size_t(lotLevel - lot_.minLevel) * lot_.width + uint32_t(x)) * lot_.height + uint32_t(y)]
                     [size_t(quarter)];
}

std::string SurfaceMap::at(int lotLevel, float x, float y) const {
    if (x < 0 || y < 0 || x >= float(lot_.width) || y >= float(lot_.height))
        return terrain_;
    const auto tx = uint32_t(x), ty = uint32_t(y);
    // The quarter tile (west, north, east, south triangles) under the point.
    const float fx = x - float(tx) - 0.5f, fy = y - float(ty) - 0.5f;
    const int quarter = std::fabs(fx) > std::fabs(fy) ? (fx < 0 ? 0 : 2) : (fy > 0 ? 1 : 3);
    for (int level = std::min(lotLevel, lot_.minLevel + lot_.levelCount - 1); level >= lot_.minLevel; --level) {
        const auto& f = lot_.floor(level, tx, ty);
        uint16_t ref = f[size_t(quarter)];
        if (!ref)
            for (uint16_t q : f)
                if (q)
                    ref = q;
        if (ref) {
            auto it = surfaces_.find(ref);
            return it == surfaces_.end() ? std::string() : it->second;
        }
        if (level <= 0)
            break;
    }
    return terrain_;
}

bool SurfaceMap::indoors(int lotLevel, float x, float y) const {
    for (int q = 0; q < 4; ++q)
        if (room(lotLevel, int(x), int(y), q))
            return true;
    return false;
}

float SurfaceMap::outsideness(float x, float y, float radius) const {
    if (lot_.rooms.empty())
        return 1;
    size_t total = 0, outside = 0;
    const int r = int(std::ceil(radius));
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (float(dx * dx + dy * dy) > radius * radius)
                continue;
            const int tx = int(x) + dx, ty = int(y) + dy;
            if (tx < 0 || ty < 0 || uint32_t(tx) >= lot_.width || uint32_t(ty) >= lot_.height) {
                ++total, ++outside; // beyond the lot is outdoors
                continue;
            }
            for (int q = 0; q < 4; ++q) {
                ++total;
                // Ground floor: lot level 0, or 1 for houses on foundations.
                outside += room(0, tx, ty, q) == 0 && room(1, tx, ty, q) == 0;
            }
        }
    return total ? float(outside) / float(total) : 1.0f;
}

std::string characterShoe(ResourceIndex& index, const Character& character, const SoundSystem& sound) {
    for (const auto& [part, tgi] : character.outfit(1)) {
        Tgi key = tgi;
        key.type = kTypeGzps;
        auto loc = index.find(key);
        if (!loc)
            continue;
        try {
            const auto props = readPropertySet(index.read(*loc));
            auto it = props.find("shoe");
            if (it != props.end() && std::atoi(it->second.c_str()) > 0)
                return sound.shoeName(uint32_t(std::atoi(it->second.c_str())));
        } catch (const std::exception&) {
        }
    }
    return {};
}

LotAudio::LotAudio(const std::filesystem::path& resDir, ResourceIndex& index, const Lot& lot, const Options& o)
    : mixer_(44100), surfaces_(lot, index, resDir) {
    mixer_.setMuted(o.muted);
    sound_ = std::make_unique<SoundSystem>(resDir, mixer_);
    ambience_ = std::make_unique<AmbiencePlayer>(*sound_);
    music_ = std::make_unique<MusicPlayer>(*sound_);
    if (!o.music.empty())
        music_->setCategory(o.music);
    if (o.device) {
        output_ = std::make_unique<AudioOutput>(mixer_);
        if (!output_->start()) {
            std::fprintf(stderr, "opensims2: no audio output device; sound is off\n");
            output_.reset();
        }
    }
}

LotAudio::~LotAudio() {
    stereos_.clear();
    music_.reset();
    ambience_.reset();
    if (output_)
        output_->stop();
}

void LotAudio::attachSim(SimActor& actor, int16_t id, std::string shoe) {
    sims_.push_back({&actor, id, std::move(shoe)});
}

SoundPos LotAudio::positionOf(vm::VM& machine, int16_t id) const {
    const vm::Object* o = machine.object(id);
    if (!o || !o->position.inWorld()) {
        // Sims are placed by their actors; objects out of the world have no position.
        for (const auto& s : sims_)
            if (s.id == id)
                return {s.actor->position[0], s.actor->position[1], s.actor->position[2]};
        return {-1, -1, 0};
    }
    return {o->position.x, o->position.y, float(o->position.level - 1) * kStoryHeight};
}

SoundModifiers LotAudio::modifiersFor(vm::VM* machine, int16_t source, const SoundPos& at,
                                      const std::string& shoe) const {
    SoundModifiers m;
    m.shoe = shoe;
    int level = int(std::floor(at.z / kStoryHeight + 0.01f));
    m.destSurface = surfaces_.at(level, at.x, at.y);
    if (machine) {
        if (const vm::Object* o = machine->object(source)) {
            m.vox = vm::voxSuffix(*o); // the speaker's voice ("vox_..." events)
            if (o->person) {
                constexpr int kCreativitySkill = 15, kBodySkill = 17; // person data (STR# 0xC8)
                m.physical = sound_->skillSuffix("Physical", o->person->data[kBodySkill]);
                m.creative = sound_->skillSuffix("Creative", o->person->data[kCreativitySkill]);
            }
            // Object sounds: what the object is made of (soundinfo), not the floor.
            const auto info = vm::soundInfo(*machine, *o);
            if (!info[0].empty())
                m.destSurface = info[0];
            m.sourceSurface = info[1];
            m.quality = info[2];
            if (o->person && o->activeInteraction >= 0 && size_t(o->activeInteraction) < o->queue.size())
                if (const vm::Object* with = machine->object(o->queue[size_t(o->activeInteraction)].object)) {
                    // A Sim's animation sound takes the object it is using (the chair it sits on).
                    const auto w = vm::soundInfo(*machine, *with);
                    if (!w[0].empty())
                        m.destSurface = w[0];
                    m.sourceSurface = w[1];
                    m.quality = w[2];
                }
        }
    }
    return m;
}

void LotAudio::handle(vm::VM& machine, const vm::SoundRequest& r) {
    using Kind = vm::SoundRequest::Kind;
    auto& list = voices_[r.source];
    auto stopWhere = [&](auto pred) {
        for (auto& v : list)
            if (pred(v))
                sound_->stop(v.handle);
        std::erase_if(list, pred);
    };
    if (r.kind == Kind::StopAll) {
        stopWhere([](const ObjectVoice&) { return true; });
        stereos_.erase(r.source);
        return;
    }
    const std::string category = r.event.starts_with("stereo_") ? r.event.substr(7) : std::string();
    if (r.kind == Kind::Stop) {
        stopWhere([&](const ObjectVoice& v) { return v.event == r.event; });
        if (!category.empty())
            stereos_.erase(r.source);
        return;
    }
    ++requests_;
    const SoundPos at = positionOf(machine, r.source);
    // Radios and stereos: "stereo_<category>" plays the songs of that music
    // category from the object (inferred; no track has those names).
    if (!category.empty() && sound_->isMusicCategory(category)) {
        auto& player = stereos_[r.source];
        if (!player)
            player = std::make_unique<MusicPlayer>(*sound_);
        player->setCategory(category, at);
        return;
    }
    const std::string name = sound_->resolveEvent(r.event, modifiersFor(&machine, r.source, at, {}));
    if (name.empty()) {
        ++unresolved_;
        ++sound_->missing[r.event];
        if (std::getenv("OS2_SOUND_DEBUG")) {
            const vm::Object* o = machine.object(r.source);
            const auto info = o ? vm::soundInfo(machine, *o) : std::array<std::string, 4>{};
            std::fprintf(stderr, "sound: %s from %d (%s, group %08X) soundinfo [%s|%s|%s]\n", r.event.c_str(), r.source,
                         o && o->definition ? o->definition->fileName.c_str() : "?", o ? o->group : 0,
                         info[0].c_str(), info[1].c_str(), info[2].c_str());
        }
        return;
    }
    SoundSystem::PlayOptions o;
    o.loop = r.loop;
    o.volume = r.volume;
    if (at.x >= 0)
        o.position = at;
    if (auto h = sound_->play(name, o))
        list.push_back({r.event, h});
}

void LotAudio::update(vm::VM* machine, double seconds, const Listener& l) {
    const double dt = started_ ? std::max(0.0, seconds - last_) : 0.0;
    sound_->setListener({l.x, l.y, l.z}, l.rightX, l.rightY);
    if (machine) {
        while (!machine->soundRequests.empty()) {
            const vm::SoundRequest r = std::move(machine->soundRequests.front());
            machine->soundRequests.pop_front();
            handle(*machine, r);
        }
        // Objects that went away take their sounds along; moving ones carry them.
        for (auto it = voices_.begin(); it != voices_.end();) {
            std::erase_if(it->second, [&](const ObjectVoice& v) { return !sound_->playing(v.handle); });
            if (!machine->object(it->first)) {
                for (auto& v : it->second)
                    sound_->stop(v.handle);
                it = voices_.erase(it);
                continue;
            }
            const SoundPos p = positionOf(*machine, it->first);
            for (auto& v : it->second)
                sound_->setPosition(v.handle, p);
            ++it;
        }
        std::erase_if(stereos_, [&](const auto& s) { return !machine->object(s.first); });
    }
    // Animation events between the last update and now.
    // (Cues that are due start here as SimActor::update would start them, so
    // that runs that pose the actors less often than this is called fire them.)
    for (const auto& s : sims_)
        while (!s.actor->cues.empty() && s.actor->cues.front().at <= seconds) {
            const auto& c = s.actor->cues.front();
            s.actor->player.play(*c.anim, c.at, c.options);
            s.actor->cues.erase(s.actor->cues.begin());
        }
    if (started_)
        for (const auto& s : sims_)
            for (const auto& e : s.actor->player.eventsBetween(last_, seconds)) {
                if (e.event->type != 1014) // sound events ("sound_name=...|sound_source=...")
                    continue;
                const std::string& d = e.event->data;
                const auto k = d.find("sound_name=");
                if (k == std::string::npos)
                    continue;
                const auto end = d.find_first_of("|\r\n", k);
                const std::string event = d.substr(k + 11, end == std::string::npos ? std::string::npos : end - k - 11);
                if (event.empty())
                    continue;
                ++animEvents_;
                const SoundPos at{s.actor->position[0], s.actor->position[1], s.actor->position[2]};
                const bool foot = event.starts_with("foot_");
                SoundModifiers m = modifiersFor(foot ? nullptr : machine, s.id, at, s.shoe);
                const std::string name = sound_->resolveEvent(event, m);
                if (std::getenv("OS2_SOUND_DEBUG"))
                    std::fprintf(stderr, "sound: anim %s at (%.2f, %.2f, %.2f) surface %s shoe %s -> %s\n", event.c_str(),
                                 at.x, at.y, at.z, m.destSurface.c_str(), m.shoe.c_str(), name.c_str());
                if (name.empty()) {
                    ++unresolved_;
                    ++sound_->missing[event];
                    continue;
                }
                sound_->play(name, {.position = at});
            }
    // Ambience: the VM's clock, the outdoors around the camera, the zoom.
    AmbiencePlayer::State st;
    if (machine) {
        st.timeOfDay = float(machine->globals[vm::global::Hour] + machine->globals[vm::global::Minute] / 60.0) / 24.0f;
        st.paused = machine->globals[vm::global::Paused] != 0;
    }
    st.zoom = l.zoom;
    st.outsideness = surfaces_.outsideness(l.x, l.y, sound_->lotAmbienceRadius(l.zoom));
    st.inside = surfaces_.indoors(int(std::floor(l.z / kStoryHeight + 0.01f)), l.x, l.y);
    ambience_->update(st, dt);
    lastTimeOfDay_ = st.timeOfDay;
    music_->update();
    for (auto& [id, s] : stereos_)
        s->update();
    sound_->update();
    last_ = seconds;
    started_ = true;
}

void LotAudio::effectSounds(const std::vector<fx::EffectSound>& due) {
    std::set<std::pair<int, int>> now;
    for (const auto& e : due) {
        const auto key = std::make_pair(e.handle, e.part);
        now.insert(key);
        const SoundPos at{e.position[0], e.position[1], e.position[2]};
        if (auto it = effectVoices_.find(key); it != effectVoices_.end()) {
            if (it->second.handle)
                sound_->setPosition(it->second.handle, at);
            continue;
        }
        // The effect names the track by its instance (CRC24 | 0xFF000000 of
        // the name): "fire" FF5EF8A5, the flies' FFFD5C9D, ...
        auto t = effectTracks_.find(e.audioId);
        if (t == effectTracks_.end())
            t = effectTracks_.emplace(e.audioId, sound_->bank().trackByInstance(e.audioId)).first;
        EffectVoice v;
        if (!t->second.empty()) {
            v.loop = sound_->loops(t->second);
            v.handle = sound_->play(t->second, {.position = at, .loop = v.loop});
            effectSounds_ += v.handle != 0;
        } else {
            char id[16];
            std::snprintf(id, sizeof id, "fx:%08X", e.audioId);
            ++sound_->missing[id];
        }
        effectVoices_.emplace(key, v);
    }
    for (auto it = effectVoices_.begin(); it != effectVoices_.end();) {
        if (now.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.loop && it->second.handle)
            sound_->fadeOut(it->second.handle, 0.5);
        it = effectVoices_.erase(it);
    }
}

void LotAudio::tvSounds(vm::VM& machine, const std::vector<TvSource>& on) {
    std::set<int16_t> now;
    for (const auto& t : on) {
        now.insert(t.object);
        const SoundPos at = positionOf(machine, t.object);
        TvVoice& v = tvVoices_[t.object];
        if (v.serial == t.serial && v.handle) {
            sound_->setPosition(v.handle, at);
            continue;
        }
        if (v.handle)
            sound_->stop(v.handle);
        v.serial = t.serial;
        v.handle = sound_->playPcm(t.pcm, {.position = at.x >= 0 ? std::optional(at) : std::nullopt,
                                           .group = SoundGroup::Sfx, .startAt = t.offset});
        tvShows_ += v.handle != 0;
    }
    for (auto it = tvVoices_.begin(); it != tvVoices_.end();) {
        if (now.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.handle)
            sound_->fadeOut(it->second.handle, 0.2);
        it = tvVoices_.erase(it);
    }
}

void LotAudio::renderOffline(double seconds) {
    const uint64_t target = uint64_t(std::max(0.0, seconds) * mixer_.sampleRate());
    if (target <= mixFrames_)
        return;
    const size_t n = size_t(target - mixFrames_);
    const size_t at = mix_.size();
    mix_.resize(at + 2 * n);
    mixer_.render(mix_.data() + at, n);
    mixFrames_ = target;
}

bool LotAudio::writeMix(const std::string& path) const {
    Pcm pcm;
    pcm.sampleRate = mixer_.sampleRate();
    pcm.channels = 2;
    pcm.samples = mix_;
    const auto wav = encodeWav(pcm);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
    std::fclose(f);
    if (ok)
        std::printf("opensims2: wrote %s (%.2f s, peak %d, RMS %.1f dBFS)\n", path.c_str(), pcm.seconds(), pcm.peak(),
                    pcm.rmsDb());
    return ok;
}

std::string LotAudio::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "%zu VM sounds, %zu animation sound events, %zu effect sounds, %zu TV shows heard, %zu unresolved, "
                  "%zu voices played, %zu ambience notes",
                  requests_, animEvents_, effectSounds_, tvShows_, unresolved_, sound_->played,
                  ambience_->notesPlayed());
    std::string out = buf;
    std::vector<std::pair<size_t, std::string>> played;
    for (const auto& [name, n] : sound_->playedNames)
        played.emplace_back(n, name);
    std::sort(played.rbegin(), played.rend());
    if (!played.empty()) {
        out += "; played:";
        for (size_t i = 0; i < played.size() && i < 8; ++i)
            out += " " + played[i].second + " (" + std::to_string(played[i].first) + ")";
    }
    // Every name played, by kind (OS2_SOUND_LOG=1 lists them all).
    if (std::getenv("OS2_SOUND_LOG")) {
        out += "; all played:";
        for (const auto& [name, n] : sound_->playedNames)
            out += " " + name + " (" + std::to_string(n) + ")";
    } else {
        std::string vox;
        size_t voxCount = 0;
        for (const auto& [name, n] : sound_->playedNames)
            if (name.starts_with("vox") || name.find("_vox_") != std::string::npos) {
                if (voxCount++ < 8)
                    vox += " " + name;
            }
        if (voxCount)
            out += "; voices (" + std::to_string(voxCount) + " names):" + vox + (voxCount > 8 ? " ..." : "");
    }
    std::vector<std::pair<size_t, std::string>> missing;
    for (const auto& [name, n] : sound_->missing)
        missing.emplace_back(n, name);
    std::sort(missing.rbegin(), missing.rend());
    if (!missing.empty()) {
        out += "; missing:";
        for (size_t i = 0; i < missing.size() && i < 8; ++i)
            out += " " + missing[i].second + " (" + std::to_string(missing[i].first) + ")";
    }
    std::string layers;
    for (const auto& [name, v] : ambience_->volumes())
        if (v > 0.001f) {
            char lb[64];
            std::snprintf(lb, sizeof lb, " %s %.2f", name.c_str(), v);
            layers += lb;
        }
    char tb[48];
    std::snprintf(tb, sizeof tb, "; ambience at %02d:%02d:", int(lastTimeOfDay_ * 24), int(lastTimeOfDay_ * 1440) % 60);
    out += tb + (layers.empty() ? std::string(" none") : layers);
    return out;
}

} // namespace os2
