#include "player.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace os2 {

namespace {

constexpr int kBuffers = 3;
constexpr double kBufferSeconds = 0.25;

} // namespace

struct SoundPlayer::Impl {
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kBuffers] = {};
    Pcm pcm;
    size_t nextFrame = 0;   // first frame not yet enqueued
    size_t pending = 0;     // buffers enqueued and not yet returned
    bool running = false;
    mutable std::mutex mutex;
    std::condition_variable done;

    // Fills `buf` with the next frames; false when nothing is left.
    bool fill(AudioQueueBufferRef buf) {
        const size_t ch = pcm.channels;
        const size_t capacity = buf->mAudioDataBytesCapacity / (2 * ch);
        const size_t n = std::min(capacity, pcm.frames() - nextFrame);
        if (n == 0)
            return false;
        std::memcpy(buf->mAudioData, pcm.samples.data() + nextFrame * ch, n * ch * 2);
        buf->mAudioDataByteSize = UInt32(n * ch * 2);
        nextFrame += n;
        return true;
    }

    static void output(void* user, AudioQueueRef q, AudioQueueBufferRef buf) {
        auto* self = static_cast<Impl*>(user);
        std::lock_guard lock(self->mutex);
        if (self->running && self->fill(buf) && AudioQueueEnqueueBuffer(q, buf, 0, nullptr) == noErr)
            return;
        if (self->pending > 0 && --self->pending == 0 && self->running)
            AudioQueueStop(q, false); // play out what is queued, then IsRunning goes to 0
    }

    static void runningChanged(void* user, AudioQueueRef q, AudioQueuePropertyID) {
        auto* self = static_cast<Impl*>(user);
        UInt32 isRunning = 0, size = sizeof isRunning;
        AudioQueueGetProperty(q, kAudioQueueProperty_IsRunning, &isRunning, &size);
        if (!isRunning) {
            std::lock_guard lock(self->mutex);
            self->running = false;
            self->done.notify_all();
        }
    }

    void dispose() {
        if (!queue)
            return;
        AudioQueueStop(queue, true);
        AudioQueueDispose(queue, true);
        queue = nullptr;
        std::lock_guard lock(mutex);
        running = false;
        pending = 0;
        done.notify_all();
    }
};

SoundPlayer::SoundPlayer() : impl_(std::make_unique<Impl>()) {}

SoundPlayer::~SoundPlayer() { impl_->dispose(); }

void SoundPlayer::play(Pcm pcm, float volume) {
    impl_->dispose();
    if (pcm.channels == 0 || pcm.sampleRate == 0 || pcm.samples.empty())
        return;
    Impl& s = *impl_;
    s.pcm = std::move(pcm);
    s.nextFrame = 0;

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = s.pcm.sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = s.pcm.channels;
    fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 2 * s.pcm.channels;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    auto check = [&](OSStatus st, const char* what) {
        if (st != noErr) {
            s.dispose();
            throw std::runtime_error(std::string("audio: ") + what + " failed (OSStatus " + std::to_string(int(st)) + ")");
        }
    };
    check(AudioQueueNewOutput(&fmt, Impl::output, &s, nullptr, nullptr, 0, &s.queue), "AudioQueueNewOutput");
    check(AudioQueueAddPropertyListener(s.queue, kAudioQueueProperty_IsRunning, Impl::runningChanged, &s),
          "AudioQueueAddPropertyListener");
    AudioQueueSetParameter(s.queue, kAudioQueueParam_Volume, volume);

    const UInt32 bytes = UInt32(std::max(1.0, kBufferSeconds * s.pcm.sampleRate)) * fmt.mBytesPerFrame;
    {
        std::lock_guard lock(s.mutex);
        s.running = true;
        s.pending = 0;
        for (auto& buf : s.buffers) {
            check(AudioQueueAllocateBuffer(s.queue, bytes, &buf), "AudioQueueAllocateBuffer");
            if (s.fill(buf)) {
                check(AudioQueueEnqueueBuffer(s.queue, buf, 0, nullptr), "AudioQueueEnqueueBuffer");
                ++s.pending;
            }
        }
    }
    check(AudioQueueStart(s.queue, nullptr), "AudioQueueStart");
}

void SoundPlayer::stop() { impl_->dispose(); }

bool SoundPlayer::playing() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->running;
}

bool SoundPlayer::wait(double timeoutSeconds) {
    std::unique_lock lock(impl_->mutex);
    return impl_->done.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), [&] { return !impl_->running; });
}

size_t SoundPlayer::framesQueued() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->nextFrame;
}

} // namespace os2
