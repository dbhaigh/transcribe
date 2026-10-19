#include "transcribe/asio_capture.hpp"

#include <stdexcept>
#include <utility>

#ifdef TRANSCRIBE_ENABLE_ASIO
#include <algorithm>
#include <array>
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
    CaptureState(std::size_t sample_capacity, std::size_t ring_capacity)
        : ring(ring_capacity), capacity(sample_capacity) {}

    AudioRingBuffer ring;
    std::size_t capacity{};
    std::atomic<std::size_t> captured{0};
    std::atomic<bool> overflow{false};
};

int capture_callback(void*, void* input, unsigned int frames, double, RtAudioStreamStatus,
                     void* user_data) {
    auto& state = *static_cast<CaptureState*>(user_data);
    const auto* samples = static_cast<const float*>(input);
    if (samples == nullptr) {
        return 2;
    }
    const auto start = state.captured.load(std::memory_order_relaxed);
    const auto available = state.capacity - start;
    const auto count = std::min<std::size_t>(frames, available);
    if (state.ring.write(std::span<const float>(samples, count)) != count) {
        state.overflow.store(true, std::memory_order_release);
        return 2;
    }
    state.captured.store(start + count, std::memory_order_release);
    return count < frames || start + count == state.capacity ? 1 : 0;
}

} // namespace

AudioBuffer capture_asio(double seconds, std::stop_token stop_token,
                         const std::function<void(double)>& progress_callback,
                         const std::function<void(double, const AudioBuffer&)>&
                             audio_update_callback) {
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
    const auto sample_capacity = static_cast<std::size_t>(seconds * sample_rate);
    CaptureState state(sample_capacity, static_cast<std::size_t>(sample_rate) * 5);
    AudioBuffer captured_audio{sample_rate, {}};
    captured_audio.samples.reserve(sample_capacity);
    RtAudio::StreamParameters parameters{};
    parameters.deviceId = device;
    parameters.nChannels = 1;
    parameters.firstChannel = 0;
    unsigned int buffer_frames = 512;

    audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                     &capture_callback, &state);
    audio.startStream();
    std::size_t last_reported = 0;
    std::size_t last_audio_update = 0;
    std::array<float, 4096> transfer{};
    while (state.captured.load(std::memory_order_acquire) < sample_capacity &&
           audio.isStreamRunning() && !stop_token.stop_requested()) {
        const auto captured = state.captured.load(std::memory_order_acquire);
        std::size_t count = 0;
        while ((count = state.ring.read(transfer)) > 0) {
            captured_audio.samples.insert(captured_audio.samples.end(), transfer.begin(),
                                          transfer.begin() + static_cast<std::ptrdiff_t>(count));
        }
        if (audio_update_callback &&
            captured_audio.samples.size() - last_audio_update >= sample_rate) {
            constexpr std::size_t preview_capacity = sample_rate * 3;
            const auto preview_start = captured_audio.samples.size() > preview_capacity
                                           ? captured_audio.samples.size() - preview_capacity
                                           : 0;
            AudioBuffer preview{sample_rate, {}};
            preview.samples.assign(
                captured_audio.samples.begin() + static_cast<std::ptrdiff_t>(preview_start),
                captured_audio.samples.end());
            audio_update_callback(static_cast<double>(preview_start) / sample_rate, preview);
            last_audio_update = captured_audio.samples.size();
        }
        if (progress_callback && captured - last_reported >= sample_rate / 5) {
            progress_callback(static_cast<double>(captured) / sample_capacity);
            last_reported = captured;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (audio.isStreamOpen()) {
        audio.stopStream();
        audio.closeStream();
    }
    std::size_t count = 0;
    while ((count = state.ring.read(transfer)) > 0) {
        captured_audio.samples.insert(captured_audio.samples.end(), transfer.begin(),
                                      transfer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (state.overflow.load(std::memory_order_acquire)) {
        throw std::runtime_error("ASIO capture ring buffer overflowed; audio was not lost silently.");
    }
    if (captured_audio.samples.empty()) {
        throw std::runtime_error(stop_token.stop_requested()
                                     ? "ASIO capture was cancelled."
                                     : "ASIO capture returned no audio samples.");
    }
    return captured_audio;
}
#else
AudioBuffer capture_asio(double, std::stop_token,
                         const std::function<void(double)>&,
                         const std::function<void(double, const AudioBuffer&)>&) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}
#endif

} // namespace transcribe
