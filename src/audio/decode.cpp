#include "decode.hpp"

#include "formats/binreader.hpp"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif

#include <algorithm>
#include <cstring>
#include <memory>

namespace os2 {

#if defined(__APPLE__)

namespace {

struct MemFile {
    std::span<const uint8_t> data;
};

OSStatus memRead(void* user, SInt64 pos, UInt32 count, void* buf, UInt32* actual) {
    const auto* f = static_cast<MemFile*>(user);
    if (pos < 0 || size_t(pos) > f->data.size()) {
        *actual = 0;
        return kAudioFilePositionError;
    }
    const size_t n = std::min<size_t>(count, f->data.size() - size_t(pos));
    std::memcpy(buf, f->data.data() + pos, n);
    *actual = UInt32(n);
    return noErr;
}

SInt64 memSize(void* user) { return SInt64(static_cast<MemFile*>(user)->data.size()); }

void check(OSStatus st, const char* what) {
    if (st != noErr)
        throw FormatError(std::string("MP3: ") + what + " failed (OSStatus " + std::to_string(int(st)) + ")");
}

} // namespace

bool haveMp3Decoder() { return true; }

Pcm decodeMp3(std::span<const uint8_t> data) {
    MemFile mem{data};
    AudioFileID file = nullptr;
    check(AudioFileOpenWithCallbacks(&mem, memRead, nullptr, memSize, nullptr, kAudioFileMP3Type, &file),
          "AudioFileOpenWithCallbacks");
    std::unique_ptr<OpaqueAudioFileID, OSStatus (*)(AudioFileID)> fileGuard(file, AudioFileClose);

    ExtAudioFileRef ext = nullptr;
    check(ExtAudioFileWrapAudioFileID(file, false, &ext), "ExtAudioFileWrapAudioFileID");
    std::unique_ptr<OpaqueExtAudioFile, OSStatus (*)(ExtAudioFileRef)> extGuard(ext, ExtAudioFileDispose);

    AudioStreamBasicDescription in{};
    UInt32 size = sizeof in;
    check(ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &size, &in), "FileDataFormat");

    Pcm pcm;
    pcm.sampleRate = uint32_t(in.mSampleRate);
    pcm.channels = uint16_t(in.mChannelsPerFrame);
    if (pcm.channels == 0 || pcm.sampleRate == 0)
        throw FormatError("MP3: no audio format");

    AudioStreamBasicDescription out{};
    out.mSampleRate = in.mSampleRate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = pcm.channels;
    out.mBitsPerChannel = 16;
    out.mBytesPerFrame = 2 * pcm.channels;
    out.mFramesPerPacket = 1;
    out.mBytesPerPacket = out.mBytesPerFrame;
    check(ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out),
          "ClientDataFormat");

    SInt64 frames = 0;
    size = sizeof frames;
    if (ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &size, &frames) == noErr && frames > 0)
        pcm.samples.reserve(size_t(frames) * pcm.channels);

    std::vector<int16_t> chunk(size_t(4096) * pcm.channels);
    for (;;) {
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = pcm.channels;
        list.mBuffers[0].mDataByteSize = UInt32(chunk.size() * 2);
        list.mBuffers[0].mData = chunk.data();
        UInt32 n = 4096;
        check(ExtAudioFileRead(ext, &n, &list), "ExtAudioFileRead");
        if (n == 0)
            break;
        pcm.samples.insert(pcm.samples.end(), chunk.begin(), chunk.begin() + ptrdiff_t(n) * pcm.channels);
    }
    return pcm;
}

#else

bool haveMp3Decoder() { return false; }

Pcm decodeMp3(std::span<const uint8_t>) { throw FormatError("MP3: no decoder on this platform"); }

#endif

Pcm decodeSound(std::span<const uint8_t> data) {
    switch (const auto f = detectAudioFormat(data)) {
    case AudioFormat::Xa: return decodeXa(data);
    case AudioFormat::Spx: return decodeSpx(data);
    case AudioFormat::Mp3: return decodeMp3(data);
    case AudioFormat::Wav: return decodeWav(data);
    default: throw FormatError(std::string("not a sample (") + audioFormatName(f) + ")");
    }
}

} // namespace os2
