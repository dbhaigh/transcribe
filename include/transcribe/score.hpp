#pragma once

#include "transcribe/guitar_tab.hpp"
#include "transcribe/interpretation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace transcribe {

inline constexpr std::uint32_t score_schema_version = 1;

enum class ScoreEditOrigin { Inference, User, Migration, Refinement, Undo, Redo };

struct ScoreEditProvenance {
    ScoreEditOrigin origin{ScoreEditOrigin::Inference};
    std::string actor;
    std::string reason;
    std::string command_id;
};

struct ScoreRevisionRecord {
    std::uint64_t revision{};
    ScoreEditOrigin origin{ScoreEditOrigin::Inference};
    std::string actor;
    std::string reason;
    std::string command_id;
};

enum class ReviewDecisionOutcome { Accepted, Rejected };

struct ScoreReviewDecision {
    std::string suggestion_id;
    std::string note_id;
    ReviewDecisionOutcome outcome{ReviewDecisionOutcome::Rejected};
    std::uint64_t revision{};
    std::string reason;
};

struct ScoreNote {
    std::string id;
    std::vector<std::string> source_event_ids;
    double measured_onset_seconds{};
    double measured_offset_seconds{};
    double onset_quarters{};
    double duration_quarters{};
    double onset_residual_seconds{};
    double offset_residual_seconds{};
    int voice{1};
    std::optional<std::string> chord_group_id;
    int sounding_midi_pitch{-1};
    int written_midi_pitch{-1};
    std::string spelling_step;
    int spelling_alter{};
    int spelling_octave{};
    bool accidental_required{};
    RhythmValue rhythm;
    std::vector<RhythmValue> rhythm_alternatives;
    double interpretation_evidence{};
    std::optional<double> calibrated_confidence;
    std::optional<double> intensity_rms;
    std::vector<EventEvidence> raw_evidence;
    std::optional<int> velocity;
    std::optional<std::string> articulation;
    std::optional<std::string> tie_group_id;
    bool tie_start{};
    bool tie_stop{};
    bool user_authored{};
    std::uint64_t revision{};
    ScoreEditProvenance provenance;
};

struct ScoreRest {
    std::string id;
    int voice{1};
    double onset_quarters{};
    double duration_quarters{};
    RhythmValue rhythm;
    bool user_authored{};
    std::uint64_t revision{};
    ScoreEditProvenance provenance;
};

struct ScoreTabNote {
    std::string id;
    std::string notation_note_id;
    std::optional<int> string_number;
    std::optional<int> fret;
    bool manually_corrected{};
    std::uint64_t revision{};
};

struct ScoreInstrument {
    std::string name;
    int written_transposition_semitones{};
    // MIDI open-string pitches in string-number order (may be reentrant).
    std::vector<int> tuning{64, 59, 55, 50, 45, 40};
    int capo_fret{};
    std::optional<int> physical_fret_count;
};

struct ScoreDocument {
    std::uint32_t schema_version{score_schema_version};
    std::string id;
    std::string source_id;
    std::uint32_t source_sample_rate{};
    std::string time_reference{"recording-relative-seconds"};
    std::uint64_t revision{};
    ScoreMetadata metadata;
    ScoreInstrument instrument;
    std::vector<CanonicalNoteEvent> source_events;
    std::vector<TempoSegment> tempo_map;
    std::vector<BeatPosition> beats;
    MeterHypothesis meter;
    KeyHypothesis key;
    std::vector<KeyHypothesis> alternative_keys;
    std::vector<TonalRegion> local_tonal_context;
    double pickup_quarters{};
    double total_quarters{};
    double swing_ratio{1.0};
    double swing_confidence{};
    bool swing_inferred{};
    bool tempo_ambiguous{};
    bool meter_ambiguous{};
    bool key_ambiguous{};
    std::vector<std::string> uncertainties;
    std::vector<ScoreNote> notes;
    std::vector<ScoreRest> rests;
    std::vector<ScoreTabNote> tab_notes;
    std::vector<ScoreReviewDecision> review_decisions;
    std::vector<ScoreRevisionRecord> history;
};

struct InsertScoreNote {
    ScoreNote note;
};

struct DeleteScoreElement {
    std::string element_id;
};

struct ChangeScorePitch {
    std::string note_id;
    int sounding_midi_pitch{};
};

struct ChangeScoreDuration {
    std::string note_id;
    double duration_quarters{};
};

struct SplitScoreNote {
    std::string note_id;
    double first_duration_quarters{};
    std::string second_note_id;
};

struct MergeScoreNotes {
    std::string first_note_id;
    std::string second_note_id;
    std::string merged_note_id;
};

struct InsertScoreRest {
    ScoreRest rest;
};

struct ChangeScoreFingering {
    std::string note_id;
    std::optional<int> string_number;
    std::optional<int> fret;
};

struct ReinterpretScoreRhythm {
    std::string note_id;
    double onset_quarters{};
    double duration_quarters{};
    RhythmValue rhythm;
};

struct UpdateScoreContext {
    ScoreMetadata metadata;
    ScoreInstrument instrument;
    double pickup_quarters{};
    bool replace_tempo_map{true};
};

struct RecordReviewDecision {
    std::string suggestion_id;
    std::string note_id;
    ReviewDecisionOutcome outcome{ReviewDecisionOutcome::Rejected};
    std::string reason;
};

using ScoreEditCommand = std::variant<
    InsertScoreNote, DeleteScoreElement, ChangeScorePitch, ChangeScoreDuration,
    SplitScoreNote, MergeScoreNotes, InsertScoreRest, ChangeScoreFingering,
    ReinterpretScoreRhythm, UpdateScoreContext, RecordReviewDecision>;

struct ScoreTransaction {
    std::uint64_t base_revision{};
    std::string actor{"user"};
    std::string reason;
    std::string command_id;
    std::vector<ScoreEditCommand> commands;
};

struct ScoreTransactionResult {
    std::uint64_t revision{};
    std::size_t changed_elements{};
};

class ScoreEditor {
public:
    explicit ScoreEditor(ScoreDocument document);

    const ScoreDocument& document() const noexcept;
    ScoreTransactionResult apply(const ScoreTransaction& transaction);
    std::uint64_t undo(std::uint64_t expected_revision);
    std::uint64_t redo(std::uint64_t expected_revision);

private:
    struct Snapshot {
        ScoreDocument before;
        ScoreDocument after;
    };

    ScoreDocument document_;
    std::vector<Snapshot> undo_stack_;
    std::vector<Snapshot> redo_stack_;
};

ScoreDocument make_score_document(
    const ScoreInterpretation& interpretation, std::size_t hypothesis_index,
    std::string source_id, const ScoreMetadata& metadata,
    const GuitarTabOptions& tab_options = {});
ScoreDocument make_empty_score_document(std::string score_id);
ScoreInterpretation to_score_interpretation(const ScoreDocument& document);
std::vector<Note> score_notes(const ScoreDocument& document);
double score_position_seconds(const ScoreDocument& document, double quarter_position);
double score_position_quarters(const ScoreDocument& document, double seconds);
double recording_sample_seconds(std::uint64_t frame, std::uint32_t sample_rate);
std::uint64_t recording_sample_position(double seconds, std::uint32_t sample_rate);
void validate_score_document(const ScoreDocument& document);
std::string serialize_score_document(const ScoreDocument& document);
ScoreDocument deserialize_score_document(std::string_view json);

} // namespace transcribe
