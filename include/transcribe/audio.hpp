#pragma once

#include <cstdint>
#include <filesystem>
#include <atomic>
#include <functional>
#include <span>
#include <vector>

namespace transcribe {

struct AudioBuffer {
    std::uint32_t sample_rate{};
    std::vector<float> samples;
};

struct AudioBlockView {
    std::uint32_t sample_rate{};
    std::size_t first_frame{};
    std::size_t total_frames{};
    std::span<const float> samples;
};

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t capacity);

    std::size_t write(std::span<const float> samples) noexcept;
    std::size_t read(std::span<float> samples) noexcept;
    std::size_t readable() const noexcept;

private:
    std::vector<float> storage_;
    std::atomic<std::size_t> write_position_{0};
    std::atomic<std::size_t> read_position_{0};
};

AudioBuffer read_wav(const std::filesystem::path& path);
void read_wav_blocks(const std::filesystem::path& path, std::size_t block_frames,
                     const std::function<void(const AudioBlockView&)>& block_callback);
AudioBuffer read_audio(const std::filesystem::path& path);

} // namespace transcribe
