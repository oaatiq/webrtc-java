// ADM that feeds WASAPI process-loopback audio into a WebRTC send-track.
// See WasapiLoopbackCapture.h for the OS-level capture engine.

#pragma once

#include "api/scoped_refptr.h"
#include "modules/audio_device/include/audio_device.h"
#include "rtc_base/ref_counted_object.h"

namespace mdt {

class WasapiLoopbackAdm : public webrtc::AudioDeviceModule {
 public:
    static rtc::scoped_refptr<WasapiLoopbackAdm> Create();

    ~WasapiLoopbackAdm() override;

    // Lifecycle
    int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override;
    int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override;
    int32_t Init() override;
    int32_t Terminate() override;
    bool Initialized() const override;

    // Device enumeration
    int16_t PlayoutDevices() override;
    int16_t RecordingDevices() override;
    int32_t PlayoutDeviceName(uint16_t index,
                              char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override;
    int32_t RecordingDeviceName(uint16_t index,
                                char name[webrtc::kAdmMaxDeviceNameSize],
                                char guid[webrtc::kAdmMaxGuidSize]) override;

    // Device selection
    int32_t SetPlayoutDevice(uint16_t index) override;
    int32_t SetPlayoutDevice(WindowsDeviceType device) override;
    int32_t SetRecordingDevice(uint16_t index) override;
    int32_t SetRecordingDevice(WindowsDeviceType device) override;

    // Availability
    int32_t PlayoutIsAvailable(bool* available) override;
    int32_t InitPlayout() override;
    bool PlayoutIsInitialized() const override;
    int32_t RecordingIsAvailable(bool* available) override;
    int32_t InitRecording() override;
    bool RecordingIsInitialized() const override;

    // Start/Stop
    int32_t StartPlayout() override;
    int32_t StopPlayout() override;
    bool Playing() const override;
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    bool Recording() const override;

    // Speaker
    int32_t InitSpeaker() override;
    bool SpeakerIsInitialized() const override;
    int32_t SpeakerVolumeIsAvailable(bool* available) override;
    int32_t SetSpeakerVolume(uint32_t volume) override;
    int32_t SpeakerVolume(uint32_t* volume) const override;
    int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override;
    int32_t MinSpeakerVolume(uint32_t* minVolume) const override;

    // Microphone
    int32_t InitMicrophone() override;
    bool MicrophoneIsInitialized() const override;
    int32_t MicrophoneVolumeIsAvailable(bool* available) override;
    int32_t SetMicrophoneVolume(uint32_t volume) override;
    int32_t MicrophoneVolume(uint32_t* volume) const override;
    int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const override;
    int32_t MinMicrophoneVolume(uint32_t* minVolume) const override;

    // Mute
    int32_t SpeakerMuteIsAvailable(bool* available) override;
    int32_t SetSpeakerMute(bool enable) override;
    int32_t SpeakerMute(bool* enabled) const override;
    int32_t MicrophoneMuteIsAvailable(bool* available) override;
    int32_t SetMicrophoneMute(bool enable) override;
    int32_t MicrophoneMute(bool* enabled) const override;

    // Stereo
    int32_t StereoPlayoutIsAvailable(bool* available) const override;
    int32_t SetStereoPlayout(bool enable) override;
    int32_t StereoPlayout(bool* enabled) const override;
    int32_t StereoRecordingIsAvailable(bool* available) const override;
    int32_t SetStereoRecording(bool enable) override;
    int32_t StereoRecording(bool* enabled) const override;

    // Built-in audio processing
    bool BuiltInAECIsAvailable() const override;
    bool BuiltInAGCIsAvailable() const override;
    bool BuiltInNSIsAvailable() const override;
    int32_t EnableBuiltInAEC(bool enable) override;
    int32_t EnableBuiltInAGC(bool enable) override;
    int32_t EnableBuiltInNS(bool enable) override;

    // Delay
    int32_t PlayoutDelay(uint16_t* delayMS) const override;

 private:
    friend class rtc::RefCountedObject<WasapiLoopbackAdm>;
    WasapiLoopbackAdm();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mdt
