#include "transcribe/review.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace transcribe {
namespace {

constexpr double simultaneous_tolerance_seconds = 0.04;

double duration_seconds(const ScoreNote& note) {
    return note.measured_offset_seconds - note.measured_onset_seconds;
}

const CanonicalNoteEvent* source_event_for(
    const ScoreDocument& score, const ScoreNote& note) {
    if (note.source_event_ids.empty()) return nullptr;
    const auto found = std::find_if(score.source_events.begin(), score.source_events.end(),
        [&](const auto& event) { return event.id == note.source_event_ids.front(); });
    return found == score.source_events.end() ? nullptr : &*found;
}

void add_assessment(NoteReview& review, ReviewDimension dimension,
                    ReviewUncertainty uncertainty, std::string explanation) {
    review.assessments.push_back(
        {dimension, uncertainty, std::move(explanation)});
}

void add_finding(NoteReview& review, std::string category,
                 std::string explanation, double ranking_score) {
    review.findings.push_back({
        review.note_id + ":" + category, review.note_id,
        std::move(category), std::move(explanation), ranking_score});
}

void add_suggestion(NoteReview& review, ReviewSuggestionKind kind,
                    std::optional<int> pitch, std::optional<double> split_time,
                    double ranking_score, std::string basis,
                    std::string explanation, std::string suffix) {
    review.suggestions.push_back({
        review.note_id + ":suggest:" + std::move(suffix), review.note_id,
        kind, pitch, split_time, std::clamp(ranking_score, 0.0, 1.0),
        std::move(basis), std::move(explanation)});
}

struct PitchEvidenceSummary {
    std::map<std::pair<EvidenceSemantics, int>, double> maxima;
};

PitchEvidenceSummary summarize_pitch_evidence(const ScoreNote& note) {
    PitchEvidenceSummary result;
    for (const auto& evidence : note.raw_evidence) {
        if (!evidence.raw_model_value || evidence.midi_pitch < 0 ||
            evidence.strength_semantics == EvidenceSemantics::CalibratedProbability ||
            evidence.raw_value_semantics == EvidenceSemantics::Unspecified ||
            *evidence.raw_model_value < 0.0) {
            continue;
        }
        const auto key = std::pair{evidence.raw_value_semantics,
                                   evidence.midi_pitch};
        const auto [it, inserted] = result.maxima.emplace(
            key, *evidence.raw_model_value);
        if (!inserted) it->second = std::max(it->second, *evidence.raw_model_value);
    }
    return result;
}

void review_pitch_evidence(const ScoreNote& note, NoteReview& review,
                           const CanonicalNoteEvent* source) {
    const int detected_pitch = source
        ? static_cast<int>(std::lround(source->midi_pitch))
        : note.sounding_midi_pitch;
    const auto summary = summarize_pitch_evidence(note);
    double strongest_alternative = -1.0;
    int strongest_pitch = -1;
    double selected_value = -1.0;
    for (const auto& [key, value] : summary.maxima) {
        if (key.second == detected_pitch) {
            selected_value = std::max(selected_value, value);
        } else if (value > strongest_alternative) {
            strongest_alternative = value;
            strongest_pitch = key.second;
        }
    }

    if (strongest_pitch >= 0 && selected_value >= 0.0 &&
        strongest_alternative >= selected_value * 0.8 &&
        !note.user_authored) {
        const double relative_rank = strongest_alternative /
            (strongest_alternative + selected_value + 1e-12);
        add_assessment(review, ReviewDimension::Pitch,
                       ReviewUncertainty::Ambiguous,
                       "A different pitch has comparable or stronger raw activation.");
        add_finding(review, "model-pitch-conflict",
                    "Alternative pitch evidence is close to or exceeds the detected pitch.",
                    relative_rank);
        add_suggestion(review, ReviewSuggestionKind::AlternativePitch,
                       strongest_pitch, std::nullopt, relative_rank,
                       "Within-event raw activation comparison; not a probability.",
                       "Review the audio before changing the detected pitch.",
                       "pitch-" + std::to_string(strongest_pitch));
    } else if (note.raw_evidence.empty()) {
        add_assessment(review, ReviewDimension::Pitch,
                       ReviewUncertainty::NotAssessed,
                       "The score has no retained detector evidence.");
    } else {
        add_assessment(review, ReviewDimension::Pitch,
                       ReviewUncertainty::NoConflictObserved,
                       "No competing pitch evidence was retained; this is not a correctness guarantee.");
    }
}

void review_octave_context(const ScoreNote& note, const ScoreDocument& score,
                           NoteReview& review, const ReviewOptions& options) {
    if (note.user_authored) return;
    const ScoreNote* previous = nullptr;
    const ScoreNote* next = nullptr;
    for (const auto& candidate : score.notes) {
        if (candidate.id == note.id || candidate.voice != note.voice ||
            candidate.user_authored) continue;
        if (candidate.measured_offset_seconds <= note.measured_onset_seconds &&
            note.measured_onset_seconds - candidate.measured_offset_seconds <=
                options.octave_neighbor_window_seconds &&
            (!previous || candidate.measured_onset_seconds >
                previous->measured_onset_seconds)) {
            previous = &candidate;
        }
        if (candidate.measured_onset_seconds >= note.measured_offset_seconds &&
            candidate.measured_onset_seconds - note.measured_offset_seconds <=
                options.octave_neighbor_window_seconds &&
            (!next || candidate.measured_onset_seconds <
                next->measured_onset_seconds)) {
            next = &candidate;
        }
    }
    if (!previous || !next ||
        std::abs(previous->sounding_midi_pitch - next->sounding_midi_pitch) > 2) {
        return;
    }
    const int neighbor_pitch = static_cast<int>(std::lround(
        (previous->sounding_midi_pitch + next->sounding_midi_pitch) / 2.0));
    const int delta = note.sounding_midi_pitch - neighbor_pitch;
    if (std::abs(std::abs(delta) - 12) > 1) return;
    const int suggested_pitch = note.sounding_midi_pitch - (delta > 0 ? 12 : -12);
    const double rank = std::clamp(
        1.0 - std::abs(previous->sounding_midi_pitch -
                       next->sounding_midi_pitch) / 3.0,
        0.0, 1.0);
    add_finding(review, "possible-isolated-octave",
                "This pitch is one octave from two nearby notes in the same voice; musical leaps remain possible.",
                rank);
    add_suggestion(review, ReviewSuggestionKind::IsolatedOctave,
                   suggested_pitch, std::nullopt, rank,
                   "Same-voice neighboring pitches; heuristic ranking, not a probability.",
                   "Compare against the surrounding notes and original audio.",
                   "octave-" + std::to_string(suggested_pitch));
}

void review_repeated_note_valley(const ScoreNote& note, NoteReview& review,
                                 const ReviewOptions& options) {
    if (note.user_authored ||
        note.raw_evidence.size() < options.minimum_valley_samples) return;
    std::vector<const EventEvidence*> samples;
    samples.reserve(note.raw_evidence.size());
    for (const auto& evidence : note.raw_evidence) {
        if (evidence.midi_pitch == note.sounding_midi_pitch) {
            samples.push_back(&evidence);
        }
    }
    if (samples.size() < options.minimum_valley_samples) return;
    std::stable_sort(samples.begin(), samples.end(), [](const auto* left, const auto* right) {
        return left->time_seconds < right->time_seconds;
    });
    constexpr std::size_t first = 2;
    const auto last = samples.size() > 2 ? samples.size() - 2 : 0;
    for (std::size_t index = first; index < last; ++index) {
        const double left = (samples[index - 2]->raw_strength +
                             samples[index - 1]->raw_strength) / 2.0;
        const double right = (samples[index + 1]->raw_strength +
                              samples[index + 2]->raw_strength) / 2.0;
        const double valley = samples[index]->raw_strength;
        const double flank = std::min(left, right);
        if (flank < 0.2 || valley > flank * options.repeated_note_valley_ratio) {
            continue;
        }
        const double split_time = samples[index]->time_seconds;
        if (split_time <= note.measured_onset_seconds + 0.06 ||
            split_time >= note.measured_offset_seconds - 0.06) continue;
        const double rank = std::clamp(1.0 - valley / flank, 0.0, 1.0);
        add_finding(review, "possible-merged-repeat",
                    "Raw frame strength contains a deep interior valley that may separate repeated notes.",
                    rank);
        add_suggestion(review, ReviewSuggestionKind::SplitRepeatedNote,
                       std::nullopt, split_time, rank,
                       "Interior raw-strength valley relative to adjacent frames; heuristic, not a probability.",
                       "A trough may be a repeated attack or an ordinary note fluctuation.",
                       "split-" + std::to_string(index));
        return;
    }
}

void review_duration_outliers(const ScoreDocument& score,
                              std::vector<NoteReview>& reviews) {
    std::map<int, std::vector<double>> durations_by_voice;
    for (const auto& note : score.notes) {
        const double duration = duration_seconds(note);
        if (!note.user_authored && duration > 0.0) {
            durations_by_voice[note.voice].push_back(duration);
        }
    }
    for (auto& [voice, durations] : durations_by_voice) {
        (void)voice;
        if (durations.size() < 3) continue;
        std::sort(durations.begin(), durations.end());
        const double median = durations[durations.size() / 2];
        if (median <= 0.0) continue;
        for (const auto& note : score.notes) {
            const double duration = duration_seconds(note);
            if (note.user_authored || note.voice != voice ||
                duration <= 0.0 || duration < median * 6.0) continue;
            auto found = std::find_if(reviews.begin(), reviews.end(),
                [&](const auto& item) { return item.note_id == note.id; });
            if (found != reviews.end()) {
                add_finding(*found, "duration-outlier",
                    "Measured duration is at least six times the median for this voice; sustained notes may be intentional.",
                    0.5);
            }
        }
    }
}

} // namespace

ScoreReview review_score(const ScoreDocument& score,
                         const ReviewOptions& options) {
    if (!std::isfinite(options.octave_neighbor_window_seconds) ||
        options.octave_neighbor_window_seconds <= 0.0 ||
        !std::isfinite(options.repeated_note_valley_ratio) ||
        options.repeated_note_valley_ratio <= 0.0 ||
        options.repeated_note_valley_ratio >= 1.0 ||
        options.minimum_valley_samples < 5) {
        throw std::invalid_argument("Review options contain an invalid threshold.");
    }

    ScoreReview result;
    result.out_of_distribution_assessed = false;
    result.calibration_limitation =
        "No representative held-out labelled calibration dataset is configured; "
        "out-of-distribution status is not assessed.";
    result.notes.reserve(score.notes.size());
    for (const auto& note : score.notes) {
        NoteReview review;
        review.note_id = note.id;
        review.raw_evidence_samples = note.raw_evidence.size();
        review.calibrated_confidence_available =
            note.calibrated_confidence.has_value();
        if (review.calibrated_confidence_available) ++result.calibrated_notes;
        if (note.raw_evidence.empty()) {
            ++result.notes_without_evidence;
            add_finding(review, "missing-evidence",
                        "No raw detector evidence was retained for this score note.", 1.0);
        }
        const auto* source = source_event_for(score, note);
        review_pitch_evidence(note, review, source);
        add_assessment(review, ReviewDimension::Onset,
                       ReviewUncertainty::NotAssessed,
                       "No validated onset-uncertainty estimator is available.");
        add_assessment(review, ReviewDimension::Offset,
                       ReviewUncertainty::NotAssessed,
                       "No validated offset-uncertainty estimator is available.");

        const bool guitar = score.instrument.name == "Guitar" ||
            !score.tab_notes.empty();
        if (guitar) {
            const auto tab = std::find_if(score.tab_notes.begin(), score.tab_notes.end(),
                [&](const auto& item) { return item.notation_note_id == note.id; });
            const bool assigned = tab != score.tab_notes.end() &&
                tab->string_number.has_value() && tab->fret.has_value();
            add_assessment(review, ReviewDimension::Fingering,
                assigned ? ReviewUncertainty::NoConflictObserved
                         : ReviewUncertainty::Ambiguous,
                assigned ? "A string/fret assignment is present; original fingering is not inferred."
                         : "No accepted string/fret assignment is available.");
            if (!assigned) {
                add_finding(review, "fingering-unassigned",
                            "The note has no accepted tab position.", 0.6);
            }
        } else {
            add_assessment(review, ReviewDimension::Fingering,
                           ReviewUncertainty::NotAssessed,
                           "The score is not configured for guitar tablature.");
        }

        const bool rhythm_ambiguous = !note.rhythm_alternatives.empty() ||
            score.tempo_ambiguous || score.meter_ambiguous;
        add_assessment(review, ReviewDimension::Rhythm,
            rhythm_ambiguous ? ReviewUncertainty::Ambiguous
                             : ReviewUncertainty::NoConflictObserved,
            rhythm_ambiguous
                ? "Tempo, meter, or rhythmic alternatives remain ambiguous."
                : "No retained rhythmic alternative is marked ambiguous.");
        add_assessment(review, ReviewDimension::Distribution,
                       ReviewUncertainty::NotAssessed,
                       "No validated distribution-shift detector is configured.");

        review_octave_context(note, score, review, options);
        review_repeated_note_valley(note, review, options);
        std::stable_sort(review.findings.begin(), review.findings.end(),
            [](const auto& left, const auto& right) {
                if (left.ranking_score != right.ranking_score) {
                    return left.ranking_score > right.ranking_score;
                }
                return left.id < right.id;
            });
        std::stable_sort(review.suggestions.begin(), review.suggestions.end(),
            [](const auto& left, const auto& right) {
                if (left.ranking_score != right.ranking_score) {
                    return left.ranking_score > right.ranking_score;
                }
                return left.id < right.id;
            });
        result.notes.push_back(std::move(review));
    }
    for (std::size_t left = 0; left < score.notes.size(); ++left) {
        for (std::size_t right = left + 1; right < score.notes.size(); ++right) {
            const auto& a = score.notes[left];
            const auto& b = score.notes[right];
            if (a.voice != b.voice ||
                std::abs(a.measured_onset_seconds - b.measured_onset_seconds) >
                    simultaneous_tolerance_seconds ||
                (a.chord_group_id && b.chord_group_id &&
                 a.chord_group_id == b.chord_group_id)) {
                continue;
            }
            for (const auto& id : {a.id, b.id}) {
                const auto found = std::find_if(result.notes.begin(), result.notes.end(),
                    [&](const auto& item) { return item.note_id == id; });
                if (found != result.notes.end()) {
                    add_finding(*found, "possible-chord-group",
                        "Notes share a voice and near-simultaneous measured onsets but lack a shared chord group.",
                        0.4);
                }
            }
        }
    }
    review_duration_outliers(score, result.notes);
    return result;
}

CalibrationReport evaluate_calibration(
    std::span<const CalibrationSample> samples,
    const CalibrationDatasetInfo& dataset,
    std::size_t reliability_bin_count,
    std::size_t minimum_sample_count) {
    if (reliability_bin_count == 0 || reliability_bin_count > 100 ||
        minimum_sample_count == 0) {
        throw std::invalid_argument("Calibration metric bounds are invalid.");
    }
    CalibrationReport report;
    report.dataset_identifier = dataset.identifier;
    report.sample_count = samples.size();
    if (dataset.identifier.empty()) {
        report.limitation = "Calibration dataset identifier is required.";
        return report;
    }
    if (!dataset.held_out_from_training || !dataset.representative_of_deployment ||
        !dataset.training_overlap_reviewed) {
        report.limitation =
            "Calibration data must be held out, representative, and have training overlap reviewed.";
        return report;
    }
    if (samples.size() < minimum_sample_count) {
        report.limitation = "Insufficient labelled validation examples for calibration diagnostics.";
        return report;
    }
    for (const auto& sample : samples) {
        if (!std::isfinite(sample.predicted_probability) ||
            sample.predicted_probability < 0.0 ||
            sample.predicted_probability > 1.0) {
            throw std::invalid_argument(
                "Calibration predictions must be finite probabilities in [0,1].");
        }
    }

    std::vector<double> probability_sum(reliability_bin_count);
    std::vector<std::size_t> correct_count(reliability_bin_count);
    std::vector<std::size_t> bin_count(reliability_bin_count);
    double brier_sum = 0.0;
    for (const auto& sample : samples) {
        const double label = sample.correct ? 1.0 : 0.0;
        const double difference = sample.predicted_probability - label;
        brier_sum += difference * difference;
        const auto bin = std::min(reliability_bin_count - 1,
            static_cast<std::size_t>(
                sample.predicted_probability * reliability_bin_count));
        probability_sum[bin] += sample.predicted_probability;
        correct_count[bin] += sample.correct ? 1U : 0U;
        ++bin_count[bin];
    }
    report.available = true;
    report.limitation.clear();
    report.brier_score = brier_sum / samples.size();
    report.reliability_bins.reserve(reliability_bin_count);
    for (std::size_t index = 0; index < reliability_bin_count; ++index) {
        const auto count = bin_count[index];
        report.reliability_bins.push_back({
            count,
            count ? probability_sum[index] / count : 0.0,
            count ? static_cast<double>(correct_count[index]) / count : 0.0});
    }
    return report;
}

} // namespace transcribe
