#pragma once

#include "transcribe/transcriber.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace transcribe {

enum class EventLifecycle { Provisional, Stabilized, Refined, ManuallyCorrected, Rejected };

std::string_view evidence_semantics_name(EvidenceSemantics semantics) noexcept;
EvidenceSemantics parse_evidence_semantics(std::string_view value);

struct EventEvidence {
    double time_seconds{};
    int midi_pitch{-1};
    double raw_strength{};
    double raw_voicing{};
    std::optional<double> fractional_midi;
    std::optional<double> raw_model_value;
    int raw_midi_pitch{-1};
    EvidenceSemantics strength_semantics{EvidenceSemantics::Unspecified};
    EvidenceSemantics raw_value_semantics{EvidenceSemantics::Unspecified};
    std::optional<double> spectral_peak_fraction;
    std::optional<double> onset_strength{};
    std::optional<double> note_state{};
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
    double pitch_stability_seconds{0.05};
    double pitch_tolerance_semitones{0.75};
    double onset_threshold{0.5};
    double minimum_onset_spacing_seconds{0.07};
    double onset_energy_rise{0.25};
    bool use_model_onsets{true};
    double onset_state_drop{0.15};
};

struct PitchActivitySegment {
    int midi_pitch{-1};
    std::vector<std::size_t> frame_indices;
    double onset_seconds{};
    double offset_seconds{};
    bool onset_from_attack{};
    bool offset_from_attack{};
};

void validate_temporal_decoding_options(const TemporalDecodingOptions& options);

std::vector<int> stabilize_pitch_activity(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options);

std::vector<PitchActivitySegment> detect_pitch_activity_regions(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options);

// Onsets use window-center times; translate detect_onset_evidence's window starts first.
std::vector<PitchActivitySegment> segment_pitch_activity(
    const FrameEvidenceTrack& track, const TemporalDecodingOptions& options,
    std::span<const OnsetEvidence> onsets = {});

struct EventDiagnostics {
    double activity_duration_seconds{};
    std::optional<double> pitch_stability;
    std::optional<double> onset_strength;
    std::optional<double> maximum_note_state;
};

EventDiagnostics summarize_event_evidence(const CanonicalNoteEvent& event, double hop_seconds,
                                         double window_seconds = 0);

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
