#pragma once

#include "transcribe/audio.hpp"

#include <functional>
#include <stop_token>

namespace transcribe {

AudioBuffer capture_asio(double seconds, std::stop_token stop_token = {},
                         const std::function<void(double)>& progress_callback = {},
                         const std::function<void(double, const AudioBuffer&)>&
                             audio_update_callback = {});

} // namespace transcribe
