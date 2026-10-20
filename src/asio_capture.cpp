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

struct LiveCaptureState {
    live::LiveTranscriptionSession* session{};
    live::AudioFormat format;
    std::uint64_t capacity{};
    std::atomic<std::uint64_t> next_frame{0};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> overflows{0};
    std::atomic<bool> rejected{false};
};

int live_capture_callback(void*, void* input, unsigned int frames, double,
                          RtAudioStreamStatus status, void* user_data) {
    auto& state = *static_cast<LiveCaptureState*>(user_data);
    const auto captured = std::chrono::steady_clock::now().time_since_epoch();
    const auto first = state.next_frame.load(std::memory_order_relaxed);
    if (first >= state.capacity) return 1;
    if (input == nullptr) return 0;
    const auto count = std::min<std::uint64_t>(frames, state.capacity - first);
    const bool overflow = (status & RTAUDIO_INPUT_OVERFLOW) != 0;
    if (overflow) state.overflows.fetch_add(1, std::memory_order_relaxed);
    const live::AudioFrameBlock block{
        state.format, first,
        std::span<const float>(static_cast<const float*>(input),
                               static_cast<std::size_t>(count) * state.format.channels),
        std::chrono::duration_cast<live::Nanoseconds>(captured), overflow};
    const auto pushed = state.session->push(block);
    if (pushed == live::PushStatus::QueueFull) {
        state.refused.fetch_add(1, std::memory_order_relaxed);
    } else if (pushed != live::PushStatus::Accepted) {
        state.rejected.store(true, std::memory_order_release);
        return 2;
    }
    // Refused frames still advance the timeline so the session reports them as a gap.
    state.next_frame.store(first + count, std::memory_order_release);
    return first + count >= state.capacity ? 1 : 0;
}

} // namespace

LiveCaptureReport capture_asio_live(double seconds, live::LiveTranscriptionSession& session,
                                    std::stop_token stop_token,
                                    const std::function<void(double)>& progress_callback) {
    if (!(seconds > 0.0 && seconds <= 3600.0)) {
        throw std::invalid_argument("Live ASIO capture duration must be greater than 0 and at most 3600 seconds.");
    }
    constexpr std::uint32_t sample_rate = 44100;
    const auto format = session.options().format;
    if (format.sample_rate != sample_rate || format.channels != 1) {
        throw std::invalid_argument("Live ASIO capture requires a 44100 Hz mono session format.");
    }
    LiveCaptureState state;
    state.session = &session;
    state.format = format;
    state.capacity = static_cast<std::uint64_t>(seconds * sample_rate);
    LiveCaptureReport report;

    RtAudio audio(RtAudio::Api::WINDOWS_ASIO);
    if (audio.getCurrentApi() != RtAudio::Api::WINDOWS_ASIO) {
        throw std::runtime_error("RtAudio did not select its ASIO backend.");
    }
    const auto open = [&]() {
        const auto device = audio.getDefaultInputDevice();
        if (device == 0) return false;
        RtAudio::StreamParameters parameters{};
        parameters.deviceId = device;
        parameters.nChannels = format.channels;
        unsigned int buffer_frames = 512;
        if (audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                             &live_capture_callback, &state) != RTAUDIO_NO_ERROR) {
            return false;
        }
        if (audio.startStream() != RTAUDIO_NO_ERROR) {
            audio.closeStream();
            return false;
        }
        return true;
    };
    if (!open()) throw std::runtime_error("No default ASIO input device could be opened.");
    const auto started = std::chrono::steady_clock::now();
    constexpr std::size_t maximum_reconnects = 5;
    std::uint64_t last_reported = 0;
    while (!stop_token.stop_requested()) {
        const auto delivered = state.next_frame.load(std::memory_order_acquire);
        if (delivered >= state.capacity || state.rejected.load(std::memory_order_acquire)) break;
        if (!audio.isStreamRunning()) {
            if (!report.input_lost) session.report_input_lost();
            report.input_lost = true;
            if (audio.isStreamOpen()) audio.closeStream();
            if (report.reconnects >= maximum_reconnects) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ++report.reconnects;
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const auto wall_frame = static_cast<std::uint64_t>(elapsed * sample_rate);
            state.next_frame.store(std::max(delivered, std::min(wall_frame, state.capacity)),
                                   std::memory_order_release);
            if (state.next_frame.load(std::memory_order_acquire) >= state.capacity) break;
            if (open()) report.input_lost = false;
            continue;
        }
        if (progress_callback && delivered - last_reported >= sample_rate / 5) {
            last_reported = delivered;
            progress_callback(static_cast<double>(delivered) / static_cast<double>(state.capacity));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (audio.isStreamOpen()) {
        if (audio.isStreamRunning()) audio.stopStream();
        audio.closeStream();
    }
    report.frames_delivered = state.next_frame.load(std::memory_order_acquire);
    report.blocks_refused = state.refused.load(std::memory_order_relaxed);
    report.driver_overflows = state.overflows.load(std::memory_order_relaxed);
    if (state.rejected.load(std::memory_order_acquire)) {
        throw std::runtime_error("The live session rejected ASIO audio (" + session.last_error() + ").");
    }
    return report;
}

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
LiveCaptureReport capture_asio_live(double, live::LiveTranscriptionSession&, std::stop_token,
                                    const std::function<void(double)>&) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}

AudioBuffer capture_asio(double, std::stop_token,
                         const std::function<void(double)>&,
                         const std::function<void(double, const AudioBuffer&)>&,
                         const RealtimeUpdateCallback&,
                         const RealtimeOptions&) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}
#endif

} // namespace transcribe
