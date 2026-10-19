#pragma once

#include "transcribe/audio.hpp"

#include <stop_token>

namespace transcribe {

AudioBuffer capture_asio(double seconds, std::stop_token stop_token = {});

} // namespace transcribe
