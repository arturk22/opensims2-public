// AudioOutput: an AudioQueue that pulls stereo int16 from an AudioMixer.
#include "mixer.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>

namespace os2 {

namespace {
constexpr int kBuffers = 3;
constexpr uint32_t kBufferFrames = 1024; // ~23 ms at 44.1 kHz each
} // namespace

struct AudioOutput::Impl {
    AudioMixer& mixer;
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kBuffers] = {};
    std::atomic<bool> running{false};

    explicit Impl(AudioMixer& m) : mixer(m) {}

    static void output(void* user, AudioQueueRef q, AudioQueueBufferRef buf) {
        auto* self = static_cast<Impl*>(user);
        if (!self->running)
            return;
        const uint32_t frames = buf->mAudioDataBytesCapacity / 4;
        self->mixer.render(static_cast<int16_t*>(buf->mAudioData), frames);
        buf->mAudioDataByteSize = frames * 4;
        AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    }

    void dispose() {
        running = false;
        if (!queue)
            return;
        AudioQueueStop(queue, true);
        AudioQueueDispose(queue, true);
        queue = nullptr;
    }
};

AudioOutput::AudioOutput(AudioMixer& mixer) : impl_(std::make_unique<Impl>(mixer)) {}

AudioOutput::~AudioOutput() { impl_->dispose(); }

bool AudioOutput::start() {
    Impl& s = *impl_;
    if (s.queue)
        return true;
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = s.mixer.sampleRate();
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&fmt, Impl::output, &s, nullptr, nullptr, 0, &s.queue) != noErr) {
        s.queue = nullptr;
        return false;
    }
    s.running = true;
    for (auto& buf : s.buffers) {
        if (AudioQueueAllocateBuffer(s.queue, kBufferFrames * 4, &buf) != noErr) {
            s.dispose();
            return false;
        }
        Impl::output(&s, s.queue, buf);
    }
    if (AudioQueueStart(s.queue, nullptr) != noErr) {
        s.dispose();
        return false;
    }
    return true;
}

void AudioOutput::stop() { impl_->dispose(); }

bool AudioOutput::running() const { return impl_->running; }

} // namespace os2
