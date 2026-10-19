#pragma once

#include "transcribe/audio.hpp"

#include <filesystem>
#include <functional>
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
};

struct AnalysisOptions {
    double minimum_confidence{0.25};
    bool parallel{true};
    std::size_t parallel_threshold{256};
    std::function<void(double)> progress_callback;
};

struct OnnxOptions {
    bool polyphonic{};
    bool independent_probabilities{};
    double minimum_confidence{0.25};
    double polyphonic_threshold{0.4};
    std::size_t maximum_pitches_per_frame{6};
    std::function<void(double)> progress_callback;
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
};

struct StreamingOptions {
    double chunk_seconds{15.0};
    double overlap_seconds{1.0};
    AnalysisOptions analysis;
};

struct StreamingResult {
    std::vector<Note> notes;
    double duration_seconds{};
};

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio);
std::vector<Note> transcribe_monophonic(const AudioBuffer& audio,
                                        const AnalysisOptions& options);
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
