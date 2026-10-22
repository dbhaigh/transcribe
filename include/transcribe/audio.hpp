#pragma once

#include <cstdint>
#include <filesystem>
#include <atomic>
#include <functional>
#include <span>
#include <stop_token>
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

struct WavInfo {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::size_t frames{};
};

// Validates the same RIFF/format/data boundaries as read_wav_blocks, without decoding samples.
WavInfo inspect_wav(const std::filesystem::path& path);

class AudioRingBuffer {
public:
    // One writer, one reader. readable() is a consumer-side snapshot, not a third observer API.
    explicit AudioRingBuffer(std::size_t capacity);

    std::size_t write(std::span<const float> samples) noexcept;
    std::size_t read(std::span<float> samples) noexcept;
    std::size_t readable() const noexcept;
    // Producer-side snapshot. A failed capacity check never consumes samples.
    std::size_t writable() const noexcept;

private:
    std::vector<float> storage_;
    std::atomic<std::size_t> write_position_{0};
    std::atomic<std::size_t> read_position_{0};
    std::size_t write_offset_{};
    std::size_t read_offset_{};
};

AudioBuffer read_wav(const std::filesystem::path& path);
AudioBuffer read_wav(const std::filesystem::path& path, std::stop_token cancellation);
void read_wav_blocks(const std::filesystem::path& path, std::size_t block_frames,
                     const std::function<void(const AudioBlockView&)>& block_callback);
AudioBuffer read_audio(const std::filesystem::path& path);
AudioBuffer read_audio(const std::filesystem::path& path, std::stop_token cancellation);

} // namespace transcribe
