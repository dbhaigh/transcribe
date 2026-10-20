#include "transcribe/asio_capture.hpp"

#include <stdexcept>
#include <utility>

#ifdef TRANSCRIBE_ENABLE_ASIO
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>
#include <vector>

#include <RtAudio.h>
#endif

namespace transcribe {

#ifdef TRANSCRIBE_ENABLE_ASIO
namespace {

struct CaptureState {
    CaptureState(std::size_t sample_capacity, std::size_t ring_capacity)
        : ring(ring_capacity), analysis_ring(ring_capacity), capacity(sample_capacity) {}

    AudioRingBuffer ring;
    AudioRingBuffer analysis_ring;
    std::size_t capacity{};
    std::atomic<std::size_t> captured{0};
    std::atomic<bool> overflow{false};
    std::atomic<bool> analysis_failed{false};
};

int capture_callback(void*, void* input, unsigned int frames, double, RtAudioStreamStatus,
                     void* user_data) {
    auto& state = *static_cast<CaptureState*>(user_data);
    if (state.analysis_failed.load(std::memory_order_acquire)) {
        return 2;
    }
    const auto* samples = static_cast<const float*>(input);
    if (samples == nullptr) {
        return 2;
    }
    const auto start = state.captured.load(std::memory_order_relaxed);
    const auto available = state.capacity - start;
    const auto count = std::min<std::size_t>(frames, available);
    const auto input_samples = std::span<const float>(samples, count);
    if (state.ring.write(input_samples) != count ||
        state.analysis_ring.write(input_samples) != count) {
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
                             audio_update_callback,
                         const RealtimeUpdateCallback& realtime_update_callback,
                         const RealtimeOptions& realtime_options) {
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
    RealtimeTranscriber analyzer(realtime_options);
    std::atomic<bool> capture_finished{false};
    std::exception_ptr analysis_error;
    AudioBuffer captured_audio{sample_rate, {}};
    captured_audio.samples.reserve(sample_capacity);
    RtAudio::StreamParameters parameters{};
    parameters.deviceId = device;
    parameters.nChannels = 1;
    parameters.firstChannel = 0;
    unsigned int buffer_frames = 512;

    audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                     &capture_callback, &state);
    std::jthread analysis_thread([&](std::stop_token worker_stop) {
        std::array<float, 4096> analysis_transfer{};
        try {
            while (!worker_stop.stop_requested() &&
                   (!capture_finished.load(std::memory_order_acquire) ||
                    state.analysis_ring.readable() > 0)) {
                const auto read = state.analysis_ring.read(analysis_transfer);
                if (read > 0) {
                    analyzer.push_samples(
                        std::span<const float>(analysis_transfer.data(), read));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (!worker_stop.stop_requested()) {
                analyzer.finish();
            }
        } catch (...) {
            analysis_error = std::current_exception();
            state.analysis_failed.store(true, std::memory_order_release);
        }
    });
    audio.startStream();
    std::size_t last_reported = 0;
    std::size_t last_audio_update = 0;
    auto last_live_update = std::chrono::steady_clock::now();
    std::array<float, 4096> transfer{};
    const auto update_audio_preview = [&] {
        if (!audio_update_callback ||
            captured_audio.samples.size() <= last_audio_update) {
            return;
        }
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
    };
    while (state.captured.load(std::memory_order_acquire) < sample_capacity &&
           audio.isStreamRunning() && !stop_token.stop_requested()) {
        const auto captured = state.captured.load(std::memory_order_acquire);
        std::size_t count = 0;
        while ((count = state.ring.read(transfer)) > 0) {
            captured_audio.samples.insert(captured_audio.samples.end(), transfer.begin(),
                                          transfer.begin() + static_cast<std::ptrdiff_t>(count));
        }
        if (captured_audio.samples.size() - last_audio_update >= sample_rate / 5) {
            update_audio_preview();
        }
        const auto now = std::chrono::steady_clock::now();
        auto realtime_events = analyzer.take_events();
        if (realtime_update_callback &&
            (!realtime_events.empty() ||
             now - last_live_update >= std::chrono::milliseconds(50))) {
            realtime_update_callback(analyzer.live_notes(), realtime_events);
            last_live_update = now;
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
    update_audio_preview();
    capture_finished.store(true, std::memory_order_release);
    analysis_thread.join();
    if (analysis_error) {
        std::rethrow_exception(analysis_error);
    }
    if (realtime_update_callback) {
        realtime_update_callback(analyzer.live_notes(), analyzer.take_events());
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
                         const std::function<void(double, const AudioBuffer&)>&,
                         const RealtimeUpdateCallback&,
                         const RealtimeOptions&) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}
#endif

} // namespace transcribe
