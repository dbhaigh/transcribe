#pragma once

#include "transcribe/transcriber.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace transcribe {

inline constexpr std::size_t maximum_guitar_strings = 12;

struct GuitarTabPosition {
    std::size_t note_index{};
    int string_number{};
    int fret{-1};
    bool manually_corrected{};
    // A suggested left-hand finger (0=open); never evidence of the performed finger.
    std::optional<int> suggested_finger{};
};

struct FingeringCorrection {
    int midi_pitch{};
    double onset_seconds{};
    double duration_seconds{};
    int string_number{};
    int fret{};
};

struct FingeringOptions {
    std::array<int, 6> tuning{64, 59, 55, 50, 45, 40};
    int capo_fret{};
    int maximum_fret{24};
    int preferred_position{5};
    double position_shift_cost{1.4};
    double simultaneous_onset_tolerance_seconds{0.03};
    double correction_match_tolerance_seconds{0.25};
    double ambiguity_cost_margin{0.5};
    int maximum_chord_fret_span{5};
    std::size_t maximum_alternatives{5};
    std::vector<GuitarTabPosition> position_overrides;
    std::vector<FingeringCorrection> manual_corrections;
    double fret_height_cost{0.12};
    double preferred_position_cost{0.75};
    double string_preference_cost{0.1};
    double stretch_cost{1.5};
    double repeated_string_cost{0.0};
    double string_crossing_cost{0.0};
    double awkward_transition_cost{0.0};
    double unresolved_note_cost{1000.0};
    int maximum_fretting_fingers{4};
    std::size_t beam_width{64};
    std::size_t local_solution_limit{64};
    std::size_t search_node_limit{100000};
    // Empty retains the legacy six-string tuning; otherwise string 1 is first.
    std::vector<int> string_tuning;
    std::optional<int> physical_fret_count;
    int minimum_midi_pitch{0};
    int maximum_midi_pitch{127};
};

std::span<const int> configured_tuning(const FingeringOptions& options) noexcept;
std::vector<int> parse_string_tuning(std::string_view midi_pitches);

struct FingeringCostComponents {
    double fret_height{};
    double preferred_position{};
    double hand_movement{};
    double string_preference{};
    double chord_stretch{};
    double unassigned_notes{};
    double repeated_strings{};
    double string_crossings{};
    double awkward_transitions{};

    double total() const;
};

struct FingeringCandidate {
    int string_number{};
    int fret{-1};
    double sequence_cost{};
    FingeringCostComponents sequence_cost_components;
};

struct FingeringNoteResult {
    GuitarTabPosition position;
    bool playable{};
    bool ambiguous{};
    bool correction_rejected{};
    std::vector<FingeringCandidate> candidates;
    // All pitch-compatible positions, including those not reached by the sequence beam.
    std::vector<GuitarTabPosition> playable_positions;
    std::string explanation;
    bool performed_position_unknown{true};
    std::optional<double> calibrated_fingering_confidence;
};

struct FingeringAlternative {
    std::vector<GuitarTabPosition> positions;
    FingeringCostComponents cost;
};

struct FingeringResult {
    std::vector<GuitarTabPosition> positions;
    std::vector<FingeringNoteResult> notes;
    std::vector<FingeringAlternative> alternatives;
    std::vector<std::size_t> rejected_corrections;
    FingeringCostComponents cost;
    bool fully_playable{};
    bool search_truncated{};
    // True only if every complete sequence was retained and returned as an alternative.
    bool alternatives_complete{};
    std::size_t visited_search_nodes{};
    std::string explanation;
};

FingeringResult assign_fingerings(const std::vector<Note>& notes,
                                  const FingeringOptions& options = {});
void validate_fingering_assignment(const std::vector<Note>& notes,
                                   const std::vector<GuitarTabPosition>& positions,
                                   const FingeringOptions& options = {});

} // namespace transcribe
