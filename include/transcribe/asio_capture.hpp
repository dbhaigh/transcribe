#pragma once

#include "transcribe/transcriber.hpp"

#include <functional>
#include <stop_token>

namespace transcribe {

using RealtimeUpdateCallback =
    std::function<void(const std::vector<Note>&, const std::vector<NoteEvent>&)>;

AudioBuffer capture_asio(double seconds, std::stop_token stop_token = {},
                         const std::function<void(double)>& progress_callback = {},
                         const std::function<void(double, const AudioBuffer&)>&
                             audio_update_callback = {},
                         const RealtimeUpdateCallback& realtime_update_callback = {},
                         const RealtimeOptions& realtime_options = {});

} // namespace transcribe
