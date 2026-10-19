#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace transcribe {

struct AudioBuffer {
    std::uint32_t sample_rate{};
    std::vector<float> samples;
};

AudioBuffer read_wav(const std::filesystem::path& path);

} // namespace transcribe
