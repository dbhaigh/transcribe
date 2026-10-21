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
    std::uint64_t frames_delivered{};
    // Blocks the session refused (queue full); their frames become explicit gaps.
    std::uint64_t blocks_refused{};
    std::uint64_t driver_overflows{};
    std::size_t reconnects{};
    bool input_lost{};
};

// Streams the selected ASIO input into a started LiveTranscriptionSession. The RtAudio callback
// only timestamps blocks and calls session.push(); it never analyses, allocates or blocks. If the
// stream stops unexpectedly, the session is told the input was lost and the selected device is
// reopened (bounded retries) with frame numbers advanced by wall time so the gap is explicit.
// The session format must match capture_options. The caller finishes the session afterwards.
LiveCaptureReport capture_asio_live(double seconds, live::LiveTranscriptionSession& session,
                                    std::stop_token stop_token = {},
                                    const std::function<void(double)>& progress_callback = {},
                                    const AsioCaptureOptions& capture_options = {},
                                    const std::function<void()>& update_callback = {});

} // namespace transcribe
