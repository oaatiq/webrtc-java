// Completed ADM that bridges WasapiLoopbackCapture → webrtc::AudioTransport.
// Every non-recording method is a one-line stub because this ADM only sends
// audio — it never plays.

#include "media/audio/windows/WasapiLoopbackAdm.h"
#include "media/audio/windows/WasapiLoopbackCapture.h"
#include "media/audio/windows/WasapiMicCapture.h"

#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "api/scoped_refptr.h"
#include "rtc_base/ref_counted_object.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace mdt {

struct WasapiLoopbackAdm::Impl {
    std::atomic<bool> initialized{false};
    std::atomic<bool> recording_initialized{false};
    std::atomic<bool> recording{false};

    std::mutex transport_mu;
    webrtc::AudioTransport* transport = nullptr;

    WasapiLoopbackCapture capture;     // render mix — the laptop's own audio
    WasapiMicCapture mic_capture;      // the microphone — surrounding room audio
    bool mic_active = false;

    // The mic thread only queues raw frames; the loopback thread (the mix clock
    // master) consumes them, so the APM is only ever touched from one thread.
    std::mutex mic_mu;
    std::deque<std::vector<int16_t>> mic_frames;
    static constexpr size_t kMaxMicFrames = 8;

    // AEC: running the mic through the APM with the render mix as the reverse
    // (reference) stream cancels the laptop's own speakers out of the room audio,
    // so the laptop audio is heard once (from the loopback), not doubled.
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
};

WasapiLoopbackAdm::WasapiLoopbackAdm() : impl_(std::make_unique<Impl>()) {}
WasapiLoopbackAdm::~WasapiLoopbackAdm() { Terminate(); }

rtc::scoped_refptr<WasapiLoopbackAdm> WasapiLoopbackAdm::Create() {
    return rtc::make_ref_counted<WasapiLoopbackAdm>();
}

// ---- Lifecycle ----

int32_t WasapiLoopbackAdm::ActiveAudioLayer(AudioLayer* audioLayer) const {
    *audioLayer = AudioLayer::kWindowsCoreAudio2;
    return 0;
}

int32_t WasapiLoopbackAdm::RegisterAudioCallback(webrtc::AudioTransport* transport) {
    std::lock_guard<std::mutex> lk(impl_->transport_mu);
    impl_->transport = transport;
    return 0;
}

int32_t WasapiLoopbackAdm::Init() { impl_->initialized = true; return 0; }

int32_t WasapiLoopbackAdm::Terminate() {
    StopRecording();
    impl_->initialized = false;
    return 0;
}

bool WasapiLoopbackAdm::Initialized() const { return impl_->initialized; }

// ---- Recording (the real work) ----

int32_t WasapiLoopbackAdm::InitRecording() {
    impl_->recording_initialized = true;
    return 0;
}

bool WasapiLoopbackAdm::RecordingIsInitialized() const {
    return impl_->recording_initialized;
}

int32_t WasapiLoopbackAdm::StartRecording() {
    if (impl_->recording.exchange(true)) return 0;

    constexpr int kAecDelayMs = 30;
    constexpr size_t kN = static_cast<size_t>(kFrameSamplesPerChannel) * kFrameChannels;

    // Build the echo canceller. If anything here fails we fall back to
    // loopback-only (the previous behaviour) — mic mixing is best-effort.
    impl_->apm = webrtc::AudioProcessingBuilder().Create();
    if (impl_->apm) {
        webrtc::AudioProcessing::Config cfg;
        cfg.echo_canceller.enabled = true;                    // strip speakers from mic
        cfg.echo_canceller.enforce_high_pass_filtering = true;
        cfg.high_pass_filter.enabled = true;
        cfg.noise_suppression.enabled = false;                // keep ambient room sound
        cfg.gain_controller2.enabled = true;                  // auto-level the room mic
        cfg.gain_controller2.adaptive_digital.enabled = true;
        impl_->apm->ApplyConfig(cfg);
    }

    // Start the microphone. Its thread only enqueues raw frames; all APM work
    // happens on the loopback thread below, so the APM stays single-threaded.
    impl_->mic_active = false;
    if (impl_->apm) {
        auto mic_sink = [this, kN](const int16_t* pcm) {
            std::lock_guard<std::mutex> lk(impl_->mic_mu);
            if (impl_->mic_frames.size() >= Impl::kMaxMicFrames) {
                impl_->mic_frames.pop_front();  // drop oldest, keep latency bounded
            }
            impl_->mic_frames.emplace_back(pcm, pcm + kN);
        };
        if (impl_->mic_capture.Start(mic_sink) == WasapiMicCapture::StartResult::kOk) {
            impl_->mic_active = true;
        } else {
            std::fprintf(stderr, "WasapiLoopbackAdm: mic unavailable (%s); loopback only\n",
                         impl_->mic_capture.last_error().c_str());
        }
    }

    // The loopback capture is the mix clock: per render frame, AEC the mic
    // against it and sum, then hand the combined frame to WebRTC.
    WasapiLoopbackCapture::Options options{};
    auto sink = [this, kN, kAecDelayMs](const int16_t* pcm) {
        int16_t mixed[kFrameSamplesPerChannel * kFrameChannels];
        std::memcpy(mixed, pcm, kN * sizeof(int16_t));  // laptop audio, heard once

        if (impl_->mic_active && impl_->apm) try {
            const webrtc::StreamConfig sc(kFrameSampleRate, kFrameChannels);
            int16_t scratch[kFrameSamplesPerChannel * kFrameChannels];
            // Reference = what the speakers are playing (the render mix).
            impl_->apm->ProcessReverseStream(pcm, sc, sc, scratch);

            std::vector<int16_t> mic;
            {
                std::lock_guard<std::mutex> lk(impl_->mic_mu);
                if (!impl_->mic_frames.empty()) {
                    mic = std::move(impl_->mic_frames.front());
                    impl_->mic_frames.pop_front();
                }
            }
            if (mic.size() == kN) {
                int16_t mic_clean[kFrameSamplesPerChannel * kFrameChannels];
                impl_->apm->set_stream_delay_ms(kAecDelayMs);
                impl_->apm->ProcessStream(mic.data(), sc, sc, mic_clean);  // echo removed
                for (size_t i = 0; i < kN; ++i) {
                    int32_t s = static_cast<int32_t>(mixed[i]) + static_cast<int32_t>(mic_clean[i]);
                    if (s > 32767) s = 32767;
                    else if (s < -32768) s = -32768;
                    mixed[i] = static_cast<int16_t>(s);
                }
            }
        } catch (...) {
            // Any failure in the mic/AEC path degrades to loopback-only for this
            // frame rather than tearing down the capture thread.
        }

        std::lock_guard<std::mutex> lk(impl_->transport_mu);
        if (!impl_->transport) return;
        uint32_t new_mic_level = 0;
        impl_->transport->RecordedDataIsAvailable(
            mixed,
            kFrameSamplesPerChannel,
            sizeof(int16_t) * kFrameChannels,
            kFrameChannels,
            kFrameSampleRate,
            0,     // total_delay_ms
            0,     // clock_drift
            0,     // current_mic_level
            false, // key_pressed
            new_mic_level);
    };
    auto res = impl_->capture.Start(options, std::move(sink));
    if (res != WasapiLoopbackCapture::StartResult::kOk) {
        impl_->recording.store(false);
        impl_->mic_capture.StopAndJoin();
        impl_->mic_active = false;
        impl_->apm = nullptr;
        return -1;
    }
    return 0;
}

int32_t WasapiLoopbackAdm::StopRecording() {
    if (!impl_->recording.exchange(false)) return 0;
    impl_->capture.StopAndJoin();
    impl_->mic_capture.StopAndJoin();
    {
        std::lock_guard<std::mutex> lk(impl_->mic_mu);
        impl_->mic_frames.clear();
    }
    impl_->mic_active = false;
    impl_->apm = nullptr;
    return 0;
}

bool WasapiLoopbackAdm::Recording() const { return impl_->recording; }

// ---- Playout (all stubs) ----

int32_t WasapiLoopbackAdm::InitPlayout() { return 0; }
bool WasapiLoopbackAdm::PlayoutIsInitialized() const { return false; }
int32_t WasapiLoopbackAdm::StartPlayout() { return 0; }
int32_t WasapiLoopbackAdm::StopPlayout() { return 0; }
bool WasapiLoopbackAdm::Playing() const { return false; }
int32_t WasapiLoopbackAdm::PlayoutDelay(uint16_t* delayMS) const { *delayMS = 0; return 0; }

// ---- Built-in audio processing (not applicable for loopback) ----
bool WasapiLoopbackAdm::BuiltInAECIsAvailable() const { return false; }
bool WasapiLoopbackAdm::BuiltInAGCIsAvailable() const { return false; }
bool WasapiLoopbackAdm::BuiltInNSIsAvailable() const { return false; }
int32_t WasapiLoopbackAdm::EnableBuiltInAEC(bool /*enable*/) { return -1; }
int32_t WasapiLoopbackAdm::EnableBuiltInAGC(bool /*enable*/) { return -1; }
int32_t WasapiLoopbackAdm::EnableBuiltInNS(bool /*enable*/) { return -1; }

// ---- Device enumeration (one virtual recording device, no playout) ----

int16_t WasapiLoopbackAdm::PlayoutDevices() { return 0; }
int16_t WasapiLoopbackAdm::RecordingDevices() { return 1; }

int32_t WasapiLoopbackAdm::PlayoutDeviceName(
    uint16_t, char name[webrtc::kAdmMaxDeviceNameSize],
    char guid[webrtc::kAdmMaxGuidSize]) {
    return -1;
}

int32_t WasapiLoopbackAdm::RecordingDeviceName(
    uint16_t, char name[webrtc::kAdmMaxDeviceNameSize],
    char guid[webrtc::kAdmMaxGuidSize]) {
    std::snprintf(name, webrtc::kAdmMaxDeviceNameSize,
                  "WASAPI loopback (excludes this process)");
    guid[0] = '\0';
    return 0;
}

// ---- Device selection (accept anything) ----

int32_t WasapiLoopbackAdm::SetPlayoutDevice(uint16_t) { return 0; }
int32_t WasapiLoopbackAdm::SetPlayoutDevice(WindowsDeviceType) { return 0; }
int32_t WasapiLoopbackAdm::SetRecordingDevice(uint16_t) { return 0; }
int32_t WasapiLoopbackAdm::SetRecordingDevice(WindowsDeviceType) { return 0; }

// ---- Availability ----

int32_t WasapiLoopbackAdm::PlayoutIsAvailable(bool* available) { *available = false; return 0; }
int32_t WasapiLoopbackAdm::RecordingIsAvailable(bool* available) { *available = true; return 0; }

// ---- Speaker / Mic init ----

int32_t WasapiLoopbackAdm::InitSpeaker() { return 0; }
bool WasapiLoopbackAdm::SpeakerIsInitialized() const { return false; }
int32_t WasapiLoopbackAdm::InitMicrophone() { return 0; }
bool WasapiLoopbackAdm::MicrophoneIsInitialized() const { return false; }

// ---- Volume (not applicable) ----

int32_t WasapiLoopbackAdm::SpeakerVolumeIsAvailable(bool* available) { *available = false; return 0; }
int32_t WasapiLoopbackAdm::SetSpeakerVolume(uint32_t) { return -1; }
int32_t WasapiLoopbackAdm::SpeakerVolume(uint32_t* volume) const { *volume = 0; return 0; }
int32_t WasapiLoopbackAdm::MaxSpeakerVolume(uint32_t* maxVolume) const { *maxVolume = 0; return 0; }
int32_t WasapiLoopbackAdm::MinSpeakerVolume(uint32_t* minVolume) const { *minVolume = 0; return 0; }

int32_t WasapiLoopbackAdm::MicrophoneVolumeIsAvailable(bool* available) { *available = false; return 0; }
int32_t WasapiLoopbackAdm::SetMicrophoneVolume(uint32_t) { return -1; }
int32_t WasapiLoopbackAdm::MicrophoneVolume(uint32_t* volume) const { *volume = 0; return 0; }
int32_t WasapiLoopbackAdm::MaxMicrophoneVolume(uint32_t* maxVolume) const { *maxVolume = 0; return 0; }
int32_t WasapiLoopbackAdm::MinMicrophoneVolume(uint32_t* minVolume) const { *minVolume = 0; return 0; }

// ---- Mute (not applicable) ----

int32_t WasapiLoopbackAdm::SpeakerMuteIsAvailable(bool* available) { *available = false; return 0; }
int32_t WasapiLoopbackAdm::SetSpeakerMute(bool) { return -1; }
int32_t WasapiLoopbackAdm::SpeakerMute(bool* enabled) const { *enabled = false; return 0; }
int32_t WasapiLoopbackAdm::MicrophoneMuteIsAvailable(bool* available) { *available = false; return 0; }
int32_t WasapiLoopbackAdm::SetMicrophoneMute(bool) { return -1; }
int32_t WasapiLoopbackAdm::MicrophoneMute(bool* enabled) const { *enabled = false; return 0; }

// ---- Stereo ----

int32_t WasapiLoopbackAdm::StereoPlayoutIsAvailable(bool* available) const { *available = true; return 0; }
int32_t WasapiLoopbackAdm::SetStereoPlayout(bool) { return 0; }
int32_t WasapiLoopbackAdm::StereoPlayout(bool* enabled) const { *enabled = true; return 0; }
int32_t WasapiLoopbackAdm::StereoRecordingIsAvailable(bool* available) const { *available = true; return 0; }
int32_t WasapiLoopbackAdm::SetStereoRecording(bool) { return 0; }
int32_t WasapiLoopbackAdm::StereoRecording(bool* enabled) const { *enabled = true; return 0; }

}  // namespace mdt
