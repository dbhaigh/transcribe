#pragma once

#include "transcribe/live.hpp"
#include "transcribe/transcriber.hpp"

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace transcribe {

struct AsioInputDevice {
    unsigned int device_id{};
    std::string name;
    unsigned int input_channels{};
    unsigned int preferred_sample_rate{};
    bool default_input{};
    std::vector<unsigned int> sample_rates;
};

struct AsioCaptureOptions {
    std::optional<unsigned int> device_id;
    std::uint32_t sample_rate{44100};
    std::uint16_t channels{1};
    // Owner/control thread after each successful open, never the audio callback.
    std::function<void(std::uint32_t buffer_frames, std::uint64_t driver_latency_frames)> stream_opened;
    live::ChannelLayout layout{live::ChannelLayout::DiscreteInterleaved};
    std::uint64_t stream_epoch{};
};

std::vector<AsioInputDevice> enumerate_asio_input_devices();

using RealtimeUpdateCallback =
    std::function<void(const std::vector<Note>&, const std::vector<NoteEvent>&)>;

AudioBuffer capture_asio(double seconds, std::stop_token stop_token = {},
                         const std::function<void(double)>& progress_callback = {},
                         const std::function<void(double, const AudioBuffer&)>&
                             audio_update_callback = {},
                         const RealtimeUpdateCallback& realtime_update_callback = {},
                         const RealtimeOptions& realtime_options = {});

struct LiveCaptureReport {
    // Timeline end (includes refused/null input and reconnect gaps), retained for compatibility.
    std::uint64_t frames_delivered{};
    // Blocks the session refused (queue full); their frames become explicit gaps.
    std::uint64_t blocks_refused{};
    std::uint64_t driver_overflows{};
    std::size_t reconnects{};
    bool input_lost{};
    std::uint64_t frames_accepted{};
    std::uint64_t null_input_frames{};
    std::uint32_t buffer_frames{};
    std::uint64_t driver_latency_frames{};
};

// Streams the selected ASIO input into a started LiveTranscriptionSession. The RtAudio callback
// only timestamps blocks and calls session.push(); it never analyses, allocates or blocks. If the
// stream stops unexpectedly, the session is told the input was lost and the selected device is
// reopened (bounded retries) with frame numbers advanced by wall time so the gap is explicit.
// The session format must match capture_options. Input origin 0 is declared before capture and
// the timeline is sealed after callbacks stop, including when capture exits with an error.
LiveCaptureReport capture_asio_live(double seconds, live::LiveTranscriptionSession& session,
                                    std::stop_token stop_token = {},
                                    const std::function<void(double)>& progress_callback = {},
                                    const AsioCaptureOptions& capture_options = {},
                                    const std::function<void()>& update_callback = {});

// Engine-compatible sink. Its push() is called by the RtAudio callback and must be
// single-producer realtime-safe: no locks, allocation, blocking or analysis. Caller
// begins the stream at frame zero before capture, then finishes it with
// report.frames_delivered. update_callback runs on the control thread.
LiveCaptureReport capture_asio_stream(double seconds, live::AudioInputSink& sink,
                                    std::stop_token stop_token = {},
                                    const std::function<void(double)>& progress_callback = {},
                                    const AsioCaptureOptions& capture_options = {},
                                    const std::function<void()>& update_callback = {});

} // namespace transcribe
