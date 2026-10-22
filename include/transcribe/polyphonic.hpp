#pragma once

#include "transcribe/transcriber.hpp"

namespace transcribe {

struct PitchActivationGrid {
    int minimum_midi{};
    int maximum_midi{};
    double hop_seconds{};
    double window_seconds{};
    EvidenceSemantics semantics{EvidenceSemantics::Unspecified};
    PitchObjective objective{PitchObjective::Unspecified};
    std::string method;
    // Frame-major values; timestamps are input-relative analysis-window starts.
    std::vector<double> timestamps;
    std::vector<double> values;
    std::vector<double> onsets;
    std::vector<double> fractional_midi;

    double frame_rate_hz() const { return 1.0 / hop_seconds; }
};

void validate_pitch_activation_grid(const PitchActivationGrid& grid);
PitchActivationGrid pitch_activation_grid(std::span<const FrameEvidenceTrack> tracks,
    PitchObjective objective = PitchObjective::Unspecified);

class HarmonicCompetition {
public:
    HarmonicCompetition(std::uint32_t sample_rate, std::size_t window_size,
                        int minimum_midi, int maximum_midi);
    std::vector<double> activations(std::span<const double> magnitudes) const;

private:
    struct BinWeight { std::size_t bin; double weight; };
    std::vector<std::vector<BinWeight>> templates_;
    std::vector<double> squared_norms_;
    std::size_t bin_count_{};
};

} // namespace transcribe
