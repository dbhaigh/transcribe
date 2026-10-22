#pragma once

#include "transcribe/decoding.hpp"

namespace transcribe {

enum class GuitarPitchGesture { None, Vibrato, SustainedRise, SustainedFall };

struct GuitarTechniqueOptions {
    bool enabled{};
    bool isolated_pitch_contour{};
    double minimum_voicing{0.8};
    double minimum_duration_seconds{0.35};
    double maximum_frame_gap_seconds{0.04};
    double minimum_coverage{0.8};
    double minimum_excursion_semitones{0.35};
    double maximum_excursion_semitones{4.0};
    double minimum_vibrato_hz{3.0};
    double maximum_vibrato_hz{9.0};
    std::size_t maximum_evidence_frames{8192};
};

struct GuitarTechniqueResult {
    GuitarPitchGesture gesture{GuitarPitchGesture::None};
    // Original pitch calibration, never reused as technique/fingering confidence.
    std::optional<double> calibrated_pitch_confidence;
    std::optional<double> calibrated_technique_confidence;
    double contour_coverage{};
    double excursion_semitones{};
    std::optional<double> modulation_hz;
    // Rule support only; not a calibrated probability.
    double heuristic_support{};
    std::vector<std::string> possible_techniques;
    bool physical_technique_unresolved{true};
    std::string explanation;
};

std::string_view guitar_pitch_gesture_name(GuitarPitchGesture gesture) noexcept;
GuitarTechniqueResult analyze_guitar_technique(
    const CanonicalNoteEvent& event, InstrumentPrior instrument,
    const GuitarTechniqueOptions& options = {});

} // namespace transcribe
