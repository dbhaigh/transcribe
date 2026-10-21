#pragma once

#include "transcribe/transcriber.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace transcribe {

struct GuitarTabPosition {
    std::size_t note_index{};
    int string_number{};
    int fret{-1};
    bool manually_corrected{};
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
};

struct FingeringCostComponents {
    double fret_height{};
    double preferred_position{};
    double hand_movement{};
    double string_preference{};
    double chord_stretch{};
    double unassigned_notes{};

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
};

FingeringResult assign_fingerings(const std::vector<Note>& notes,
                                  const FingeringOptions& options = {});

} // namespace transcribe
