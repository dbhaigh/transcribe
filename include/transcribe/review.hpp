#pragma once

#include "transcribe/score.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace transcribe {

enum class ReviewDimension { Pitch, Onset, Offset, Fingering, Rhythm, Distribution };
enum class ReviewUncertainty { NotAssessed, NoConflictObserved, Ambiguous };
enum class ReviewSuggestionKind { AlternativePitch, IsolatedOctave, SplitRepeatedNote };

struct ReviewAssessment {
    ReviewDimension dimension{ReviewDimension::Pitch};
    ReviewUncertainty uncertainty{ReviewUncertainty::NotAssessed};
    std::string explanation;
};

struct ReviewFinding {
    std::string id;
    std::string note_id;
    std::string category;
    std::string explanation;
    double ranking_score{};
};

struct ReviewSuggestion {
    std::string id;
    std::string note_id;
    ReviewSuggestionKind kind{ReviewSuggestionKind::AlternativePitch};
    std::optional<int> suggested_midi_pitch;
    std::optional<double> split_time_seconds;
    double ranking_score{};
    std::string ranking_basis;
    std::string explanation;
};

struct NoteReview {
    std::string note_id;
    std::size_t raw_evidence_samples{};
    bool calibrated_confidence_available{};
    std::vector<ReviewAssessment> assessments;
    std::vector<ReviewFinding> findings;
    std::vector<ReviewSuggestion> suggestions;
};

struct ScoreReview {
    std::vector<NoteReview> notes;
    std::size_t notes_without_evidence{};
    std::size_t calibrated_notes{};
    bool out_of_distribution_assessed{};
    std::string calibration_limitation;
};

struct ReviewOptions {
    double octave_neighbor_window_seconds{1.5};
    double repeated_note_valley_ratio{0.45};
    std::size_t minimum_valley_samples{5};
};

struct CalibrationSample {
    double predicted_probability{};
    bool correct{};
};

struct CalibrationDatasetInfo {
    std::string identifier;
    bool held_out_from_training{};
    bool representative_of_deployment{};
    bool training_overlap_reviewed{};
};

struct ReliabilityBin {
    std::size_t sample_count{};
    double mean_predicted_probability{};
    double observed_accuracy{};
};

struct CalibrationReport {
    bool available{};
    std::string limitation;
    std::string dataset_identifier;
    std::size_t sample_count{};
    std::optional<double> brier_score;
    std::vector<ReliabilityBin> reliability_bins;
};

ScoreReview review_score(const ScoreDocument& score,
                         const ReviewOptions& options = {});

CalibrationReport evaluate_calibration(
    std::span<const CalibrationSample> samples,
    const CalibrationDatasetInfo& dataset,
    std::size_t reliability_bin_count = 10,
    std::size_t minimum_sample_count = 100);

} // namespace transcribe
