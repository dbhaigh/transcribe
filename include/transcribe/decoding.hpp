#pragma once

#include "transcribe/transcriber.hpp"

#include <cstdint>
#include <string>

namespace transcribe {

enum class EventLifecycle { Provisional, Stabilized, Refined, ManuallyCorrected, Rejected };

struct EventEvidence {
    double time_seconds{};
    int midi_pitch{-1};
    double raw_strength{};
    double raw_voicing{};
    std::optional<double> fractional_midi;
    std::optional<double> raw_model_value;
    int raw_midi_pitch{-1};
};

struct CanonicalNoteEvent {
    std::string id;
    double midi_pitch{};
    double onset_seconds{};
    double offset_seconds{};
    std::uint64_t onset_sample{};
    std::uint64_t offset_sample{};
    std::uint32_t source_sample_rate{};
    std::string audio_source_id;
    std::string source_model;
    std::string detection_method;
    std::string evidence_kind;
    EventLifecycle state{EventLifecycle::Stabilized};
    std::optional<double> calibrated_confidence;
    std::optional<double> intensity_rms;
    std::optional<int> velocity;
    std::optional<std::string> instrument;
    std::optional<std::string> voice;
    std::optional<std::string> articulation;
    std::vector<EventEvidence> evidence;
};

struct TemporalDecodingOptions {
    double activation_on{0.4};
    double activation_off{0.4};
    double maximum_gap_seconds{0.0};
    double minimum_duration_seconds{0.055};
    double energy_floor_rms{0.0015};
    double energy_relative_threshold{0.08};
    double spectral_peak_fraction{0.02};
    bool refine_with_audio{true};
    bool correct_isolated_octaves{false};
};

struct DecodedTranscription {
    std::vector<CanonicalNoteEvent> events;
    std::vector<CanonicalNoteEvent> rejected_events;
    std::vector<FrameEvidenceTrack> raw_tracks;
};

DecodedTranscription decode_note_events(
    const AudioBuffer& audio, std::span<const FrameEvidenceTrack> tracks,
    const TemporalDecodingOptions& options, const std::string& audio_source_id,
    const std::string& source_model);

DecodedTranscription transcribe_note_events(
    const AudioBuffer& audio, const AnalysisOptions& analysis,
    const TemporalDecodingOptions& decoding, const std::string& audio_source_id);

DecodedTranscription transcribe_note_events_wav_streaming(
    const std::filesystem::path& path, const StreamingOptions& streaming,
    const TemporalDecodingOptions& decoding, const std::string& audio_source_id);

DecodedTranscription transcribe_note_events_with_onnx(
    const AudioBuffer& audio, const std::filesystem::path& model,
    const OnnxOptions& analysis, const TemporalDecodingOptions& decoding,
    const std::string& audio_source_id, const std::string& model_identifier);

} // namespace transcribe
