#include "transcribe/audio.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace transcribe {
namespace {

std::uint16_t read_u16(std::istream& input) {
    std::array<unsigned char, 2> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t read_u32(std::istream& input) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string read_fourcc(std::istream& input) {
    std::array<char, 4> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }
    return {bytes.data(), bytes.size()};
}

struct Format {
    std::uint16_t encoding{};
    std::uint16_t channels{};
    std::uint32_t sample_rate{};
    std::uint16_t block_align{};
    std::uint16_t bits_per_sample{};
};

float decode_sample(const unsigned char* data, std::uint16_t encoding, std::uint16_t bits) {
    if (encoding == 3 && bits == 32) {
        const std::uint32_t raw = static_cast<std::uint32_t>(data[0]) |
                                  (static_cast<std::uint32_t>(data[1]) << 8) |
                                  (static_cast<std::uint32_t>(data[2]) << 16) |
                                  (static_cast<std::uint32_t>(data[3]) << 24);
        const float value = std::bit_cast<float>(raw);
        return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    }

    if (encoding != 1) {
        throw std::runtime_error("Only PCM and 32-bit IEEE float WAV files are supported.");
    }
    if (bits == 8) {
        return (static_cast<float>(data[0]) - 128.0f) / 128.0f;
    }
    if (bits == 16) {
        const auto raw = static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
        return static_cast<float>(std::bit_cast<std::int16_t>(raw)) / 32768.0f;
    }
    if (bits == 24) {
        std::int32_t value = static_cast<std::int32_t>(data[0]) |
                             (static_cast<std::int32_t>(data[1]) << 8) |
                             (static_cast<std::int32_t>(data[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xff000000);
        }
        return static_cast<float>(value) / 8388608.0f;
    }
    if (bits == 32) {
        const std::uint32_t raw = static_cast<std::uint32_t>(data[0]) |
                                  (static_cast<std::uint32_t>(data[1]) << 8) |
                                  (static_cast<std::uint32_t>(data[2]) << 16) |
                                  (static_cast<std::uint32_t>(data[3]) << 24);
        return static_cast<float>(std::bit_cast<std::int32_t>(raw)) / 2147483648.0f;
    }
    throw std::runtime_error("Unsupported WAV sample width.");
}

} // namespace

#ifdef TRANSCRIBE_ENABLE_FFMPEG
AudioBuffer read_audio_ffmpeg(const std::filesystem::path& path);
#endif

AudioBuffer read_audio(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (extension == ".wav") {
        return read_wav(path);
    }
#ifdef TRANSCRIBE_ENABLE_FFMPEG
    return read_audio_ffmpeg(path);
#else
    throw std::runtime_error(
        "Unsupported audio format. This build reads WAV; enable TRANSCRIBE_ENABLE_FFMPEG for other formats.");
#endif
}

AudioBuffer read_wav(const std::filesystem::path& path) {
    AudioBuffer audio;
    read_wav_blocks(path, 4096, [&audio](const AudioBlockView& block) {
        audio.sample_rate = block.sample_rate;
        audio.samples.insert(audio.samples.end(), block.samples.begin(), block.samples.end());
    });
    return audio;
}

void read_wav_blocks(
    const std::filesystem::path& path, std::size_t block_frames,
    const std::function<void(const AudioBlockView&)>& block_callback) {
    if (block_frames == 0 || !block_callback) {
        throw std::invalid_argument("WAV block size and callback must be valid.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open input file: " + path.string());
    }
    if (read_fourcc(input) != "RIFF") {
        throw std::runtime_error("Input is not a RIFF/WAVE file.");
    }
    (void)read_u32(input);
    if (read_fourcc(input) != "WAVE") {
        throw std::runtime_error("Input is not a RIFF/WAVE file.");
    }

    Format format{};
    bool have_format = false;
    bool have_data = false;
    std::streampos data_start{};
    std::uint32_t data_size = 0;
    while (input && !(have_format && have_data)) {
        const std::string chunk = read_fourcc(input);
        const std::uint32_t size = read_u32(input);
        const auto chunk_start = input.tellg();

        if (chunk == "fmt ") {
            if (size < 16) {
                throw std::runtime_error("Malformed WAV format chunk.");
            }
            format.encoding = read_u16(input);
            format.channels = read_u16(input);
            format.sample_rate = read_u32(input);
            (void)read_u32(input);
            format.block_align = read_u16(input);
            format.bits_per_sample = read_u16(input);
            if (format.encoding == 0xfffe) {
                throw std::runtime_error("WAV extensible encoding is not supported.");
            }
            have_format = true;
        } else if (chunk == "data") {
            data_start = chunk_start;
            data_size = size;
            have_data = true;
        }

        if (have_format && have_data) {
            break;
        }
        input.clear();
        input.seekg(chunk_start + static_cast<std::streamoff>(size + (size & 1U)));
    }

    if (!have_format || !have_data || format.channels == 0 || format.sample_rate == 0 ||
        format.block_align == 0 || data_size % format.block_align != 0) {
        throw std::runtime_error("WAV file has missing or inconsistent format/data chunks.");
    }
    const std::size_t bytes_per_sample = (format.bits_per_sample + 7U) / 8U;
    if (bytes_per_sample * format.channels > format.block_align) {
        throw std::runtime_error("WAV block alignment is smaller than its sample data.");
    }
    const auto maximum_block =
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) /
        format.block_align;
    if (block_frames > maximum_block ||
        block_frames > std::numeric_limits<std::size_t>::max() / format.block_align) {
        throw std::length_error("Requested WAV read block is too large.");
    }

    const std::size_t frames = data_size / format.block_align;
    input.clear();
    input.seekg(data_start);
    if (frames == 0) {
        block_callback({format.sample_rate, 0, 0, {}});
        return;
    }
    std::vector<unsigned char> chunk(block_frames * format.block_align);
    std::size_t frames_read = 0;
    while (frames_read < frames) {
        const std::size_t count = std::min(block_frames, frames - frames_read);
        const auto byte_count = count * format.block_align;
        input.read(reinterpret_cast<char*>(chunk.data()),
                   static_cast<std::streamsize>(byte_count));
        if (!input) {
            throw std::runtime_error("WAV data chunk is truncated.");
        }
        std::vector<float> decoded;
        decoded.reserve(count);
        for (std::size_t frame = 0; frame < count; ++frame) {
            float sum = 0.0f;
            for (std::uint16_t channel = 0; channel < format.channels; ++channel) {
                const auto offset = frame * format.block_align + channel * bytes_per_sample;
                sum += decode_sample(chunk.data() + offset, format.encoding,
                                     format.bits_per_sample);
            }
            decoded.push_back(sum / format.channels);
        }
        block_callback({format.sample_rate, frames_read, frames, decoded});
        frames_read += count;
    }
}

} // namespace transcribe
