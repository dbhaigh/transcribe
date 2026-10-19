#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#ifdef TRANSCRIBE_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace transcribe {
namespace {

constexpr std::size_t window_size = 4096;
constexpr std::size_t hop_size = 512;
constexpr double silence_rms = 0.008;
constexpr double yin_threshold = 0.15;

struct PitchFrame {
    int midi_pitch{-1};
    double time_seconds{};
};

std::vector<Note> notes_from_frames(const std::vector<PitchFrame>& frames,
                                    double duration_seconds) {
    std::vector<Note> notes;
    std::size_t run_start = 0;
    while (run_start < frames.size()) {
        if (frames[run_start].midi_pitch < 0) {
            ++run_start;
            continue;
        }
        std::size_t run_end = run_start + 1;
        while (run_end < frames.size() &&
               frames[run_end].midi_pitch == frames[run_start].midi_pitch) {
            ++run_end;
        }
        const double start_time = frames[run_start].time_seconds;
        const double end_time = run_end < frames.size()
                                    ? frames[run_end].time_seconds
                                    : duration_seconds;
        if (end_time - start_time >= 0.07) {
            notes.push_back({frames[run_start].midi_pitch, start_time, end_time - start_time,
                             midi_note_name(frames[run_start].midi_pitch)});
        }
        run_start = run_end;
    }
    return notes;
}

int detect_pitch(const float* samples, std::uint32_t sample_rate) {
    double energy = 0.0;
    for (std::size_t i = 0; i < window_size; ++i) {
        energy += static_cast<double>(samples[i]) * samples[i];
    }
    if (std::sqrt(energy / window_size) < silence_rms) {
        return -1;
    }

    const auto min_lag = std::max<std::size_t>(2, sample_rate / 1000);
    const auto max_lag = std::min<std::size_t>(window_size / 2, sample_rate / 65);
    std::vector<double> difference(max_lag + 1, 0.0);
    std::vector<double> cumulative_mean(max_lag + 1, 1.0);
    for (std::size_t lag = 1; lag <= max_lag; ++lag) {
        for (std::size_t i = 0; i < window_size - lag; ++i) {
            const double delta = static_cast<double>(samples[i]) - samples[i + lag];
            difference[lag] += delta * delta;
        }
    }

    double running_sum = 0.0;
    for (std::size_t lag = 1; lag <= max_lag; ++lag) {
        running_sum += difference[lag];
        cumulative_mean[lag] = running_sum > 0.0
                                   ? difference[lag] * lag / running_sum
                                   : 1.0;
    }

    std::size_t selected = max_lag;
    for (std::size_t lag = min_lag; lag <= max_lag; ++lag) {
        if (cumulative_mean[lag] < yin_threshold) {
            while (lag + 1 <= max_lag && cumulative_mean[lag + 1] < cumulative_mean[lag]) {
                ++lag;
            }
            selected = lag;
            break;
        }
    }
    if (selected == max_lag && cumulative_mean[selected] >= yin_threshold) {
        return -1;
    }

    const double frequency = static_cast<double>(sample_rate) / selected;
    const int midi_pitch = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(frequency / 440.0)));
    return midi_pitch >= 28 && midi_pitch <= 96 ? midi_pitch : -1;
}

} // namespace

std::string midi_note_name(int midi_pitch) {
    static constexpr std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int pitch_class = ((midi_pitch % 12) + 12) % 12;
    return std::string(names[static_cast<std::size_t>(pitch_class)]) +
           std::to_string(midi_pitch / 12 - 1);
}

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio) {
    if (audio.sample_rate < 2000) {
        throw std::invalid_argument("Audio sample rate must be at least 2000 Hz.");
    }
    if (audio.samples.size() < window_size) {
        return {};
    }

    std::vector<PitchFrame> frames;
    for (std::size_t start = 0; start + window_size <= audio.samples.size(); start += hop_size) {
        frames.push_back({detect_pitch(audio.samples.data() + start, audio.sample_rate),
                          static_cast<double>(start) / audio.sample_rate});
    }

    return notes_from_frames(frames, static_cast<double>(audio.samples.size()) / audio.sample_rate);
}

#ifdef TRANSCRIBE_ENABLE_ONNX
std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path) {
    if (audio.sample_rate < 2000) {
        throw std::invalid_argument("Audio sample rate must be at least 2000 Hz.");
    }
    if (audio.samples.empty()) {
        throw std::invalid_argument("Audio input is empty.");
    }

    constexpr std::uint32_t model_sample_rate = 16000;
    constexpr std::size_t class_count = 70;
    constexpr std::size_t silence_class = 69;
    constexpr double frame_step_seconds = 0.01;

    std::vector<float> model_input;
    const double output_count = static_cast<double>(audio.samples.size()) *
                                model_sample_rate / audio.sample_rate;
    model_input.reserve(static_cast<std::size_t>(std::ceil(output_count)));
    for (std::size_t index = 0; index < static_cast<std::size_t>(std::ceil(output_count)); ++index) {
        const double source_position = static_cast<double>(index) * audio.sample_rate /
                                       model_sample_rate;
        const auto left = std::min(static_cast<std::size_t>(source_position),
                                   audio.samples.size() - 1);
        const auto right = std::min(left + 1, audio.samples.size() - 1);
        const float fraction = static_cast<float>(source_position - left);
        model_input.push_back(audio.samples[left] * (1.0f - fraction) +
                              audio.samples[right] * fraction);
    }

    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "transcribe");
    Ort::SessionOptions session_options;
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(environment, model_path.c_str(), session_options);
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session.GetInputNameAllocated(0, allocator);
    auto output_name = session.GetOutputNameAllocated(0, allocator);

    const std::array<std::int64_t, 2> input_shape{
        1, static_cast<std::int64_t>(model_input.size())};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, model_input.data(), model_input.size(), input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name.get()};
    const char* output_names[] = {output_name.get()};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1,
                               output_names, 1);
    if (outputs.empty() || !outputs.front().IsTensor()) {
        throw std::runtime_error("ONNX model did not return a tensor output.");
    }

    const auto output_info = outputs.front().GetTensorTypeAndShapeInfo();
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("ONNX model output must use float32 logits.");
    }
    const auto shape = output_info.GetShape();
    if (shape.size() != 3 || shape[0] != 1 ||
        shape[2] != static_cast<std::int64_t>(class_count) || shape[1] <= 0) {
        throw std::runtime_error("ONNX output must have shape [1, frames, 70].");
    }
    const float* logits = outputs.front().GetTensorData<float>();
    const std::size_t frame_count = static_cast<std::size_t>(shape[1]);
    std::vector<PitchFrame> frames;
    frames.reserve(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        std::size_t best_class = 0;
        for (std::size_t pitch = 1; pitch < class_count; ++pitch) {
            if (logits[frame * class_count + pitch] >
                logits[frame * class_count + best_class]) {
                best_class = pitch;
            }
        }
        const int midi_pitch = best_class == silence_class
                                   ? -1
                                   : static_cast<int>(best_class) + 28;
        frames.push_back({midi_pitch, frame * frame_step_seconds});
    }
    return notes_from_frames(frames, static_cast<double>(audio.samples.size()) / audio.sample_rate);
}
#endif

} // namespace transcribe
