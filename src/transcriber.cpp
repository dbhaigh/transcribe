#include "transcribe/transcriber.hpp"
#include "transcribe/path.hpp"
#include "transcribe/polyphonic.hpp"
#include "transcribe/decoding.hpp"
#include "onnx_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <future>
#include <limits>
#include <mutex>
#include <numbers>
#include <numeric>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef TRANSCRIBE_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#ifdef TRANSCRIBE_ONNX_ENABLE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

namespace transcribe {
namespace {

void check_cancelled(std::stop_token token) {
    if (token.stop_requested()) throw std::runtime_error("Inference cancelled");
}

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
    std::size_t lag = 1;
    // Independent lags share the input scan; each sum retains its original order.
    for (; lag + 3 <= max_lag; lag += 4) {
        std::array<double, 4> sums{};
        const std::size_t common = samples.size() - lag - 3;
        for (std::size_t i = 0; i < common; ++i) {
            const double sample = samples[i];
            const double d0 = sample - samples[i + lag];
            const double d1 = sample - samples[i + lag + 1];
            const double d2 = sample - samples[i + lag + 2];
            const double d3 = sample - samples[i + lag + 3];
            sums[0] += d0 * d0;
            sums[1] += d1 * d1;
            sums[2] += d2 * d2;
            sums[3] += d3 * d3;
        }
        for (std::size_t lane = 0; lane < sums.size(); ++lane) {
            for (std::size_t i = common; i + lag + lane < samples.size(); ++i) {
                const double delta = static_cast<double>(samples[i]) - samples[i + lag + lane];
                sums[lane] += delta * delta;
            }
            workspace.difference[lag + lane] = sums[lane];
        }
    }
    for (; lag <= max_lag; ++lag) {
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
    const double fractional_midi = 69.0 + 12.0 * std::log2(frequency / 440.0);
    const int midi_pitch = static_cast<int>(std::lround(fractional_midi));
    if (midi_pitch < 28 || midi_pitch > 96) {
        return {};
    }

    const double confidence = clamp_probability(1.0 - best_value);
    const double voicing_probability =
        clamp_probability((threshold + 0.12 - best_value) / 0.12);
    return {midi_pitch, confidence, voicing_probability, fractional_midi};
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

struct FftPlan {
    explicit FftPlan(std::size_t size) : factors(size - 1) {
        for (std::size_t i = 1, j = 0; i < size; ++i) {
            std::size_t bit = size >> 1;
            for (; (j & bit) != 0; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) swaps.emplace_back(i, j);
        }
        for (std::size_t length = 2; length <= size; length <<= 1) {
            const double angle = -2.0 * std::numbers::pi / length;
            const std::complex<double> root(std::cos(angle), std::sin(angle));
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t i = 0; i < length / 2; ++i) {
                factors[length / 2 - 1 + i] = factor;
                factor *= root;
            }
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> swaps;
    std::vector<std::complex<double>> factors;
};

template <std::size_t Size>
const FftPlan& fft_plan() {
    static const FftPlan plan(Size);
    return plan;
}

void fft(std::vector<std::complex<double>>& values) {
    const std::size_t size = values.size();
    const FftPlan* plan = nullptr;
    switch (size) {
    case 256: plan = &fft_plan<256>(); break;
    case 512: plan = &fft_plan<512>(); break;
    case 1024: plan = &fft_plan<1024>(); break;
    case 2048: plan = &fft_plan<2048>(); break;
    case 4096: plan = &fft_plan<4096>(); break;
    default: throw std::logic_error("Unsupported internal FFT size");
    }
    for (const auto& [i, j] : plan->swaps) {
        std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const auto* factors = plan->factors.data() + length / 2 - 1;
        for (std::size_t offset = 0; offset < size; offset += length) {
            for (std::size_t i = 0; i < length / 2; ++i) {
                const auto even = values[offset + i];
                const auto odd = values[offset + i + length / 2] * factors[i];
                values[offset + i] = even + odd;
                values[offset + i + length / 2] = even - odd;
            }
        }
    }
}

template <std::size_t Size>
const std::array<double, Size>& hann_window() {
    static const auto values = [] {
        std::array<double, Size> window{};
        for (std::size_t i = 0; i < Size; ++i) {
            window[i] = 0.5 - 0.5 * std::cos(
                2.0 * std::numbers::pi * i / (Size - 1));
        }
        return window;
    }();
    return values;
}

struct Spectrogram {
    std::size_t frame_count{};
    std::size_t bin_count{};
    std::vector<double> magnitudes;
};

struct SpectrogramKey {
    std::uint64_t sample_hash{};
    std::size_t sample_count{};
    std::uint32_t sample_rate{};
    std::size_t window_size{};
    std::size_t hop_size{};

    bool operator==(const SpectrogramKey&) const = default;
};

std::uint64_t hash_audio(const AudioBuffer& audio) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float sample : audio.samples) {
        hash ^= std::bit_cast<std::uint32_t>(sample);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::shared_ptr<const Spectrogram> get_spectrogram(
    const AudioBuffer& audio, std::size_t window_size, std::size_t hop_size,
    bool parallel, bool& cache_hit) {
    const SpectrogramKey key{hash_audio(audio), audio.samples.size(),
                             audio.sample_rate, window_size, hop_size};
    static std::mutex cache_mutex;
    static SpectrogramKey cached_key;
    static std::shared_ptr<const Spectrogram> cached_spectrogram;
    {
        std::lock_guard lock(cache_mutex);
        if (cached_spectrogram && cached_key == key) {
            cache_hit = true;
            return cached_spectrogram;
        }
    }

    const std::size_t frame_count =
        std::max<std::size_t>(1, (audio.samples.size() + hop_size - 1) / hop_size);
    const std::size_t bin_count = window_size / 2 + 1;
    if (frame_count > std::numeric_limits<std::size_t>::max() / bin_count) {
        throw std::length_error("Spectrogram dimensions exceed addressable memory.");
    }
    auto computed = std::make_shared<Spectrogram>();
    computed->frame_count = frame_count;
    computed->bin_count = bin_count;
    computed->magnitudes.resize(frame_count * bin_count);
    const std::span<const double> window = window_size == 2048
        ? std::span<const double>(hann_window<2048>())
        : std::span<const double>(hann_window<4096>());
    const auto analyze_range = [&audio, &window, &computed, window_size,
                                hop_size, bin_count](std::size_t begin,
                                                     std::size_t end) {
        std::vector<std::complex<double>> spectrum(window_size);
        for (std::size_t frame = begin; frame < end; ++frame) {
            const std::size_t start = frame * hop_size;
            for (std::size_t i = 0; i < window_size; ++i) {
                const std::size_t sample_index = start + i;
                const double sample = sample_index < audio.samples.size()
                                          ? audio.samples[sample_index]
                                          : 0.0;
                spectrum[i] = {sample * window[i], 0.0};
            }
            fft(spectrum);
            double* const destination =
                computed->magnitudes.data() + frame * bin_count;
            for (std::size_t bin = 1; bin < bin_count; ++bin) {
                destination[bin] = std::abs(spectrum[bin]);
            }
        }
    };
    constexpr std::size_t parallel_threshold = 256;
    const std::size_t worker_count =
        parallel && frame_count >= parallel_threshold
            ? std::min<std::size_t>(
                  std::min(std::max(1U, std::thread::hardware_concurrency()),
                           8U),
                  (frame_count + parallel_threshold - 1) / parallel_threshold)
            : 1;
    if (worker_count == 1) {
        analyze_range(0, frame_count);
    } else {
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            const std::size_t begin = frame_count * worker / worker_count;
            const std::size_t end = frame_count * (worker + 1) / worker_count;
            workers.push_back(std::async(std::launch::async, analyze_range,
                                         begin, end));
        }
        for (auto& worker : workers) {
            worker.get();
        }
    }

    constexpr std::size_t cache_capacity_bytes = 64 * 1024 * 1024;
    std::shared_ptr<const Spectrogram> retired;
    if (computed->magnitudes.size() <=
        cache_capacity_bytes / sizeof(double)) {
        std::lock_guard lock(cache_mutex);
        cached_key = key;
        retired = std::move(cached_spectrogram);
        cached_spectrogram = computed;
    }
    cache_hit = false;
    return computed;
}

std::vector<Note> transcribe_spectral_polyphonic(
    const AudioBuffer& audio, std::span<const double> onsets,
    double duration_seconds, const AnalysisOptions& options,
    bool& spectrogram_cache_hit) {
    const std::size_t window_size = options.fast_preview ? 2048 : 4096;
    const std::size_t hop_size = options.fast_preview ? 512 : 256;
    const std::uint32_t sample_rate = audio.sample_rate;
    constexpr std::size_t first_pitch = 28;
    constexpr std::size_t last_pitch = 96;
    constexpr std::size_t pitch_count = last_pitch - first_pitch + 1;
    const std::size_t frame_count =
        std::max<std::size_t>(1, (audio.samples.size() + hop_size - 1) / hop_size);

    int minimum_pitch = static_cast<int>(first_pitch);
    int maximum_pitch = static_cast<int>(last_pitch);
    std::size_t maximum_pitches = options.maximum_pitches_per_frame;
    switch (options.instrument) {
    case InstrumentPrior::Guitar:
        minimum_pitch = 40;
        maximum_pitch = 88;
        maximum_pitches = std::min<std::size_t>(maximum_pitches, 6);
        break;
    case InstrumentPrior::Piano:
        maximum_pitches = std::min<std::size_t>(maximum_pitches, 10);
        break;
    case InstrumentPrior::Voice:
        minimum_pitch = 45;
        maximum_pitch = 84;
        maximum_pitches = std::min<std::size_t>(maximum_pitches, 2);
        break;
    case InstrumentPrior::Bass:
        minimum_pitch = 28;
        maximum_pitch = 72;
        maximum_pitches = std::min<std::size_t>(maximum_pitches, 4);
        break;
    case InstrumentPrior::None:
        break;
    }

    std::vector<std::vector<PitchFrame>> pitch_frames(pitch_count);
    for (auto& frames : pitch_frames) {
        frames.reserve(frame_count);
    }
    const auto spectrogram =
        get_spectrogram(audio, window_size, hop_size, options.parallel,
                        spectrogram_cache_hit);
    struct HarmonicTemplate {
        std::array<double, 8> frequencies{};
        std::size_t count{};
    };
    std::array<HarmonicTemplate, pitch_count> harmonic_templates{};
    for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
        const int midi = static_cast<int>(pitch + first_pitch);
        const double frequency = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        for (std::size_t harmonic = 1; harmonic <= 8; ++harmonic) {
            const double harmonic_frequency = frequency * harmonic;
            if (harmonic_frequency >= sample_rate / 2.0) {
                break;
            }
            harmonic_templates[pitch].frequencies[harmonic_templates[pitch].count++] =
                harmonic_frequency;
        }
    }
    std::array<double, pitch_count> salience{};
    std::optional<HarmonicCompetition> competition;
    if (options.spectral_decoder == SpectralDecoder::HarmonicCompetition) {
        competition.emplace(sample_rate, window_size, minimum_pitch, maximum_pitch);
    }
    std::vector<double> peak_fractions;
    std::array<std::vector<double>, pitch_count> raw_salience;
    if (options.evidence_callback) peak_fractions.reserve(frame_count);
    std::vector<std::pair<std::size_t, double>> candidates;
    candidates.reserve(pitch_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        check_cancelled(options.cancellation);
        const double* const magnitudes =
            spectrogram->magnitudes.data() + frame * spectrogram->bin_count;
        const std::size_t start = frame * hop_size;
        double maximum_spectrum = 0.0;
        for (std::size_t bin = 1; bin <= window_size / 2; ++bin) {
            maximum_spectrum = std::max(maximum_spectrum, magnitudes[bin]);
        }
        if (options.evidence_callback) {
            const double total = std::accumulate(magnitudes + 1,
                                                 magnitudes + window_size / 2 + 1, 0.0);
            peak_fractions.push_back(total > 0 ? maximum_spectrum / total : 0.0);
        }

        salience.fill(0.0);
        if (maximum_spectrum > 1e-9) {
            for (int midi = minimum_pitch; midi <= maximum_pitch; ++midi) {
                const std::size_t pitch_index =
                    static_cast<std::size_t>(midi - static_cast<int>(first_pitch));
                const auto bin_peak = [magnitudes, window_size, sample_rate](
                                          double target_frequency) {
                    const double bin =
                        target_frequency * window_size / sample_rate;
                    if (bin < 1.0 || bin >= window_size / 2.0) {
                        return 0.0;
                    }
                    const auto lower = static_cast<std::size_t>(bin);
                    const auto upper = lower + 1;
                    const double fraction = bin - lower;
                    return magnitudes[lower] * (1.0 - fraction) +
                           magnitudes[upper] * fraction;
                };

                const auto& harmonics = harmonic_templates[pitch_index];
                const double fundamental =
                    harmonics.count > 0 ? bin_peak(harmonics.frequencies[0]) : 0.0;
                double strongest_harmonic = fundamental;
                double score = fundamental;
                for (std::size_t harmonic = 1; harmonic < harmonics.count; ++harmonic) {
                    const double peak = bin_peak(harmonics.frequencies[harmonic]);
                    strongest_harmonic = std::max(strongest_harmonic, peak);
                    score += peak / std::sqrt(static_cast<double>(harmonic + 1));
                }
                if (fundamental >= strongest_harmonic * 0.12) {
                    salience[pitch_index] = score;
                }
            }
        }

        if (options.spectral_decoder == SpectralDecoder::HarmonicCompetition &&
            maximum_spectrum > 1e-9) {
            const auto activity = competition->activations(
                std::span<const double>(magnitudes, spectrogram->bin_count));
            salience.fill(0);
            std::copy(activity.begin(), activity.end(),
                      salience.begin() + minimum_pitch - first_pitch);
        }
        const double strongest = *std::max_element(salience.begin(), salience.end());
        if (options.evidence_callback) {
            for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
                raw_salience[pitch].push_back(salience[pitch]);
            }
        }
        candidates.clear();
        if (strongest > 0.0) {
            for (int midi = minimum_pitch; midi <= maximum_pitch; ++midi) {
                const std::size_t pitch_index =
                    static_cast<std::size_t>(midi - static_cast<int>(first_pitch));
                const double value = salience[pitch_index];
                const double left = pitch_index == 0 ? 0.0 : salience[pitch_index - 1];
                const double right = pitch_index + 1 == pitch_count
                                         ? 0.0
                                         : salience[pitch_index + 1];
                if (value >= strongest * options.spectral_threshold &&
                    (options.spectral_decoder == SpectralDecoder::HarmonicCompetition ||
                     (value >= left && value >= right)) && value > 0.0) {
                    candidates.emplace_back(pitch_index, value / strongest);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& left, const auto& right) {
                      return left.second > right.second;
                  });
        if (candidates.size() > maximum_pitches) {
            candidates.resize(maximum_pitches);
        }
        const double time = static_cast<double>(start) / analysis_rate;
        for (std::size_t pitch = 0; pitch < pitch_count; ++pitch) {
            const auto selected = std::find_if(
                candidates.begin(), candidates.end(),
                [pitch](const auto& candidate) { return candidate.first == pitch; });
            const bool voiced = selected != candidates.end();
            const double confidence = voiced ? selected->second : 0.0;
            pitch_frames[pitch].push_back(
                {voiced ? static_cast<int>(pitch + first_pitch) : -1, time,
                 confidence, confidence, 0.0, std::nullopt});
        }
    }

    std::vector<Note> notes;
    for (std::size_t pitch = 0; pitch < pitch_frames.size(); ++pitch) {
        auto& frames = pitch_frames[pitch];
        if (options.evidence_callback) {
            options.evidence_callback({frames, static_cast<double>(window_size) / sample_rate,
                static_cast<double>(hop_size) / sample_rate, "spectral",
                options.spectral_decoder == SpectralDecoder::HarmonicCompetition ?
                    "normalized-harmonic-fit" : "normalized-salience",
                peak_fractions, raw_salience[pitch], static_cast<int>(pitch + first_pitch),
                EvidenceSemantics::RelativeActivation, EvidenceSemantics::RawActivation, {}, {}, {}});
        }
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
    return notes;
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

void append_execution_provider(Ort::SessionOptions& options,
                               OnnxExecutionProvider provider, int device_id) {
    (void)options;
    (void)device_id;
    const auto providers = detail::onnx_providers();
    const auto found = std::find_if(providers.begin(), providers.end(), [&](const auto& p) {
        return p.provider == provider && p.compiled && p.runtime_available;
    });
    if (found == providers.end()) {
        throw std::runtime_error("Requested ONNX provider '" + inference_provider_name(provider) +
            "' is not enabled in this build/runtime; install a matching runtime and enable its CMake option");
    }
    if (provider != OnnxExecutionProvider::Cpu) {
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
    switch (provider) {
    case OnnxExecutionProvider::Cpu:
        return;
    case OnnxExecutionProvider::Cuda:
#ifdef TRANSCRIBE_ONNX_ENABLE_CUDA
    {
        OrtCUDAProviderOptions provider_options{};
        provider_options.device_id = device_id;
        options.AppendExecutionProvider_CUDA(provider_options);
        return;
    }
#else
        throw std::runtime_error(
            "CUDA provider selection requires TRANSCRIBE_ONNX_ENABLE_CUDA=ON.");
#endif
    case OnnxExecutionProvider::Rocm:
#ifdef TRANSCRIBE_ONNX_ENABLE_ROCM
    {
        OrtROCMProviderOptions provider_options{};
        provider_options.device_id = device_id;
        options.AppendExecutionProvider_ROCM(provider_options);
        return;
    }
#else
        throw std::runtime_error(
            "ROCm provider selection requires TRANSCRIBE_ONNX_ENABLE_ROCM=ON.");
#endif
    case OnnxExecutionProvider::DirectML:
#ifdef TRANSCRIBE_ONNX_ENABLE_DIRECTML
    {
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        const void* provider_api = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi(
            "DML", ORT_API_VERSION, &provider_api));
        const auto* dml_api = static_cast<const OrtDmlApi*>(provider_api);
        Ort::ThrowOnError(
            dml_api->SessionOptionsAppendExecutionProvider_DML(options, device_id));
        return;
    }
#else
        throw std::runtime_error(
            "DirectML provider selection requires TRANSCRIBE_ONNX_ENABLE_DIRECTML=ON.");
#endif
    }
    throw std::invalid_argument("Unknown ONNX execution provider.");
}

std::shared_ptr<Ort::Session> create_onnx_session(
    const std::filesystem::path& path, OnnxExecutionProvider provider, int device_id) {
    if (device_id < 0) throw std::invalid_argument("ONNX device ID must be nonnegative");
    if (!std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument("ONNX model file does not exist: " + path_utf8(path));
    }
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    append_execution_provider(options, provider, device_id);
    return std::make_shared<Ort::Session>(onnx_environment(), path.c_str(), options);
}

std::shared_ptr<Ort::Session> load_onnx_session(
    const std::filesystem::path& path, OnnxExecutionProvider provider,
    int device_id) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::wstring, std::shared_ptr<Ort::Session>> cache;
    auto key = std::filesystem::absolute(path).lexically_normal().wstring();
    key += L"\n" + std::to_wstring(static_cast<int>(provider)) + L"\n" +
           std::to_wstring(device_id);
    key += L"\n" + std::to_wstring(std::filesystem::file_size(path)) + L"\n" +
        std::to_wstring(static_cast<std::int64_t>(
            std::filesystem::last_write_time(path).time_since_epoch().count()));
    {
        std::lock_guard lock(cache_mutex);
        const auto existing = cache.find(key);
        if (existing != cache.end()) return existing->second;
    }
    auto session = create_onnx_session(path, provider, device_id);
    std::shared_ptr<Ort::Session> retired;
    {
        std::lock_guard lock(cache_mutex);
        const auto existing = cache.find(key);
        if (existing != cache.end()) return existing->second;
        if (cache.size() >= 2) {
            retired = std::move(cache.begin()->second);
            cache.erase(cache.begin());
        }
        cache.emplace(key, session);
    }
    return session;
}

void validate_onnx_contract(Ort::Session& session, ModelContract contract) {
    if (session.GetInputCount() != 1) {
        throw std::invalid_argument("Supported ONNX contracts require exactly one waveform input");
    }
    const auto input = session.GetInputTypeInfo(0);
    if (input.GetONNXType() != ONNX_TYPE_TENSOR) {
        throw std::invalid_argument("ONNX waveform input must be a float32 tensor");
    }
    const auto info = input.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    const bool basic = contract == ModelContract::BasicPitchV1;
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        (basic ? (shape.size() != 3 || (shape[0] != 1 && shape[0] != -1) ||
                  shape[1] != 43844 || shape[2] != 1) :
                 (shape.size() != 2 || (shape[0] != 1 && shape[0] != -1) ||
                  shape[1] == 0 || shape[1] < -1))) {
        throw std::invalid_argument(basic ?
            "BasicPitchV1 requires float32 [1,43844,1] at 22050 Hz; batch size is fixed at one" :
            "LegacyPitchV1 requires float32 [1,samples] at 16000 Hz; other layouts need another adapter");
    }
    Ort::AllocatorWithDefaultOptions allocator;
    bool supported_output = false;
    for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
        const auto name = session.GetOutputNameAllocated(i, allocator);
        const std::string_view output_name(name.get());
        if (basic && output_name != "StatefulPartitionedCall:1" &&
            output_name != "StatefulPartitionedCall:2" &&
            output_name != "StatefulPartitionedCall:0") continue;
        const auto type = session.GetOutputTypeInfo(i);
        if (type.GetONNXType() != ONNX_TYPE_TENSOR) continue;
        const auto tensor = type.GetTensorTypeAndShapeInfo();
        const auto dims = tensor.GetShape();
        const bool matches = tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            dims.size() == 3 && (dims[0] == 1 || dims[0] == -1) &&
            (basic ? (dims[1] == 172 &&
                       dims[2] == (output_name == "StatefulPartitionedCall:0" ? 264 : 88)) :
                     ((dims[1] > 0 || dims[1] == -1) && (dims[2] == 69 || dims[2] == 70)));
        if (basic && !matches) {
            throw std::invalid_argument("BasicPitchV1 incompatible head '" + std::string(output_name) +
                "': expected float32 [1,172," +
                (output_name == "StatefulPartitionedCall:0" ? "264]" : "88]"));
        }
        if (matches && (basic ? output_name == "StatefulPartitionedCall:1" : i < 2)) {
            supported_output = true;
        }
    }
    if (!supported_output) {
        throw std::invalid_argument(basic ?
            "BasicPitchV1 requires note head StatefulPartitionedCall:1 float32 [1,172,88]" :
            "LegacyPitchV1 requires a first/second float32 [1,frames,69|70] output (MIDI 28-96)");
    }
}

void validate_activations(const float* data, std::size_t size) {
    if (std::any_of(data, data + size, [](float v) { return !std::isfinite(v); })) {
        throw std::runtime_error("ONNX output contains NaN/Infinity; check model contract and input scaling");
    }
}

std::vector<Ort::Value> run_onnx(Ort::Session& session, std::stop_token cancellation,
    const char* const* input_names, const Ort::Value* inputs, std::size_t input_count,
    const char* const* output_names, std::size_t output_count) {
    check_cancelled(cancellation);
    Ort::RunOptions run;
    std::atomic<bool> termination_failed{false};
    std::stop_callback terminate(cancellation, [&] {
        if (auto* status = Ort::GetApi().RunOptionsSetTerminate(run)) {
            termination_failed.store(true);
            Ort::GetApi().ReleaseStatus(status);
        }
    });
    auto result = session.Run(run, input_names, inputs, input_count, output_names, output_count);
    if (termination_failed.load()) throw std::runtime_error("ONNX Runtime could not terminate the cancelled run");
    check_cancelled(cancellation);
    return result;
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

std::vector<Note> transcribe_basic_pitch(const AudioBuffer& audio, Ort::Session& session,
                                         const OnnxOptions& options) {
    constexpr std::uint32_t sample_rate = 22050;
    constexpr std::size_t window_samples = 43844;
    constexpr std::size_t fft_hop = 256;
    constexpr std::size_t overlap_frames = 30;
    constexpr std::size_t edge_frames = overlap_frames / 2;
    constexpr std::size_t hop_samples = window_samples - overlap_frames * fft_hop;
    constexpr std::size_t output_frames = 172;
    constexpr std::size_t usable_frames = output_frames - overlap_frames;
    constexpr std::size_t first_supported_pitch = 7; // MIDI 28 in Basic Pitch's MIDI 21 range.
    constexpr std::size_t supported_pitches = 69;     // MIDI 28 through 96.
    constexpr double frame_step = static_cast<double>(fft_hop) / sample_rate;

    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session.GetInputNameAllocated(0, allocator);
    const std::size_t output_count = session.GetOutputCount();
    std::vector<Ort::AllocatedStringPtr> owned_output_names;
    std::vector<const char*> output_names;
    owned_output_names.reserve(output_count);
    output_names.reserve(output_count);
    std::size_t note_output_index = output_count;
    std::size_t onset_output_index = output_count;
    std::size_t contour_output_index = output_count;
    for (std::size_t i = 0; i < output_count; ++i) {
        owned_output_names.emplace_back(session.GetOutputNameAllocated(i, allocator));
        output_names.push_back(owned_output_names.back().get());
        if (std::string_view(output_names.back()) == "StatefulPartitionedCall:1") {
            note_output_index = i;
        }
        if (std::string_view(output_names.back()) == "StatefulPartitionedCall:2") {
            onset_output_index = i;
        }
        if (std::string_view(output_names.back()) == "StatefulPartitionedCall:0") {
            contour_output_index = i;
        }
    }
    if (note_output_index == output_count) {
        throw std::runtime_error(
            "Basic Pitch ONNX model is missing its StatefulPartitionedCall:1 note output.");
    }

    const auto model_audio = resample_linear(audio, sample_rate);
    std::vector<float> padded(edge_frames * fft_hop + model_audio.samples.size(), 0.0f);
    std::copy(model_audio.samples.begin(), model_audio.samples.end(),
              padded.begin() + static_cast<std::ptrdiff_t>(edge_frames * fft_hop));
    const std::size_t window_count =
        (padded.size() + hop_samples - 1) / hop_samples;
    std::vector<std::array<double, supported_pitches>> frame_probabilities;
    std::array<std::vector<double>, supported_pitches> raw_activations;
    std::array<std::vector<double>, supported_pitches> onset_activations;
    std::array<std::vector<double>, supported_pitches> contour_activations;
    std::vector<double> frame_positions;
    frame_probabilities.reserve(window_count * usable_frames);
    std::vector<float> window(window_samples, 0.0f);
    const std::array<std::int64_t, 3> input_shape{
        1, static_cast<std::int64_t>(window_samples), 1};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, window.data(), window.size(), input_shape.data(), input_shape.size());

    for (std::size_t window_index = 0; window_index < window_count; ++window_index) {
        check_cancelled(options.cancellation);
        const std::size_t window_start = window_index * hop_samples;
        std::fill(window.begin(), window.end(), 0.0f);
        if (window_start < padded.size()) {
            const std::size_t count =
                std::min(window_samples, padded.size() - window_start);
            std::copy_n(padded.begin() + static_cast<std::ptrdiff_t>(window_start),
                        count, window.begin());
        }
        const char* input_names[] = {input_name.get()};
        auto outputs = run_onnx(session, options.cancellation, input_names, &input_tensor, 1,
                               output_names.data(), output_names.size());
        if (note_output_index >= outputs.size() || !outputs[note_output_index].IsTensor()) {
            throw std::runtime_error("Basic Pitch note output is not a tensor.");
        }
        const auto output_info =
            outputs[note_output_index].GetTensorTypeAndShapeInfo();
        const auto shape = output_info.GetShape();
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            shape.size() != 3 || shape[0] != 1 ||
            shape[1] != static_cast<std::int64_t>(output_frames) ||
            shape[2] != 88) {
            throw std::runtime_error(
                "Basic Pitch note output must be float32 with shape [1, 172, 88].");
        }
        const float* activations = outputs[note_output_index].GetTensorData<float>();
        validate_activations(activations, output_info.GetElementCount());
        const float* onset_values = nullptr;
        if (options.evidence_callback && onset_output_index < output_count) {
            const auto& onset_output = outputs[onset_output_index];
            if (!onset_output.IsTensor()) {
                throw std::runtime_error("Basic Pitch onset output is not a tensor");
            }
            const auto onset_info = onset_output.GetTensorTypeAndShapeInfo();
            if (onset_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                onset_info.GetShape() != shape) {
                throw std::runtime_error("Basic Pitch onset output must be float32 [1,172,88]");
            }
            onset_values = onset_output.GetTensorData<float>();
            validate_activations(onset_values, onset_info.GetElementCount());
        }
        const float* contour_values = nullptr;
        if (options.aligned_model_heads && contour_output_index < output_count) {
            const auto& contour = outputs[contour_output_index];
            if (!contour.IsTensor()) throw std::runtime_error("Basic Pitch contour is not a tensor");
            const auto info = contour.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                info.GetShape() != std::vector<std::int64_t>{1, 172, 264}) {
                throw std::runtime_error("Basic Pitch contour must be float32 [1,172,264]");
            }
            contour_values = contour.GetTensorData<float>();
            validate_activations(contour_values, info.GetElementCount());
        }
        for (std::size_t frame = edge_frames; frame < output_frames - edge_frames; ++frame) {
            frame_positions.push_back(static_cast<double>(
                window_start + (frame - edge_frames) * fft_hop));
            std::array<double, supported_pitches> probabilities{};
            const float* row = activations + frame * 88 + first_supported_pitch;
            for (std::size_t pitch = 0; pitch < supported_pitches; ++pitch) {
                probabilities[pitch] = clamp_probability(row[pitch]);
                if (options.evidence_callback) raw_activations[pitch].push_back(row[pitch]);
                if (onset_values) onset_activations[pitch].push_back(
                    onset_values[frame * 88 + first_supported_pitch + pitch]);
                if (contour_values) {
                    const auto center = (first_supported_pitch + pitch) * 3;
                    double weighted = 0, sum = 0;
                    for (int offset = -1; offset <= 1; ++offset) {
                        const auto bin = static_cast<std::size_t>(
                            static_cast<int>(center) + offset);
                        const double value = std::max(0.0f, contour_values[frame * 264 + bin]);
                        weighted += value * offset;
                        sum += value;
                    }
                    contour_activations[pitch].push_back(
                        28 + pitch + (sum > 0 ? weighted / (3 * sum) : 0));
                }
            }
            frame_probabilities.push_back(probabilities);
        }
        if (options.progress_callback) {
            options.progress_callback(0.5 + 0.4 *
                static_cast<double>(window_index + 1) / window_count);
        }
    }

    // Match Basic Pitch's overlap stitching length, then decode each pitch as its own voice.
    if (options.aligned_model_heads) {
        // Window advances are not an integer number of output hops. Regrid all heads together.
        const auto count = (model_audio.samples.size() + fft_hop - 1) / fft_hop;
        std::vector<std::array<double, supported_pitches>> aligned(count);
        const auto regrid = [&](std::vector<double>& values) {
            if (values.empty()) return;
            std::vector<double> result(count);
            std::size_t left = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const double position = static_cast<double>(i * fft_hop);
                while (left + 1 < frame_positions.size() &&
                    frame_positions[left + 1] <= position) ++left;
                const auto right = std::min(left + 1, frame_positions.size() - 1);
                const double weight = right == left ? 0 : std::clamp(
                    (position - frame_positions[left]) /
                    (frame_positions[right] - frame_positions[left]), 0.0, 1.0);
                result[i] = values[left] * (1 - weight) + values[right] * weight;
            }
            values = std::move(result);
        };
        for (std::size_t pitch = 0; pitch < supported_pitches; ++pitch) {
            std::vector<double> values;
            for (const auto& frame : frame_probabilities) values.push_back(frame[pitch]);
            regrid(values);
            for (std::size_t i = 0; i < count; ++i) aligned[i][pitch] = values[i];
            regrid(raw_activations[pitch]);
            regrid(onset_activations[pitch]);
            regrid(contour_activations[pitch]);
        }
        frame_probabilities = std::move(aligned);
    }
    const std::size_t expected_frames = options.aligned_model_heads ?
        frame_probabilities.size() : std::min(
        frame_probabilities.size(),
        static_cast<std::size_t>(static_cast<double>(model_audio.samples.size()) /
                                 hop_samples * usable_frames));
    const double duration_seconds =
        static_cast<double>(audio.samples.size()) / audio.sample_rate;
    const auto onsets = options.note_decoder == NeuralNoteDecoder::Legacy ?
        detect_onsets(preprocess_audio(audio)) : std::vector<double>{};
    std::vector<std::vector<PitchFrame>> pitch_frames(supported_pitches);
    for (std::size_t frame = 0; frame < expected_frames; ++frame) {
        const auto& probabilities = frame_probabilities[frame];
        std::vector<int> selected;
        if (options.polyphonic) {
            std::array<float, supported_pitches> values{};
            std::transform(probabilities.begin(), probabilities.end(), values.begin(),
                           [](double value) { return static_cast<float>(value); });
            if (options.aligned_model_heads) {
                for (std::size_t pitch = 0; pitch < supported_pitches; ++pitch) {
                    if (values[pitch] >= options.polyphonic_threshold) {
                        selected.push_back(static_cast<int>(pitch));
                    }
                }
                std::stable_sort(selected.begin(), selected.end(),
                    [&](int left, int right) { return values[left] > values[right]; });
                if (selected.size() > options.maximum_pitches_per_frame) {
                    selected.resize(options.maximum_pitches_per_frame);
                }
            } else {
                selected = greedy_peaks(values.data(), supported_pitches, true,
                                        options.polyphonic_threshold,
                                        options.maximum_pitches_per_frame);
            }
        } else {
            const auto best = static_cast<int>(
                std::max_element(probabilities.begin(), probabilities.end()) -
                probabilities.begin());
            if (probabilities[static_cast<std::size_t>(best)] >=
                options.polyphonic_threshold) {
                selected.push_back(best);
            }
        }
        const double time = frame * frame_step;
        for (std::size_t pitch = 0; pitch < supported_pitches; ++pitch) {
            const bool voiced = std::find(selected.begin(), selected.end(),
                                          static_cast<int>(pitch)) != selected.end();
            const double confidence = probabilities[pitch];
            pitch_frames[pitch].push_back(
                {voiced ? static_cast<int>(pitch) + 28 : -1, time,
                 voiced ? confidence : 0.0, voiced ? confidence : 0.0});
            if (!contour_activations[pitch].empty()) {
                pitch_frames[pitch].back().fractional_midi = contour_activations[pitch][frame];
            }
        }
    }

    std::vector<Note> notes;
    for (std::size_t pitch = 0; pitch < pitch_frames.size(); ++pitch) {
        auto& frames = pitch_frames[pitch];
        if (options.evidence_callback) {
            raw_activations[pitch].resize(expected_frames);
            FrameEvidenceTrack evidence{frames, 0.0, frame_step, "basic-pitch",
                                       "selected-note-head-activation", {}, raw_activations[pitch],
                                       static_cast<int>(pitch) + 28,
                                       EvidenceSemantics::RelativeActivation,
                                       EvidenceSemantics::RawActivation};
            if (!onset_activations[pitch].empty()) {
                onset_activations[pitch].resize(expected_frames);
                evidence.onset_strengths = std::move(onset_activations[pitch]);
            }
            evidence.note_states = raw_activations[pitch];
            if (!contour_activations[pitch].empty()) {
                contour_activations[pitch].resize(expected_frames);
                evidence.contour_midi = std::move(contour_activations[pitch]);
            }
            options.evidence_callback(evidence);
        }
        if (options.note_decoder == NeuralNoteDecoder::Legacy) {
            auto detected = notes_from_frames(frames, duration_seconds, onsets,
                                              options.minimum_confidence);
            notes.insert(notes.end(), detected.begin(), detected.end());
        }
    }
    std::sort(notes.begin(), notes.end(), [](const Note& left, const Note& right) {
        if (left.start_seconds != right.start_seconds) {
            return left.start_seconds < right.start_seconds;
        }
        return left.midi_pitch < right.midi_pitch;
    });
    return notes;
}
#endif

} // namespace

AudioPreviewData make_audio_preview(std::span<const float> samples,
                                    std::uint32_t sample_rate,
                                    std::size_t maximum_columns,
                                    std::size_t frequency_bands) {
    if (samples.empty()) {
        return {};
    }
    if (sample_rate < 2000 || maximum_columns == 0 || maximum_columns > 1024 ||
        frequency_bands == 0 || frequency_bands > 128) {
        throw std::invalid_argument("Audio preview dimensions or sample rate are invalid.");
    }

    std::size_t window_size = 1;
    while (window_size <= 1024 && window_size * 2 <= samples.size()) {
        window_size *= 2;
    }
    window_size = std::min<std::size_t>(window_size, 2048);
    if (window_size < 256) {
        window_size = 256;
    }
    const std::size_t quarter_window = std::max<std::size_t>(1, window_size / 4);
    const std::size_t preview_span =
        samples.size() > window_size ? samples.size() - window_size : 0;
    const std::size_t stride_limit = maximum_columns > 1
                                         ? maximum_columns - 1
                                         : 1;
    const std::size_t capped_stride =
        preview_span / stride_limit +
        static_cast<std::size_t>(preview_span % stride_limit != 0);
    const std::size_t maximum_stride =
        std::max(quarter_window, capped_stride);
    const std::size_t column_count =
        samples.size() <= window_size
            ? 1
            : std::min(maximum_columns,
                       1 + (samples.size() - window_size) / maximum_stride);

    AudioPreviewData preview;
    preview.duration_seconds =
        static_cast<double>(samples.size()) / sample_rate;
    preview.columns = column_count;
    preview.frequency_bands = frequency_bands;
    preview.minimum_frequency_hz = 55.0;
    preview.maximum_frequency_hz = sample_rate * 0.5;
    preview.waveform.resize(column_count);
    preview.waveform_minimum.resize(column_count);
    preview.waveform_maximum.resize(column_count);
    preview.spectrogram.assign(column_count * frequency_bands, 0.0f);

    for (std::size_t column = 0; column < column_count; ++column) {
        const std::size_t samples_per_bucket = samples.size() / column_count;
        const std::size_t remainder = samples.size() % column_count;
        const std::size_t bucket_start =
            samples_per_bucket * column + std::min(column, remainder);
        const std::size_t bucket_end =
            samples_per_bucket * (column + 1) +
            std::min(column + 1, remainder);
        float peak = 0.0f;
        float minimum = 0.0f;
        float maximum = 0.0f;
        bool has_finite_sample = false;
        for (std::size_t sample = bucket_start;
             sample < std::min(bucket_end, samples.size()); ++sample) {
            const float value = samples[sample];
            if (!std::isfinite(value)) {
                continue;
            }
            const float normalized = std::clamp(value, -1.0f, 1.0f);
            if (!has_finite_sample) {
                minimum = normalized;
                maximum = normalized;
                has_finite_sample = true;
            } else {
                minimum = std::min(minimum, normalized);
                maximum = std::max(maximum, normalized);
            }
            peak = std::max(peak, std::abs(normalized));
        }
        preview.waveform[column] = std::clamp(peak, 0.0f, 1.0f);
        preview.waveform_minimum[column] = minimum;
        preview.waveform_maximum[column] = maximum;
    }

    if (window_size < 256 || samples.size() < 64) {
        return preview;
    }
    std::vector<double> window(window_size);
    for (std::size_t index = 0; index < window_size; ++index) {
        window[index] = 0.5 - 0.5 * std::cos(
            2.0 * std::numbers::pi * index / (window_size - 1));
    }
    const double lowest_frequency = 55.0;
    const double nyquist = sample_rate * 0.5;
    const double log_frequency_range =
        std::log(nyquist / lowest_frequency);
    std::vector<double> band_energy(column_count * frequency_bands, 0.0);
    std::vector<std::complex<double>> spectrum(window_size);
    double maximum_energy = 0.0;
    for (std::size_t column = 0; column < column_count; ++column) {
        const std::size_t start = samples.size() <= window_size
                                      ? 0
                                      : column * maximum_stride;
        for (std::size_t index = 0; index < window_size; ++index) {
            const std::size_t source_index = start + index;
            const double sample =
                source_index < samples.size() ? samples[source_index] : 0.0;
            spectrum[index] = {sample * window[index], 0.0};
        }
        fft(spectrum);
        for (std::size_t bin = 1; bin < window_size / 2; ++bin) {
            const double frequency =
                static_cast<double>(bin) * sample_rate / window_size;
            if (frequency < lowest_frequency || frequency >= nyquist) {
                continue;
            }
            const auto band = std::min<std::size_t>(
                frequency_bands - 1,
                static_cast<std::size_t>(
                    std::log(frequency / lowest_frequency) /
                    log_frequency_range * frequency_bands));
            const double magnitude = std::abs(spectrum[bin]);
            auto& energy = band_energy[column * frequency_bands + band];
            energy += magnitude * magnitude;
            maximum_energy = std::max(maximum_energy, energy);
        }
    }
    if (maximum_energy > 0.0) {
        const double maximum_db = 10.0 * std::log10(maximum_energy);
        for (std::size_t index = 0; index < band_energy.size(); ++index) {
            const double decibels =
                10.0 * std::log10(std::max(band_energy[index], 1e-12));
            preview.spectrogram[index] = static_cast<float>(
                std::clamp((decibels - maximum_db + 60.0) / 60.0, 0.0, 1.0));
        }
    }
    return preview;
}

struct RealtimeTranscriber::Impl {
    explicit Impl(const RealtimeOptions& requested) : options(requested) {
        if (options.input_sample_rate < 2000) {
            throw std::invalid_argument("Realtime input sample rate must be at least 2000 Hz.");
        }
        if (!std::isfinite(options.latency_ms) || options.latency_ms < 20.0 ||
            options.latency_ms > 50.0) {
            throw std::invalid_argument("Realtime latency must be between 20 and 50 ms.");
        }
        if (!std::isfinite(options.update_rate_hz) || options.update_rate_hz < 50.0 ||
            options.update_rate_hz > 100.0) {
            throw std::invalid_argument("Realtime update rate must be between 50 and 100 Hz.");
        }
        if (!std::isfinite(options.minimum_confidence) ||
            options.minimum_confidence < 0.0 || options.minimum_confidence > 1.0 ||
            options.onset_stability_frames == 0 || options.offset_stability_frames == 0) {
            throw std::invalid_argument("Realtime confidence and stability settings are invalid.");
        }
        window_size = static_cast<std::size_t>(
            std::llround(analysis_rate * options.latency_ms / 1000.0));
        hop_size = std::max<std::size_t>(
            1, static_cast<std::size_t>(
                   std::llround(analysis_rate / options.update_rate_hz)));
    }

    void append_analysis_sample(float sample) {
        analysis_samples.push_back(sample);
        ++analysis_samples_generated;
    }

    void process_pitch(int pitch, double confidence, double frame_time,
                       double detected_at_time) {
        if (active_note == no_note) {
            if (pitch < 0) {
                pending_pitch = -1;
                pending_frames = 0;
                return;
            }
            if (pending_pitch != pitch) {
                pending_pitch = pitch;
                pending_frames = 1;
                pending_start = frame_time;
                pending_confidence = confidence;
            } else {
                ++pending_frames;
                pending_confidence = std::max(pending_confidence, confidence);
            }
            if (pending_frames >= options.onset_stability_frames) {
                start_note(pending_pitch, pending_start, pending_confidence,
                           detected_at_time);
                pending_pitch = -1;
                pending_frames = 0;
            }
            return;
        }

        if (pitch == live_notes[active_note].midi_pitch) {
            pending_pitch = -1;
            pending_frames = 0;
            release_frames = 0;
            live_notes[active_note].confidence =
                std::max(live_notes[active_note].confidence, confidence);
            live_notes[active_note].duration_seconds =
                std::max(0.0, frame_time + static_cast<double>(window_size) / analysis_rate -
                                  live_notes[active_note].start_seconds);
            return;
        }

        if (pitch >= 0) {
            if (pending_pitch != pitch) {
                pending_pitch = pitch;
                pending_frames = 1;
                pending_start = frame_time;
                pending_confidence = confidence;
            } else {
                ++pending_frames;
                pending_confidence = std::max(pending_confidence, confidence);
            }
            if (pending_frames >= options.onset_stability_frames) {
                close_note(pending_start, detected_at_time);
                start_note(pending_pitch, pending_start, pending_confidence,
                           detected_at_time);
                pending_pitch = -1;
                pending_frames = 0;
                release_frames = 0;
            }
            return;
        }

        pending_pitch = -1;
        pending_frames = 0;
        if (release_frames == 0) {
            release_start = frame_time;
        }
        ++release_frames;
        if (release_frames >= options.offset_stability_frames) {
            close_note(release_start, detected_at_time);
            release_frames = 0;
        }
    }

    void start_note(int pitch, double start, double confidence,
                    double detected_at_time) {
        live_notes.push_back(
            {pitch, start, 0.0, midi_note_name(pitch), confidence});
        active_note = live_notes.size() - 1;
        events.push_back(
            {NoteEventType::Onset, pitch, start, confidence, detected_at_time});
    }

    void close_note(double end, double detected_at_time) {
        if (active_note == no_note) {
            return;
        }
        auto& note = live_notes[active_note];
        note.duration_seconds = std::max(0.0, end - note.start_seconds);
        events.push_back(
            {NoteEventType::Offset, note.midi_pitch, end, note.confidence,
             detected_at_time});
        active_note = no_note;
    }

    void analyze_available_frames() {
        while (analysis_cursor + window_size <= analysis_samples.size()) {
            const auto frame = std::span<const float>(analysis_samples)
                                   .subspan(analysis_cursor, window_size);
            double energy = 0.0;
            for (const float sample : frame) {
                energy += static_cast<double>(sample) * sample;
            }
            const double rms = std::sqrt(energy / frame.size());
            const double threshold = options.adaptive_threshold
                                         ? std::clamp(0.12 + 0.08 * noise_floor /
                                                                  (rms + 1e-6),
                                                      0.12, 0.24)
                                         : 0.15;
            const auto estimate = rms >= std::max(0.0015, noise_floor * 1.7)
                                      ? multi_resolution_yin(frame, analysis_rate,
                                                            threshold, workspace)
                                      : PitchEstimate{};
            const int pitch = estimate.confidence >= options.minimum_confidence
                                  ? estimate.midi_pitch
                                  : -1;
            const double frame_time =
                static_cast<double>(frame_index * hop_size) / analysis_rate;
            const double detected_at_time =
                frame_time + static_cast<double>(window_size) / analysis_rate;
            process_pitch(pitch, estimate.confidence, frame_time,
                          detected_at_time);
            if (pitch < 0 && rms < noise_floor * 2.0) {
                noise_floor = 0.98 * noise_floor + 0.02 * rms;
            }
            ++frame_index;
            analysis_cursor += hop_size;
        }
        if (analysis_cursor >= window_size * 2) {
            analysis_samples.erase(
                analysis_samples.begin(),
                analysis_samples.begin() + static_cast<std::ptrdiff_t>(analysis_cursor));
            analysis_cursor = 0;
        }
    }

    void resample_available() {
        while (true) {
            const auto left_frame = static_cast<std::size_t>(next_source_position);
            const auto right_frame = left_frame + 1;
            if (right_frame >= source_frames_received ||
                left_frame < source_buffer_start_frame) {
                break;
            }
            const double fraction = next_source_position - left_frame;
            const auto left = source_samples[left_frame - source_buffer_start_frame];
            const auto right = source_samples[right_frame - source_buffer_start_frame];
            append_analysis_sample(static_cast<float>(left + (right - left) * fraction));
            next_source_position += static_cast<double>(options.input_sample_rate) /
                                    analysis_rate;
        }
        analyze_available_frames();
        const auto keep_frame = static_cast<std::size_t>(next_source_position);
        if (keep_frame > source_buffer_start_frame) {
            const auto removable = std::min(
                keep_frame - source_buffer_start_frame,
                source_samples.size() > 1 ? source_samples.size() - 1 : 0);
            source_samples.erase(
                source_samples.begin(),
                source_samples.begin() + static_cast<std::ptrdiff_t>(removable));
            source_buffer_start_frame += removable;
        }
    }

    static constexpr std::size_t no_note = std::numeric_limits<std::size_t>::max();
    static constexpr std::uint32_t analysis_rate = 11025;
    RealtimeOptions options;
    std::size_t window_size{};
    std::size_t hop_size{};
    std::vector<float> source_samples;
    std::size_t source_buffer_start_frame{};
    std::size_t source_frames_received{};
    double next_source_position{};
    std::vector<float> analysis_samples;
    std::size_t analysis_samples_generated{};
    std::size_t analysis_cursor{};
    std::size_t frame_index{};
    double noise_floor{0.0015};
    YinWorkspace workspace;
    std::vector<Note> live_notes;
    std::vector<NoteEvent> events;
    std::size_t active_note{no_note};
    int pending_pitch{-1};
    std::size_t pending_frames{};
    double pending_start{};
    double pending_confidence{};
    std::size_t release_frames{};
    double release_start{};
    bool finished{};
    mutable std::mutex mutex;
};

RealtimeTranscriber::RealtimeTranscriber(const RealtimeOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

RealtimeTranscriber::~RealtimeTranscriber() = default;

void RealtimeTranscriber::push_samples(std::span<const float> samples) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->finished) {
        throw std::logic_error("Cannot add samples after realtime transcription is finished.");
    }
    impl_->source_samples.insert(impl_->source_samples.end(), samples.begin(), samples.end());
    impl_->source_frames_received += samples.size();
    impl_->resample_available();
}

void RealtimeTranscriber::finish() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->finished) {
        return;
    }
    if (impl_->active_note != Impl::no_note) {
        const double end_time =
            static_cast<double>(impl_->analysis_samples_generated) / Impl::analysis_rate;
        impl_->close_note(end_time, end_time);
    }
    impl_->finished = true;
}

std::vector<Note> RealtimeTranscriber::live_notes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->live_notes;
}

std::vector<NoteEvent> RealtimeTranscriber::take_events() {
    std::lock_guard lock(impl_->mutex);
    std::vector<NoteEvent> result;
    result.swap(impl_->events);
    return result;
}

RealtimeSnapshot RealtimeTranscriber::take_snapshot() {
    std::lock_guard lock(impl_->mutex);
    RealtimeSnapshot result;
    result.notes = impl_->live_notes;
    result.events.swap(impl_->events);
    return result;
}

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

std::vector<OnsetEvidence> detect_onset_evidence(
    const AudioBuffer& audio, const OnsetDetectionOptions& options) {
    for (const double value : {options.minimum_strength, options.deviation_multiplier,
                              options.minimum_spacing_seconds}) {
        if (!std::isfinite(value) || value < 0) {
            throw std::invalid_argument("Onset parameters must be finite and nonnegative");
        }
    }
    if (audio.sample_rate == 0 || audio.samples.size() < onset_window) {
        return {};
    }
    std::vector<double> flux;
    std::vector<double> energy;
    std::vector<OnsetEvidence> evidence;
    const std::size_t frame_count = 1 + (audio.samples.size() - onset_window) / onset_hop;
    flux.reserve(frame_count);
    energy.reserve(frame_count);
    std::vector<double> previous_magnitudes(onset_window / 2 + 1, 0.0);
    std::vector<std::complex<double>> spectrum(onset_window);
    const auto& window = hann_window<onset_window>();
    for (std::size_t start = 0; start + onset_window <= audio.samples.size();
         start += onset_hop) {
        double frame_energy = 0.0;
        for (std::size_t i = 0; i < onset_window; ++i) {
            const double sample = audio.samples[start + i] * window[i];
            spectrum[i] = {sample, 0.0};
            frame_energy += sample * sample;
        }
        fft(spectrum);
        double positive_flux = 0.0;
        double magnitude_sum = 0.0;
        double high_frequency_flux = 0.0;
        double high_frequency_sum = 0.0;
        for (std::size_t bin = 1; bin < previous_magnitudes.size(); ++bin) {
            const double magnitude = std::abs(spectrum[bin]);
            const double increase =
                std::max(0.0, magnitude - previous_magnitudes[bin]);
            positive_flux += increase;
            magnitude_sum += magnitude;
            if (bin >= previous_magnitudes.size() / 3) {
                const double weight = static_cast<double>(bin) /
                                      (previous_magnitudes.size() - 1);
                high_frequency_flux += increase * weight;
                high_frequency_sum += magnitude * weight;
            }
            previous_magnitudes[bin] = magnitude;
        }
        const double rms = std::sqrt(frame_energy / onset_window);
        const double energy_rise = energy.empty()
                                       ? rms
                                       : std::max(0.0, rms - energy.back()) /
                                             (energy.back() + 0.002);
        const double spectral_flux = positive_flux / (magnitude_sum + 1e-9);
        const double high_frequency_content =
            high_frequency_flux / (high_frequency_sum + 1e-9);
        flux.push_back(0.75 * spectral_flux + 0.25 * high_frequency_content +
                       0.15 * energy_rise);
        evidence.push_back({static_cast<double>(start) / audio.sample_rate, flux.back(),
                            spectral_flux, high_frequency_content, energy_rise});
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
    const double threshold = std::max(options.minimum_strength,
        median + options.deviation_multiplier * percentile(deviations, 0.5));
    std::vector<OnsetEvidence> onsets;
    double last_onset = -1.0;
    for (std::size_t i = 1; i + 1 < flux.size(); ++i) {
        if (flux[i] > threshold && flux[i] >= flux[i - 1] && flux[i] > flux[i + 1]) {
            const double time = static_cast<double>(i * onset_hop) / audio.sample_rate;
            if (time - last_onset >= options.minimum_spacing_seconds) {
                onsets.push_back(evidence[i]);
                last_onset = time;
            }
        }
    }
    return onsets;
}

std::vector<double> detect_onsets(const AudioBuffer& audio) {
    std::vector<double> times;
    for (const auto& onset : detect_onset_evidence(audio)) times.push_back(onset.time_seconds);
    return times;
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
        double amplitude_sum = 0.0;
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
            amplitude_sum += frames[index - 1].amplitude;
            ++confidence_count;
            ++index;
        }
        confidence_sum += frames[index - 1].confidence;
        amplitude_sum += frames[index - 1].amplitude;
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
                             confidence, amplitude_sum / confidence_count});
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
        if (interval >= 0.08 && interval <= 2.0) {
            intervals.push_back(interval);
        }
    }
    if (!intervals.empty()) {
        double observed_interval = percentile(intervals, 0.5);
        std::vector<double> swung_beat_candidates;
        for (std::size_t phase = 0; phase < 2; ++phase) {
            std::vector<double> phase_candidates;
            for (std::size_t i = phase; i + 1 < intervals.size(); i += 2) {
                const double shorter = std::min(intervals[i], intervals[i + 1]);
                const double longer = std::max(intervals[i], intervals[i + 1]);
                const double ratio = longer / std::max(shorter, 1e-6);
                if (ratio >= 1.35 && ratio <= 3.0) {
                    phase_candidates.push_back(intervals[i] + intervals[i + 1]);
                }
            }
            if (phase_candidates.size() > swung_beat_candidates.size()) {
                swung_beat_candidates = std::move(phase_candidates);
            }
        }
        if (swung_beat_candidates.size() >= 3) {
            observed_interval = percentile(swung_beat_candidates, 0.5);
        }
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

        double best_swing_confidence = 0.0;
        for (std::size_t phase = 0; phase < 2; ++phase) {
            std::vector<double> short_intervals;
            std::vector<double> long_intervals;
            std::size_t pair_count = 0;
            for (std::size_t i = phase; i + 1 < onsets.size(); i += 2) {
                const double first_interval = onsets[i + 1] - onsets[i];
                if (i + 2 >= onsets.size()) {
                    continue;
                }
                const double second_interval = onsets[i + 2] - onsets[i + 1];
                const double pair_duration = first_interval + second_interval;
                if (pair_duration < beat * 0.75 || pair_duration > beat * 1.25) {
                    continue;
                }
                const double shorter = std::min(first_interval, second_interval);
                const double longer = std::max(first_interval, second_interval);
                const double ratio = longer / std::max(shorter, 1e-6);
                if (ratio < 1.35 || ratio > 3.0) {
                    continue;
                }
                short_intervals.push_back(shorter);
                long_intervals.push_back(longer);
                ++pair_count;
            }
            if (pair_count < 3) {
                continue;
            }
            const double short_median = percentile(short_intervals, 0.5);
            const double long_median = percentile(long_intervals, 0.5);
            const double ratio = long_median / std::max(short_median, 1e-6);
            const double confidence =
                std::clamp((ratio - 1.25) / 1.25, 0.0, 1.0) *
                static_cast<double>(pair_count) / (pair_count + 2.0);
            if (confidence > best_swing_confidence) {
                result.swing_ratio = std::clamp(ratio, 1.5, 3.0);
                best_swing_confidence = confidence;
            }
        }
        result.swing_confidence = best_swing_confidence;
        result.swing_inferred = best_swing_confidence >= 0.2;
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
    if (options.maximum_pitches_per_frame == 0) {
        throw std::invalid_argument("Maximum pitches per frame must be positive.");
    }
    check_cancelled(options.cancellation);
    if (!std::isfinite(options.spectral_threshold) ||
        !(options.spectral_threshold > 0.0 && options.spectral_threshold <= 1.0)) {
        throw std::invalid_argument("Spectral threshold must be greater than 0 and at most 1.");
    }
    if (audio.samples.empty()) {
        return {};
    }
    AnalysisTimings timings;
    const auto report_timings = [&options, &timings] {
        if (options.timing_callback) {
            options.timing_callback(timings);
        }
    };
    auto stage_started = std::chrono::steady_clock::now();
    const auto filtered = preprocess_audio(audio);
    timings.preprocessing_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stage_started).count();
    stage_started = std::chrono::steady_clock::now();
    const auto onsets = detect_onsets(filtered);
    timings.onset_fft_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stage_started).count();
    if (options.polyphonic) {
        stage_started = std::chrono::steady_clock::now();
        const auto analysis_audio = resample_linear(filtered, analysis_rate);
        timings.resampling_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stage_started).count();
        stage_started = std::chrono::steady_clock::now();
        bool spectrogram_cache_hit = false;
        auto notes = transcribe_spectral_polyphonic(
            analysis_audio, onsets,
            static_cast<double>(audio.samples.size()) / audio.sample_rate, options,
            spectrogram_cache_hit);
        timings.pitch_analysis_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stage_started).count();
        timings.spectrogram_cache_hit = spectrogram_cache_hit;
        report_timings();
        return notes;
    }
    const std::uint32_t selected_sample_rate =
        options.fast_preview ? 8000 : analysis_rate;
    const std::size_t selected_window =
        options.fast_preview ? 1024 : analysis_window;
    const std::size_t selected_hop =
        options.fast_preview ? 512 : analysis_hop;
    stage_started = std::chrono::steady_clock::now();
    const auto analysis_audio = resample_linear(filtered, selected_sample_rate);
    timings.resampling_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stage_started).count();
    if (analysis_audio.samples.size() < selected_window) {
        report_timings();
        return {};
    }
    const std::size_t frame_count =
        1 + (analysis_audio.samples.size() - selected_window) / selected_hop;
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

    stage_started = std::chrono::steady_clock::now();
    const auto analyze_range = [&analysis_audio, &frames, adaptive_yin_threshold,
                                selected_window, selected_hop, &options](
                                   std::size_t begin, std::size_t end) {
        YinWorkspace workspace;
        for (std::size_t index = begin; index < end; ++index) {
            check_cancelled(options.cancellation);
            const std::size_t start = index * selected_hop;
            const auto sample_span = std::span<const float>(analysis_audio.samples)
                                         .subspan(start, selected_window);
            const auto pitch =
                multi_resolution_yin(sample_span, analysis_audio.sample_rate,
                                     adaptive_yin_threshold, workspace);
            double energy = 0.0;
            for (const float sample : sample_span) {
                energy += static_cast<double>(sample) * sample;
            }
            frames[index] = {pitch.midi_pitch,
                             static_cast<double>(start) /
                                 analysis_audio.sample_rate,
                             pitch.confidence, pitch.voicing_probability,
                             std::sqrt(energy / sample_span.size()), pitch.fractional_midi};
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
    timings.pitch_analysis_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - stage_started).count();
    report_timings();
    if (options.evidence_callback) {
        options.evidence_callback({frames, static_cast<double>(selected_window) / selected_sample_rate,
            static_cast<double>(selected_hop) / selected_sample_rate, "yin",
            "one-minus-cmnd-and-voicing-heuristic", {}, {}, -1,
            EvidenceSemantics::HeuristicScore, EvidenceSemantics::Unspecified, {}, {}, {}});
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
        const auto user_evidence = options.analysis.evidence_callback;
        if (user_evidence) {
            analysis.evidence_callback =
                [&, user_evidence](const FrameEvidenceTrack& track) {
                    auto absolute_track = track;
                    const double time_offset =
                        static_cast<double>(chunk_start_frame) / sample_rate;
                    for (auto& frame : absolute_track.frames) {
                        frame.time_seconds += time_offset;
                    }
                    user_evidence(absolute_track);
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
        if (options.notes_callback) {
            options.notes_callback(result.notes);
        }
    };

    read_wav_blocks(path, 4096, [&](const AudioBlockView& block) {
        check_cancelled(options.analysis.cancellation);
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
        if (options.audio_callback) {
            options.audio_callback(block);
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

namespace {
std::vector<Note> infer_onnx_session(const AudioBuffer& audio,
    const std::shared_ptr<Ort::Session>& session, const OnnxOptions& options) {
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
    if (options.note_decoder != NeuralNoteDecoder::Legacy &&
        options.note_decoder != NeuralNoteDecoder::MultiHeadEvents) {
        throw std::invalid_argument("Unknown neural note decoder");
    }
    if (options.device_id < 0) {
        throw std::invalid_argument("ONNX execution provider device ID must be non-negative.");
    }
    check_cancelled(options.cancellation);
    if (std::any_of(audio.samples.begin(), audio.samples.end(),
                   [](float v) { return !std::isfinite(v); }) ||
        !std::isfinite(options.minimum_confidence) || options.minimum_confidence < 0 ||
        options.minimum_confidence > 1 || !std::isfinite(options.polyphonic_threshold) ||
        options.polyphonic_threshold < 0 || options.polyphonic_threshold > 1) {
        throw std::invalid_argument("ONNX requires finite audio and activation/confidence thresholds in [0,1]");
    }
    if (session->GetInputCount() != 1) {
        throw std::invalid_argument("ONNX adapter requires exactly one waveform input");
    }
    const auto input_info = session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
    const auto model_input_shape = input_info.GetShape();
    if (model_input_shape.size() == 3 && model_input_shape[1] == 43844 &&
        model_input_shape[2] == 1) {
        validate_onnx_contract(*session, ModelContract::BasicPitchV1);
        if (options.progress_callback) {
            options.progress_callback(0.1);
        }
        auto effective = options;
        std::vector<FrameEvidenceTrack> tracks;
        if (options.note_decoder == NeuralNoteDecoder::MultiHeadEvents) {
            if (!options.polyphonic) {
                throw std::invalid_argument("Multi-head event decoding requires polyphonic BasicPitchV1");
            }
            effective.aligned_model_heads = true;
            effective.evidence_callback = [&](const FrameEvidenceTrack& track) {
                tracks.push_back(track);
                if (options.evidence_callback) options.evidence_callback(track);
            };
        }
        auto notes = transcribe_basic_pitch(audio, *session, effective);
        if (options.note_decoder == NeuralNoteDecoder::MultiHeadEvents) {
            if (tracks.empty() || tracks.front().onset_strengths.empty()) {
                throw std::invalid_argument("Multi-head event decoding requires the Basic Pitch onset head");
            }
            check_cancelled(options.cancellation);
            TemporalDecodingOptions decoding;
            decoding.activation_on = options.polyphonic_threshold;
            decoding.activation_off = options.polyphonic_threshold;
            const auto decoded = decode_note_events(audio, tracks, decoding,
                "neural-input", "basic-pitch-v1");
            notes.clear();
            for (const auto& event : decoded.events) {
                double strength = 0;
                for (const auto& evidence : event.evidence) strength += evidence.raw_strength;
                if (!event.evidence.empty()) strength /= event.evidence.size();
                const auto pitch = static_cast<int>(std::lround(event.midi_pitch));
                notes.push_back({pitch, event.onset_seconds,
                    event.offset_seconds - event.onset_seconds, midi_note_name(pitch), strength});
            }
            check_cancelled(options.cancellation);
        }
        if (options.progress_callback) {
            options.progress_callback(1.0);
        }
        return notes;
    }
    validate_onnx_contract(*session, ModelContract::LegacyPitchV1);
    if (options.note_decoder != NeuralNoteDecoder::Legacy) {
        throw std::invalid_argument("Multi-head event decoding requires the BasicPitchV1 contract");
    }
    if (options.aligned_model_heads) {
        throw std::invalid_argument(
            "Aligned onset/contour decoding requires the BasicPitchV1 contract; "
            "generic pitch outputs have no declared onset or contour alignment");
    }
    if (options.polyphonic && options.objective != PitchObjective::IndependentMultiPitch) {
        throw std::invalid_argument(
            "LegacyPitchV1 polyphony requires an explicit independent multi-pitch training objective");
    }
    const auto model_input_audio = resample_linear(audio, model_sample_rate);
    if (model_input_shape[1] > 0 && static_cast<std::size_t>(model_input_shape[1]) !=
        model_input_audio.samples.size()) {
        throw std::invalid_argument("Fixed LegacyPitchV1 waveform length does not match resampled input");
    }
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
    auto outputs = run_onnx(*session, options.cancellation, input_names, &input_tensor, 1,
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
    validate_activations(activations, output_info.GetElementCount());

    if (options.progress_callback) {
        options.progress_callback(0.5);
    }
    const double duration_seconds =
        static_cast<double>(audio.samples.size()) / audio.sample_rate;
    const auto filtered = preprocess_audio(audio);
    const auto onsets = detect_onsets(filtered);
    if (!options.polyphonic) {
        std::vector<PitchFrame> frames;
        std::vector<double> raw_selected;
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
            if (options.evidence_callback) raw_selected.push_back(row[best]);
        }
        if (options.progress_callback) {
            options.progress_callback(0.9);
        }
        auto notes = notes_from_frames(frames, duration_seconds, onsets,
                                       options.minimum_confidence);
        if (options.evidence_callback) {
            options.evidence_callback({frames, 0.0, frame_step_seconds, "onnx-generic",
                                       "selected-normalized-activation", {}, raw_selected, -1,
                                       EvidenceSemantics::RelativeActivation,
                                       EvidenceSemantics::RawActivation});
        }
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
    for (std::size_t pitch = 0; pitch < pitch_frames.size(); ++pitch) {
        auto& frames = pitch_frames[pitch];
        if (options.evidence_callback) {
            std::vector<double> raw;
            raw.reserve(frame_count);
            for (std::size_t frame = 0; frame < frame_count; ++frame) {
                raw.push_back(activations[frame * class_count + pitch]);
            }
            options.evidence_callback({frames, 0.0, frame_step_seconds, "onnx-generic",
                                       "selected-normalized-activation", {}, raw,
                                       static_cast<int>(pitch) + 28,
                                       EvidenceSemantics::RelativeActivation,
                                       EvidenceSemantics::RawActivation});
        }
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
}

std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
    const std::filesystem::path& model_path, const OnnxOptions& options) {
    return infer_onnx_session(audio,
        load_onnx_session(model_path, options.execution_provider, options.device_id), options);
}
#endif

namespace detail {
#ifdef TRANSCRIBE_ENABLE_ONNX
class RuntimeSession final : public OnnxSession {
public:
    RuntimeSession(const ModelDefinition& definition, const ModelLoadOptions& options) :
        session_(create_onnx_session(definition.path, options.provider, options.device_id)) {
        validate_onnx_contract(*session_, definition.contract);
    }
    std::vector<Note> infer(const AudioBuffer& audio, const OnnxOptions& options) override {
        return infer_onnx_session(audio, session_, options);
    }
    void describe(ModelMetadata& metadata) const override {
        Ort::AllocatorWithDefaultOptions allocator;
        const auto describe_tensors = [&](bool inputs) {
            std::vector<TensorContract> tensors;
            const auto count = inputs ? session_->GetInputCount() : session_->GetOutputCount();
            for (std::size_t i = 0; i < count; ++i) {
                const auto name = inputs ? session_->GetInputNameAllocated(i, allocator) :
                    session_->GetOutputNameAllocated(i, allocator);
                const auto type = inputs ? session_->GetInputTypeInfo(i) : session_->GetOutputTypeInfo(i);
                if (type.GetONNXType() != ONNX_TYPE_TENSOR) {
                    tensors.push_back({name.get(), "non-tensor", {}});
                } else {
                    const auto tensor = type.GetTensorTypeAndShapeInfo();
                    tensors.push_back({name.get(),
                        tensor.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? "float32" :
                            "onnx-type:" + std::to_string(tensor.GetElementType()), tensor.GetShape()});
                }
            }
            return tensors;
        };
        metadata.inputs = describe_tensors(true);
        metadata.outputs = describe_tensors(false);
    }
private:
    std::shared_ptr<Ort::Session> session_;
};
#endif

std::unique_ptr<OnnxSession> open_onnx_session(
    const ModelDefinition& definition, const ModelLoadOptions& options) {
#ifdef TRANSCRIBE_ENABLE_ONNX
    return std::make_unique<RuntimeSession>(definition, options);
#else
    (void)definition; (void)options;
    throw std::runtime_error("ONNX support is disabled; configure TRANSCRIBE_ENABLE_ONNX=ON");
#endif
}

std::string onnx_runtime_version() {
#ifdef TRANSCRIBE_ENABLE_ONNX
    return Ort::GetVersionString();
#else
    return "disabled";
#endif
}

std::vector<ProviderAvailability> onnx_providers() {
    std::vector<ProviderAvailability> result;
    std::vector<std::string> runtime;
#ifdef TRANSCRIBE_ENABLE_ONNX
    runtime = Ort::GetAvailableProviders();
#endif
    for (const auto provider : {OnnxExecutionProvider::Cpu, OnnxExecutionProvider::Cuda,
                               OnnxExecutionProvider::DirectML, OnnxExecutionProvider::Rocm}) {
        bool compiled = false;
        std::string name;
        switch (provider) {
        case OnnxExecutionProvider::Cpu:
            name = "CPUExecutionProvider";
#ifdef TRANSCRIBE_ENABLE_ONNX
            compiled = true;
#endif
            break;
        case OnnxExecutionProvider::Cuda:
            name = "CUDAExecutionProvider";
#ifdef TRANSCRIBE_ONNX_ENABLE_CUDA
            compiled = true;
#endif
            break;
        case OnnxExecutionProvider::DirectML:
            name = "DmlExecutionProvider";
#ifdef TRANSCRIBE_ONNX_ENABLE_DIRECTML
            compiled = true;
#endif
            break;
        case OnnxExecutionProvider::Rocm:
            name = "ROCMExecutionProvider";
#ifdef TRANSCRIBE_ONNX_ENABLE_ROCM
            compiled = true;
#endif
            break;
        }
        const bool available = std::find(runtime.begin(), runtime.end(), name) != runtime.end();
        result.push_back({provider, compiled, available,
            !compiled ? "Not enabled in this build" :
                (available ? "Runtime advertises provider; device/session validation occurs at load" :
                             "Provider absent from loaded ONNX Runtime")});
    }
    return result;
}
}

} // namespace transcribe
