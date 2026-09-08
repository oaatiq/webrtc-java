// WASAPI default-input (microphone) capture — 48 kHz s16 stereo, 10 ms frames.
//
// Same frame contract and threading model as WasapiLoopbackCapture, but it
// captures the default communications *input* endpoint (the microphone) instead
// of the process-loopback render mix. WasapiLoopbackAdm mixes this in after
// running it through WebRTC's AEC (with the loopback render mix as the echo
// reference), so the laptop's own speakers are cancelled out of the room audio.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Reuse kFrameSamplesPerChannel / kFrameChannels / kFrameSampleRate + FrameSink.
#include "media/audio/windows/WasapiLoopbackCapture.h"

namespace mdt {

class WasapiMicCapture {
 public:
    enum class StartResult {
        kOk,
        kNoDevice,        // no default capture endpoint (no mic)
        kFormatRejected,  // 48 kHz float32 stereo not accepted even with autoconvert
        kOsError,         // any other COM/WASAPI failure — see last_error()
    };

    WasapiMicCapture();
    ~WasapiMicCapture();

    WasapiMicCapture(const WasapiMicCapture&) = delete;
    WasapiMicCapture& operator=(const WasapiMicCapture&) = delete;

    // Sink must live until StopAndJoin() returns. Idempotent.
    StartResult Start(FrameSink sink);
    void StopAndJoin();

    std::string last_error() const;

 private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mdt
