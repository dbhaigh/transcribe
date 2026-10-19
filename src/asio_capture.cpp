#include "transcribe/asio_capture.hpp"

#include <stdexcept>
#include <utility>

#ifdef TRANSCRIBE_ENABLE_ASIO
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <RtAudio.h>
#endif

namespace transcribe {

#ifdef TRANSCRIBE_ENABLE_ASIO
namespace {

struct CaptureState {
    std::vector<float> samples;
    std::atomic<std::size_t> captured{0};
};

int capture_callback(void* input, void*, unsigned int frames, double, RtAudioStreamStatus,
                     void* user_data) {
    auto& state = *static_cast<CaptureState*>(user_data);
    const auto* samples = static_cast<const float*>(input);
    if (samples == nullptr) {
        return 2;
    }
    const auto start = state.captured.load(std::memory_order_relaxed);
    const auto available = state.samples.size() - start;
    const auto count = std::min<std::size_t>(frames, available);
    std::copy_n(samples, count, state.samples.data() + start);
    state.captured.store(start + count, std::memory_order_release);
    return count < frames || start + count == state.samples.size() ? 1 : 0;
}

} // namespace

AudioBuffer capture_asio(double seconds, std::stop_token stop_token) {
    if (seconds <= 0.0 || seconds > 300.0) {
        throw std::invalid_argument("ASIO capture duration must be greater than 0 and at most 300 seconds.");
    }

    RtAudio audio(RtAudio::Api::WINDOWS_ASIO);
    if (audio.getCurrentApi() != RtAudio::Api::WINDOWS_ASIO) {
        throw std::runtime_error("RtAudio did not select its ASIO backend.");
    }
    const auto device = audio.getDefaultInputDevice();
    if (device == 0) {
        throw std::runtime_error("No default ASIO input device is available.");
    }

    constexpr std::uint32_t sample_rate = 44100;
    CaptureState state;
    state.samples.resize(static_cast<std::size_t>(seconds * sample_rate));
    RtAudio::StreamParameters parameters{};
    parameters.deviceId = device;
    parameters.nChannels = 1;
    parameters.firstChannel = 0;
    unsigned int buffer_frames = 512;

    audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                     &capture_callback, &state);
    audio.startStream();
    while (state.captured.load(std::memory_order_acquire) < state.samples.size() &&
           audio.isStreamRunning() && !stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (audio.isStreamOpen()) {
        audio.stopStream();
        audio.closeStream();
    }
    state.samples.resize(state.captured.load(std::memory_order_acquire));
    if (state.samples.empty()) {
        throw std::runtime_error(stop_token.stop_requested()
                                     ? "ASIO capture was cancelled."
                                     : "ASIO capture returned no audio samples.");
    }
    return {sample_rate, std::move(state.samples)};
}
#else
AudioBuffer capture_asio(double, std::stop_token) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}
#endif

} // namespace transcribe
