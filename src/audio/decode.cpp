#include "decode.hpp"

#include "formats/binreader.hpp"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
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

#elif defined(_WIN32)

namespace {

template <class T>
struct ComRelease {
    void operator()(T* p) const noexcept {
        if (p)
            p->Release();
    }
};

template <class T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

void mfCheck(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw FormatError(std::string("MP3: ") + what + " failed (0x" + hexString(uint32_t(hr)) + ")");
}

struct MfSession {
    MfSession() { mfCheck(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET), "MFStartup"); }
};

} // namespace

bool haveMp3Decoder() {
    static const bool ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    return ok;
}

Pcm decodeMp3(std::span<const uint8_t> data) {
    if (data.empty())
        throw FormatError("MP3: empty");
    static MfSession session;

    IStream* rawStream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
    if (!rawStream)
        throw FormatError("MP3: out of memory");
    ComPtr<IStream> stream(rawStream);
    IMFByteStream* rawBytes = nullptr;
    mfCheck(MFCreateMFByteStreamOnStream(stream.get(), &rawBytes), "MFCreateMFByteStreamOnStream");
    ComPtr<IMFByteStream> bytes(rawBytes);
    IMFAttributes* rawAttrs = nullptr;
    mfCheck(bytes->QueryInterface(IID_PPV_ARGS(&rawAttrs)), "byte stream attributes");
    ComPtr<IMFAttributes> attrs(rawAttrs);
    attrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"audio/mpeg");

    IMFSourceReader* rawReader = nullptr;
    mfCheck(MFCreateSourceReaderFromByteStream(bytes.get(), nullptr, &rawReader), "MFCreateSourceReaderFromByteStream");
    ComPtr<IMFSourceReader> reader(rawReader);
    const DWORD streamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(streamIndex, TRUE);

    IMFMediaType* rawNative = nullptr;
    mfCheck(reader->GetCurrentMediaType(streamIndex, &rawNative), "native media type");
    ComPtr<IMFMediaType> native(rawNative);
    UINT32 rate = 0, channels = 0;
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (rate == 0 || channels == 0)
        throw FormatError("MP3: no audio format");

    IMFMediaType* rawOut = nullptr;
    mfCheck(MFCreateMediaType(&rawOut), "MFCreateMediaType");
    ComPtr<IMFMediaType> out(rawOut);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    out->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    mfCheck(reader->SetCurrentMediaType(streamIndex, nullptr, out.get()), "PCM media type");

    Pcm pcm;
    pcm.sampleRate = rate;
    pcm.channels = uint16_t(channels);
    for (;;) {
        DWORD flags = 0;
        IMFSample* rawSample = nullptr;
        mfCheck(reader->ReadSample(streamIndex, 0, nullptr, &flags, nullptr, &rawSample), "ReadSample");
        ComPtr<IMFSample> sample(rawSample);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;
        IMFMediaBuffer* rawBuffer = nullptr;
        mfCheck(sample->ConvertToContiguousBuffer(&rawBuffer), "ConvertToContiguousBuffer");
        ComPtr<IMFMediaBuffer> buffer(rawBuffer);
        BYTE* bytesOut = nullptr;
        DWORD current = 0;
        mfCheck(buffer->Lock(&bytesOut, nullptr, &current), "Lock");
        const auto* samples = reinterpret_cast<const int16_t*>(bytesOut);
        const size_t count = current / sizeof(int16_t);
        pcm.samples.insert(pcm.samples.end(), samples, samples + count);
        buffer->Unlock();
    }
    if (pcm.channels)
        pcm.samples.resize(pcm.samples.size() - pcm.samples.size() % pcm.channels);
    if (pcm.samples.empty())
        throw FormatError("MP3: no samples");
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
