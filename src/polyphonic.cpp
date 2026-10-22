#include "transcribe/polyphonic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace transcribe {

void validate_pitch_activation_grid(const PitchActivationGrid& grid) {
    if (grid.minimum_midi < 0 || grid.maximum_midi > 127 ||
        grid.minimum_midi > grid.maximum_midi || grid.method.empty() ||
        !std::isfinite(grid.hop_seconds) || grid.hop_seconds <= 0 ||
        !std::isfinite(grid.window_seconds) || grid.window_seconds < 0 ||
        grid.semantics == EvidenceSemantics::Unspecified ||
        static_cast<int>(grid.semantics) < 0 ||
        grid.semantics > EvidenceSemantics::CalibratedProbability ||
        grid.objective < PitchObjective::Unspecified ||
        grid.objective > PitchObjective::IndependentMultiPitch) {
        throw std::invalid_argument("Invalid pitch activation range, time geometry or semantics");
    }
    const auto pitches = static_cast<std::size_t>(grid.maximum_midi - grid.minimum_midi + 1);
    if (grid.timestamps.size() > std::numeric_limits<std::size_t>::max() / pitches ||
        grid.values.size() != grid.timestamps.size() * pitches ||
        (!grid.onsets.empty() && grid.onsets.size() != grid.values.size()) ||
        (!grid.fractional_midi.empty() && grid.fractional_midi.size() != grid.values.size())) {
        throw std::invalid_argument("Pitch activation heads must have frame-by-pitch dimensions");
    }
    double previous = -1;
    for (const auto time : grid.timestamps) {
        if (!std::isfinite(time) || time < 0 || time <= previous) {
            throw std::invalid_argument("Pitch activation timestamps must be finite and strictly ordered");
        }
        if (previous >= 0 && std::abs(time - previous - grid.hop_seconds) > 1e-8) {
            throw std::invalid_argument("Pitch activation timestamps do not match the declared frame hop");
        }
        previous = time;
    }
    for (const auto* head : {&grid.values, &grid.onsets, &grid.fractional_midi}) {
        for (const auto value : *head) {
            if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite pitch activation");
        }
    }
    for (const auto value : grid.fractional_midi) {
        if (value < 0 || value > 127) throw std::invalid_argument("Invalid fractional MIDI evidence");
    }
}

PitchActivationGrid pitch_activation_grid(std::span<const FrameEvidenceTrack> tracks,
                                         PitchObjective objective) {
    if (tracks.empty()) throw std::invalid_argument("Pitch activation grid needs at least one track");
    PitchActivationGrid grid;
    grid.objective = objective;
    grid.minimum_midi = 127;
    grid.maximum_midi = 0;
    const auto& first = tracks.front();
    grid.hop_seconds = first.hop_seconds;
    grid.window_seconds = first.window_seconds;
    grid.method = first.method;
    grid.semantics = first.raw_values.empty() ? first.selected_strength_semantics :
        first.raw_value_semantics;
    for (const auto& frame : first.frames) grid.timestamps.push_back(frame.time_seconds);
    const bool onsets = !first.onset_strengths.empty();
    const bool fractional = !first.contour_midi.empty();
    for (const auto& track : tracks) {
        if (track.raw_midi_pitch < 0 || track.raw_midi_pitch > 127 ||
            track.frames.size() != grid.timestamps.size() || track.method != grid.method ||
            track.hop_seconds != grid.hop_seconds || track.window_seconds != grid.window_seconds ||
            (track.raw_values.empty() ? track.selected_strength_semantics :
                track.raw_value_semantics) != grid.semantics ||
            (!track.onset_strengths.empty()) != onsets ||
            (!track.contour_midi.empty()) != fractional) {
            throw std::invalid_argument("Pitch grid requires aligned fixed-pitch tracks with identical heads");
        }
        grid.minimum_midi = std::min(grid.minimum_midi, track.raw_midi_pitch);
        grid.maximum_midi = std::max(grid.maximum_midi, track.raw_midi_pitch);
    }
    const auto pitches = static_cast<std::size_t>(grid.maximum_midi - grid.minimum_midi + 1);
    if (tracks.size() != pitches) throw std::invalid_argument("Pitch grid has missing pitch tracks");
    grid.values.resize(grid.timestamps.size() * pitches);
    if (onsets) grid.onsets.resize(grid.values.size());
    if (fractional) grid.fractional_midi.resize(grid.values.size());
    std::vector<bool> seen(pitches);
    for (const auto& track : tracks) {
        const auto pitch = static_cast<std::size_t>(track.raw_midi_pitch - grid.minimum_midi);
        if (seen[pitch]) throw std::invalid_argument("Duplicate pitch activation track");
        seen[pitch] = true;
        for (const auto* values : {&track.raw_values, &track.onset_strengths, &track.contour_midi}) {
            if (!values->empty() && values->size() != grid.timestamps.size()) {
                throw std::invalid_argument("Pitch grid evidence arrays must align with frames");
            }
        }
        for (std::size_t frame = 0; frame < grid.timestamps.size(); ++frame) {
            if (track.frames[frame].time_seconds != grid.timestamps[frame]) {
                throw std::invalid_argument("Pitch grid track timestamps differ");
            }
            const auto index = frame * pitches + pitch;
            grid.values[index] = track.raw_values.empty() ? track.frames[frame].confidence :
                track.raw_values[frame];
            if (onsets) grid.onsets[index] = track.onset_strengths[frame];
            if (fractional) grid.fractional_midi[index] = track.contour_midi[frame];
        }
    }
    validate_pitch_activation_grid(grid);
    return grid;
}

HarmonicCompetition::HarmonicCompetition(std::uint32_t sample_rate, std::size_t window_size,
                                       int minimum_midi, int maximum_midi) {
    if (sample_rate < 2000 || window_size < 32 || minimum_midi < 0 ||
        maximum_midi > 127 || minimum_midi > maximum_midi) {
        throw std::invalid_argument("Invalid harmonic dictionary dimensions");
    }
    bin_count_ = window_size / 2 + 1;
    for (int midi = minimum_midi; midi <= maximum_midi; ++midi) {
        std::vector<double> weights(bin_count_);
        const double frequency = 440 * std::exp2((midi - 69) / 12.0);
        for (int harmonic = 1; harmonic <= 8; ++harmonic) {
            const double center = frequency * harmonic * window_size / sample_rate;
            if (center >= bin_count_ - 1) break;
            const int first = std::max(1, static_cast<int>(std::ceil(center - 2)));
            const int last = std::min(static_cast<int>(bin_count_ - 1),
                                      static_cast<int>(std::floor(center + 2)));
            for (int bin = first; bin <= last; ++bin) {
                const double x = std::abs(bin - center);
                const double hann = x < 1e-8 ? 1 : std::abs(x - 1) < 1e-8 ? .5 :
                    std::abs(std::sin(std::numbers::pi * x) /
                        (std::numbers::pi * x * (1 - x * x)));
                weights[bin] += hann / std::pow(harmonic, 1.2);
            }
        }
        std::vector<BinWeight> sparse;
        double norm = 0;
        for (std::size_t bin = 1; bin < weights.size(); ++bin) {
            if (weights[bin] > 0) sparse.push_back({bin, weights[bin]});
            norm += weights[bin] * weights[bin];
        }
        templates_.push_back(std::move(sparse));
        squared_norms_.push_back(norm);
    }
}

std::vector<double> HarmonicCompetition::activations(std::span<const double> magnitudes) const {
    if (magnitudes.size() != bin_count_ ||
        std::any_of(magnitudes.begin(), magnitudes.end(),
                    [](double value) { return !std::isfinite(value) || value < 0; })) {
        throw std::invalid_argument("Harmonic decoding requires finite nonnegative FFT magnitudes");
    }
    std::vector<double> residual(magnitudes.begin(), magnitudes.end());
    std::vector<double> activity(templates_.size());
    // Joint nonnegative fitting assigns shared partials rather than counting them for every pitch.
    for (int iteration = 0; iteration < 12; ++iteration) {
        for (std::size_t pitch = 0; pitch < templates_.size(); ++pitch) {
            double correlation = 0;
            for (const auto& entry : templates_[pitch]) {
                correlation += residual[entry.bin] * entry.weight;
            }
            const double next = squared_norms_[pitch] > 0 ?
                std::max(0.0, activity[pitch] + correlation / squared_norms_[pitch]) : 0;
            const double change = next - activity[pitch];
            for (const auto& entry : templates_[pitch]) residual[entry.bin] -= change * entry.weight;
            activity[pitch] = next;
        }
    }
    return activity;
}

} // namespace transcribe
