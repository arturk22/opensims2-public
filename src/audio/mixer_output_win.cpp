// AudioOutput: WASAPI shared mode pulling stereo int16 from an AudioMixer.
// The engine converts to the device rate (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM),
// which is what AudioQueue does with the mixer's rate on macOS.

#include "mixer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <future>
#include <thread>

namespace os2 {

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

} // namespace

struct AudioOutput::Impl {
    AudioMixer& mixer;
    std::atomic<bool> running{false};
    std::thread thread;
    HANDLE wake = nullptr;

    explicit Impl(AudioMixer& m) : mixer(m) {}

    void join() {
        running = false;
        if (wake)
            SetEvent(wake);
        if (thread.joinable())
            thread.join();
        wake = nullptr;
    }
};

AudioOutput::AudioOutput(AudioMixer& mixer) : impl_(std::make_unique<Impl>(mixer)) {}

AudioOutput::~AudioOutput() { impl_->join(); }

bool AudioOutput::start() {
    Impl& slot = *impl_;
    if (slot.running)
        return true;
    slot.join();
    std::promise<bool> ready;
    std::future<bool> done = ready.get_future();
    slot.thread = std::thread([&slot, ready = std::move(ready)]() mutable {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Every COM pointer below is released before CoUninitialize runs:
        // releasing one after it is what crashes on a machine with no audio
        // device (mmdevapi tore down with the apartment).
        struct Uninit {
            HRESULT co;
            ~Uninit() {
                if (SUCCEEDED(co))
                    CoUninitialize();
            }
        } uncom{co};
        IMMDeviceEnumerator* rawEnum = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&rawEnum));
        ComPtr<IMMDeviceEnumerator> enumerator(rawEnum);
        if (FAILED(hr) || !enumerator) {
            ready.set_value(false);
            return;
        }
        IMMDevice* rawDevice = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &rawDevice);
        ComPtr<IMMDevice> device(rawDevice);
        if (FAILED(hr) || !device) {
            ready.set_value(false);
            return;
        }
        auto activate = [&] {
            IAudioClient* raw = nullptr;
            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&raw));
            return ComPtr<IAudioClient>(raw);
        };
        ComPtr<IAudioClient> client = activate();
        if (!client) {
            ready.set_value(false);
            return;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = slot.mixer.sampleRate();
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        REFERENCE_TIME period = 0;
        client->GetDevicePeriod(&period, nullptr);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        REFERENCE_TIME duration = period > 0 ? period * 4 : 700000;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned = 0;
            client->GetBufferSize(&aligned);
            client.reset();
            client = activate();
            if (client && aligned && format.nSamplesPerSec)
                duration = REFERENCE_TIME((10000000.0 * aligned / format.nSamplesPerSec) + 0.5);
            hr = client ? client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &format, nullptr) : E_FAIL;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr) || !event || FAILED(client->SetEventHandle(event))) {
            if (event)
                CloseHandle(event);
            ready.set_value(false);
            return;
        }
        IAudioRenderClient* rawRender = nullptr;
        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rawRender));
        ComPtr<IAudioRenderClient> render(rawRender);
        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || !render || bufferFrames == 0 || FAILED(client->Start())) {
            CloseHandle(event);
            ready.set_value(false);
            return;
        }
        slot.wake = event;
        slot.running = true;
        ready.set_value(true);
        while (slot.running.load()) {
            WaitForSingleObject(event, 200);
            if (!slot.running.load())
                break;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding >= bufferFrames)
                continue;
            const UINT32 need = bufferFrames - padding;
            BYTE* dest = nullptr;
            if (FAILED(render->GetBuffer(need, &dest)) || !dest)
                continue;
            slot.mixer.render(reinterpret_cast<int16_t*>(dest), need);
            render->ReleaseBuffer(need, 0);
        }
        client->Stop();
        slot.wake = nullptr;
        CloseHandle(event);
    });
    return done.get();
}

void AudioOutput::stop() { impl_->join(); }

bool AudioOutput::running() const { return impl_->running; }

} // namespace os2
