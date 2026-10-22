#include "transcribe/guitar_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace transcribe {

std::string_view guitar_pitch_gesture_name(GuitarPitchGesture gesture) noexcept {
    switch (gesture) {
    case GuitarPitchGesture::None: return "none";
    case GuitarPitchGesture::Vibrato: return "vibrato-like modulation";
    case GuitarPitchGesture::SustainedRise: return "sustained pitch rise";
    case GuitarPitchGesture::SustainedFall: return "sustained pitch fall";
    }
    return "none";
}

GuitarTechniqueResult analyze_guitar_technique(
    const CanonicalNoteEvent& event, InstrumentPrior instrument,
    const GuitarTechniqueOptions& options) {
    if (!std::isfinite(options.minimum_voicing) || options.minimum_voicing < 0 ||
        options.minimum_voicing > 1 ||
        !std::isfinite(options.minimum_duration_seconds) || options.minimum_duration_seconds <= 0 ||
        !std::isfinite(options.maximum_frame_gap_seconds) || options.maximum_frame_gap_seconds <= 0 ||
        !std::isfinite(options.minimum_coverage) || options.minimum_coverage <= 0 || options.minimum_coverage > 1 ||
        !std::isfinite(options.minimum_excursion_semitones) || options.minimum_excursion_semitones <= 0 ||
        !std::isfinite(options.maximum_excursion_semitones) ||
        options.maximum_excursion_semitones <= options.minimum_excursion_semitones ||
        !std::isfinite(options.minimum_vibrato_hz) || options.minimum_vibrato_hz <= 0 ||
        !std::isfinite(options.maximum_vibrato_hz) || options.maximum_vibrato_hz <= options.minimum_vibrato_hz ||
        options.maximum_evidence_frames < 8 || options.maximum_evidence_frames > 1000000) {
        throw std::invalid_argument("Invalid guitar technique duration, coverage, contour, modulation or resource limits.");
    }
    GuitarTechniqueResult result;
    result.calibrated_pitch_confidence = event.calibrated_confidence;
    if (!options.enabled || instrument != InstrumentPrior::Guitar) {
        result.explanation = "Guitar technique analysis is opt-in and restricted to the guitar instrument module.";
        return result;
    }
    if (!std::isfinite(event.onset_seconds) || !std::isfinite(event.offset_seconds) ||
        event.onset_seconds < 0 || event.offset_seconds <= event.onset_seconds) {
        throw std::invalid_argument("Guitar technique analysis requires a valid event interval.");
    }
    if (!options.isolated_pitch_contour) {
        result.explanation = "No isolated continuous pitch contour: polyphonic activations or note pitches cannot identify technique.";
        return result;
    }
    if (event.evidence.size() > options.maximum_evidence_frames) {
        result.explanation = "Technique evidence exceeds the configured frame budget; no partial-contour inference was made.";
        return result;
    }
    std::vector<std::pair<double, double>> contour;
    contour.reserve(event.evidence.size());
    double previous = -1;
    for (const auto& frame : event.evidence) {
        if (!std::isfinite(frame.time_seconds) || frame.time_seconds <= previous ||
            !std::isfinite(frame.raw_voicing) ||
            (frame.fractional_midi && (!std::isfinite(*frame.fractional_midi) ||
             *frame.fractional_midi < 0 || *frame.fractional_midi > 127))) {
            throw std::invalid_argument("Guitar technique evidence needs strictly increasing finite times and valid fractional MIDI.");
        }
        previous = frame.time_seconds;
        if (frame.time_seconds < event.onset_seconds || frame.time_seconds > event.offset_seconds ||
            !frame.fractional_midi || frame.raw_voicing < options.minimum_voicing) continue;
        contour.emplace_back(frame.time_seconds, *frame.fractional_midi);
    }
    if (contour.size() < 8) {
        result.explanation = "Insufficient reliable fractional-pitch evidence; no technique inferred.";
        return result;
    }
    const double duration = contour.back().first - contour.front().first;
    result.contour_coverage = duration / (event.offset_seconds - event.onset_seconds);
    if (duration < options.minimum_duration_seconds || result.contour_coverage < options.minimum_coverage) {
        result.explanation = "Contour is too short or does not cover enough of the note.";
        return result;
    }
    double variation = 0;
    std::vector<double> pitches;
    pitches.reserve(contour.size());
    for (std::size_t i = 0; i < contour.size(); ++i) {
        pitches.push_back(contour[i].second);
        if (i == 0) continue;
        const double dt = contour[i].first - contour[i - 1].first;
        const double delta = std::abs(contour[i].second - contour[i - 1].second);
        if (dt > options.maximum_frame_gap_seconds || delta > 0.5) {
            result.explanation = "Contour has a gap or abrupt pitch jump; a continuous guitar gesture is not established.";
            return result;
        }
        variation += delta;
    }
    std::sort(pitches.begin(), pitches.end());
    const double low = pitches[pitches.size() / 20];
    const double high = pitches[pitches.size() - 1 - pitches.size() / 20];
    result.excursion_semitones = high - low;
    if (result.excursion_semitones < options.minimum_excursion_semitones ||
        result.excursion_semitones > options.maximum_excursion_semitones) {
        result.explanation = "Pitch excursion is outside supported gesture bounds.";
        return result;
    }
    const double midpoint = (low + high) / 2;
    const double hysteresis = result.excursion_semitones * 0.2;
    int side = 0, crossings = 0;
    std::vector<double> crossing_times;
    for (const auto& [time, pitch] : contour) {
        const int next = pitch > midpoint + hysteresis ? 1 :
            pitch < midpoint - hysteresis ? -1 : side;
        if (next != 0 && side != 0 && next != side) {
            ++crossings;
            crossing_times.push_back(time);
        }
        side = next;
    }
    const double net_change = contour.back().second - contour.front().second;
    const std::size_t edge_frames = std::max<std::size_t>(1, contour.size() / 4);
    double edge_drift = 0;
    for (std::size_t i = 0; i < edge_frames; ++i) {
        edge_drift += contour[contour.size() - edge_frames + i].second - contour[i].second;
    }
    edge_drift /= static_cast<double>(edge_frames);
    const double monotonicity = variation > 0 ? std::abs(net_change) / variation : 0;
    if (crossings >= 5) {
        const double half_period = (crossing_times.back() - crossing_times.front()) /
            static_cast<double>(crossing_times.size() - 1);
        const double hz = 0.5 / half_period;
        result.modulation_hz = hz;
        bool regular = true;
        for (std::size_t i = 1; i < crossing_times.size(); ++i) {
            const double interval = crossing_times[i] - crossing_times[i - 1];
            regular &= std::abs(interval - half_period) <= half_period * 0.4;
        }
        if (regular && hz >= options.minimum_vibrato_hz && hz <= options.maximum_vibrato_hz &&
            std::abs(edge_drift) <= result.excursion_semitones * 0.5) {
            result.gesture = GuitarPitchGesture::Vibrato;
            result.modulation_hz = hz;
            result.heuristic_support = result.contour_coverage;
            result.possible_techniques = {"vibrato", "external pitch modulation"};
            result.explanation = "Regular continuous pitch modulation; physical vibrato versus effects is unresolved.";
            return result;
        }
        result.explanation = "Pitch modulation failed the supported rate, regularity or drift bounds.";
        return result;
    }
    if (monotonicity >= 0.85 && std::abs(net_change) >= options.minimum_excursion_semitones) {
        result.gesture = net_change > 0 ? GuitarPitchGesture::SustainedRise :
            GuitarPitchGesture::SustainedFall;
        result.heuristic_support = result.contour_coverage * monotonicity;
        result.possible_techniques = {"bend/release", "slide", "external pitch modulation"};
        result.explanation = "Continuous pitch motion supports a gesture, not a unique bend or slide. String/contact evidence is unavailable.";
        return result;
    }
    result.explanation = "Available contour does not establish a supported gesture; hammer-ons, pull-offs and harmonics need additional evidence.";
    return result;
}

} // namespace transcribe
