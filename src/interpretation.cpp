#include "transcribe/interpretation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace transcribe {
namespace {

struct SourceNote {
    std::size_t index{};
    double pitch{};
    double onset{};
    double offset{};
    double weight{1.0};
    std::optional<int> velocity;
    std::optional<std::string> articulation;
};

struct OnsetGroup {
    double seconds{};
    double weight{};
    std::vector<std::size_t> source_indices;
};

struct Candidate {
    double value{};
    double confidence{};
    double score{};
};

constexpr std::array<double, 12> major_profile{
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> minor_profile{
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr std::array<int, 12> major_fifths_by_tonic{
    0, 7, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5};

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2 != 0) return *middle;
    const double upper = *middle;
    const auto lower = std::max_element(values.begin(), middle);
    return (*lower + upper) * 0.5;
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

std::vector<int> conventional_grouping(const MeterConstraint& meter) {
    if (!meter.grouping.empty()) return meter.grouping;
    if (meter.denominator == 8 && meter.numerator % 3 == 0) {
        return std::vector<int>(static_cast<std::size_t>(meter.numerator / 3), 3);
    }
    if (meter.denominator == 8 && meter.numerator == 5) return {3, 2};
    if (meter.denominator == 8 && meter.numerator == 7) return {2, 2, 3};
    return std::vector<int>(static_cast<std::size_t>(meter.numerator), 1);
}

double quarter_measure_length(const MeterHypothesis& meter) {
    return static_cast<double>(meter.numerator) * 4.0 / meter.denominator;
}

std::vector<SourceNote> active_notes(
    std::span<const std::shared_ptr<const CanonicalNoteEvent>> events) {
    std::vector<SourceNote> result;
    result.reserve(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = *events[i];
        if (event.state == EventLifecycle::Rejected) continue;
        result.push_back({i, event.midi_pitch, event.onset_seconds, event.offset_seconds,
                          event.calibrated_confidence.value_or(0.5),
                          event.velocity, event.articulation});
    }
    std::stable_sort(result.begin(), result.end(), [](const SourceNote& a, const SourceNote& b) {
        if (a.onset != b.onset) return a.onset < b.onset;
        return a.pitch < b.pitch;
    });
    return result;
}

std::vector<OnsetGroup> make_onset_groups(const std::vector<SourceNote>& notes,
                                          double tolerance) {
    std::vector<OnsetGroup> groups;
    for (const auto& note : notes) {
        if (groups.empty() || note.onset - groups.back().seconds > tolerance) {
            groups.push_back({note.onset, 0.0, {}});
        }
        auto& group = groups.back();
        group.weight += std::max(0.1, note.weight);
        group.source_indices.push_back(note.index);
    }
    return groups;
}

std::vector<Candidate> estimate_tempo_candidates(const std::vector<OnsetGroup>& groups,
                                                  const InterpretationOptions& options,
                                                  double& swing_ratio,
                                                  double& swing_confidence) {
    swing_ratio = 1.0;
    swing_confidence = 0.0;
    if (options.tempo_bpm) return {{*options.tempo_bpm, 1.0, 0.0}};
    std::vector<double> intervals;
    for (std::size_t i = 1; i < groups.size(); ++i) {
        const double delta = groups[i].seconds - groups[i - 1].seconds;
        if (delta >= 0.08 && delta <= 2.0) intervals.push_back(delta);
    }
    if (intervals.empty()) return {{120.0, 0.0, 1.0}};

    std::vector<double> swing_beats;
    double best_swing = 0.0;
    for (std::size_t phase = 0; phase < 2; ++phase) {
        std::vector<double> short_intervals;
        std::vector<double> long_intervals;
        std::vector<double> pair_beats;
        for (std::size_t i = phase; i + 1 < intervals.size(); i += 2) {
            const double shorter = std::min(intervals[i], intervals[i + 1]);
            const double longer = std::max(intervals[i], intervals[i + 1]);
            const double ratio = longer / std::max(shorter, 1e-9);
            const double pair = intervals[i] + intervals[i + 1];
            if (ratio >= 1.35 && ratio <= 3.0 && pair >= 0.25 && pair <= 2.0) {
                short_intervals.push_back(shorter);
                long_intervals.push_back(longer);
                pair_beats.push_back(pair);
            }
        }
        if (pair_beats.size() < 3) continue;
        const double short_median = median(short_intervals);
        const double long_median = median(long_intervals);
        const double ratio = long_median / std::max(short_median, 1e-9);
        const double pair_median = median(pair_beats);
        std::vector<double> deviations;
        for (const auto pair : pair_beats) deviations.push_back(std::abs(pair - pair_median));
        const double regularity = 1.0 - std::min(1.0, median(deviations) / pair_median);
        const double confidence =
            clamp01((ratio - 1.25) / 1.25) *
            static_cast<double>(pair_beats.size()) / (pair_beats.size() + 1.0) *
            regularity;
        if (confidence > best_swing) {
            best_swing = confidence;
            swing_ratio = ratio;
            swing_beats = std::move(pair_beats);
        }
    }
    swing_confidence = best_swing;

    double beat_seconds = swing_beats.empty() ? median(intervals) : median(swing_beats);
    if (swing_beats.empty()) {
        while (beat_seconds < 1.0 / 3.0) beat_seconds *= 2.0;
        while (beat_seconds > 1.0) beat_seconds *= 0.5;
    }
    beat_seconds = std::clamp(beat_seconds, 60.0 / 240.0, 60.0 / 40.0);
    const double base_bpm = 60.0 / beat_seconds;

    std::vector<double> bpm_values{base_bpm, base_bpm * 0.5, base_bpm * 2.0};
    std::vector<Candidate> candidates;
    for (double bpm : bpm_values) {
        if (bpm < 40.0 || bpm > 240.0) continue;
        const auto duplicate = std::find_if(candidates.begin(), candidates.end(), [bpm](const Candidate& c) {
            return std::abs(c.value - bpm) < 0.1;
        });
        if (duplicate != candidates.end()) continue;
        const double beat = 60.0 / bpm;
        double error = 0.0;
        for (const auto interval : intervals) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const double fraction : {1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75,
                                          1.0, 1.5, 2.0, 3.0, 4.0}) {
                nearest = std::min(nearest, std::abs(interval / beat - fraction) /
                                             std::max(fraction, 0.2));
            }
            error += std::min(nearest, 1.0);
        }
        error /= static_cast<double>(intervals.size());
        const double confidence = clamp01(1.0 - error) *
            static_cast<double>(intervals.size()) / (intervals.size() + 4.0);
        candidates.push_back({bpm, confidence, error});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [base_bpm](const Candidate& a,
                                                                     const Candidate& b) {
        if (std::abs(a.score - b.score) > 1e-9) return a.score < b.score;
        return std::abs(a.value - base_bpm) < std::abs(b.value - base_bpm);
    });
    return candidates;
}

std::vector<MeterHypothesis> meter_candidates(const InterpretationOptions& options) {
    std::vector<MeterConstraint> candidates;
    if (options.meter) {
        candidates.push_back(*options.meter);
    } else {
        candidates = {{4, 4, {1, 1, 1, 1}}, {3, 4, {1, 1, 1}}, {6, 8, {3, 3}},
                      {2, 4, {1, 1}}, {5, 4, {1, 1, 1, 1, 1}}, {3, 8, {3}},
                      {5, 8, {3, 2}}, {7, 8, {2, 2, 3}}, {9, 8, {3, 3, 3}},
                      {12, 8, {3, 3, 3, 3}}};
    }
    std::vector<MeterHypothesis> result;
    for (auto candidate : candidates) {
        if (candidate.numerator < 1 || candidate.numerator > 16 ||
            candidate.denominator < 2 || candidate.denominator > 16 ||
            (candidate.denominator & (candidate.denominator - 1)) != 0) {
            throw std::invalid_argument("Invalid meter constraint or grouping.");
        }
        candidate.grouping = conventional_grouping(candidate);
        const int grouping_sum = std::accumulate(candidate.grouping.begin(),
                                                 candidate.grouping.end(), 0);
        if (candidate.grouping.empty() ||
            std::any_of(candidate.grouping.begin(), candidate.grouping.end(),
                        [](int size) { return size <= 0; }) ||
            grouping_sum != candidate.numerator) {
            throw std::invalid_argument("Invalid meter constraint or grouping.");
        }
        MeterHypothesis meter{candidate.numerator, candidate.denominator,
                              candidate.grouping, options.meter ? 1.0 : 0.0,
                              options.meter.has_value()};
        result.push_back(meter);
    }
    return result;
}

std::vector<KeyHypothesis> estimate_keys(const std::vector<SourceNote>& notes,
                                         const InterpretationOptions& options) {
    if (options.key_fifths || options.minor_key) {
        const auto key = infer_score_metadata([&notes] {
            std::vector<Note> converted;
            converted.reserve(notes.size());
            for (const auto& note : notes) {
                converted.push_back({static_cast<int>(std::lround(note.pitch)), note.onset,
                                     note.offset - note.onset, {}, 1.0, 0.0});
            }
            return converted;
        }());
        KeyHypothesis constrained{
            options.key_fifths.value_or(key.key_fifths),
            options.minor_key.value_or(key.minor_key), 1.0, true};
        return {constrained};
    }
    if (notes.empty()) return {{0, false, 0.0, false}};

    std::array<double, 12> histogram{};
    for (const auto& note : notes) {
        const int midi = static_cast<int>(std::lround(note.pitch));
        const auto pitch_class = static_cast<std::size_t>((midi % 12 + 12) % 12);
        histogram[pitch_class] += std::max(0.05, note.offset - note.onset) *
                                  std::max(0.1, note.weight);
    }
    const double mean = std::accumulate(histogram.begin(), histogram.end(), 0.0) / 12.0;
    double histogram_variance = 0.0;
    for (const auto value : histogram) histogram_variance += (value - mean) * (value - mean);
    std::vector<std::pair<KeyHypothesis, double>> ranked;
    for (int tonic = 0; tonic < 12; ++tonic) {
        for (int mode = 0; mode < 2; ++mode) {
            const auto& profile = mode == 0 ? major_profile : minor_profile;
            const double profile_mean =
                std::accumulate(profile.begin(), profile.end(), 0.0) / profile.size();
            double covariance = 0.0;
            double profile_variance = 0.0;
            for (int pitch = 0; pitch < 12; ++pitch) {
                const double centered_profile =
                    profile[static_cast<std::size_t>((pitch - tonic + 12) % 12)] - profile_mean;
                covariance += (histogram[static_cast<std::size_t>(pitch)] - mean) *
                              centered_profile;
                profile_variance += centered_profile * centered_profile;
            }
            const double correlation = covariance /
                std::sqrt(histogram_variance * profile_variance + 1e-12);
            const int major_tonic = mode == 0 ? tonic : (tonic + 3) % 12;
            ranked.push_back({{major_fifths_by_tonic[static_cast<std::size_t>(major_tonic)],
                               mode != 0, 0.0, false}, correlation});
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    const double best = ranked.front().second;
    const double second = ranked.size() > 1 ? ranked[1].second : best;
    const double evidence = static_cast<double>(notes.size()) / (notes.size() + 4.0);
    std::vector<KeyHypothesis> result;
    for (std::size_t i = 0; i < std::min<std::size_t>(ranked.size(), 3); ++i) {
        auto key = ranked[i].first;
        key.confidence = i == 0
            ? clamp01((best - second) * 3.0) * evidence
            : clamp01((ranked[i].second - ranked[std::min<std::size_t>(i + 1, ranked.size() - 1)].second) *
                      3.0) * evidence;
        result.push_back(key);
    }
    return result;
}

std::vector<MeterHypothesis> rank_meters(std::vector<MeterHypothesis> meters,
                                         const std::vector<SourceNote>& notes,
                                         const std::vector<OnsetGroup>& groups,
                                         const std::vector<TempoSegment>& tempo,
                                         const InterpretationOptions& options) {
    if (options.meter || groups.empty()) return meters;
    const auto quarter_at = [&tempo](double seconds) {
        double result = 0.0;
        for (const auto& segment : tempo) {
            if (seconds <= segment.start_seconds) break;
            const double end = std::min(seconds, segment.end_seconds);
            result += std::max(0.0, end - segment.start_seconds) *
                      segment.beats_per_minute / 60.0;
            if (seconds <= segment.end_seconds) break;
        }
        return result;
    };
    std::vector<double> raw_scores;
    raw_scores.reserve(meters.size());
    for (auto& meter : meters) {
        const double bar = quarter_measure_length(meter);
        double score = 0.0;
        double weight_sum = 0.0;
        for (const auto& group : groups) {
            const double q = quarter_at(group.seconds);
            const double phase = std::fmod(q, bar);
            double boundary = 0.0;
            double best_distance = phase;
            double nearest_strength = 1.0;
            for (std::size_t i = 0; i < meter.grouping.size(); ++i) {
                const double distance = std::abs(phase - boundary);
                if (distance < best_distance) {
                    best_distance = distance;
                    nearest_strength = i == 0 ? 1.0 : 0.65;
                }
                boundary += static_cast<double>(meter.grouping[i]) * 4.0 / meter.denominator;
            }
            best_distance = std::min(best_distance, std::abs(bar - phase));
            const double beat_alignment = std::exp(-best_distance / 0.15);
            const double weight = std::max(0.1, group.weight);
            score += weight * (0.5 * beat_alignment + 0.5 * nearest_strength);
            weight_sum += weight;
        }
        raw_scores.push_back(weight_sum > 0.0 ? score / weight_sum : 0.0);
    }
    std::vector<std::size_t> order(meters.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&raw_scores](std::size_t a, std::size_t b) {
        return raw_scores[a] > raw_scores[b];
    });
    const double best_score = raw_scores[order.front()];
    const double second_score = order.size() > 1 ? raw_scores[order[1]] : best_score;
    const double evidence = std::min<double>(notes.size(), 24.0) /
                            (std::min<double>(notes.size(), 24.0) + 8.0);
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto index = order[rank];
        const double gap = rank == 0 ? best_score - second_score :
            best_score - raw_scores[index];
        meters[index].confidence = clamp01(gap * 4.0) * evidence;
    }
    std::vector<MeterHypothesis> ranked;
    ranked.reserve(meters.size());
    for (const auto index : order) ranked.push_back(std::move(meters[index]));
    return ranked;
}

std::vector<TempoSegment> make_tempo_map(const std::vector<OnsetGroup>& groups,
                                         double end_seconds, double base_bpm,
                                         double confidence, const InterpretationOptions& options,
                                         double swing_confidence) {
    std::vector<TempoSegment> result{{0.0, end_seconds, 0.0, base_bpm,
                                      confidence, options.tempo_bpm.has_value()}};
    if (!options.detect_tempo_changes || options.tempo_bpm || groups.size() < 9 ||
        swing_confidence >= 0.5) {
        return result;
    }
    struct Block { std::size_t first_group; double bpm; double confidence; };
    std::vector<Block> blocks;
    constexpr std::size_t block_intervals = 4;
    for (std::size_t first = 0; first + 3 < groups.size(); first += block_intervals) {
        std::vector<double> intervals;
        for (std::size_t i = first + 1; i < std::min(groups.size(), first + block_intervals + 1); ++i) {
            const double interval = groups[i].seconds - groups[i - 1].seconds;
            if (interval >= 0.08 && interval <= 2.0) intervals.push_back(interval);
        }
        if (intervals.size() < 3) continue;
        const double beat = median(intervals);
        const double bpm = std::clamp(60.0 / beat, 40.0, 240.0);
        std::vector<double> deviations;
        for (const auto interval : intervals) deviations.push_back(std::abs(interval - beat));
        const double local_confidence = clamp01(1.0 - median(deviations) / beat) *
            static_cast<double>(intervals.size()) / (intervals.size() + 2.0);
        blocks.push_back({first, bpm, local_confidence});
    }
    if (!blocks.empty()) {
        result.front().beats_per_minute = blocks.front().bpm;
        result.front().confidence = blocks.front().confidence;
    }
    for (std::size_t i = 1; i < blocks.size(); ++i) {
        const double difference = std::abs(blocks[i].bpm - blocks[i - 1].bpm) /
                                  blocks[i - 1].bpm;
        if (difference >= options.tempo_change_threshold &&
            blocks[i].confidence >= 0.55 && blocks[i - 1].confidence >= 0.55) {
            const double start = groups[blocks[i].first_group].seconds;
            result.back().end_seconds = start;
            result.push_back({start, end_seconds, 0.0, blocks[i].bpm,
                              blocks[i].confidence, false});
        }
    }
    return result;
}

double quarter_at(const std::vector<TempoSegment>& tempo, double seconds) {
    double quarters = 0.0;
    for (const auto& segment : tempo) {
        if (seconds <= segment.start_seconds) break;
        const double end = std::min(seconds, segment.end_seconds);
        quarters += std::max(0.0, end - segment.start_seconds) *
                    segment.beats_per_minute / 60.0;
        if (seconds <= segment.end_seconds) break;
    }
    return quarters;
}

double seconds_at(const std::vector<TempoSegment>& tempo, double quarters) {
    for (const auto& segment : tempo) {
        const double end_quarters = segment.start_quarters +
            std::max(0.0, segment.end_seconds - segment.start_seconds) *
            segment.beats_per_minute / 60.0;
        if (quarters <= end_quarters + 1e-9) {
            return segment.start_seconds +
                   (quarters - segment.start_quarters) * 60.0 / segment.beats_per_minute;
        }
    }
    if (tempo.empty()) return quarters * 0.5;
    const auto& last = tempo.back();
    return last.end_seconds +
           (quarters - last.start_quarters) * 60.0 / last.beats_per_minute;
}

void set_tempo_quarter_offsets(std::vector<TempoSegment>& tempo) {
    double quarters = 0.0;
    for (auto& segment : tempo) {
        segment.start_quarters = quarters;
        quarters += std::max(0.0, segment.end_seconds - segment.start_seconds) *
                    segment.beats_per_minute / 60.0;
    }
}

double straighten_swing(double quarters, double ratio, bool enabled) {
    if (!enabled) return quarters;
    const double beat = std::floor(quarters);
    const double phase = quarters - beat;
    const double long_phase = ratio / (ratio + 1.0);
    const double short_phase = 1.0 / (ratio + 1.0);
    if (std::abs(phase - long_phase) <= 0.08 ||
        std::abs(phase - short_phase) <= 0.08) {
        return beat + 0.5;
    }
    return quarters;
}

double snap_onset(double quarters, double tolerance_quarters, bool& snapped) {
    static constexpr std::array<double, 14> grids{
        2.0, 1.5, 1.0, 0.75, 2.0 / 3.0, 0.5, 0.375, 1.0 / 3.0,
        0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 1.0 / 16.0, 1.0 / 24.0};
    for (const double grid : grids) {
        const double candidate = std::round(quarters / grid) * grid;
        if (std::abs(candidate - quarters) <= tolerance_quarters) {
            snapped = true;
            return std::max(0.0, candidate);
        }
    }
    snapped = false;
    return quarters;
}

std::vector<RhythmValue> rhythm_candidates() {
    std::vector<RhythmValue> values;
    for (const int denominator : {1, 2, 4, 8, 16, 32, 64, 128}) {
        const double base = 4.0 / denominator;
        for (int dots = 0; dots <= 2; ++dots) {
            const double dotted = dots == 0 ? 1.0 : dots == 1 ? 1.5 : 1.75;
            values.push_back({denominator, dots, 0, 0, base * dotted, 0.0, true});
            for (const auto [actual, normal] :
                 {std::pair{3, 2}, std::pair{5, 4}, std::pair{7, 4}}) {
                values.push_back({denominator, dots, actual, normal,
                                  base * dotted * normal / actual, 0.0, true});
            }
        }
    }
    std::stable_sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
        if (std::abs(a.duration_quarters - b.duration_quarters) > 1e-9) {
            return a.duration_quarters > b.duration_quarters;
        }
        if ((a.tuplet_actual == 0) != (b.tuplet_actual == 0)) return a.tuplet_actual == 0;
        return a.denominator < b.denominator;
    });
    return values;
}

std::pair<RhythmValue, std::vector<RhythmValue>> match_rhythm(
    double duration, const std::vector<RhythmValue>& candidates, double tolerance) {
    std::vector<RhythmValue> ranked = candidates;
    for (auto& value : ranked) {
        value.fit_error_quarters = std::abs(duration - value.duration_quarters);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (std::abs(a.fit_error_quarters - b.fit_error_quarters) > 1e-9) {
            return a.fit_error_quarters < b.fit_error_quarters;
        }
        if ((a.tuplet_actual == 0) != (b.tuplet_actual == 0)) return a.tuplet_actual == 0;
        if (a.dots != b.dots) return a.dots < b.dots;
        return a.denominator < b.denominator;
    });
    auto selected = ranked.front();
    if (selected.fit_error_quarters > tolerance) {
        selected = {0, 0, 0, 0, duration, selected.fit_error_quarters, false};
    }
    std::vector<RhythmValue> alternatives;
    for (const auto& value : ranked) {
        if (value.fit_error_quarters > tolerance) break;
        if (value.denominator == selected.denominator && value.dots == selected.dots &&
            value.tuplet_actual == selected.tuplet_actual &&
            value.tuplet_normal == selected.tuplet_normal) continue;
        alternatives.push_back(value);
        if (alternatives.size() == 2) break;
    }
    return {selected, alternatives};
}

int key_alteration_for_step(int fifths, int step) {
    static constexpr std::array<int, 7> sharp_order{3, 0, 4, 1, 5, 2, 6};
    static constexpr std::array<int, 7> flat_order{6, 2, 5, 1, 4, 0, 3};
    const auto& order = fifths >= 0 ? sharp_order : flat_order;
    const int count = std::abs(fifths);
    for (int i = 0; i < count; ++i) {
        if (order[static_cast<std::size_t>(i)] == step) return fifths >= 0 ? 1 : -1;
    }
    return 0;
}

struct Spelling {
    std::string step;
    int alter{};
    int octave{};
    bool accidental{};
};

Spelling spell_pitch(int midi, int fifths) {
    static constexpr std::array<const char*, 7> steps{"C", "D", "E", "F", "G", "A", "B"};
    const bool flats = fifths < 0;
    static constexpr std::array<int, 12> sharp_letter{
        0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static constexpr std::array<int, 12> flat_letter{
        0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6};
    static constexpr std::array<int, 12> sharp_alter{
        0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    static constexpr std::array<int, 12> flat_alter{
        0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0};
    const auto pc = static_cast<std::size_t>((midi % 12 + 12) % 12);
    const int letter = flats ? flat_letter[pc] : sharp_letter[pc];
    const int alter = flats ? flat_alter[pc] : sharp_alter[pc];
    const int octave = midi / 12 - 1;
    return {steps[static_cast<std::size_t>(letter)], alter, octave,
            alter != key_alteration_for_step(fifths, letter)};
}

std::vector<TonalRegion> local_keys_for(
    const std::vector<SourceNote>& notes, const std::vector<InterpretedNote>& interpreted,
    const MeterHypothesis& meter, double pickup) {
    std::vector<TonalRegion> result;
    if (notes.size() < 10 || interpreted.empty()) return result;
    const double measure = quarter_measure_length(meter);
    const auto measure_of = [measure, pickup](double q) {
        if (pickup > 0.0 && q < pickup) return std::size_t{0};
        const double shifted = std::max(0.0, q - pickup);
        return static_cast<std::size_t>(std::floor(shifted / measure)) + (pickup > 0.0 ? 1 : 0);
    };
    const std::size_t last_measure = measure_of(interpreted.back().onset_quarters);
    for (std::size_t first = 0; first <= last_measure; first += 4) {
        std::vector<SourceNote> group;
        for (const auto& note : notes) {
            const auto found = std::find_if(interpreted.begin(), interpreted.end(),
                [&note](const InterpretedNote& candidate) {
                    return candidate.source_event_index == note.index;
                });
            if (found == interpreted.end()) continue;
            const auto at = measure_of(found->onset_quarters);
            if (at >= first && at < first + 4) group.push_back(note);
        }
        if (group.size() < 5) continue;
        auto keys = estimate_keys(group, {});
        if (!keys.empty()) {
            result.push_back({first, std::min(first + 4, last_measure + 1), keys.front()});
        }
    }
    return result;
}

std::vector<BeatPosition> make_beats(const std::vector<TempoSegment>& tempo,
                                     const MeterHypothesis& meter, double total_quarters,
                                     double pickup, const std::vector<OnsetGroup>& groups) {
    double beat_step = 1.0;
    if (meter.denominator == 8 && !meter.grouping.empty()) {
        beat_step = static_cast<double>(meter.grouping.front()) * 0.5;
    } else if (meter.denominator != 4) {
        beat_step = 4.0 / meter.denominator;
    }
    std::vector<BeatPosition> beats;
    const double bar = quarter_measure_length(meter);
    for (double q = 0.0; q <= total_quarters + 1e-8; q += beat_step) {
        const double time = seconds_at(tempo, q);
        double confidence = 0.0;
        for (const auto& group : groups) {
            if (std::abs(group.seconds - time) < 0.12) {
                confidence = std::max(confidence, clamp01(group.weight / 3.0));
            }
        }
        const double phase = pickup > 0.0 && q >= pickup
            ? std::fmod(q - pickup, bar)
            : std::fmod(q, bar);
        beats.push_back({time, q, confidence, q == 0.0 ||
                           (pickup > 0.0 && std::abs(q - pickup) < 1e-8) ||
                           std::abs(phase) < 1e-8});
    }
    return beats;
}

void make_rests(ScoreHypothesis& score, const std::vector<RhythmValue>& rhythms) {
    std::vector<int> voices;
    for (const auto& note : score.notes) {
        if (std::find(voices.begin(), voices.end(), note.voice) == voices.end()) {
            voices.push_back(note.voice);
        }
    }
    std::sort(voices.begin(), voices.end());
    for (const auto voice : voices) {
        std::vector<const InterpretedNote*> notes;
        for (const auto& note : score.notes) if (note.voice == voice) notes.push_back(&note);
        std::stable_sort(notes.begin(), notes.end(), [](const auto* a, const auto* b) {
            if (a->onset_quarters != b->onset_quarters) return a->onset_quarters < b->onset_quarters;
            return a->duration_quarters > b->duration_quarters;
        });
        double cursor = 0.0;
        for (std::size_t i = 0; i < notes.size();) {
            const double onset = notes[i]->onset_quarters;
            double end = onset;
            std::size_t next = i;
            while (next < notes.size() && std::abs(notes[next]->onset_quarters - onset) < 1e-8) {
                end = std::max(end, onset + notes[next]->duration_quarters);
                ++next;
            }
            if (onset > cursor + 1e-6) {
                const double duration = onset - cursor;
                const auto [rhythm, unused] = match_rhythm(duration, rhythms, 0.06);
                (void)unused;
                score.rests.push_back({voice, cursor,
                                       rhythm.symbolic ? rhythm.duration_quarters : duration,
                                       rhythm});
            }
            cursor = std::max(cursor, end);
            i = next;
        }
        if (score.total_quarters > cursor + 1e-6) {
            const double duration = score.total_quarters - cursor;
            const auto [rhythm, unused] = match_rhythm(duration, rhythms, 0.06);
            (void)unused;
            score.rests.push_back({voice, cursor,
                                   rhythm.symbolic ? rhythm.duration_quarters : duration,
                                   rhythm});
        }
    }
}

ScoreHypothesis build_hypothesis(
    const std::vector<SourceNote>& notes, const std::vector<OnsetGroup>& groups,
    const Candidate& tempo_candidate, const MeterHypothesis& meter,
    const KeyHypothesis& key, double swing_ratio, double swing_confidence,
    const InterpretationOptions& options, std::size_t serial) {
    ScoreHypothesis score;
    score.meter = meter;
    score.key = key;
    score.pickup_quarters = options.pickup_quarters.value_or(0.0);
    score.swing_ratio = swing_ratio;
    score.swing_confidence = swing_confidence;
    score.swing_inferred = swing_confidence >= 0.5;
    score.instrument = options.instrument;
    score.written_transposition_semitones = options.written_transposition_semitones;

    double end_seconds = 0.0;
    for (const auto& note : notes) end_seconds = std::max(end_seconds, note.offset);
    score.tempo_map = make_tempo_map(groups, end_seconds, tempo_candidate.value,
                                     tempo_candidate.confidence, options, swing_confidence);
    if (!options.tempo_bpm && score.tempo_map.size() == 1) {
        score.tempo_map.front().beats_per_minute = tempo_candidate.value;
    }
    set_tempo_quarter_offsets(score.tempo_map);

    struct Positioned {
        SourceNote source;
        double raw_onset_quarters{};
        double raw_offset_quarters{};
        double onset_quarters{};
        double duration_quarters{};
        double onset_residual_seconds{};
        double offset_residual_seconds{};
        bool onset_snapped{};
        RhythmValue rhythm;
        std::vector<RhythmValue> rhythm_alternatives;
        int voice{};
        std::optional<std::size_t> chord_group;
    };
    std::vector<Positioned> positioned;
    positioned.reserve(notes.size());
    const auto rhythms = rhythm_candidates();
    for (const auto& note : notes) {
        const double raw_onset = quarter_at(score.tempo_map, note.onset);
        const double raw_offset = quarter_at(score.tempo_map, note.offset);
        const double onset_unwarped = straighten_swing(raw_onset, swing_ratio, score.swing_inferred);
        const double offset_unwarped = straighten_swing(raw_offset, swing_ratio, score.swing_inferred);
        bool snapped = false;
        const double tolerance_q = std::max(0.01,
            options.onset_quantization_tolerance_seconds * tempo_candidate.value / 60.0);
        double onset = snap_onset(onset_unwarped, tolerance_q, snapped);
        const double raw_duration = std::max(1.0 / 960.0, offset_unwarped - onset_unwarped);
        auto [rhythm, alternatives] = match_rhythm(
            raw_duration, rhythms,
            std::max(0.03, raw_duration * options.rhythm_quantization_tolerance));
        const double duration = rhythm.symbolic ? rhythm.duration_quarters : raw_duration;
        if (!snapped) onset = onset_unwarped;
        const double offset = onset + duration;
        positioned.push_back({note, raw_onset, raw_offset, onset, duration,
                              seconds_at(score.tempo_map, onset) - note.onset,
                              seconds_at(score.tempo_map, offset) - note.offset,
                              snapped, rhythm, std::move(alternatives), 0, std::nullopt});
    }

    std::stable_sort(positioned.begin(), positioned.end(), [](const auto& a, const auto& b) {
        if (a.onset_quarters != b.onset_quarters) return a.onset_quarters < b.onset_quarters;
        return a.source.pitch < b.source.pitch;
    });
    std::vector<double> voice_ends;
    std::size_t next_chord = 1;
    for (std::size_t first = 0; first < positioned.size();) {
        std::size_t end = first + 1;
        const double anchor = positioned[first].source.onset;
        while (end < positioned.size() &&
               positioned[end].source.onset - anchor <= options.chord_onset_tolerance_seconds) {
            ++end;
        }
        const double onset = positioned[first].onset_quarters;
        std::size_t voice = 0;
        while (voice < voice_ends.size() && voice_ends[voice] > onset + 1e-5) ++voice;
        if (voice == voice_ends.size()) voice_ends.push_back(0.0);
        const bool chord = end - first > 1;
        const auto chord_id = chord ? std::optional<std::size_t>(next_chord++) : std::nullopt;
        for (std::size_t i = first; i < end; ++i) {
            positioned[i].voice = static_cast<int>(voice + 1);
            positioned[i].chord_group = chord_id;
            voice_ends[voice] = std::max(voice_ends[voice],
                positioned[i].onset_quarters + positioned[i].duration_quarters);
        }
        first = end;
    }

    score.total_quarters = 0.0;
    for (const auto& note : positioned) {
        score.total_quarters = std::max(score.total_quarters,
            note.onset_quarters + note.duration_quarters);
    }
    const double bar = quarter_measure_length(meter);
    const std::size_t measure_count = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(std::max(0.0, score.total_quarters -
                score.pickup_quarters) / bar)) + (score.pickup_quarters > 0.0 ? 1 : 0));
    score.total_quarters = std::max(score.total_quarters,
        score.pickup_quarters + static_cast<double>(measure_count -
            (score.pickup_quarters > 0.0 ? 1 : 0)) * bar);

    score.notes.reserve(positioned.size());
    for (const auto& note : positioned) {
        const double measure_position = note.onset_quarters >= score.pickup_quarters
            ? std::floor((note.onset_quarters - score.pickup_quarters) / bar)
            : 0.0;
        const auto measure_index = static_cast<std::size_t>(std::max(0.0, measure_position));
        const KeyHypothesis* spelling_key = &key;
        for (const auto& region : score.local_tonal_context) {
            if (measure_index >= region.first_measure && measure_index < region.end_measure) {
                spelling_key = &region.key;
                break;
            }
        }
        const int sounding_pitch = static_cast<int>(std::lround(note.source.pitch));
        const int written_pitch = sounding_pitch + options.written_transposition_semitones;
        if (written_pitch < 0 || written_pitch > 127) {
            throw std::invalid_argument("Instrument transposition moves a note outside MIDI range.");
        }
        const auto spelling = spell_pitch(written_pitch, spelling_key->fifths);
        score.notes.push_back({
            note.source.index, note.onset_quarters, note.duration_quarters,
            note.onset_residual_seconds, note.offset_residual_seconds,
            note.voice, note.chord_group, written_pitch,
            spelling.step, spelling.alter, spelling.octave, spelling.accidental,
            note.rhythm, note.rhythm_alternatives,
            note.rhythm.symbolic ? clamp01(1.0 - note.rhythm.fit_error_quarters /
                std::max(note.rhythm.duration_quarters, 0.1)) : 0.0,
            note.source.velocity, note.source.articulation, false, false,
            sounding_pitch});
    }

    score.local_tonal_context = local_keys_for(notes, score.notes, meter, score.pickup_quarters);
    if (options.infer_local_keys) {
        for (auto& note : score.notes) {
            const double measure_position = note.onset_quarters >= score.pickup_quarters
                ? std::floor((note.onset_quarters - score.pickup_quarters) / bar)
                : 0.0;
            const auto measure_index = static_cast<std::size_t>(std::max(0.0, measure_position));
            for (const auto& region : score.local_tonal_context) {
                if (measure_index >= region.first_measure && measure_index < region.end_measure) {
                    const auto spelling = spell_pitch(note.written_midi_pitch, region.key.fifths);
                    note.spelling_step = spelling.step;
                    note.spelling_alter = spelling.alter;
                    note.spelling_octave = spelling.octave;
                    note.accidental_required = spelling.accidental;
                    break;
                }
            }
        }
    }

    make_rests(score, rhythms);
    score.beats = make_beats(score.tempo_map, meter, score.total_quarters,
                             score.pickup_quarters, groups);
    const double tempo_confidence = score.tempo_map.empty() ? 0.0 : score.tempo_map.front().confidence;
    score.tempo_ambiguous = !options.tempo_bpm && tempo_confidence < 0.55;
    score.meter_ambiguous = !meter.user_constrained && meter.confidence < 0.55;
    score.key_ambiguous = !key.user_constrained && key.confidence < 0.45;
    if (score.tempo_ambiguous) score.uncertainties.push_back("Tempo is ambiguous.");
    if (score.meter_ambiguous) score.uncertainties.push_back("Meter is ambiguous.");
    if (score.key_ambiguous) score.uncertainties.push_back("Key is ambiguous.");
    if (score.swing_inferred) {
        score.uncertainties.push_back("Swing interpretation is a hypothesis, not a tupleting rule.");
    }
    score.confidence = 0.4 * tempo_confidence + 0.3 * meter.confidence + 0.3 * key.confidence;
    score.id = "score-" + std::to_string(serial) + "-" +
               std::to_string(meter.numerator) + "-" + std::to_string(meter.denominator);
    return score;
}

} // namespace

ScoreInterpretation interpret_note_events(std::span<const CanonicalNoteEvent> events,
                                           const InterpretationOptions& options) {
    if (options.tempo_bpm &&
        (!std::isfinite(*options.tempo_bpm) || *options.tempo_bpm < 40.0 ||
         *options.tempo_bpm > 240.0)) {
        throw std::invalid_argument("Interpretation tempo must be between 40 and 240 BPM.");
    }
    if (options.key_fifths && (*options.key_fifths < -7 || *options.key_fifths > 7)) {
        throw std::invalid_argument("Key signature must be between -7 and 7 fifths.");
    }
    if (options.pickup_quarters &&
        (!std::isfinite(*options.pickup_quarters) || *options.pickup_quarters <= 0.0 ||
         *options.pickup_quarters > 16.0)) {
        throw std::invalid_argument("Pickup length must be greater than 0 and at most 16 quarters.");
    }
    if (!std::isfinite(options.chord_onset_tolerance_seconds) ||
        options.chord_onset_tolerance_seconds < 0.0 ||
        !std::isfinite(options.onset_quantization_tolerance_seconds) ||
        options.onset_quantization_tolerance_seconds < 0.0 ||
        !std::isfinite(options.rhythm_quantization_tolerance) ||
        options.rhythm_quantization_tolerance < 0.0 ||
        !std::isfinite(options.tempo_change_threshold) ||
        options.tempo_change_threshold <= 0.0 || options.maximum_hypotheses == 0) {
        throw std::invalid_argument("Interpretation tolerances and hypothesis limit are invalid.");
    }
    if (options.written_transposition_semitones < -48 ||
        options.written_transposition_semitones > 48) {
        throw std::invalid_argument("Instrument transposition must be between -48 and 48 semitones.");
    }

    ScoreInterpretation result;
    result.source_events.reserve(events.size());
    for (const auto& event : events) {
        if (!std::isfinite(event.midi_pitch) || event.midi_pitch < 0.0 ||
            event.midi_pitch > 127.0 || !std::isfinite(event.onset_seconds) ||
            !std::isfinite(event.offset_seconds) || event.onset_seconds < 0.0 ||
            event.offset_seconds < event.onset_seconds) {
            throw std::invalid_argument("Raw note event has invalid pitch or measured timing.");
        }
        result.source_events.push_back(std::make_shared<const CanonicalNoteEvent>(event));
    }
    const auto notes = active_notes(result.source_events);
    const auto groups = make_onset_groups(notes, options.chord_onset_tolerance_seconds);
    double swing_ratio = 1.0;
    double swing_confidence = 0.0;
    const auto tempos = estimate_tempo_candidates(groups, options, swing_ratio, swing_confidence);
    const double end_seconds = notes.empty() ? 0.0 :
        std::max_element(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
            return a.offset < b.offset;
        })->offset;
    std::vector<TempoSegment> initial_tempo =
        make_tempo_map(groups, end_seconds, tempos.front().value, tempos.front().confidence,
                       options, swing_confidence);
    if (!options.tempo_bpm && initial_tempo.size() == 1) {
        initial_tempo.front().beats_per_minute = tempos.front().value;
    }
    set_tempo_quarter_offsets(initial_tempo);
    const auto candidates = meter_candidates(options);
    auto ranked_meters = rank_meters(candidates, notes, groups, initial_tempo, options);
    const auto keys = estimate_keys(notes, options);
    std::vector<Candidate> selected_tempos = tempos;
    if (selected_tempos.size() > 2) selected_tempos.resize(2);
    if (ranked_meters.size() > (options.meter ? 1U : 3U)) {
        ranked_meters.resize(options.meter ? 1U : 3U);
    }
    const std::size_t key_count = options.key_fifths || options.minor_key
        ? 1 : std::min<std::size_t>(2, keys.size());

    struct Combination {
        Candidate tempo;
        MeterHypothesis meter;
        KeyHypothesis key;
        double score{};
    };
    std::vector<Combination> combinations;
    for (const auto& tempo : selected_tempos) {
        for (const auto& meter : ranked_meters) {
            for (std::size_t key_index = 0; key_index < key_count; ++key_index) {
                const auto& key = keys[key_index];
                const double confidence =
                    0.4 * tempo.confidence + 0.3 * meter.confidence + 0.3 * key.confidence;
                combinations.push_back({tempo, meter, key, confidence});
            }
        }
    }
    std::stable_sort(combinations.begin(), combinations.end(), [](const auto& a, const auto& b) {
        return a.score > b.score;
    });
    if (combinations.size() > options.maximum_hypotheses) {
        combinations.resize(options.maximum_hypotheses);
    }
    for (std::size_t i = 0; i < combinations.size(); ++i) {
        const auto& combination = combinations[i];
        auto hypothesis = build_hypothesis(notes, groups, combination.tempo,
            combination.meter, combination.key, swing_ratio, swing_confidence,
            options, i + 1);
        for (std::size_t key_index = 1; key_index < keys.size(); ++key_index) {
            hypothesis.alternative_keys.push_back(keys[key_index]);
        }
        result.hypotheses.push_back(std::move(hypothesis));
    }
    return result;
}

ScoreInterpretation interpret_notes(std::span<const Note> notes,
                                    const InterpretationOptions& options) {
    std::vector<CanonicalNoteEvent> events;
    events.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& note = notes[i];
        events.push_back({"note-" + std::to_string(i + 1),
                          static_cast<double>(note.midi_pitch), note.start_seconds,
                          note.start_seconds + note.duration_seconds, 0, 0, 0,
                          "notes-adapter", {}, "note-vector", {}, EventLifecycle::Stabilized,
                          note.confidence, note.amplitude, std::nullopt, std::nullopt,
                          std::nullopt, std::nullopt, {}});
    }
    return interpret_note_events(events, options);
}

ScoreMetadata metadata_for_hypothesis(const ScoreHypothesis& hypothesis) {
    ScoreMetadata metadata;
    metadata.tempo_bpm = hypothesis.tempo_map.empty()
        ? 120.0 : hypothesis.tempo_map.front().beats_per_minute;
    metadata.time_numerator = hypothesis.meter.numerator;
    metadata.time_denominator = hypothesis.meter.denominator;
    metadata.key_fifths = hypothesis.key.fifths;
    metadata.minor_key = hypothesis.key.minor;
    metadata.tempo_inferred = !hypothesis.tempo_ambiguous &&
        (hypothesis.tempo_map.empty() || !hypothesis.tempo_map.front().user_constrained);
    metadata.meter_inferred = !hypothesis.meter_ambiguous && !hypothesis.meter.user_constrained;
    metadata.key_inferred = !hypothesis.key_ambiguous && !hypothesis.key.user_constrained;
    metadata.tempo_confidence = hypothesis.tempo_map.empty()
        ? 0.0 : hypothesis.tempo_map.front().confidence;
    metadata.meter_confidence = hypothesis.meter.confidence;
    metadata.key_confidence = hypothesis.key.confidence;
    metadata.swing_ratio = hypothesis.swing_ratio;
    metadata.swing_inferred = hypothesis.swing_inferred;
    metadata.swing_confidence = hypothesis.swing_confidence;
    return metadata;
}

} // namespace transcribe
