// See WasapiMicCapture.h. Standard shared-mode capture of the default
// communications input endpoint. Structure mirrors WasapiLoopbackCapture, minus
// the process-loopback activation: here we take the default mic via the device
// enumerator and let WASAPI AUTOCONVERTPCM give us a fixed 48 kHz float32 stereo
// stream regardless of the mic's native format.

#include "media/audio/windows/WasapiMicCapture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace mdt {
namespace {

constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;  // informational; 0 is passed with autoconvert

std::string HrToString(const char* what, HRESULT hr) {
    std::ostringstream ss;
    ss << what << " hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}

inline int16_t FloatToS16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

}  // namespace

class WasapiMicCapture::Impl {
 public:
    Impl() = default;
    ~Impl() { StopAndJoin(); }

    StartResult Start(FrameSink sink) {
        StopAndJoin();
        sink_ = std::move(sink);
        last_error_.clear();
        stop_.store(false);
        thread_ = std::thread([this] { CaptureLoop(); });
        std::unique_lock<std::mutex> lk(start_mu_);
        start_cv_.wait(lk, [this] { return start_ready_; });
        return start_result_;
    }

    void StopAndJoin() {
        if (!thread_.joinable()) return;
        stop_.store(true);
        if (frame_ready_) SetEvent(frame_ready_);
        thread_.join();
        start_ready_ = false;
    }

    std::string last_error() const { return last_error_; }

 private:
    void SignalStart(StartResult result, std::string err = {}) {
        {
            std::lock_guard<std::mutex> lk(start_mu_);
            start_result_ = result;
            last_error_ = std::move(err);
            start_ready_ = true;
        }
        start_cv_.notify_one();
    }

    void CaptureLoop() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) { SignalStart(StartResult::kOsError, HrToString("CoInitializeEx", hr)); return; }
        struct CoGuard { ~CoGuard() { CoUninitialize(); } } co_guard;

        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator));
        if (FAILED(hr) || !enumerator) { SignalStart(StartResult::kOsError, HrToString("MMDeviceEnumerator", hr)); return; }

        Microsoft::WRL::ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (FAILED(hr) || !device) { SignalStart(StartResult::kNoDevice, HrToString("GetDefaultAudioEndpoint(eCapture)", hr)); return; }

        Microsoft::WRL::ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr) || !client) { SignalStart(StartResult::kOsError, HrToString("IMMDevice::Activate", hr)); return; }

        // 48 kHz float32 stereo — matched to the loopback/output format so the
        // ADM can feed both into one StreamConfig without resampling. AUTOCONVERTPCM
        // lets WASAPI up/down-convert from whatever the mic's native format is.
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = kFrameChannels;
        fmt.Format.nSamplesPerSec = kFrameSampleRate;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = fmt.Format.nChannels * fmt.Format.wBitsPerSample / 8;
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        // AUTOCONVERTPCM requires hnsBufferDuration and hnsPeriodicity to be 0
        // (else AUDCLNT_E_INVALID_STREAM_FLAG). No LOOPBACK flag here — this is a
        // real capture endpoint, not the process-loopback virtual device.
        const DWORD init_flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, init_flags, 0, 0,
                                reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) { SignalStart(StartResult::kFormatRejected, HrToString("IAudioClient::Initialize", hr)); return; }

        frame_ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(frame_ready_);
        if (FAILED(hr)) { SignalStart(StartResult::kOsError, HrToString("SetEventHandle", hr)); return; }

        Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) { SignalStart(StartResult::kOsError, HrToString("GetService", hr)); return; }

        hr = client->Start();
        if (FAILED(hr)) { SignalStart(StartResult::kOsError, HrToString("IAudioClient::Start", hr)); return; }
        struct StopGuard { IAudioClient* c; ~StopGuard() { if (c) c->Stop(); } } stop_guard{client.Get()};

        SignalStart(StartResult::kOk);

        std::vector<int16_t> pending;
        pending.reserve(kFrameSamplesPerChannel * kFrameChannels * 2);

        while (!stop_.load()) {
            WaitForSingleObject(frame_ready_, 100);
            if (stop_.load()) break;

            UINT32 packet_frames = 0;
            hr = capture->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) break;

            while (packet_frames > 0 && !stop_.load()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;

                const size_t need_samples = static_cast<size_t>(frames) * kFrameChannels;
                const size_t old = pending.size();
                pending.resize(old + need_samples);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(pending.data() + old, 0, need_samples * sizeof(int16_t));
                } else {
                    const float* src = reinterpret_cast<const float*>(data);
                    for (size_t i = 0; i < need_samples; ++i) pending[old + i] = FloatToS16(src[i]);
                }

                capture->ReleaseBuffer(frames);

                const size_t frame_len = static_cast<size_t>(kFrameSamplesPerChannel) * kFrameChannels;
                size_t offset = 0;
                while (pending.size() - offset >= frame_len && !stop_.load()) {
                    sink_(pending.data() + offset);
                    offset += frame_len;
                }
                if (offset > 0) pending.erase(pending.begin(), pending.begin() + offset);

                hr = capture->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) break;
            }
        }

        if (frame_ready_) { CloseHandle(frame_ready_); frame_ready_ = nullptr; }
    }

    FrameSink sink_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    HANDLE frame_ready_ = nullptr;

    std::mutex start_mu_;
    std::condition_variable start_cv_;
    bool start_ready_ = false;
    StartResult start_result_ = StartResult::kOsError;
    std::string last_error_;
};

WasapiMicCapture::WasapiMicCapture() : impl_(std::make_unique<Impl>()) {}
WasapiMicCapture::~WasapiMicCapture() = default;
WasapiMicCapture::StartResult WasapiMicCapture::Start(FrameSink sink) { return impl_->Start(std::move(sink)); }
void WasapiMicCapture::StopAndJoin() { impl_->StopAndJoin(); }
std::string WasapiMicCapture::last_error() const { return impl_->last_error(); }

}  // namespace mdt
