// Completed ADM that bridges WasapiLoopbackCapture → webrtc::AudioTransport.
// Every non-recording method is a one-line stub because this ADM only sends
// audio — it never plays.

#include "media/audio/windows/WasapiLoopbackAdm.h"
#include "media/audio/windows/WasapiLoopbackCapture.h"

#include "modules/audio_device/include/audio_device.h"
#include "rtc_base/ref_counted_object.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace mdt {

struct WasapiLoopbackAdm::Impl {
    std::atomic<bool> initialized{false};
    std::atomic<bool> recording_initialized{false};
    std::atomic<bool> recording{false};

    std::mutex transport_mu;
    webrtc::AudioTransport* transport = nullptr;

    WasapiLoopbackCapture capture;
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

    WasapiLoopbackCapture::Options options{};
    auto sink = [this](const int16_t* pcm) {
        std::lock_guard<std::mutex> lk(impl_->transport_mu);
        if (!impl_->transport) return;
        uint32_t new_mic_level = 0;
        impl_->transport->RecordedDataIsAvailable(
            pcm,
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
        return -1;
    }
    return 0;
}

int32_t WasapiLoopbackAdm::StopRecording() {
    if (!impl_->recording.exchange(false)) return 0;
    impl_->capture.StopAndJoin();
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
