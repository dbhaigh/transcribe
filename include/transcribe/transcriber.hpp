#pragma once

#include "transcribe/audio.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace transcribe {

struct Note {
    int midi_pitch{};
    double start_seconds{};
    double duration_seconds{};
    std::string name;
    double confidence{1.0};
    double amplitude{};
};

enum class NoteEventType {
    Onset,
    Offset
};

struct NoteEvent {
    NoteEventType type{NoteEventType::Onset};
    int midi_pitch{-1};
    double time_seconds{};
    double confidence{};
    double detected_at_seconds{};
};

struct RealtimeOptions {
    std::uint32_t input_sample_rate{44100};
    double latency_ms{40.0};
    double update_rate_hz{100.0};
    double minimum_confidence{0.45};
    bool adaptive_threshold{true};
    std::size_t onset_stability_frames{2};
    std::size_t offset_stability_frames{3};
};

class RealtimeTranscriber {
public:
    explicit RealtimeTranscriber(const RealtimeOptions& options = {});
    ~RealtimeTranscriber();
    RealtimeTranscriber(const RealtimeTranscriber&) = delete;
    RealtimeTranscriber& operator=(const RealtimeTranscriber&) = delete;

    void push_samples(std::span<const float> samples);
    void finish();
    std::vector<Note> live_notes() const;
    std::vector<NoteEvent> take_events();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PitchEstimate {
    int midi_pitch{-1};
    double confidence{};
    double voicing_probability{};
};

struct PitchFrame {
    int midi_pitch{-1};
    double time_seconds{};
    double confidence{};
    double voicing_probability{};
    double amplitude{};
};

struct AudioPreviewData {
    double duration_seconds{};
    std::size_t columns{};
    std::size_t frequency_bands{};
    std::vector<float> waveform;
    std::vector<float> spectrogram;
};

enum class InstrumentPrior {
    None,
    Guitar,
    Piano,
    Voice
};

enum class OnnxExecutionProvider {
    Cpu,
    Cuda,
    DirectML,
    Rocm
};

struct AnalysisTimings {
    double preprocessing_seconds{};
    double onset_fft_seconds{};
    double resampling_seconds{};
    double pitch_analysis_seconds{};
    bool spectrogram_cache_hit{};
};

struct AnalysisOptions {
    double minimum_confidence{0.25};
    bool parallel{true};
    std::size_t parallel_threshold{256};
    bool fast_preview{};
    bool polyphonic{};
    InstrumentPrior instrument{InstrumentPrior::None};
    std::size_t maximum_pitches_per_frame{6};
    double spectral_threshold{0.3};
    std::function<void(double)> progress_callback;
    std::function<void(const AnalysisTimings&)> timing_callback;
};

struct OnnxOptions {
    bool polyphonic{};
    bool independent_probabilities{};
    double minimum_confidence{0.25};
    double polyphonic_threshold{0.4};
    std::size_t maximum_pitches_per_frame{6};
    std::function<void(double)> progress_callback;
    OnnxExecutionProvider execution_provider{OnnxExecutionProvider::Cpu};
    int device_id{};
};

struct ScoreMetadata {
    double tempo_bpm{120.0};
    int time_numerator{4};
    int time_denominator{4};
    int key_fifths{};
    bool minor_key{};
    bool tempo_inferred{};
    bool meter_inferred{};
    bool key_inferred{};
    double tempo_confidence{};
    double meter_confidence{};
    double key_confidence{};
    double swing_ratio{1.0};
    bool swing_inferred{};
    double swing_confidence{};
};

struct StreamingOptions {
    double chunk_seconds{15.0};
    double overlap_seconds{1.0};
    AnalysisOptions analysis;
    std::function<void(const std::vector<Note>&)> notes_callback;
    std::function<void(const AudioBlockView&)> audio_callback;
};

struct StreamingResult {
    std::vector<Note> notes;
    double duration_seconds{};
};

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio);
std::vector<Note> transcribe_monophonic(const AudioBuffer& audio,
                                        const AnalysisOptions& options);
AudioPreviewData make_audio_preview(std::span<const float> samples,
                                    std::uint32_t sample_rate,
                                    std::size_t maximum_columns = 128,
                                    std::size_t frequency_bands = 48);
StreamingResult transcribe_wav_streaming(const std::filesystem::path& path,
                                         const StreamingOptions& options = {});
std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path);
std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path,
                                       const OnnxOptions& options);
AudioBuffer preprocess_audio(const AudioBuffer& audio, double high_pass_hz = 55.0);
PitchEstimate yin_pitch_estimate(std::span<const float> samples,
                                 std::uint32_t sample_rate,
                                 double threshold = 0.15);
std::vector<double> detect_onsets(const AudioBuffer& audio);
std::vector<Note> notes_from_frames(std::span<const PitchFrame> frames,
                                    double duration_seconds,
                                    std::span<const double> onsets = {},
                                    double minimum_confidence = 0.25);
ScoreMetadata infer_score_metadata(const std::vector<Note>& notes);
std::string midi_note_name(int midi_pitch);

} // namespace transcribe
