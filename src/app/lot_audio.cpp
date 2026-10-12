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
    // Water: pools, and terrain a pond covers (the water level above the ground at a corner of the tile).
    water_.assign(size_t(lot.width) * lot.height, 0);
    for (const auto& pool : lot.pools)
        for (uint32_t dy = 0; dy < pool.height; ++dy)
            for (uint32_t dx = 0; dx < pool.width; ++dx) {
                const int tx = int(pool.x) + int(dx), ty = int(pool.y) + int(dy);
                if (tx >= 0 && ty >= 0 && uint32_t(tx) < lot.width && uint32_t(ty) < lot.height)
                    water_[size_t(ty) * lot.width + size_t(tx)] = 1;
            }
    for (uint32_t y = 0; y < lot.height; ++y)
        for (uint32_t x = 0; x < lot.width; ++x)
            for (int c = 0; c < 4 && !water_[size_t(y) * lot.width + x]; ++c) {
                const int gx = int(x) + (c & 1), gy = int(y) + (c >> 1);
                if (lot.waterLevel(gx, gy) > lot.elevation(0, gx, gy) + 0.01f)
                    water_[size_t(y) * lot.width + x] = 1;
            }
    for (uint8_t w : water_)
        waterTiles_ += w;
}

bool SurfaceMap::waterNear(float x, float y, float radius) const {
    if (!waterTiles_)
        return false;
    const int r = int(std::ceil(radius));
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const int tx = int(std::floor(x)) + dx, ty = int(std::floor(y)) + dy;
            if (float(dx * dx + dy * dy) <= radius * radius && tx >= 0 && ty >= 0 && uint32_t(tx) < lot_.width &&
                uint32_t(ty) < lot_.height && water_[size_t(ty) * lot_.width + size_t(tx)])
                return true;
        }
    return false;
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

GameAudio::GameAudio(const std::filesystem::path& resDir, const Options& o) : mixer_(44100), resDir_(resDir) {
    mixer_.setMuted(o.muted);
    sound_ = std::make_unique<SoundSystem>(resDir, mixer_);
    hood_ = std::make_unique<AmbiencePlayer>(*sound_);
    if (o.device) {
        output_ = std::make_unique<AudioOutput>(mixer_);
        if (!output_->start()) {
            std::fprintf(stderr, "opensims2: no audio output device; sound is off\n");
            output_.reset();
        }
    }
}

GameAudio::~GameAudio() {
    hood_.reset();
    players_.clear();
    if (output_)
        output_->stop();
}

void GameAudio::setMusic(std::string_view category) {
    if (category == wanted_)
        return;
    musicLog_.push_back(category.empty() ? "(lot)" : std::string(category));
    if (auto old = players_.find(wanted_); old != players_.end()) {
        // The opening music fades over 6 s; so does the neighborhood's when a
        // lot is ready (0x4C8E7905, instance +0x8c) -- it does not come back
        // paused: the neighborhood starts afresh (main.mm finishScene).
        // Inferred: any switch from NHood but to CAS (a stopped CAS lot
        // keeps the hood paused) counts as entering a lot.
        const std::string was = lowerAscii(wanted_);
        if (was == "splash" || (was == "nhood" && lowerAscii(category) != "cas")) {
            old->second->setCategory("", std::nullopt, 6.0);
            players_.erase(old);
        } else {
            old->second->setPaused(true); // its state keeps it for when the game comes back
        }
    }
    wanted_ = category;
    if (wanted_.empty())
        return;
    auto& p = players_[wanted_];
    if (!p) {
        p = std::make_unique<MusicPlayer>(*sound_);
        p->setBackgroundDecoding(output_ != nullptr);
        p->setCategory(wanted_);
    }
    p->setPaused(held_);
}

void GameAudio::stopAllMusic() {
    for (auto& [name, p] : players_)
        p->setCategory("", std::nullopt, 0.5);
    players_.clear();
    if (!wanted_.empty())
        musicLog_.push_back("(stop)");
    wanted_.clear();
}

std::string GameAudio::nowPlaying() const {
    auto it = players_.find(wanted_);
    return it == players_.end() ? std::string() : it->second->nowPlaying();
}

void GameAudio::setHeld(bool held) {
    if (held == held_)
        return;
    held_ = held;
    if (auto it = players_.find(wanted_); it != players_.end())
        it->second->setPaused(held);
}

void GameAudio::playUi(const std::string& name) {
    SoundSystem::PlayOptions o;
    o.group = SoundGroup::Ui;
    sound_->play(name, o);
}

void GameAudio::update(double seconds) {
    const double dt = last_ < 0 ? 0.0 : std::clamp(seconds - last_, 0.0, 0.25);
    last_ = seconds;
    for (auto& [name, p] : players_)
        p->update();
    if (hoodAmbience_ || !hood_->volumes().empty()) {
        AmbiencePlayer::State st;
        st.onLot = false;
        st.zoom = 0;
        st.outsideness = 1;
        // The neighborhood has no clock of its own: its day layers (inferred).
        st.timeOfDay = 0.5f;
        // Its water and open ground (NHWater, NHWind) as flags of the whole
        // view; in the game they are areas around the camera (inferred).
        st.partitions = {{"isflora", true}, {"iswater", true}, {"isempty", true}};
        if (!hoodAmbience_)
            st.onLot = true, st.partitions.clear(), st.outsideness = 0, st.paused = true;
        hood_->update(st, dt);
    }
    // Dipping while a cinematic plays: effects, voices and music drop to
    // their dipping volumes over dippingstarttime and come back over
    // dippingstoptime (5 s) after the scene. The binary dips buses 0, 1 and
    // 3/4 (the groups each bus is are inferred from the volumes); the
    // ambience is left as it is (inferred).
    const auto& dip = sound_->dipping();
    const std::array<std::pair<SoundGroup, float>, 3> groups{
        {{SoundGroup::Sfx, dip.sfx}, {SoundGroup::Vox, dip.vox}, {SoundGroup::Music, dip.music}}};
    for (size_t i = 0; i < groups.size(); ++i) {
        duck_[i] = stepDuck(duck_[i], dipped_, groups[i].second, dip, dt);
        mixer_.setGroupDuck(groups[i].first, duck_[i]);
    }
    sound_->update();
}

void GameAudio::beginScene() {
    // The scene's own sounds go; the music (and its fades) goes on.
    hoodAmbience_ = false;
    hood_ = std::make_unique<AmbiencePlayer>(*sound_);
    for (SoundGroup g : {SoundGroup::Sfx, SoundGroup::Vox, SoundGroup::Ambience})
        mixer_.stopGroup(g);
    sceneBase_ = mixFrames_;
    sceneSeconds_ = 0;
}

void GameAudio::renderOffline(double sceneSeconds) {
    if (output_)
        return;
    sceneSeconds_ = std::max(sceneSeconds_, sceneSeconds);
    const uint64_t target = sceneBase_ + uint64_t(std::max(0.0, sceneSeconds) * mixer_.sampleRate());
    if (target <= mixFrames_)
        return;
    const size_t n = size_t(target - mixFrames_);
    const size_t at = mix_.size();
    mix_.resize(at + 2 * n);
    mixer_.render(mix_.data() + at, n);
    mixFrames_ = target;
}

void GameAudio::dwell(double seconds) {
    const double start = sceneSeconds_;
    for (double t = 0.25; t <= seconds + 1e-6; t += 0.25) {
        update(start + t);
        renderOffline(start + t);
    }
}

bool GameAudio::writeMix(const std::string& path) {
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
    if (ok) {
        written_ = mixFrames_;
        std::printf("opensims2: wrote %s (%.2f s, peak %d, RMS %.1f dBFS)\n", path.c_str(), pcm.seconds(), pcm.peak(),
                    pcm.rmsDb());
    }
    return ok;
}

LotAudio::LotAudio(GameAudio& game, ResourceIndex& index, const Lot& lot, const Options& o)
    : game_(game), sound_(&game.sound()), liveMusic_(o.music), surfaces_(lot, index, game.resDir()) {
    ambience_ = std::make_unique<AmbiencePlayer>(*sound_);
}

LotAudio::~LotAudio() {
    stereos_.clear();
    ambience_.reset();
    for (auto& [id, list] : voices_)
        for (auto& v : list)
            sound_->stop(v.handle);
    for (auto& [key, v] : effectVoices_)
        sound_->stop(v.handle);
    for (auto& [id, v] : tvVoices_)
        sound_->stop(v.handle);
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
    const std::optional<StereoEvent> stereo = parseStereoEvent(r.event);
    if (r.kind == Kind::Stop) {
        stopWhere([&](const ObjectVoice& v) { return v.event == r.event; });
        if (stereo)
            stereos_.erase(r.source);
        return;
    }
    ++requests_;
    const SoundPos at = positionOf(machine, r.source);
    // Radios and stereos: "stereo_<station>" plays the songs of that music
    // category from the object (inferred; no track has those names), through
    // the quality's filters and the event's volume/lopass/hipass commands
    // (cTSAudioMusicManager::_HandleStartStereoEvent); a wall speaker
    // ("stereo_wall_speaker") plays what the nearest stereo plays.
    if (stereo && (stereo->wall || sound_->isMusicCategory(stereo->station))) {
        StereoEvent style = *stereo;
        if (style.lopass == 0 && style.hipass == 0) { // the object's own quality (soundinfo), if it has one
            const std::string q = modifiersFor(&machine, r.source, at, {}).quality;
            if (q == "chp" || q == "cheap")
                style.lopass = kCheapLowPass, style.hipass = kCheapHighPass;
        }
        auto& st = stereos_[r.source];
        if (!st.player) {
            st.player = std::make_unique<MusicPlayer>(*sound_);
            st.player->setBackgroundDecoding(game_.hasDevice());
        }
        st.wall = stereo->wall;
        st.style = style;
        st.player->setStyle(style.volume * r.volume, style.lopass, style.hipass);
        if (!stereo->wall)
            st.player->setCategory(stereo->station, at);
        else
            updateWallSpeakers(machine);
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
        updateWallSpeakers(*machine);
    }
    // Animation events between the last update and now.
    // (Cues that are due start here as SimActor::update would start them, so
    // that runs that pose the actors less often than this is called fire them.)
    for (const auto& s : sims_)
        s.actor->startCues(seconds);
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
    if (const char* h = std::getenv("OS2_SOUND_HOUR")) // test aid: the ambience at that hour
        st.timeOfDay = float(std::atof(h)) / 24.0f;
    st.zoom = l.zoom;
    // Partitions are areas in the game (LotPartitionSize 3 tiles): iswater and
    // istombstone hold when such a place is within the radius heard around the
    // camera; isflora stays lot-wide (the trees' partition is not tracked).
    const float heard = sound_->lotAmbienceRadius(l.zoom);
    if (machine && seconds - tombstonesAt_ >= 1.0) {
        tombstonesAt_ = seconds;
        tombstones_.clear();
        for (const auto& [id, o] : machine->objects()) {
            if (!o.position.inWorld())
                continue;
            auto g = tombstoneGroups_.find(o.group);
            if (g == tombstoneGroups_.end())
                g = tombstoneGroups_.emplace(o.group, vm::soundInfo(*machine, o)[3] == "object:tombstone").first;
            if (g->second)
                tombstones_.push_back({o.position.x, o.position.y});
        }
    }
    bool tomb = false;
    for (const auto& [tx, ty] : tombstones_)
        tomb = tomb || std::hypot(tx - l.x, ty - l.y) <= heard;
    st.partitions = {{"isflora", true}, {"iswater", surfaces_.waterNear(l.x, l.y, heard)}, {"istombstone", tomb}};
    st.outsideness = surfaces_.outsideness(l.x, l.y, heard);
    st.inside = surfaces_.indoors(int(std::floor(l.z / kStoryHeight + 0.01f)), l.x, l.y);
    ambience_->update(st, dt);
    lastTimeOfDay_ = st.timeOfDay;
    game_.update(seconds);
    // The stereos belong to the lot's music state: paused in Buy/Build mode.
    const bool lotState = game_.music() == liveMusic_;
    for (auto& [id, s] : stereos_) {
        if (s.player->paused() == lotState)
            s.player->setPaused(!lotState);
        s.player->update();
    }
    last_ = seconds;
    started_ = true;
}

// A wall speaker plays the station of the nearest stereo (same story first;
// inferred from _FindWallSpeakerSource's name: the binary finds the source
// stereo of the speaker), in step with it, and goes quiet with it.
void LotAudio::updateWallSpeakers(vm::VM& machine) {
    for (auto& [id, w] : stereos_) {
        if (!w.wall)
            continue;
        const SoundPos at = positionOf(machine, id);
        const Stereo* best = nullptr;
        float bestD = 1e30f;
        for (const auto& [sid, src] : stereos_) {
            if (src.wall || src.player->category().empty())
                continue;
            const SoundPos p = positionOf(machine, sid);
            const float d = std::hypot(p.x - at.x, p.y - at.y) + (std::fabs(p.z - at.z) > 1 ? 1000.0f : 0.0f);
            if (d < bestD)
                bestD = d, best = &src;
        }
        if (best) {
            // The quality's filters and volume come from the source; the speaker's event adds its own.
            const StereoEvent& s = best->style;
            w.player->setStyle(w.style.volume * s.volume, w.style.lopass ? w.style.lopass : s.lopass,
                               w.style.hipass ? w.style.hipass : s.hipass);
            w.player->setCategory(best->player->category(), at);
        } else {
            w.player->setCategory("", at);
        }
    }
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
    if (!game_.musicLog().empty()) {
        out += "; music:";
        for (const auto& m : game_.musicLog())
            out += " " + m;
    }
    return out;
}

} // namespace os2
