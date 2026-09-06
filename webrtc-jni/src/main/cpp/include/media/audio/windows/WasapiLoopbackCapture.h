// WASAPI process-loopback capture engine (Windows 11+).
//
// Zero libwebrtc dependencies — the ADM in wasapi_loopback_adm.cc owns the
// libwebrtc-facing side (AudioTransport, ADM interface) and delegates the
// actual OS I/O here. Keeping the split lets step-0's POC and PR 1's ADM share
// the exact same, already-verified capture body.
//
// Output format is fixed: 48 kHz, 16-bit signed PCM, stereo, packaged as
// 10 ms frames (480 samples per channel — the frame size libwebrtc's
// AudioTransport::RecordedDataIsAvailable expects on the send path). Any
// resampling or channel-count fixup that Windows can't do itself lives in this
// class; the ADM never sees anything else.
//
// Thread model: Start() launches one dedicated capture thread; StopAndJoin()
// terminates it. The sink callback runs on that capture thread, so the ADM
// forwards to AudioTransport directly on it (as libwebrtc's own Windows ADM
// does — see modules/audio_device/win/audio_device_core_win.cc).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mdt {

// One 10 ms frame's worth of s16 PCM, stereo, 48 kHz. That is exactly
// 480 samples per channel, 960 samples total, 1920 bytes. Fixed so the sink
// callback never needs a size parameter.
constexpr int kFrameSamplesPerChannel = 480;
constexpr int kFrameChannels = 2;
constexpr int kFrameSampleRate = 48000;
constexpr int kFrameBytes =
    kFrameSamplesPerChannel * kFrameChannels * static_cast<int>(sizeof(int16_t));

// The sink is called once per 10 ms frame. `pcm` points at kFrameBytes bytes
// owned by the capturer; copy anything you need to keep. Return value is
// currently ignored; reserved for future backpressure signalling.
using FrameSink = std::function<void(const int16_t* pcm)>;

class WasapiLoopbackCapture {
 public:
    // If include_target_process_tree_only is false (the ADM's default), the
    // capturer runs in EXCLUDE mode against the current process's tree — the
    // echo-removal path. Passing true is only useful for diagnostics
    // (e.g. verifying that INCLUDE against a browser PID captures only that
    // browser's audio).
    struct Options {
        uint32_t target_process_id = 0;   // 0 → current PID
        bool include_target_process_tree_only = false;
    };

    enum class StartResult {
        kOk,
        kActivationRefused,               // OS lacks process-loopback (Win10 pre-20348)
        kFormatRejected,                  // 48 kHz float32 stereo not accepted
        kOsError,                         // any other COM/WASAPI failure — see last_error()
    };

    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    WasapiLoopbackCapture(const WasapiLoopbackCapture&) = delete;
    WasapiLoopbackCapture& operator=(const WasapiLoopbackCapture&) = delete;

    // Sink must live until StopAndJoin() returns. Options may only be changed
    // between Stop and Start; the capture thread reads them once at startup.
    StartResult Start(const Options& options, FrameSink sink);
    void StopAndJoin();

    // Diagnostics for the last failed Start(); empty when Start() returned kOk.
    std::string last_error() const;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mdt
