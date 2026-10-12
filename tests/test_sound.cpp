// The mixer, FSC scores, curves and the VM's sound primitives (no game data).
#include "audio/mixer.hpp"
#include "audio/sound_system.hpp"
#include "vm/sound.hpp"
#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace os2;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static std::shared_ptr<const Pcm> tone(uint32_t rate, size_t frames, int16_t value) {
    auto p = std::make_shared<Pcm>();
    p->sampleRate = rate;
    p->channels = 1;
    p->samples.assign(frames, value);
    return p;
}

static void testMixer() {
    AudioMixer m(44100);
    // A 22050 Hz sound of 100 frames lasts 200 output frames.
    auto id = m.play(tone(22050, 100, 16384), {});
    check(id != 0 && m.voiceCount() == 1, "voice started");
    std::vector<float> out(2 * 150);
    m.renderFloat(out.data(), 150);
    check(std::fabs(out[0] - 0.5f) < 0.01f && std::fabs(out[1] - 0.5f) < 0.01f, "centred mono at full volume");
    check(m.playing(id), "still playing after 150 of 200 frames");
    m.renderFloat(out.data(), 150);
    check(!m.playing(id) && out[2 * 100] == 0.0f, "ends after 200 frames (resampled 2x)");

    // Groups, pan and mute.
    m.setGroupVolume(SoundGroup::Music, 0.5f);
    id = m.play(tone(44100, 1000, 16384), {.group = SoundGroup::Music, .pan = -1});
    m.renderFloat(out.data(), 10);
    check(std::fabs(out[0] - 0.5f * 0.5f * std::sqrt(2.0f)) < 0.01f && std::fabs(out[1]) < 0.001f,
          "hard left, group at half");
    m.setMuted(true);
    m.renderFloat(out.data(), 10);
    check(out[0] == 0 && m.playing(id), "muted: silent but advancing");
    m.setMuted(false);
    m.stopGroup(SoundGroup::Music);
    check(m.voiceCount() == 0, "stop group");

    // Loops wrap; a fade to zero with stop removes the voice.
    id = m.play(tone(44100, 10, 1000), {.loop = true});
    m.renderFloat(out.data(), 100);
    check(m.playing(id) && out[2 * 99] > 0, "loop keeps playing");
    m.fade(id, 0, 50.0 / 44100, true);
    m.renderFloat(out.data(), 100);
    check(!m.playing(id), "faded out and stopped");

    // int16 output clips softly instead of wrapping.
    m.play(tone(44100, 100, 30000), {});
    m.play(tone(44100, 100, 30000), {});
    std::vector<int16_t> pcm(2 * 10);
    m.render(pcm.data(), 10);
    check(pcm[0] > 29000 && pcm[0] <= 32767, "two loud voices clip below full scale");
}

static void testPauseAndStartAt() {
    AudioMixer m(44100);
    auto id = m.play(tone(44100, 100, 16384), {});
    std::vector<float> out(2 * 50);
    m.renderFloat(out.data(), 50);
    m.setPaused(id, true);
    m.renderFloat(out.data(), 50);
    check(out[0] == 0.0f && m.playing(id), "a paused voice is silent and stays");
    m.setPaused(id, false);
    m.renderFloat(out.data(), 50);
    check(out[0] > 0.4f && !m.playing(id), "resumed where it was: the last 50 frames");
    AudioMixer::Params late;
    late.startAt = 90.0 / 44100;
    auto id2 = m.play(tone(44100, 100, 16384), late);
    m.renderFloat(out.data(), 20);
    check(!m.playing(id2) && out[2 * 9] > 0.4f && out[2 * 10] == 0.0f, "startAt skips into the sound");
    check(m.play(tone(44100, 100, 16384), {.startAt = 1.0}) == 0, "starting past the end plays nothing");
}

static void testCurvesAndFsc() {
    const std::vector<float> day{0, 0, 0.49f, 0, 0.5f, 1, 0.74f, 1, 0.75f, 0};
    check(evalCurve(day, 0.3f) == 0 && evalCurve(day, 0.6f) == 1 && evalCurve(day, 0.9f) == 0, "curveisday");
    check(std::fabs(evalCurve({0, 0.2f, 0.7f, 1}, 0.35f) - 0.6f) < 1e-5f, "linear between points");
    const char* text = "version 7\r\n#\tvol\tpri\r\n\t763\t0\t1\t2\t8\t6\t145\t4\t3\t4\t2\t2\t0\t0\r\n#\tvol\r\n"
                       "\t835\t1\t512\t512\t1\t0\t0\t0\t0\t8\t0\t0\t10\t-3\t2\t0\t0\t0\tbrdmrn10i.xa\r\n"
                       "\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\tNONE\r\n"
                       "\t565\t1\t100\t512\t1\t0\t0\t0\t0\t8\t0\t0\t7\t-3\t0\t0\t0\t0\tdoggie2.xa\t(4,5)\t(5,5)\r\n";
    const FscScore s = parseFsc(text);
    check(s.masterVolume == 763 && s.tempo == 145, "FSC header");
    check(s.notes.size() == 3 && s.notes[0].file == "brdmrn10i" && s.notes[0].loop == 8 && s.notes[0].prob == 10,
          "FSC notes, .xa stripped");
    check(s.notes[1].file == "NONE" && s.notes[2].pan == 100, "rest and pan");
    check(s.jumpPoints == std::vector<size_t>({0, 2}), "random jump points at rand notes");
}

namespace vmt {
using namespace os2::vm;
using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, E{Kind::Error, 0};
static Target go(uint16_t i) { return {Kind::Instruction, i}; }
static Behavior::Instruction instr(uint16_t op, Target t, std::initializer_list<uint8_t> operands) {
    Behavior::Instruction in;
    in.opcode = op;
    in.onTrue = t;
    in.onFalse = E;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}
static Behavior tree(std::vector<Behavior::Instruction> code) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.instructions = std::move(code);
    return b;
}
static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

static void testPrimitives() {
    constexpr uint32_t kGroup = 0x7F000001, kSemi = 0x7F000002;
    CodeLibrary code;
    VM vm(code);
    code.add(kGroup, kSoundIdTable, strings({"clock_alarm_lp_exp", "clock_ding"}));
    code.add(kSemi, kSoundIdTable, strings({"stove_fry_lp", "stove_simmer_lp"}));
    code.add(kGlobalGroup, kSoundIdTable, strings({"sting_genericmemory_pos"}));
    code.add(kGroup, kSoundInfoTable, strings({"destination", "plastic", "exp"}));
    Object& o = vm.createObject(kGroup);
    o.semiGroup = kSemi;
    // play 1 (private), 20001 (semi-global, loops: "_lp"), 10000 (global), the
    // same loop again (ignored), stop 20001, stop all.
    code.add(kGroup, 0x1000,
             tree({instr(0x17, go(1), {1, 0, 0, 0, 0x02}), instr(0x17, go(2), {0x21, 0x4E, 0, 0, 0x02}),
                   instr(0x17, go(3), {0x10, 0x27, 0, 0, 0}), instr(0x17, go(4), {0x21, 0x4E, 0, 0, 0x02}),
                   instr(0x17, go(5), {0x21, 0x4E, 0, 0, 0x42}), instr(0x17, go(6), {9, 0, 0, 0, 0}),
                   instr(0x30, T, {0})}));
    Thread t = vm.start(o.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished, "sound tree runs to the end");
    check(vm.soundRequests.size() == 5, "five requests (the repeated loop is not sent)");
    if (vm.soundRequests.size() == 5) {
        check(vm.soundRequests[0].event == "clock_ding" && !vm.soundRequests[0].loop, "private table");
        check(vm.soundRequests[1].event == "stove_simmer_lp" && vm.soundRequests[1].loop, "semi-global, loops");
        check(vm.soundRequests[2].event == "sting_genericmemory_pos", "global table");
        check(vm.soundRequests[3].kind == SoundRequest::Kind::Stop && vm.soundRequests[3].event == "stove_simmer_lp",
              "stop flag 0x40");
        check(vm.soundRequests[4].kind == SoundRequest::Kind::StopAll && vm.soundRequests[4].source == o.id,
              "stop all");
    }
    check(vm.missingSounds[9] == 1, "missing ID counted, tree goes on");
    const auto info = soundInfo(vm, o);
    check(info[0].empty() && info[1] == "plastic" && info[2] == "exp", "soundinfo without placeholders");
}

static void testVoxSuffix() {
    Object sim;
    sim.person.emplace();
    auto& d = sim.person->data;
    auto at = [&](int age, int gender, int voice, int ghost) {
        d[58] = int16_t(age), d[65] = int16_t(gender), d[88] = int16_t(voice), d[68] = int16_t(ghost);
        return voxSuffix(sim);
    };
    check(at(0x13, 0, 0, 0) == "ama" && at(0x13, 1, 1, 0) == "afb" && at(0x13, 0, 3, 0) == "amb", "adults: two voices");
    check(at(0x10, 1, 1, 0) == "tfa" && at(0x33, 0, 1, 0) == "ema", "teen, elder: one voice");
    check(at(3, 1, 0, 0) == "ca" && at(2, 0, 0, 0) == "p" && at(1, 1, 0, 0) == "b", "child, toddler, baby");
    check(at(0x13, 1, 0, 1) == "afg", "ghost voice");
    Object chair;
    check(voxSuffix(chair).empty(), "objects have no voice");
}
} // namespace vmt

static std::shared_ptr<const Pcm> sine(uint32_t rate, double hz, double seconds) {
    auto p = std::make_shared<Pcm>();
    p->sampleRate = rate;
    p->channels = 1;
    for (size_t i = 0; i < size_t(rate * seconds); ++i)
        p->samples.push_back(int16_t(20000 * std::sin(2 * M_PI * hz * double(i) / rate)));
    return p;
}

static float rms(AudioMixer& m, size_t frames) {
    std::vector<float> out(2 * frames);
    m.renderFloat(out.data(), frames);
    double sum = 0;
    for (size_t i = 2 * frames / 2; i < 2 * frames; ++i) // the second half: the filter has settled
        sum += double(out[i]) * out[i];
    return float(std::sqrt(sum / (frames)));
}

static void testFilters() {
    // The cheap quality's band (high-pass 3500, low-pass 8500) passes 5 kHz and cuts 100 Hz and 18 kHz.
    float level[3];
    int k = 0;
    for (double hz : {100.0, 5000.0, 18000.0}) {
        AudioMixer m(44100);
        AudioMixer::Params p;
        p.lopass = kCheapLowPass, p.hipass = kCheapHighPass;
        m.play(sine(44100, hz, 0.2), p);
        level[k++] = rms(m, 8000);
    }
    AudioMixer plain(44100);
    plain.play(sine(44100, 5000, 0.2), {});
    const float ref = rms(plain, 8000);
    check(level[1] > 0.7f * ref, "cheap band keeps 5 kHz");
    check(level[0] < 0.05f * ref, "cheap band cuts 100 Hz");
    check(level[2] < 0.25f * ref, "cheap band cuts 18 kHz");
    AudioMixer m(44100);
    auto id = m.play(sine(44100, 100, 0.2), {});
    m.setFilter(id, 0, kCheapHighPass);
    check(rms(m, 8000) < 0.05f * ref, "setFilter on a playing voice");
    // Ducking is a separate factor on the group.
    AudioMixer d(44100);
    AudioMixer::Params mp;
    mp.group = SoundGroup::Music;
    d.play(tone(44100, 400, 16384), mp);
    d.setGroupDuck(SoundGroup::Music, 0.1f);
    std::vector<float> out(2 * 10);
    d.renderFloat(out.data(), 10);
    check(std::fabs(out[0] - 0.05f) < 0.005f, "duck scales the group (0.5 * 0.1)");
    check(d.groupActive(SoundGroup::Music) && !d.groupActive(SoundGroup::Vox), "groupActive");
    SoundSystem::Dipping dip; // sfx 0.2, vox 0.3, music 0.1, 0.5 s down, 5 s up
    float duck = 1;
    for (int i = 0; i < 2; ++i)
        duck = stepDuck(duck, true, dip.music, dip, 0.25);
    check(std::fabs(duck - 0.1f) < 1e-4f, "music dips to musicdippingvolume in dippingstarttime");
    duck = stepDuck(duck, false, dip.music, dip, 2.5);
    check(duck > 0.5f && duck < 0.6f, "and comes back at half speed after 2.5 s of 5");
    for (int i = 0; i < 4; ++i)
        duck = stepDuck(duck, false, dip.music, dip, 1);
    check(std::fabs(stepDuck(1, true, dip.vox, dip, 1) - 0.3f) < 1e-4f && dip.sfx == 0.2f,
          "voices dip to voxdippingvolume, effects to sfxdippingvolume");
    check(duck == 1, "fully back after dippingstoptime");
    const auto a = parseStereoEvent("stereo_pop");
    check(a && a->station == "pop" && !a->wall && a->lopass == 0, "stereo_pop");
    const auto b = parseStereoEvent("stereo_techno_cheap");
    check(b && b->station == "techno" && b->lopass == kCheapLowPass && b->hipass == kCheapHighPass, "cheap quality");
    const auto c = parseStereoEvent("stereo_wall_speaker");
    check(c && c->wall && c->station.empty(), "wall speaker");
    const auto e = parseStereoEvent("stereo_salsa_lopass5000_hipass200_volume50");
    check(e && e->station == "salsa" && e->lopass == 5000 && e->hipass == 200 && e->volume == 0.5f, "commands");
    check(!parseStereoEvent("sfx_stereo_change_channel"), "not a stereo event");
}

int main() {
    testFilters();
    vmt::testVoxSuffix();
    testMixer();
    testPauseAndStartAt();
    testCurvesAndFsc();
    vmt::testPrimitives();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("sound: all tests passed\n");
    return failures ? 1 : 0;
}
