#include "transcribe/asio_capture.hpp"
#include "asio_transport.hpp"

#include <stdexcept>
#include <utility>
#include <cmath>

#ifdef TRANSCRIBE_ENABLE_ASIO
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <RtAudio.h>
#endif

namespace transcribe {

#ifdef TRANSCRIBE_ENABLE_ASIO
namespace {

std::mutex asio_owner_mutex;

std::unique_lock<std::mutex> acquire_asio_owner() {
    std::unique_lock lock(asio_owner_mutex, std::try_to_lock);
    if (!lock.owns_lock()) throw std::runtime_error("Another ASIO device operation is already active.");
    return lock;
}

using CaptureState = detail::CaptureTransport;
using LiveCaptureState = detail::LiveCaptureTransport;

LiveCaptureReport capture_asio_stream_impl(
    double seconds, live::AudioInputSink& sink, std::stop_token stop_token,
    const std::function<void(double)>& progress_callback,
    const AsioCaptureOptions& capture_options,
    const std::function<void()>& update_callback,
    LiveCaptureReport* report_on_failure);

int capture_callback(void*, void* input, unsigned int frames, double, RtAudioStreamStatus status,
                     void* user_data) {
    auto& state = *static_cast<CaptureState*>(user_data);
    return state.receive(static_cast<const float*>(input), frames,
                         (status & RTAUDIO_INPUT_OVERFLOW) != 0);
}

int live_capture_callback(void*, void* input, unsigned int frames, double,
                          RtAudioStreamStatus status, void* user_data) {
    auto& state = *static_cast<LiveCaptureState*>(user_data);
    const auto captured = std::chrono::steady_clock::now().time_since_epoch();
    return state.receive(static_cast<const float*>(input), frames,
        (status & RTAUDIO_INPUT_OVERFLOW) != 0,
        std::chrono::duration_cast<live::Nanoseconds>(captured));
}

struct StreamGuard {
    RtAudio& audio;
    ~StreamGuard() { if (audio.isStreamOpen()) audio.closeStream(); }
};

} // namespace

std::vector<AsioInputDevice> enumerate_asio_input_devices() {
    auto owner = acquire_asio_owner();
    RtAudio audio(RtAudio::Api::WINDOWS_ASIO);
    if (audio.getCurrentApi() != RtAudio::Api::WINDOWS_ASIO) {
        throw std::runtime_error("RtAudio did not select its ASIO backend.");
    }
    std::vector<AsioInputDevice> result;
    for (const auto device_id : audio.getDeviceIds()) {
        const auto info = audio.getDeviceInfo(device_id);
        if (info.inputChannels == 0) continue;
        result.push_back({device_id, info.name, info.inputChannels,
                          info.preferredSampleRate, info.isDefaultInput,
                          info.sampleRates});
    }
    return result;
}

LiveCaptureReport capture_asio_live(double seconds, live::LiveTranscriptionSession& session,
                                    std::stop_token stop_token,
                                    const std::function<void(double)>& progress_callback,
                                    const AsioCaptureOptions& capture_options,
                                    const std::function<void()>& update_callback) {
    if (session.state() != live::LiveState::Running)
        throw std::invalid_argument("Start the live session before ASIO capture.");
    if (session.options().format.sample_rate != capture_options.sample_rate ||
        session.options().format.channels != capture_options.channels ||
        session.options().format.layout != capture_options.layout ||
        session.options().stream_epoch != capture_options.stream_epoch)
        throw std::invalid_argument("Live session format/epoch must match ASIO; use capture_asio_stream for explicit sinks.");
    session.begin_input(0);
    LiveCaptureReport report;
    try {
        report = capture_asio_stream_impl(seconds, session, stop_token, progress_callback,
            capture_options, [&] {
                if (update_callback) update_callback();
                if (session.state() == live::LiveState::AnalysisFailed)
                    throw std::runtime_error("Live analysis failed: " + session.last_error());
            }, &report);
    } catch (...) {
        session.end_input(report.frames_delivered);
        throw;
    }
    session.end_input(report.frames_delivered);
    return report;
}

namespace {

LiveCaptureReport capture_asio_stream_impl(double seconds, live::AudioInputSink& sink,
                                    std::stop_token stop_token,
                                    const std::function<void(double)>& progress_callback,
                                    const AsioCaptureOptions& capture_options,
                                    const std::function<void()>& update_callback,
                                    LiveCaptureReport* report_on_failure) {
    if (!(seconds > 0.0 && seconds <= 3600.0)) {
        throw std::invalid_argument("Live ASIO capture duration must be greater than 0 and at most 3600 seconds.");
    }
    const live::AudioFormat format{capture_options.sample_rate, capture_options.channels, capture_options.layout};
    if (format.sample_rate < 2000 || format.sample_rate > 768000 ||
        format.channels == 0 || format.channels > 64 ||
        (format.layout == live::ChannelLayout::Mono && format.channels != 1) ||
        (format.layout == live::ChannelLayout::Stereo && format.channels != 2) ||
        format.layout < live::ChannelLayout::DiscreteInterleaved ||
        format.layout > live::ChannelLayout::Stereo) {
        throw std::invalid_argument(
            "The live session format must match the selected ASIO sample rate and channel count.");
    }
    LiveCaptureState state;
    state.session = &sink;
    state.format = format;
    state.stream_epoch = capture_options.stream_epoch;
    state.capacity = static_cast<std::uint64_t>(seconds * format.sample_rate);
    if (state.capacity == 0) throw std::invalid_argument("ASIO duration is shorter than one input frame.");
    if (stop_token.stop_requested()) return {};
    LiveCaptureReport report;

    auto owner = acquire_asio_owner();
    RtAudio audio(RtAudio::Api::WINDOWS_ASIO);
    if (audio.getCurrentApi() != RtAudio::Api::WINDOWS_ASIO) {
        throw std::runtime_error("RtAudio did not select its ASIO backend.");
    }
    StreamGuard stream_guard{audio};
    const auto publish_report = [&] {
        report.frames_delivered = state.next_frame.load(std::memory_order_acquire);
        report.frames_accepted = state.delivered_frames.load(std::memory_order_relaxed);
        report.null_input_frames = state.missing_frames.load(std::memory_order_relaxed);
        report.blocks_refused = state.refused.load(std::memory_order_relaxed);
        report.driver_overflows = state.overflows.load(std::memory_order_relaxed);
        if (report_on_failure) *report_on_failure = report;
    };
    try {
        const auto device_ids = audio.getDeviceIds();
        const auto selected_device = capture_options.device_id
            ? *capture_options.device_id : audio.getDefaultInputDevice();
        if (std::find(device_ids.begin(), device_ids.end(), selected_device) ==
            device_ids.end()) {
            throw std::runtime_error("The selected ASIO input device is no longer available.");
        }
        const auto device_info = audio.getDeviceInfo(selected_device);
        if (device_info.inputChannels < format.channels) {
            throw std::runtime_error(
                "The selected ASIO device does not provide the requested input channels.");
        }
        if (std::find(device_info.sampleRates.begin(), device_info.sampleRates.end(),
                      format.sample_rate) == device_info.sampleRates.end()) {
            throw std::runtime_error(
                "The selected ASIO device does not support the requested sample rate.");
        }
        const auto open = [&]() {
            RtAudio::StreamParameters parameters{};
            parameters.deviceId = selected_device;
            parameters.nChannels = format.channels;
            unsigned int buffer_frames = 512;
            if (audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32,
                                 format.sample_rate, &buffer_frames,
                                 &live_capture_callback, &state) != RTAUDIO_NO_ERROR) {
                return false;
            }
            report.buffer_frames = buffer_frames;
            report.driver_latency_frames = std::max<long>(0, audio.getStreamLatency());
            if (audio.startStream() != RTAUDIO_NO_ERROR) {
                audio.closeStream();
                return false;
            }
            if (capture_options.stream_opened) {
                capture_options.stream_opened(report.buffer_frames, report.driver_latency_frames);
            }
            return true;
        };
        if (!open()) {
            throw std::runtime_error("Could not open ASIO input device '" +
                                     device_info.name + "' at " +
                                     std::to_string(format.sample_rate) + " Hz.");
        }
        const auto started = std::chrono::steady_clock::now();
        auto last_delivery = started;
        std::uint64_t observed_frame = 0;
        constexpr std::size_t maximum_reconnects = 5;
        std::uint64_t last_reported = 0;
        while (!stop_token.stop_requested()) {
            const auto delivered = state.next_frame.load(std::memory_order_acquire);
            const auto now = std::chrono::steady_clock::now();
            if (delivered != observed_frame) {
                observed_frame = delivered;
                last_delivery = now;
            }
            if (delivered >= state.capacity || state.rejected.load(std::memory_order_acquire)) break;
            if (!audio.isStreamRunning() || now - last_delivery > std::chrono::seconds(2)) {
                if (!report.input_lost) sink.report_input_lost();
                report.input_lost = true;
                if (audio.isStreamOpen()) audio.closeStream();
                if (report.reconnects >= maximum_reconnects) break;
                for (int wait = 0; wait < 50 && !stop_token.stop_requested(); ++wait) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (stop_token.stop_requested()) break;
                ++report.reconnects;
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
                const auto wall_frame =
                    static_cast<std::uint64_t>(elapsed * format.sample_rate);
                state.next_frame.store(std::max(delivered, std::min(wall_frame, state.capacity)),
                                       std::memory_order_release);
                if (state.next_frame.load(std::memory_order_acquire) >= state.capacity) break;
                if (open()) {
                    report.input_lost = false;
                    last_delivery = std::chrono::steady_clock::now();
                }
                continue;
            }
            if (progress_callback &&
                delivered - last_reported >= format.sample_rate / 5) {
                last_reported = delivered;
                progress_callback(static_cast<double>(delivered) / static_cast<double>(state.capacity));
            }
            if (update_callback) update_callback();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (audio.isStreamOpen()) {
            if (audio.isStreamRunning() && audio.stopStream() != RTAUDIO_NO_ERROR) {
                throw std::runtime_error("Could not stop ASIO input stream.");
            }
            audio.closeStream();
        }
        publish_report();
        if (state.rejected.load(std::memory_order_acquire)) {
            throw std::runtime_error("The incremental sink rejected ASIO audio; inspect engine diagnostics.");
        }
        return report;
    } catch (...) {
        if (audio.isStreamOpen()) {
            if (audio.isStreamRunning()) (void)audio.stopStream();
            audio.closeStream();
        }
        publish_report();
        throw;
    }
}

} // namespace

LiveCaptureReport capture_asio_stream(double seconds, live::AudioInputSink& sink,
                                    std::stop_token stop_token,
                                    const std::function<void(double)>& progress_callback,
                                    const AsioCaptureOptions& capture_options,
                                    const std::function<void()>& update_callback) {
    return capture_asio_stream_impl(seconds, sink, stop_token, progress_callback,
                                    capture_options, update_callback, nullptr);
}

AudioBuffer capture_asio(double seconds, std::stop_token stop_token,
                         const std::function<void(double)>& progress_callback,
                         const std::function<void(double, const AudioBuffer&)>&
                             audio_update_callback,
                         const RealtimeUpdateCallback& realtime_update_callback,
                         const RealtimeOptions& realtime_options) {
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 300.0) {
        throw std::invalid_argument("ASIO capture duration must be greater than 0 and at most 300 seconds.");
    }
    if (stop_token.stop_requested()) throw std::runtime_error("ASIO capture was cancelled.");

    auto owner = acquire_asio_owner();
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
    auto analysis_options = realtime_options;
    analysis_options.input_sample_rate = sample_rate;
    RealtimeTranscriber analyzer(analysis_options);
    std::atomic<bool> capture_finished{false};
    std::exception_ptr analysis_error;
    AudioBuffer captured_audio{sample_rate, {}};
    captured_audio.samples.reserve(sample_capacity);
    RtAudio::StreamParameters parameters{};
    parameters.deviceId = device;
    parameters.nChannels = 1;
    parameters.firstChannel = 0;
    unsigned int buffer_frames = 512;

    StreamGuard stream_guard{audio};
    if (audio.openStream(nullptr, &parameters, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                         &capture_callback, &state) != RTAUDIO_NO_ERROR) {
        throw std::runtime_error("Could not open default ASIO input stream.");
    }
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
    // Constructed after the worker: closes the callback before either the worker or state dies.
    StreamGuard running_guard{audio};
    if (audio.startStream() != RTAUDIO_NO_ERROR) {
        throw std::runtime_error("Could not start ASIO input stream.");
    }
    std::size_t last_reported = 0;
    std::size_t last_audio_update = 0;
    auto last_live_update = std::chrono::steady_clock::now();
    auto last_delivery = last_live_update;
    std::size_t observed_frame = 0;
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
        if (captured != observed_frame) {
            observed_frame = captured;
            last_delivery = now;
        } else if (now - last_delivery > std::chrono::seconds(2)) {
            throw std::runtime_error("ASIO input stopped delivering callbacks.");
        }
        auto realtime = analyzer.take_snapshot();
        if (realtime_update_callback &&
            (!realtime.events.empty() ||
             now - last_live_update >= std::chrono::milliseconds(50))) {
            realtime_update_callback(realtime.notes, realtime.events);
            last_live_update = now;
        }
        if (progress_callback && captured - last_reported >= sample_rate / 5) {
            progress_callback(static_cast<double>(captured) / sample_capacity);
            last_reported = captured;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (audio.isStreamOpen()) {
        if (audio.isStreamRunning() && audio.stopStream() != RTAUDIO_NO_ERROR) {
            throw std::runtime_error("Could not stop ASIO input stream.");
        }
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
        const auto realtime = analyzer.take_snapshot();
        realtime_update_callback(realtime.notes, realtime.events);
    }
    if (state.overflow.load(std::memory_order_acquire)) {
        throw std::runtime_error("ASIO capture ring buffer overflowed; audio was not lost silently.");
    }
    if (state.input_failed.load(std::memory_order_acquire) ||
        (!stop_token.stop_requested() && state.captured.load() < sample_capacity)) {
        throw std::runtime_error("ASIO input stopped or overflowed; capture is incomplete.");
    }
    if (captured_audio.samples.empty()) {
        throw std::runtime_error(stop_token.stop_requested()
                                     ? "ASIO capture was cancelled."
                                     : "ASIO capture returned no audio samples.");
    }
    return captured_audio;
}
#else
std::vector<AsioInputDevice> enumerate_asio_input_devices() { return {}; }

LiveCaptureReport capture_asio_stream(
    double, live::AudioInputSink&, std::stop_token,
    const std::function<void(double)>&, const AsioCaptureOptions&,
    const std::function<void()>&) {
    throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
}

LiveCaptureReport capture_asio_live(
    double, live::LiveTranscriptionSession&, std::stop_token,
    const std::function<void(double)>&, const AsioCaptureOptions&,
    const std::function<void()>&) {
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
