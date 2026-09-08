// See wasapi_loopback_capture.h for the contract and rationale.
//
// The COM/WASAPI body here is the same one the step-0 POC proved on real
// hardware (third_party/wasapi-loopback-poc/loopback_poc.cc); the only
// differences are (a) it accumulates variable-size WASAPI packets into fixed
// 10 ms s16 frames instead of one big vector, and (b) it converts float32 →
// s16 on the fly rather than at the end. Everything about activation, the
// process-loopback params, and the initFlags is identical.

#include "media/audio/windows/WasapiLoopbackCapture.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::FtmBase;

namespace mdt {
namespace {

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
constexpr const wchar_t* kProcessLoopbackDevice = L"VAD\\Process_Loopback";
#else
constexpr const wchar_t* kProcessLoopbackDevice = VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK;
#endif

// 200 ms WASAPI buffer — well above the OS minimum and generous enough that a
// GC pause or a stray syscall in the sink can't cause an overrun. WASAPI still
// wakes us every ~10 ms via the event.
constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;

class ActivateCompletion
    : public RuntimeClass<
          RuntimeClassFlags<ClassicCom>,
          IActivateAudioInterfaceCompletionHandler,
          FtmBase> {
 public:
    HANDLE done = nullptr;
    HRESULT activate_hr = E_FAIL;
    ComPtr<IUnknown> unknown;

    ActivateCompletion() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivateCompletion() { if (done) CloseHandle(done); }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hr = op->GetActivateResult(&activate_hr, &unknown);
        if (FAILED(hr)) activate_hr = hr;
        SetEvent(done);
        return S_OK;
    }
};

std::string HrToString(const char* what, HRESULT hr) {
    std::ostringstream ss;
    ss << what << " hr=0x" << std::hex << static_cast<unsigned long>(hr);
    return ss.str();
}

// Clamp a float in [-1, +1) to a signed 16-bit sample. Values just outside are
// clamped rather than wrapped — a small guard against any codec's overshoot on
// the render side leaking through as a wraparound click.
inline int16_t FloatToS16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    // 32767 not 32768 so +1.0f maps to INT16_MAX (no wrap); -1.0f maps to -32767.
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

}  // namespace

class WasapiLoopbackCapture::Impl {
 public:
    Impl() = default;
    ~Impl() { StopAndJoin(); }

    StartResult Start(const Options& options, FrameSink sink) {
        StopAndJoin();  // idempotent
        options_ = options;
        if (options_.target_process_id == 0) {
            options_.target_process_id = GetCurrentProcessId();
        }
        sink_ = std::move(sink);
        last_error_.clear();

        stop_.store(false);
        thread_ = std::thread([this] { CaptureLoop(); });

        // Rendezvous with the capture thread: it flips start_result_ before
        // it either enters its read loop or bails on activation failure.
        std::unique_lock<std::mutex> lk(start_mu_);
        start_cv_.wait(lk, [this] { return start_ready_; });
        return start_result_;
    }

    void StopAndJoin() {
        if (!thread_.joinable()) return;
        stop_.store(true);
        if (frame_ready_) SetEvent(frame_ready_);  // wake a blocked WaitForSingleObject
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
        // Every WASAPI/COM object must be freed on the thread that made it, so
        // scope them tightly and let CoUninitialize catch the tail.
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            SignalStart(StartResult::kOsError, HrToString("CoInitializeEx", hr));
            return;
        }
        struct CoGuard { ~CoGuard() { CoUninitialize(); } } co_guard;

        AUDIOCLIENT_ACTIVATION_PARAMS activation{};
        activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation.ProcessLoopbackParams.TargetProcessId = options_.target_process_id;
        activation.ProcessLoopbackParams.ProcessLoopbackMode =
            options_.include_target_process_tree_only
                ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT prop{};
        prop.vt = VT_BLOB;
        prop.blob.cbSize = sizeof(activation);
        prop.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

        auto handler = Microsoft::WRL::Make<ActivateCompletion>();
        if (!handler || !handler->done) {
            SignalStart(StartResult::kOsError, "could not build completion handler");
            return;
        }

        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        hr = ActivateAudioInterfaceAsync(
            kProcessLoopbackDevice, __uuidof(IAudioClient), &prop, handler.Get(), &op);
        if (FAILED(hr)) {
            SignalStart(StartResult::kActivationRefused,
                        HrToString("ActivateAudioInterfaceAsync", hr));
            return;
        }
        if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) {
            SignalStart(StartResult::kOsError, "activation timed out");
            return;
        }
        if (FAILED(handler->activate_hr)) {
            SignalStart(StartResult::kActivationRefused,
                        HrToString("process-loopback activation refused", handler->activate_hr));
            return;
        }

        ComPtr<IAudioClient> client;
        hr = handler->unknown.As(&client);
        if (FAILED(hr) || !client) {
            SignalStart(StartResult::kOsError, HrToString("QI IAudioClient", hr));
            return;
        }

        // 48 kHz float32 stereo — matches the fixed output format so no
        // resample is needed; float32 is the shared-mode capture native format.
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

        // Process-loopback capture REQUIRES AUDCLNT_STREAMFLAGS_LOOPBACK to be
        // set explicitly. Without it, IAudioClient::Initialize returns
        // AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021) — verified on real
        // hardware via third_party/wasapi-loopback-poc. AUTOCONVERTPCM /
        // SRC_DEFAULT_QUALITY are ALSO invalid in this flags position for the
        // process-loopback virtual device (same INVALID_STREAM_FLAG), and are
        // unnecessary: the 48 kHz float32 stereo format below is provided to
        // Initialize directly, so the device converts to it internally.
        const DWORD init_flags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, init_flags,
                                kBufferDuration, 0,
                                reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) {
            SignalStart(StartResult::kFormatRejected, HrToString("IAudioClient::Initialize", hr));
            return;
        }

        frame_ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->SetEventHandle(frame_ready_);
        if (FAILED(hr)) {
            SignalStart(StartResult::kOsError, HrToString("SetEventHandle", hr));
            return;
        }

        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) {
            SignalStart(StartResult::kOsError, HrToString("GetService", hr));
            return;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            SignalStart(StartResult::kOsError, HrToString("IAudioClient::Start", hr));
            return;
        }
        struct StopGuard { IAudioClient* c; ~StopGuard() { if (c) c->Stop(); } } stop_guard{client.Get()};

        // Ready — unblock Start(). From here on, only push frames + poll stop.
        SignalStart(StartResult::kOk);

        // Accumulator so we always call the sink with exactly kFrameBytes,
        // regardless of the packet sizes WASAPI hands us. Interleaved s16.
        std::vector<int16_t> pending;
        pending.reserve(kFrameSamplesPerChannel * kFrameChannels * 2);

        while (!stop_.load()) {
            // 100 ms poll — WASAPI's event fires every ~10 ms so we always
            // wake much earlier, but this bounds stop-latency independently
            // of the event ever firing.
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
                    // Loopback fills the buffer with silence when the mix is
                    // empty (or, in exclude mode, when only excluded processes
                    // are playing) — write zeros rather than reading `data`.
                    std::memset(pending.data() + old, 0, need_samples * sizeof(int16_t));
                } else {
                    const float* src = reinterpret_cast<const float*>(data);
                    for (size_t i = 0; i < need_samples; ++i) {
                        pending[old + i] = FloatToS16(src[i]);
                    }
                }

                capture->ReleaseBuffer(frames);

                // Emit as many full 10 ms frames as we now have; carry the
                // remainder to the next packet.
                const size_t frame_len = static_cast<size_t>(kFrameSamplesPerChannel) * kFrameChannels;
                size_t offset = 0;
                while (pending.size() - offset >= frame_len && !stop_.load()) {
                    sink_(pending.data() + offset);
                    offset += frame_len;
                }
                if (offset > 0) {
                    pending.erase(pending.begin(), pending.begin() + offset);
                }

                hr = capture->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) break;
            }
        }

        if (frame_ready_) {
            CloseHandle(frame_ready_);
            frame_ready_ = nullptr;
        }
    }

    Options options_{};
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

WasapiLoopbackCapture::WasapiLoopbackCapture() : impl_(std::make_unique<Impl>()) {}
WasapiLoopbackCapture::~WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::StartResult WasapiLoopbackCapture::Start(
    const Options& options, FrameSink sink) {
    return impl_->Start(options, std::move(sink));
}

void WasapiLoopbackCapture::StopAndJoin() { impl_->StopAndJoin(); }

std::string WasapiLoopbackCapture::last_error() const { return impl_->last_error(); }

}  // namespace mdt
