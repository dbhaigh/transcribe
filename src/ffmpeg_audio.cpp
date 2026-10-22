#include "transcribe/audio.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace transcribe {
namespace {

struct FormatDeleter {
    void operator()(AVFormatContext* context) const {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct CodecDeleter {
    void operator()(AVCodecContext* context) const {
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
    }
};

struct ResamplerDeleter {
    void operator()(SwrContext* context) const {
        if (context != nullptr) {
            swr_free(&context);
        }
    }
};

[[noreturn]] void throw_ffmpeg_error(const std::string& operation, int error_code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(error_code, message.data(), message.size());
    throw std::runtime_error(operation + ": " + message.data());
}

} // namespace

AudioBuffer read_audio_ffmpeg(const std::filesystem::path& path, std::stop_token cancellation) {
    const auto check_cancelled = [&] {
        if (cancellation.stop_requested()) throw std::runtime_error("FFmpeg decoding cancelled.");
    };
    check_cancelled();
    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) throw std::runtime_error("Could not allocate FFmpeg input context.");
    raw_format->interrupt_callback = {
        [](void* token) { return static_cast<std::stop_token*>(token)->stop_requested() ? 1 : 0; },
        &cancellation};
    const auto encoded_path = path.u8string();
    const std::string input_path(encoded_path.begin(), encoded_path.end());
    int result = avformat_open_input(&raw_format, input_path.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw_ffmpeg_error("Could not open audio input", result);
    }
    std::unique_ptr<AVFormatContext, FormatDeleter> format(raw_format);
    result = avformat_find_stream_info(format.get(), nullptr);
    if (result < 0) {
        throw_ffmpeg_error("Could not read audio stream information", result);
    }

    const int stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1,
                                                 nullptr, 0);
    if (stream_index < 0) {
        throw_ffmpeg_error("No audio stream was found", stream_index);
    }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        throw std::runtime_error("No FFmpeg decoder is available for this audio codec.");
    }
    std::unique_ptr<AVCodecContext, CodecDeleter> codec_context(
        avcodec_alloc_context3(codec));
    if (!codec_context) {
        throw std::runtime_error("Could not allocate an FFmpeg decoder context.");
    }
    result = avcodec_parameters_to_context(codec_context.get(), stream->codecpar);
    if (result < 0) {
        throw_ffmpeg_error("Could not configure the FFmpeg decoder", result);
    }
    result = avcodec_open2(codec_context.get(), codec, nullptr);
    if (result < 0) {
        throw_ffmpeg_error("Could not start the FFmpeg decoder", result);
    }

    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_MONO;
    AVChannelLayout input_layout{};
    result = av_channel_layout_copy(&input_layout, &codec_context->ch_layout);
    if (result < 0) {
        throw_ffmpeg_error("Could not read the input channel layout", result);
    }
    if (input_layout.nb_channels == 0) {
        const int channels = stream->codecpar->ch_layout.nb_channels;
        if (channels <= 0) {
            av_channel_layout_uninit(&input_layout);
            throw std::runtime_error("The audio stream does not specify its channel count.");
        }
        av_channel_layout_default(&input_layout, channels);
    }
    SwrContext* raw_resampler = nullptr;
    constexpr int output_sample_rate = 44100;
    result = swr_alloc_set_opts2(&raw_resampler, &output_layout, AV_SAMPLE_FMT_FLT,
                                 output_sample_rate, &input_layout,
                                 codec_context->sample_fmt, codec_context->sample_rate,
                                 0, nullptr);
    av_channel_layout_uninit(&input_layout);
    std::unique_ptr<SwrContext, ResamplerDeleter> resampler(raw_resampler);
    if (result < 0) {
        throw_ffmpeg_error("Could not configure audio resampling", result);
    }
    result = swr_init(resampler.get());
    if (result < 0) {
        throw_ffmpeg_error("Could not initialize audio resampling", result);
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        throw std::runtime_error("Could not allocate FFmpeg decode buffers.");
    }
    AudioBuffer audio{output_sample_rate, {}};
    const auto receive_frames = [&]() {
        while (true) {
            check_cancelled();
            const int receive_result = avcodec_receive_frame(codec_context.get(), frame.get());
            if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
                return;
            }
            if (receive_result < 0) {
                throw_ffmpeg_error("Audio decoding failed", receive_result);
            }
            const int capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(resampler.get(), codec_context->sample_rate) + frame->nb_samples,
                output_sample_rate, codec_context->sample_rate, AV_ROUND_UP));
            std::vector<float> converted(static_cast<std::size_t>(capacity));
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(converted.data())};
            const int converted_samples = swr_convert(
                resampler.get(), output, capacity,
                const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
            if (converted_samples < 0) {
                throw_ffmpeg_error("Audio resampling failed", converted_samples);
            }
            audio.samples.insert(audio.samples.end(), converted.begin(),
                                 converted.begin() + converted_samples);
            av_frame_unref(frame.get());
        }
    };

    while ((result = av_read_frame(format.get(), packet.get())) >= 0) {
        check_cancelled();
        if (packet->stream_index == stream_index) {
            result = avcodec_send_packet(codec_context.get(), packet.get());
            if (result < 0) {
                av_packet_unref(packet.get());
                throw_ffmpeg_error("Could not submit audio to the decoder", result);
            }
            receive_frames();
        }
        av_packet_unref(packet.get());
    }
    if (result != AVERROR_EOF) {
        throw_ffmpeg_error("Could not read audio packets", result);
    }
    result = avcodec_send_packet(codec_context.get(), nullptr);
    if (result < 0 && result != AVERROR_EOF) {
        throw_ffmpeg_error("Could not flush the audio decoder", result);
    }
    receive_frames();

    while (true) {
        check_cancelled();
        const int capacity = swr_get_out_samples(resampler.get(), 0);
        if (capacity <= 0) {
            break;
        }
        std::vector<float> converted(static_cast<std::size_t>(capacity));
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(converted.data())};
        const int converted_samples =
            swr_convert(resampler.get(), output, capacity, nullptr, 0);
        if (converted_samples < 0) {
            throw_ffmpeg_error("Could not flush audio resampling", converted_samples);
        }
        if (converted_samples == 0) {
            break;
        }
        audio.samples.insert(audio.samples.end(), converted.begin(),
                             converted.begin() + converted_samples);
    }
    if (audio.samples.empty()) {
        throw std::runtime_error("FFmpeg decoded no audio samples.");
    }
    return audio;
}

} // namespace transcribe
