#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#ifdef TRANSCRIBE_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace transcribe {
namespace {

constexpr std::size_t analysis_window = 2048;
constexpr std::size_t short_window = 1024;
constexpr std::size_t analysis_hop = 256;
constexpr std::uint32_t analysis_rate = 11025;
constexpr std::size_t onset_window = 1024;
constexpr std::size_t onset_hop = 256;
constexpr std::array<int, 12> midi_to_pitch_class{
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

double clamp_probability(double value) {
    return std::clamp(value, 0.0, 1.0);
}

struct YinWorkspace {
    std::vector<double> difference;
    std::vector<double> cumulative;
};

PitchEstimate yin_with_workspace(std::span<const float> samples, std::uint32_t sample_rate,
                                 double threshold, YinWorkspace& workspace) {
    if (sample_rate < 2000 || samples.size() < 64) {
        return {};
    }
    double energy = 0.0;
    for (const float sample : samples) {
        energy += static_cast<double>(sample) * sample;
    }
    const double rms = std::sqrt(energy / samples.size());
    if (rms < 0.0015) {
        return {};
    }

    const std::size_t min_lag = std::max<std::size_t>(2, sample_rate / 1000);
    const std::size_t max_lag =
        std::min<std::size_t>(samples.size() / 2, sample_rate / 65);
    if (min_lag > max_lag) {
        return {};
    }
    workspace.difference.assign(max_lag + 1, 0.0);
    workspace.cumulative.assign(max_lag + 1, 1.0);
    for (std::size_t lag = 1; lag <= max_lag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < samples.size(); ++i) {
            const double delta = static_cast<double>(samples[i]) - samples[i + lag];
            sum += delta * delta;
        }
        workspace.difference[lag] = sum;
    }

    double running_sum = 0.0;
    for (std::size_t lag = 1; lag <= max_lag; ++lag) {
        running_sum += workspace.difference[lag];
        const double value = running_sum > 0.0
                                 ? workspace.difference[lag] * lag / running_sum
                                 : 1.0;
        workspace.cumulative[lag] = value;
    }

    double best_value = std::numeric_limits<double>::infinity();
    std::size_t best_lag = 0;
    for (std::size_t lag = min_lag; lag <= max_lag; ++lag) {
        const double value = workspace.cumulative[lag];
        if (lag >= min_lag && value < best_value) {
            best_value = value;
            best_lag = lag;
        }
        if (lag >= min_lag && value < threshold) {
            while (lag + 1 <= max_lag &&
                   workspace.cumulative[lag + 1] < workspace.cumulative[lag]) {
                ++lag;
            }
            best_lag = lag;
            best_value = workspace.cumulative[lag];
            break;
        }
    }
    if (best_lag == 0 || best_value > threshold) {
        return {};
    }

    double refined_lag = static_cast<double>(best_lag);
    if (best_lag > min_lag && best_lag < max_lag) {
        const double before = workspace.cumulative[best_lag - 1];
        const double center = workspace.cumulative[best_lag];
        const double after = workspace.cumulative[best_lag + 1];
        const double denominator = before - 2.0 * center + after;
        if (std::abs(denominator) > 1e-9) {
            refined_lag += std::clamp(0.5 * (before - after) / denominator, -0.5, 0.5);
        }
    }
    const double frequency = sample_rate / refined_lag;
    const int midi_pitch =
        static_cast<int>(std::lround(69.0 + 12.0 * std::log2(frequency / 440.0)));
    if (midi_pitch < 28 || midi_pitch > 96) {
        return {};
    }

    const double confidence = clamp_probability(1.0 - best_value);
    const double voicing_probability =
        clamp_probability((threshold + 0.12 - best_value) / 0.12);
    return {midi_pitch, confidence, voicing_probability};
}

PitchEstimate multi_resolution_yin(std::span<const float> samples,
                                   std::uint32_t sample_rate, double threshold,
                                   YinWorkspace& workspace) {
    if (samples.size() < analysis_window) {
        return yin_with_workspace(samples, sample_rate, threshold, workspace);
    }
    const auto long_result =
        yin_with_workspace(samples.first(analysis_window), sample_rate, threshold, workspace);
    const auto short_start = (analysis_window - short_window) / 2;
    const auto short_result = yin_with_workspace(
        samples.subspan(short_start, short_window), sample_rate, threshold, workspace);
    if (long_result.midi_pitch < 0) {
        return short_result;
    }
    if (short_result.midi_pitch < 0) {
        return long_result;
    }
    if (std::abs(short_result.midi_pitch - long_result.midi_pitch) <= 1) {
        return short_result.confidence > long_result.confidence ? short_result : long_result;
    }
    if (short_result.confidence > long_result.confidence + 0.12 &&
        short_result.midi_pitch >= 45) {
        return short_result;
    }
    return long_result;
}

AudioBuffer resample_linear(const AudioBuffer& source, std::uint32_t destination_rate) {
    if (source.sample_rate == destination_rate) {
        return source;
    }
    if (source.samples.empty()) {
        return {destination_rate, {}};
    }
    const auto output_size = static_cast<std::size_t>(
        std::ceil(static_cast<double>(source.samples.size()) * destination_rate /
                  source.sample_rate));
    AudioBuffer result{destination_rate, {}};
    result.samples.reserve(output_size);
    for (std::size_t i = 0; i < output_size; ++i) {
        const double position = static_cast<double>(i) * source.sample_rate / destination_rate;
        const auto left = std::min(static_cast<std::size_t>(position), source.samples.size() - 1);
        const auto right = std::min(left + 1, source.samples.size() - 1);
        const float fraction = static_cast<float>(position - left);
        result.samples.push_back(source.samples[left] * (1.0f - fraction) +
                                 source.samples[right] * fraction);
    }
    return result;
}

void fft(std::vector<std::complex<double>>& values) {
    const std::size_t size = values.size();
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        std::size_t bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(values[i], values[j]);
        }
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / length;
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < size; offset += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t i = 0; i < length / 2; ++i) {
                const auto even = values[offset + i];
                const auto odd = values[offset + i + length / 2] * factor;
                values[offset + i] = even + odd;
                values[offset + i + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
}

std::vector<double> histogram_for_notes(const std::vector<Note>& notes) {
    std::vector<double> histogram(12, 0.0);
    for (const auto& note : notes) {
        const auto index = static_cast<std::size_t>(midi_to_pitch_class[
            static_cast<std::size_t>((note.midi_pitch % 12 + 12) % 12)]);
        histogram[index] += std::max(0.02, note.duration_seconds) *
                            std::max(0.05, note.confidence);
    }
    return histogram;
}

int fifths_for_tonic(int tonic) {
    static constexpr std::array<int, 12> fifths_by_tonic{
        0, 7, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};
    int fifths = fifths_by_tonic[static_cast<std::size_t>(tonic)];
    if (fifths > 6) {
        fifths -= 12;
    }
    return fifths;
}

#ifdef TRANSCRIBE_ENABLE_ONNX
Ort::Env& onnx_environment() {
    static Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "transcribe");
    return environment;
}

std::shared_ptr<Ort::Session> load_onnx_session(const std::filesystem::path& path) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::wstring, std::shared_ptr<Ort::Session>> cache;
    const auto key = path.lexically_normal().wstring();
    std::lock_guard lock(cache_mutex);
    const auto existing = cache.find(key);
    if (existing != cache.end()) {
        return existing->second;
    }
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    auto session =
        std::make_shared<Ort::Session>(onnx_environment(), path.c_str(), options);
    cache.emplace(key, session);
    return session;
}

double sigmoid(double value) {
    if (value >= 0.0) {
        return 1.0 / (1.0 + std::exp(-value));
    }
    const double exponent = std::exp(value);
    return exponent / (1.0 + exponent);
}

std::vector<double> mono_probabilities(const float* values, std::size_t count,
                                       bool already_probabilities) {
    std::vector<double> probabilities(count);
    if (already_probabilities) {
        std::transform(values, values + count, probabilities.begin(),
                       [](float value) { return clamp_probability(value); });
        return probabilities;
    }
    const double maximum = *std::max_element(values, values + count);
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        probabilities[i] = std::exp(static_cast<double>(values[i]) - maximum);
        sum += probabilities[i];
    }
    for (double& probability : probabilities) {
        probability /= sum;
    }
    return probabilities;
}

std::vector<int> greedy_peaks(const float* values, std::size_t pitch_count,
                              bool already_probabilities, double threshold,
                              std::size_t maximum_pitches) {
    struct Peak {
        int pitch;
        double probability;
    };
    std::vector<Peak> peaks;
    for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
        const double probability = already_probabilities
                                       ? clamp_probability(values[pitch])
                                       : sigmoid(values[pitch]);
        const double left = pitch == 0
                                ? -std::numeric_limits<double>::infinity()
                                : (already_probabilities
                                       ? clamp_probability(values[pitch - 1])
                                       : sigmoid(values[pitch - 1]));
        const double right = pitch + 1 == pitch_count
                                 ? -std::numeric_limits<double>::infinity()
                                 : (already_probabilities
                                        ? clamp_probability(values[pitch + 1])
                                        : sigmoid(values[pitch + 1]));
        if (probability >= threshold && probability >= left && probability >= right) {
            peaks.push_back({static_cast<int>(pitch), probability});
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        return left.probability > right.probability;
    });
    std::vector<int> selected;
    for (const auto& peak : peaks) {
        const bool adjacent = std::any_of(selected.begin(), selected.end(),
                                          [&peak](int pitch) {
                                              return std::abs(pitch - peak.pitch) <= 1;
                                          });
        if (!adjacent) {
            selected.push_back(peak.pitch);
            if (selected.size() >= maximum_pitches) {
                break;
            }
        }
    }
    return selected;
}
#endif

} // namespace

std::string midi_note_name(int midi_pitch) {
    static constexpr std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int pitch_class = ((midi_pitch % 12) + 12) % 12;
    return std::string(names[static_cast<std::size_t>(pitch_class)]) +
           std::to_string(midi_pitch / 12 - 1);
}

PitchEstimate yin_pitch_estimate(std::span<const float> samples, std::uint32_t sample_rate,
                                 double threshold) {
    YinWorkspace workspace;
    return yin_with_workspace(samples, sample_rate, std::clamp(threshold, 0.02, 0.5),
                              workspace);
}

AudioBuffer preprocess_audio(const AudioBuffer& audio, double high_pass_hz) {
    if (audio.sample_rate < 2000) {
        throw std::invalid_argument("Audio sample rate must be at least 2000 Hz.");
    }
    if (!(high_pass_hz > 0.0 && high_pass_hz < audio.sample_rate / 2.0)) {
        throw std::invalid_argument("High-pass cutoff must be between 0 and Nyquist.");
    }
    AudioBuffer filtered{audio.sample_rate, std::vector<float>(audio.samples.size())};
    const double alpha = 1.0 / (1.0 + 2.0 * std::numbers::pi * high_pass_hz / audio.sample_rate);
    double previous_input = 0.0;
    double previous_output = 0.0;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double input = audio.samples[i];
        const double output = alpha * (previous_output + input - previous_input);
        filtered.samples[i] = static_cast<float>(output);
        previous_input = input;
        previous_output = output;
    }

    constexpr std::size_t gate_window = 1024;
    std::vector<double> block_rms;
    for (std::size_t start = 0; start < filtered.samples.size(); start += gate_window) {
        const std::size_t end = std::min(start + gate_window, filtered.samples.size());
        double energy = 0.0;
        for (std::size_t i = start; i < end; ++i) {
            energy += static_cast<double>(filtered.samples[i]) * filtered.samples[i];
        }
        block_rms.push_back(std::sqrt(energy / std::max<std::size_t>(1, end - start)));
    }
    const double low = percentile(block_rms, 0.2);
    const double high = percentile(block_rms, 0.95);
    const double gate = std::max(0.0015, std::min(low * 1.25, high * 0.15));
    for (std::size_t block = 0; block < block_rms.size(); ++block) {
        if (block_rms[block] < gate) {
            const auto start = block * gate_window;
            const auto end = std::min(start + gate_window, filtered.samples.size());
            std::fill(filtered.samples.begin() + static_cast<std::ptrdiff_t>(start),
                      filtered.samples.begin() + static_cast<std::ptrdiff_t>(end), 0.0f);
        }
    }
    return filtered;
}

std::vector<double> detect_onsets(const AudioBuffer& audio) {
    if (audio.sample_rate == 0 || audio.samples.size() < onset_window) {
        return {};
    }
    std::vector<double> flux;
    std::vector<double> energy;
    std::vector<double> previous_magnitudes(onset_window / 2 + 1, 0.0);
    std::vector<std::complex<double>> spectrum(onset_window);
    for (std::size_t start = 0; start + onset_window <= audio.samples.size();
         start += onset_hop) {
        double frame_energy = 0.0;
        for (std::size_t i = 0; i < onset_window; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i /
                                                       (onset_window - 1));
            const double sample = audio.samples[start + i] * window;
            spectrum[i] = {sample, 0.0};
            frame_energy += sample * sample;
        }
        fft(spectrum);
        double positive_flux = 0.0;
        double magnitude_sum = 0.0;
        for (std::size_t bin = 1; bin < previous_magnitudes.size(); ++bin) {
            const double magnitude = std::abs(spectrum[bin]);
            positive_flux += std::max(0.0, magnitude - previous_magnitudes[bin]);
            magnitude_sum += magnitude;
            previous_magnitudes[bin] = magnitude;
        }
        const double rms = std::sqrt(frame_energy / onset_window);
        const double energy_rise = energy.empty()
                                       ? rms
                                       : std::max(0.0, rms - energy.back()) /
                                             (energy.back() + 0.002);
        flux.push_back(positive_flux / (magnitude_sum + 1e-9) + 0.15 * energy_rise);
        energy.push_back(rms);
    }
    if (flux.size() < 3) {
        return {};
    }

    const double median = percentile(flux, 0.5);
    std::vector<double> deviations;
    deviations.reserve(flux.size());
    for (const double value : flux) {
        deviations.push_back(std::abs(value - median));
    }
    const double threshold = std::max(0.08, median + 2.5 * percentile(deviations, 0.5));
    std::vector<double> onsets;
    double last_onset = -1.0;
    constexpr double minimum_spacing = 0.07;
    for (std::size_t i = 1; i + 1 < flux.size(); ++i) {
        if (flux[i] > threshold && flux[i] >= flux[i - 1] && flux[i] > flux[i + 1]) {
            const double time = static_cast<double>(i * onset_hop) / audio.sample_rate;
            if (time - last_onset >= minimum_spacing) {
                onsets.push_back(time);
                last_onset = time;
            }
        }
    }
    return onsets;
}

std::vector<Note> notes_from_frames(std::span<const PitchFrame> frames,
                                    double duration_seconds,
                                    std::span<const double> onsets,
                                    double minimum_confidence) {
    std::vector<Note> notes;
    if (frames.empty()) {
        return notes;
    }
    minimum_confidence = std::clamp(minimum_confidence, 0.0, 1.0);
    std::size_t index = 0;
    while (index < frames.size()) {
        if (frames[index].midi_pitch < 0 ||
            frames[index].confidence < minimum_confidence ||
            frames[index].voicing_probability < 0.2) {
            ++index;
            continue;
        }

        const int pitch = frames[index].midi_pitch;
        const std::size_t first = index;
        double confidence_sum = 0.0;
        std::size_t confidence_count = 0;
        ++index;
        while (index < frames.size() && frames[index].midi_pitch == pitch &&
               frames[index].confidence >= minimum_confidence &&
               frames[index].voicing_probability >= 0.2) {
            const double previous_time = frames[index - 1].time_seconds;
            const double current_time = frames[index].time_seconds;
            const auto onset = std::lower_bound(onsets.begin(), onsets.end(), previous_time + 0.001);
            if (onset != onsets.end() && *onset <= current_time + 0.005) {
                break;
            }
            confidence_sum += frames[index - 1].confidence;
            ++confidence_count;
            ++index;
        }
        confidence_sum += frames[index - 1].confidence;
        ++confidence_count;

        double start_time = frames[first].time_seconds;
        const auto nearby = std::lower_bound(onsets.begin(), onsets.end(), start_time - 0.05);
        if (nearby != onsets.end() && *nearby <= start_time + 0.04) {
            start_time = *nearby;
        }
        const double end_time = index < frames.size()
                                    ? std::min(duration_seconds, frames[index].time_seconds)
                                    : duration_seconds;
        const double confidence = confidence_sum / confidence_count;
        if (end_time - start_time >= 0.055 && confidence >= minimum_confidence) {
            notes.push_back({pitch, start_time, end_time - start_time, midi_note_name(pitch),
                             confidence});
        }
    }
    std::vector<Note> filtered;
    filtered.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        bool short_same_pitch_fragment = false;
        // Keep the onset-aligned run when a brief same-pitch tracker fragment precedes it.
        if (notes[i].duration_seconds < 0.15 && i + 1 < notes.size() &&
            notes[i + 1].midi_pitch == notes[i].midi_pitch &&
            notes[i + 1].duration_seconds >= 0.15) {
            const double gap = notes[i + 1].start_seconds -
                               (notes[i].start_seconds + notes[i].duration_seconds);
            const auto onset = std::lower_bound(onsets.begin(), onsets.end(),
                                                notes[i + 1].start_seconds - 0.03);
            short_same_pitch_fragment =
                gap >= -0.03 && gap <= 0.05 && onset != onsets.end() &&
                *onset <= notes[i + 1].start_seconds + 0.03;
        }
        if (!short_same_pitch_fragment) {
            filtered.push_back(notes[i]);
        }
    }
    return filtered;
}

ScoreMetadata infer_score_metadata(const std::vector<Note>& notes) {
    ScoreMetadata result{};
    if (notes.empty()) {
        return result;
    }
    std::vector<double> onsets;
    onsets.reserve(notes.size());
    for (const auto& note : notes) {
        onsets.push_back(note.start_seconds);
    }
    std::sort(onsets.begin(), onsets.end());
    onsets.erase(std::unique(onsets.begin(), onsets.end(),
                             [](double left, double right) {
                                 return std::abs(left - right) < 0.035;
                             }),
                 onsets.end());
    std::vector<double> intervals;
    for (std::size_t i = 1; i < onsets.size(); ++i) {
        const double interval = onsets[i] - onsets[i - 1];
        if (interval >= 0.18 && interval <= 2.0) {
            intervals.push_back(interval);
        }
    }
    if (!intervals.empty()) {
        const double observed_interval = percentile(intervals, 0.5);
        std::vector<double> interval_deviations;
        interval_deviations.reserve(intervals.size());
        for (const double interval : intervals) {
            interval_deviations.push_back(std::abs(interval - observed_interval));
        }
        const double relative_mad =
            percentile(interval_deviations, 0.5) / std::max(observed_interval, 1e-6);
        result.tempo_confidence = intervals.size() >= 2
                                      ? std::clamp(1.0 - 2.0 * relative_mad, 0.0, 1.0) *
                                            static_cast<double>(intervals.size()) /
                                            (intervals.size() + 4.0)
                                      : 0.0;
        double beat = observed_interval;
        while (beat < 1.0 / 3.0) {
            beat *= 2.0;
        }
        while (beat > 1.0) {
            beat *= 0.5;
        }
        result.tempo_bpm = std::clamp(60.0 / beat, 60.0, 180.0);
        result.tempo_inferred = true;

        double phrase_end = onsets.back();
        for (const auto& note : notes) {
            phrase_end = std::max(phrase_end, note.start_seconds +
                                                std::max(0.0, note.duration_seconds));
        }
        const double phrase_duration = phrase_end - onsets.front();
        const double bar_3 = 3.0 * 60.0 / result.tempo_bpm;
        const double bar_4 = 4.0 * 60.0 / result.tempo_bpm;
        const auto alignment = [phrase_duration](double bar_length) {
            return std::abs(std::remainder(phrase_duration, bar_length));
        };
        const double score_3 = alignment(bar_3);
        const double score_4 = alignment(bar_4);
        bool compound_eighth_grouping = false;
        double compound_confidence = 0.0;
        if (intervals.size() >= 6) {
            for (std::size_t phase = 0; phase < 3 && !compound_eighth_grouping; ++phase) {
                double short_sum = 0.0;
                double long_sum = 0.0;
                std::size_t short_count = 0;
                std::size_t long_count = 0;
                for (std::size_t i = 0; i < intervals.size(); ++i) {
                    if ((i + phase) % 3 == 2) {
                        long_sum += intervals[i];
                        ++long_count;
                    } else {
                        short_sum += intervals[i];
                        ++short_count;
                    }
                }
                compound_eighth_grouping =
                    long_count >= 2 && short_count >= 4 &&
                    long_sum / long_count > 1.35 * short_sum / short_count;
                if (compound_eighth_grouping) {
                    const double ratio = long_sum / long_count / (short_sum / short_count);
                    const double group_count =
                        static_cast<double>(std::min(long_count, short_count / 2));
                    compound_confidence =
                        std::clamp((ratio - 1.35) / 0.65, 0.0, 1.0) *
                        group_count / (group_count + 1.0);
                }
            }
        }
        if (compound_eighth_grouping) {
            result.time_numerator = 6;
            result.time_denominator = 8;
            result.meter_confidence = compound_confidence;
        } else if (score_3 < score_4) {
            result.time_numerator = 3;
            result.time_denominator = 4;
            result.meter_confidence =
                std::clamp((score_4 - score_3) / std::max(phrase_duration, 0.25),
                           0.0, 1.0);
        } else {
            result.time_numerator = 4;
            result.time_denominator = 4;
            result.meter_confidence =
                std::clamp((score_3 - score_4) / std::max(phrase_duration, 0.25),
                           0.0, 1.0);
        }
        result.tempo_inferred = intervals.size() >= 2 && result.tempo_confidence >= 0.15;
        if (onsets.size() < 4) {
            result.meter_confidence = 0.0;
        }
        result.meter_inferred =
            onsets.size() >= 4 && result.meter_confidence >= 0.1;
    }

    const auto histogram = histogram_for_notes(notes);
    const double total = std::accumulate(histogram.begin(), histogram.end(), 0.0);
    if (total > 0.0) {
        static constexpr std::array<double, 12> major_profile{
            6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
        static constexpr std::array<double, 12> minor_profile{
            6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
        const double mean_histogram = total / 12.0;
        double best_correlation = -std::numeric_limits<double>::infinity();
        double second_best_correlation = -std::numeric_limits<double>::infinity();
        for (int tonic = 0; tonic < 12; ++tonic) {
            for (int mode = 0; mode < 2; ++mode) {
                const auto& profile = mode == 0 ? major_profile : minor_profile;
                double covariance = 0.0;
                double histogram_variance = 0.0;
                double profile_variance = 0.0;
                const double profile_mean =
                    std::accumulate(profile.begin(), profile.end(), 0.0) / 12.0;
                for (int pitch = 0; pitch < 12; ++pitch) {
                    const double centered_histogram =
                        histogram[static_cast<std::size_t>(pitch)] - mean_histogram;
                    const double centered_profile =
                        profile[static_cast<std::size_t>((pitch - tonic + 12) % 12)] -
                        profile_mean;
                    covariance += centered_histogram * centered_profile;
                    histogram_variance += centered_histogram * centered_histogram;
                    profile_variance += centered_profile * centered_profile;
                }
                const double correlation =
                    covariance / std::sqrt(histogram_variance * profile_variance + 1e-12);
                if (correlation > best_correlation) {
                    second_best_correlation = best_correlation;
                    best_correlation = correlation;
                    const int major_tonic = mode == 0 ? tonic : (tonic + 3) % 12;
                    result.key_fifths = fifths_for_tonic(major_tonic);
                    result.minor_key = mode != 0;
                } else {
                    second_best_correlation =
                        std::max(second_best_correlation, correlation);
                }
            }
        }
        result.key_confidence =
            notes.size() >= 5
                ? std::clamp((best_correlation - second_best_correlation) * 3.0, 0.0, 1.0) *
                      static_cast<double>(notes.size()) / (notes.size() + 4.0)
                : 0.0;
        result.key_inferred = notes.size() >= 5 && result.key_confidence >= 0.12;
    }
    return result;
}

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio) {
    return transcribe_monophonic(audio, AnalysisOptions{});
}

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio,
                                        const AnalysisOptions& options) {
    if (audio.sample_rate < 2000) {
        throw std::invalid_argument("Audio sample rate must be at least 2000 Hz.");
    }
    if (audio.samples.empty()) {
        return {};
    }
    const auto filtered = preprocess_audio(audio);
    const auto onsets = detect_onsets(filtered);
    const auto analysis_audio = resample_linear(filtered, analysis_rate);
    if (analysis_audio.samples.size() < analysis_window) {
        return {};
    }
    const std::size_t frame_count =
        1 + (analysis_audio.samples.size() - analysis_window) / analysis_hop;
    std::vector<PitchFrame> frames(frame_count);
    std::vector<double> energy_blocks;
    energy_blocks.reserve((filtered.samples.size() + 511) / 512);
    for (std::size_t start = 0; start < filtered.samples.size(); start += 512) {
        const std::size_t end = std::min(start + 512, filtered.samples.size());
        double energy = 0.0;
        for (std::size_t i = start; i < end; ++i) {
            energy += static_cast<double>(filtered.samples[i]) * filtered.samples[i];
        }
        energy_blocks.push_back(std::sqrt(energy / (end - start)));
    }
    const double noise_floor = percentile(energy_blocks, 0.2);
    const double signal_level = percentile(energy_blocks, 0.95);
    const double adaptive_yin_threshold =
        std::clamp(0.12 + 0.1 * noise_floor / (signal_level + 1e-6), 0.1, 0.24);

    const auto analyze_range = [&analysis_audio, &frames, adaptive_yin_threshold](
                                   std::size_t begin, std::size_t end) {
        YinWorkspace workspace;
        for (std::size_t index = begin; index < end; ++index) {
            const std::size_t start = index * analysis_hop;
            const auto sample_span = std::span<const float>(analysis_audio.samples)
                                         .subspan(start, analysis_window);
            const auto pitch =
                multi_resolution_yin(sample_span, analysis_audio.sample_rate,
                                     adaptive_yin_threshold, workspace);
            frames[index] = {pitch.midi_pitch,
                             static_cast<double>(start) / analysis_audio.sample_rate,
                             pitch.confidence, pitch.voicing_probability};
        }
    };

    if (options.progress_callback) {
        options.progress_callback(0.0);
    }
    const unsigned int available_threads =
        std::max(1U, std::thread::hardware_concurrency());
    const std::size_t parallel_threshold = std::max<std::size_t>(1, options.parallel_threshold);
    const std::size_t worker_count =
        options.parallel && frame_count >= parallel_threshold
            ? std::min<std::size_t>(std::min(available_threads, 8U),
                                    (frame_count + parallel_threshold - 1) /
                                        parallel_threshold)
            : 1;
    if (worker_count == 1) {
        analyze_range(0, frame_count);
        if (options.progress_callback) {
            options.progress_callback(0.9);
        }
    } else {
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            const std::size_t begin = frame_count * worker / worker_count;
            const std::size_t end = frame_count * (worker + 1) / worker_count;
            workers.push_back(std::async(std::launch::async, analyze_range, begin, end));
        }
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            workers[worker].get();
            if (options.progress_callback) {
                options.progress_callback(0.9 * (worker + 1) / workers.size());
            }
        }
    }
    if (options.progress_callback) {
        options.progress_callback(1.0);
    }
    const double duration_seconds =
        static_cast<double>(audio.samples.size()) / audio.sample_rate;
    return notes_from_frames(frames, duration_seconds, onsets,
                             options.minimum_confidence);
}

StreamingResult transcribe_wav_streaming(const std::filesystem::path& path,
                                         const StreamingOptions& options) {
    if (!std::isfinite(options.chunk_seconds) || options.chunk_seconds < 1.0 ||
        options.chunk_seconds > 300.0 || !std::isfinite(options.overlap_seconds) ||
        options.overlap_seconds < 0.25 ||
        options.overlap_seconds >= options.chunk_seconds) {
        throw std::invalid_argument(
            "Streaming chunks must be 1-300 seconds with an overlap from 0.25 seconds to less than the chunk length.");
    }

    StreamingResult result;
    AudioBuffer chunk;
    std::uint32_t sample_rate = 0;
    std::size_t total_frames = 0;
    std::size_t chunk_frames = 0;
    std::size_t overlap_frames = 0;
    std::size_t stride_frames = 0;
    std::size_t chunk_start_frame = 0;
    std::size_t frames_read = 0;
    double last_progress = 0.0;
    bool first_chunk = true;
    bool finished = false;

    const auto process_chunk = [&](bool last) {
        if (chunk.samples.empty()) {
            return;
        }
        const std::size_t sample_count = chunk.samples.size();
        const double chunk_duration =
            static_cast<double>(sample_count) / sample_rate;
        const double core_start =
            first_chunk ? 0.0 : static_cast<double>(overlap_frames) / (2.0 * sample_rate);
        const double core_end =
            last ? chunk_duration
                 : chunk_duration -
                       static_cast<double>(overlap_frames) / (2.0 * sample_rate);
        AnalysisOptions analysis = options.analysis;
        const auto user_progress = options.analysis.progress_callback;
        if (user_progress && total_frames > 0) {
            analysis.progress_callback =
                [&, user_progress, core_start, core_end](double progress) {
                    const double stream_position =
                        static_cast<double>(chunk_start_frame) / total_frames +
                        ((core_start + (core_end - core_start) *
                                         std::clamp(progress, 0.0, 1.0)) *
                         sample_rate) /
                            total_frames;
                    last_progress = std::max(last_progress, std::min(1.0, stream_position));
                    user_progress(last_progress);
                };
        }

        const auto notes = transcribe_monophonic(chunk, analysis);
        for (const auto& note : notes) {
            const double absolute_start =
                static_cast<double>(chunk_start_frame) / sample_rate + note.start_seconds;
            if (note.start_seconds < core_start) {
                for (auto previous = result.notes.rbegin(); previous != result.notes.rend();
                     ++previous) {
                    const double previous_end =
                        previous->start_seconds + previous->duration_seconds;
                    if (previous->midi_pitch == note.midi_pitch &&
                        previous->start_seconds <= absolute_start + 0.05 &&
                        previous_end >= absolute_start - 0.05) {
                        previous->duration_seconds =
                            std::max(previous_end, absolute_start + note.duration_seconds) -
                            previous->start_seconds;
                        break;
                    }
                }
                continue;
            }
            if (note.start_seconds < core_end) {
                result.notes.push_back({note.midi_pitch, absolute_start,
                                        note.duration_seconds, note.name,
                                        note.confidence});
            }
        }
        if (user_progress && total_frames > 0) {
            last_progress = std::max(
                last_progress,
                std::min(1.0, static_cast<double>(chunk_start_frame) / total_frames +
                                  core_end * sample_rate / total_frames));
            user_progress(last_progress);
        }
    };

    read_wav_blocks(path, 4096, [&](const AudioBlockView& block) {
        if (sample_rate == 0) {
            sample_rate = block.sample_rate;
            total_frames = block.total_frames;
            result.duration_seconds =
                static_cast<double>(total_frames) / sample_rate;
            if (total_frames == 0) {
                return;
            }
            chunk_frames = static_cast<std::size_t>(
                std::llround(options.chunk_seconds * sample_rate));
            overlap_frames = static_cast<std::size_t>(
                std::llround(options.overlap_seconds * sample_rate));
            if (chunk_frames <= overlap_frames ||
                chunk_frames > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
                throw std::length_error("Requested streaming chunk size is not representable.");
            }
            stride_frames = chunk_frames - overlap_frames;
            chunk = {sample_rate, {}};
            chunk.samples.reserve(chunk_frames);
        } else if (block.sample_rate != sample_rate ||
                   block.total_frames != total_frames) {
            throw std::runtime_error("WAV stream format changed while reading.");
        }
        if (block.first_frame != frames_read) {
            throw std::runtime_error("WAV stream returned a discontinuous sample block.");
        }
        frames_read += block.samples.size();
        std::size_t offset = 0;
        while (offset < block.samples.size()) {
            if (finished) {
                throw std::runtime_error("WAV reader produced samples beyond its declared length.");
            }
            const std::size_t count =
                std::min(chunk_frames - chunk.samples.size(), block.samples.size() - offset);
            chunk.samples.insert(
                chunk.samples.end(),
                block.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                block.samples.begin() + static_cast<std::ptrdiff_t>(offset + count));
            offset += count;
            if (chunk.samples.size() == chunk_frames) {
                const bool last = chunk_frames >= total_frames - chunk_start_frame;
                process_chunk(last);
                if (last) {
                    finished = true;
                } else {
                    chunk.samples.erase(
                        chunk.samples.begin(),
                        chunk.samples.begin() + static_cast<std::ptrdiff_t>(stride_frames));
                    chunk_start_frame += stride_frames;
                    first_chunk = false;
                }
            }
        }
    });
    if (frames_read != total_frames) {
        throw std::runtime_error("WAV stream ended before its declared sample count.");
    }
    if (!finished && !chunk.samples.empty()) {
        process_chunk(true);
    }
    std::sort(result.notes.begin(), result.notes.end(),
              [](const Note& left, const Note& right) {
                  if (left.start_seconds != right.start_seconds) {
                      return left.start_seconds < right.start_seconds;
                  }
                  return left.midi_pitch < right.midi_pitch;
              });
    if (options.analysis.progress_callback && last_progress < 1.0) {
        options.analysis.progress_callback(1.0);
    }
    return result;
}

#ifdef TRANSCRIBE_ENABLE_ONNX
std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path) {
    return transcribe_with_onnx(audio, model_path, OnnxOptions{});
}

std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path,
                                       const OnnxOptions& options) {
    if (audio.sample_rate < 2000 || audio.samples.empty()) {
        throw std::invalid_argument("ONNX inference requires non-empty audio sampled at 2000 Hz or higher.");
    }
    constexpr std::uint32_t model_sample_rate = 16000;
    constexpr std::size_t pitch_count = 69;
    constexpr std::size_t silence_class = 69;
    constexpr double frame_step_seconds = 0.01;
    if (options.maximum_pitches_per_frame == 0) {
        throw std::invalid_argument("Maximum polyphonic pitch count must be positive.");
    }
    const auto model_input_audio = resample_linear(audio, model_sample_rate);
    auto session = load_onnx_session(model_path);
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session->GetInputNameAllocated(0, allocator);
    const std::size_t output_count = session->GetOutputCount();
    if (output_count == 0) {
        throw std::runtime_error("ONNX model has no output tensors.");
    }
    auto first_output_name = session->GetOutputNameAllocated(0, allocator);
    std::optional<Ort::AllocatedStringPtr> second_output_name;
    if (output_count > 1) {
        second_output_name.emplace(session->GetOutputNameAllocated(1, allocator));
    }

    const std::array<std::int64_t, 2> input_shape{
        1, static_cast<std::int64_t>(model_input_audio.samples.size())};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, const_cast<float*>(model_input_audio.samples.data()),
        model_input_audio.samples.size(), input_shape.data(), input_shape.size());
    const char* input_names[] = {input_name.get()};
    const char* output_names[] = {
        first_output_name.get(), second_output_name ? second_output_name->get() : nullptr};
    const std::size_t requested_outputs = second_output_name ? 2 : 1;
    auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1,
                                output_names, requested_outputs);
    if (outputs.empty() || !outputs.front().IsTensor()) {
        throw std::runtime_error("ONNX model did not return a tensor output.");
    }

    std::size_t selected_output = 0;
    if (options.polyphonic && outputs.size() > 1 && outputs[1].IsTensor()) {
        const auto second_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
        if (second_shape.size() == 3 && second_shape[0] == 1 &&
            (second_shape[2] == 69 || second_shape[2] == 70)) {
            selected_output = 1;
        }
    }
    const auto output_info = outputs[selected_output].GetTensorTypeAndShapeInfo();
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("ONNX pitch output must use float32 values.");
    }
    const auto shape = output_info.GetShape();
    if (shape.size() != 3 || shape[0] != 1 ||
        (shape[2] != 69 && shape[2] != 70) || shape[1] <= 0) {
        throw std::runtime_error("ONNX pitch output must have shape [1, frames, 69] or [1, frames, 70].");
    }
    const std::size_t frame_count = static_cast<std::size_t>(shape[1]);
    const std::size_t class_count = static_cast<std::size_t>(shape[2]);
    const float* activations = outputs[selected_output].GetTensorData<float>();

    if (options.progress_callback) {
        options.progress_callback(0.5);
    }
    const double duration_seconds =
        static_cast<double>(audio.samples.size()) / audio.sample_rate;
    const auto filtered = preprocess_audio(audio);
    const auto onsets = detect_onsets(filtered);
    if (!options.polyphonic) {
        std::vector<PitchFrame> frames;
        frames.reserve(frame_count);
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const float* row = activations + frame * class_count;
            const auto probabilities =
                mono_probabilities(row, class_count, options.independent_probabilities);
            const auto best = static_cast<std::size_t>(
                std::max_element(probabilities.begin(), probabilities.end()) -
                probabilities.begin());
            const bool voiced = best != silence_class;
            const int pitch = voiced ? static_cast<int>(best) + 28 : -1;
            const double confidence = voiced ? probabilities[best] : 0.0;
            frames.push_back({pitch, frame * frame_step_seconds, confidence,
                              voiced ? confidence : 0.0});
        }
        if (options.progress_callback) {
            options.progress_callback(0.9);
        }
        auto notes = notes_from_frames(frames, duration_seconds, onsets,
                                       options.minimum_confidence);
        if (options.progress_callback) {
            options.progress_callback(1.0);
        }
        return notes;
    }

    std::vector<std::vector<PitchFrame>> pitch_frames(pitch_count);
    for (auto& frames : pitch_frames) {
        frames.reserve(frame_count);
    }
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const float* row = activations + frame * class_count;
        const auto selected_pitches =
            greedy_peaks(row, std::min(pitch_count, class_count),
                         options.independent_probabilities, options.polyphonic_threshold,
                         options.maximum_pitches_per_frame);
        std::array<double, pitch_count> probabilities{};
        for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
            probabilities[pitch] = options.independent_probabilities
                                       ? clamp_probability(row[pitch])
                                       : sigmoid(row[pitch]);
        }
        for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
            const auto found = std::find(selected_pitches.begin(), selected_pitches.end(),
                                         static_cast<int>(pitch));
            const double probability = found == selected_pitches.end()
                                          ? 0.0
                                          : probabilities[pitch];
            pitch_frames[pitch].push_back({
                found == selected_pitches.end() ? -1 : static_cast<int>(pitch) + 28,
                frame * frame_step_seconds, probability, probability});
        }
    }
    std::vector<Note> notes;
    for (auto& frames : pitch_frames) {
        auto detected = notes_from_frames(frames, duration_seconds, onsets,
                                          options.minimum_confidence);
        notes.insert(notes.end(), detected.begin(), detected.end());
    }
    std::sort(notes.begin(), notes.end(), [](const Note& left, const Note& right) {
        if (left.start_seconds != right.start_seconds) {
            return left.start_seconds < right.start_seconds;
        }
        return left.midi_pitch < right.midi_pitch;
    });
    if (options.progress_callback) {
        options.progress_callback(1.0);
    }
    return notes;
}
#endif

} // namespace transcribe
