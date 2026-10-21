#pragma once

#include "transcribe/decoding.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace transcribe {

struct MeterConstraint {
    int numerator{4};
    int denominator{4};
    // Group sizes are in denominator units. Empty uses conventional grouping.
    std::vector<int> grouping;
};

struct InterpretationOptions {
    std::optional<double> tempo_bpm;
    std::optional<MeterConstraint> meter;
    std::optional<int> key_fifths;
    std::optional<bool> minor_key;
    // Length of an explicitly requested pickup measure in quarter-note units.
    std::optional<double> pickup_quarters;
    std::string instrument;
    // Written pitch = sounding pitch + this value.
    int written_transposition_semitones{};
    double chord_onset_tolerance_seconds{0.04};
    double onset_quantization_tolerance_seconds{0.04};
    double rhythm_quantization_tolerance{0.10};
    double tempo_change_threshold{0.15};
    std::size_t maximum_hypotheses{6};
    bool detect_tempo_changes{true};
    bool infer_local_keys{true};
};

struct TempoSegment {
    double start_seconds{};
    double end_seconds{};
    double start_quarters{};
    double beats_per_minute{120.0};
    double confidence{};
    bool user_constrained{};
};

struct BeatPosition {
    double time_seconds{};
    double quarter_position{};
    double confidence{};
    bool downbeat{};
};

struct MeterHypothesis {
    int numerator{4};
    int denominator{4};
    std::vector<int> grouping{1, 1, 1, 1};
    double confidence{};
    bool user_constrained{};
};

struct KeyHypothesis {
    int fifths{};
    bool minor{};
    double confidence{};
    bool user_constrained{};
};

struct RhythmValue {
    // MusicXML note type denominator: 1=whole, 2=half, 4=quarter, etc.
    int denominator{};
    int dots{};
    int tuplet_actual{};
    int tuplet_normal{};
    double duration_quarters{};
    double fit_error_quarters{};
    bool symbolic{};
};

struct InterpretedNote {
    std::size_t source_event_index{};
    double onset_quarters{};
    double duration_quarters{};
    double onset_residual_seconds{};
    double offset_residual_seconds{};
    int voice{};
    std::optional<std::size_t> chord_group;
    int written_midi_pitch{-1};
    std::string spelling_step;
    int spelling_alter{};
    int spelling_octave{};
    bool accidental_required{};
    RhythmValue rhythm;
    std::vector<RhythmValue> rhythm_alternatives;
    double confidence{};
    std::optional<int> velocity;
    std::optional<std::string> articulation;
    bool tie_start{};
    bool tie_stop{};
    int sounding_midi_pitch{-1};
};

struct InterpretedRest {
    int voice{};
    double onset_quarters{};
    double duration_quarters{};
    RhythmValue rhythm;
};

struct TonalRegion {
    std::size_t first_measure{};
    std::size_t end_measure{};
    KeyHypothesis key;
};

struct ScoreHypothesis {
    std::string id;
    std::vector<TempoSegment> tempo_map;
    std::vector<BeatPosition> beats;
    MeterHypothesis meter;
    KeyHypothesis key;
    std::vector<KeyHypothesis> alternative_keys;
    std::vector<TonalRegion> local_tonal_context;
    std::vector<InterpretedNote> notes;
    std::vector<InterpretedRest> rests;
    double pickup_quarters{};
    double total_quarters{};
    double swing_ratio{1.0};
    double swing_confidence{};
    bool swing_inferred{};
    double confidence{};
    bool tempo_ambiguous{};
    bool meter_ambiguous{};
    bool key_ambiguous{};
    std::vector<std::string> uncertainties;
    std::string instrument;
    int written_transposition_semitones{};
};

struct ScoreInterpretation {
    // Immutable snapshots preserve the original detector output and its measured times.
    std::vector<std::shared_ptr<const CanonicalNoteEvent>> source_events;
    // Ordered highest-ranked first; close alternatives are retained rather than discarded.
    std::vector<ScoreHypothesis> hypotheses;
};

ScoreInterpretation interpret_note_events(
    std::span<const CanonicalNoteEvent> events,
    const InterpretationOptions& options = {});

ScoreInterpretation interpret_notes(
    std::span<const Note> notes,
    const InterpretationOptions& options = {});

ScoreMetadata metadata_for_hypothesis(const ScoreHypothesis& hypothesis);

} // namespace transcribe
