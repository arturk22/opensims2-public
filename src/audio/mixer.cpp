#include "mixer.hpp"

#include <algorithm>
#include <cmath>

namespace os2 {

const char* soundGroupLabel(SoundGroup g) {
    switch (g) {
    case SoundGroup::Sfx: return "sfx";
    case SoundGroup::Vox: return "vox";
    case SoundGroup::Music: return "music";
    case SoundGroup::Ambience: return "ambience";
    case SoundGroup::Ui: return "ui";
    default: return "?";
    }
}

AudioMixer::AudioMixer(uint32_t sampleRate) : rate_(sampleRate ? sampleRate : 44100) { groupVolume_.fill(1); }

AudioMixer::Voice* AudioMixer::find(VoiceId id) {
    for (auto& v : voices_)
        if (v.id == id)
            return &v;
    return nullptr;
}

const AudioMixer::Voice* AudioMixer::find(VoiceId id) const {
    for (auto& v : voices_)
        if (v.id == id)
            return &v;
    return nullptr;
}

AudioMixer::VoiceId AudioMixer::play(std::shared_ptr<const Pcm> pcm, const Params& params) {
    if (!pcm || pcm->channels == 0 || pcm->sampleRate == 0 || pcm->frames() == 0)
        return 0;
    std::lock_guard lock(mutex_);
    Voice v;
    v.id = next_++;
    if (next_ == 0)
        next_ = 1;
    v.params = params;
    v.step = double(pcm->sampleRate) / rate_ * std::max(0.01f, params.pitch);
    v.gain = v.target = params.volume;
    v.delayFrames = int64_t(std::max(0.0, params.startDelay) * rate_);
    v.position = std::max(0.0, params.startAt) * pcm->sampleRate;
    if (v.position >= double(pcm->frames()))
        return 0;
    v.pcm = std::move(pcm);
    voices_.push_back(std::move(v));
    return voices_.back().id;
}

void AudioMixer::stop(VoiceId id) {
    std::lock_guard lock(mutex_);
    std::erase_if(voices_, [&](const Voice& v) { return v.id == id; });
}

void AudioMixer::stopAll() {
    std::lock_guard lock(mutex_);
    voices_.clear();
}

void AudioMixer::stopGroup(SoundGroup g) {
    std::lock_guard lock(mutex_);
    std::erase_if(voices_, [&](const Voice& v) { return v.params.group == g; });
}

void AudioMixer::fade(VoiceId id, float volume, double seconds, bool stopAtEnd) {
    std::lock_guard lock(mutex_);
    Voice* v = find(id);
    if (!v)
        return;
    v->target = std::max(0.0f, volume);
    v->params.volume = v->target;
    v->stopAtEnd = stopAtEnd;
    const double frames = seconds * rate_;
    if (frames < 1) {
        v->gain = v->target;
        v->gainStep = 0;
        if (stopAtEnd && v->target <= 0)
            std::erase_if(voices_, [&](const Voice& x) { return x.id == id; });
    } else {
        v->gainStep = float((v->target - v->gain) / frames);
    }
}

void AudioMixer::setPan(VoiceId id, float pan) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(id))
        v->params.pan = std::clamp(pan, -1.0f, 1.0f);
}

bool AudioMixer::playing(VoiceId id) const {
    std::lock_guard lock(mutex_);
    return find(id) != nullptr;
}

size_t AudioMixer::voiceCount() const {
    std::lock_guard lock(mutex_);
    return voices_.size();
}

void AudioMixer::setGroupVolume(SoundGroup g, float volume) {
    std::lock_guard lock(mutex_);
    if (g < SoundGroup::Count)
        groupVolume_[size_t(g)] = std::max(0.0f, volume);
}

float AudioMixer::groupVolume(SoundGroup g) const {
    std::lock_guard lock(mutex_);
    return g < SoundGroup::Count ? groupVolume_[size_t(g)] : 0;
}

void AudioMixer::setMasterVolume(float volume) {
    std::lock_guard lock(mutex_);
    master_ = std::max(0.0f, volume);
}

void AudioMixer::setMuted(bool muted) {
    std::lock_guard lock(mutex_);
    muted_ = muted;
}

bool AudioMixer::muted() const {
    std::lock_guard lock(mutex_);
    return muted_;
}

uint64_t AudioMixer::framesRendered() const {
    std::lock_guard lock(mutex_);
    return rendered_;
}

void AudioMixer::renderFloat(float* out, size_t frames) {
    std::lock_guard lock(mutex_);
    std::fill(out, out + 2 * frames, 0.0f);
    for (auto& v : voices_) {
        const Pcm& p = *v.pcm;
        const size_t n = p.frames(), ch = p.channels;
        const float group = groupVolume_[size_t(v.params.group)];
        // Constant-power pan.
        const float angle = (std::clamp(v.params.pan, -1.0f, 1.0f) + 1) * 0.25f * float(M_PI);
        const float panL = std::cos(angle) * float(M_SQRT2), panR = std::sin(angle) * float(M_SQRT2);
        size_t i = 0;
        if (v.delayFrames > 0) {
            const size_t skip = size_t(std::min<int64_t>(v.delayFrames, int64_t(frames)));
            v.delayFrames -= int64_t(skip);
            i = skip;
        }
        for (; i < frames; ++i) {
            if (v.position >= double(n)) {
                if (!v.params.loop)
                    break;
                v.position = std::fmod(v.position, double(n));
            }
            const size_t a = size_t(v.position);
            const size_t b = a + 1 < n ? a + 1 : (v.params.loop ? 0 : a);
            const float t = float(v.position - double(a));
            float l, r;
            if (ch == 1) {
                l = r = (p.samples[a] + (p.samples[b] - p.samples[a]) * t) / 32768.0f;
            } else {
                l = (p.samples[a * ch] + (p.samples[b * ch] - p.samples[a * ch]) * t) / 32768.0f;
                r = (p.samples[a * ch + 1] + (p.samples[b * ch + 1] - p.samples[a * ch + 1]) * t) / 32768.0f;
            }
            const float g = v.gain * group;
            out[2 * i] += l * g * panL;
            out[2 * i + 1] += r * g * panR;
            if (v.gainStep != 0) {
                v.gain += v.gainStep;
                if ((v.gainStep > 0 && v.gain >= v.target) || (v.gainStep < 0 && v.gain <= v.target))
                    v.gain = v.target, v.gainStep = 0;
            }
            v.position += v.step;
        }
    }
    std::erase_if(voices_, [](const Voice& v) {
        return (!v.params.loop && v.position >= double(v.pcm->frames())) ||
               (v.stopAtEnd && v.gainStep == 0 && v.gain <= 0);
    });
    const float m = muted_ ? 0.0f : master_;
    for (size_t k = 0; k < 2 * frames; ++k)
        out[k] *= m;
    rendered_ += frames;
}

void AudioMixer::render(int16_t* out, size_t frames) {
    scratch_.resize(2 * frames);
    // scratch_ is only touched here; render runs on one thread at a time.
    renderFloat(scratch_.data(), frames);
    for (size_t k = 0; k < 2 * frames; ++k) {
        // Soft clip above -1 dBFS so a pile-up of voices does not wrap.
        float s = scratch_[k];
        const float a = std::fabs(s);
        if (a > 0.89f)
            s = std::copysign(0.89f + (1 - 0.89f) * std::tanh((a - 0.89f) / (1 - 0.89f)), s);
        out[k] = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
    }
}

} // namespace os2
