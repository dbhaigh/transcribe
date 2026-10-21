#include "transcribe/review.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

transcribe::ScoreNote note(std::string id, int pitch, double onset,
                           double offset) {
    transcribe::ScoreNote result;
    result.id = std::move(id);
    result.sounding_midi_pitch = pitch;
    result.written_midi_pitch = pitch;
    result.measured_onset_seconds = onset;
    result.measured_offset_seconds = offset;
    result.duration_quarters = offset - onset;
    result.rhythm.duration_quarters = result.duration_quarters;
    return result;
}

const transcribe::ReviewSuggestion* find_suggestion(
    const transcribe::NoteReview& review,
    transcribe::ReviewSuggestionKind kind) {
    const auto found = std::find_if(review.suggestions.begin(),
        review.suggestions.end(), [&](const auto& suggestion) {
            return suggestion.kind == kind;
        });
    return found == review.suggestions.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("review reports missing evidence without manufacturing confidence",
          "[review]") {
    transcribe::ScoreDocument score;
    score.notes.push_back(note("note-1", 60, 0.1, 0.4));
    const auto review = transcribe::review_score(score);

    REQUIRE(review.notes.size() == 1);
    REQUIRE(review.notes_without_evidence == 1);
    REQUIRE(review.calibrated_notes == 0);
    REQUIRE_FALSE(review.out_of_distribution_assessed);
    REQUIRE(review.calibration_limitation.find("not assessed") != std::string::npos);
    REQUIRE_FALSE(review.notes.front().calibrated_confidence_available);
    REQUIRE(std::any_of(review.notes.front().findings.begin(),
        review.notes.front().findings.end(), [](const auto& finding) {
            return finding.category == "missing-evidence";
        }));
    REQUIRE(std::any_of(review.notes.front().assessments.begin(),
        review.notes.front().assessments.end(), [](const auto& assessment) {
            return assessment.dimension == transcribe::ReviewDimension::Onset &&
                assessment.uncertainty == transcribe::ReviewUncertainty::NotAssessed;
        }));
}

TEST_CASE("unfamiliar score evidence remains unassessed for distribution shift",
          "[review][distribution]") {
    transcribe::ScoreDocument score;
    score.notes.push_back(note("unfamiliar", 11, 0.0, 0.9));

    const auto review = transcribe::review_score(score);
    REQUIRE_FALSE(review.out_of_distribution_assessed);
    const auto distribution = std::find_if(
        review.notes.front().assessments.begin(),
        review.notes.front().assessments.end(), [](const auto& assessment) {
            return assessment.dimension ==
                transcribe::ReviewDimension::Distribution;
        });
    REQUIRE(distribution != review.notes.front().assessments.end());
    REQUIRE(distribution->uncertainty ==
            transcribe::ReviewUncertainty::NotAssessed);
}

TEST_CASE("review ranks conflicting raw pitch evidence without calling it probability",
          "[review]") {
    transcribe::ScoreDocument score;
    auto detected = note("note-2", 60, 0.1, 0.5);
    detected.source_event_ids.push_back("event-2");
    detected.raw_evidence = {
        {0.2, 60, 0.8, 0.9, std::nullopt, 0.3, 60,
         transcribe::EvidenceSemantics::RelativeActivation,
         transcribe::EvidenceSemantics::RawActivation, std::nullopt},
        {0.2, 64, 0.0, 0.0, std::nullopt, 0.9, 64,
         transcribe::EvidenceSemantics::RelativeActivation,
         transcribe::EvidenceSemantics::RawActivation, std::nullopt}};
    score.notes.push_back(detected);
    transcribe::CanonicalNoteEvent source;
    source.id = "event-2";
    source.midi_pitch = 60;
    score.source_events.push_back(source);

    const auto review = transcribe::review_score(score);
    REQUIRE(review.notes.front().findings.front().category ==
            "model-pitch-conflict");
    const auto* suggestion = find_suggestion(
        review.notes.front(), transcribe::ReviewSuggestionKind::AlternativePitch);
    REQUIRE(suggestion != nullptr);
    REQUIRE(suggestion->suggested_midi_pitch == 64);
    REQUIRE(suggestion->ranking_score > 0.7);
    REQUIRE(suggestion->ranking_basis.find("not a probability") !=
            std::string::npos);
    REQUIRE_FALSE(review.notes.front().calibrated_confidence_available);
}

TEST_CASE("review flags an isolated octave excursion as a reversible suggestion",
          "[review]") {
    transcribe::ScoreDocument score;
    score.notes = {
        note("before", 60, 0.0, 0.2),
        note("middle", 72, 0.3, 0.5),
        note("after", 60, 0.6, 0.8)};

    const auto review = transcribe::review_score(score);
    const auto middle = std::find_if(review.notes.begin(), review.notes.end(),
        [](const auto& item) { return item.note_id == "middle"; });
    REQUIRE(middle != review.notes.end());
    const auto* suggestion = find_suggestion(
        *middle, transcribe::ReviewSuggestionKind::IsolatedOctave);
    REQUIRE(suggestion != nullptr);
    REQUIRE(suggestion->suggested_midi_pitch == 60);
}

TEST_CASE("review suggests a repeated-note split only at a measured evidence valley",
          "[review]") {
    transcribe::ScoreDocument score;
    auto repeated = note("repeat", 60, 0.0, 1.0);
    for (int index = 0; index < 8; ++index) {
        const double strength = index == 4 ? 0.1 : 0.8;
        repeated.raw_evidence.push_back({
            0.1 * (index + 1), 60, strength, 0.9, std::nullopt,
            std::nullopt, 60, transcribe::EvidenceSemantics::HeuristicScore,
            transcribe::EvidenceSemantics::Unspecified, std::nullopt});
    }
    score.notes.push_back(repeated);

    const auto review = transcribe::review_score(score);
    const auto* suggestion = find_suggestion(
        review.notes.front(), transcribe::ReviewSuggestionKind::SplitRepeatedNote);
    REQUIRE(suggestion != nullptr);
    REQUIRE(suggestion->split_time_seconds == 0.5);
}

TEST_CASE("calibration diagnostics require sufficient independent representative labels",
          "[review]") {
    const transcribe::CalibrationDatasetInfo suitable{
        "held-out-guitar-set-v1", true, true, true};
    std::vector<transcribe::CalibrationSample> samples(100, {0.8, true});
    const auto report = transcribe::evaluate_calibration(samples, suitable);
    REQUIRE(report.available);
    REQUIRE(report.sample_count == 100);
    REQUIRE(report.brier_score.has_value());
    REQUIRE(std::abs(*report.brier_score - 0.04) < 1e-12);
    REQUIRE(report.reliability_bins.size() == 10);
    REQUIRE(report.reliability_bins[8].sample_count == 100);

    const auto overlap_unknown = transcribe::evaluate_calibration(
        samples, {"overlap-unknown", true, true, false});
    REQUIRE_FALSE(overlap_unknown.available);
    REQUIRE(overlap_unknown.limitation.find("overlap reviewed") !=
            std::string::npos);

    const auto too_small = transcribe::evaluate_calibration(
        std::span<const transcribe::CalibrationSample>(samples.data(), 10),
        suitable);
    REQUIRE_FALSE(too_small.available);
    REQUIRE_FALSE(too_small.brier_score);
    const std::vector<transcribe::CalibrationSample> invalid{{1.1, false}};
    REQUIRE_THROWS_AS(
        transcribe::evaluate_calibration(invalid, suitable, 10, 1),
        std::invalid_argument);
}
