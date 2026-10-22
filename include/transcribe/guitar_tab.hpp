#pragma once

#include "transcribe/fingering.hpp"
#include "transcribe/transcriber.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace transcribe {

struct ScoreDocument;

struct GuitarTabOptions {
    int maximum_fret{24};
    int preferred_position{5};
    double position_shift_cost{1.4};
    bool duration_proportional_ascii{};
    std::array<int, 6> tuning{64, 59, 55, 50, 45, 40};
    int capo_fret{};
    std::size_t alternative_fingerings{};
    std::vector<GuitarTabPosition> position_overrides;
    std::vector<FingeringCorrection> manual_corrections;
    // Search/cost settings shared with the solver. Instrument fields above take precedence.
    FingeringOptions search;
    bool suggest_legato_marks{};
    std::vector<int> string_tuning;
};

std::span<const int> configured_tuning(const GuitarTabOptions& options) noexcept;

FingeringResult assign_guitar_tab_fingerings(
    const std::vector<Note>& notes, const GuitarTabOptions& options = {});
std::vector<GuitarTabPosition> assign_guitar_tab_positions(
    const std::vector<Note>& notes, const GuitarTabOptions& options = {});
void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& options);
void write_guitar_tab(const std::filesystem::path& path,
                      const std::vector<Note>& notes,
                      const GuitarTabOptions& options,
                      const FingeringResult& fingering);
void write_guitar_tab(const std::filesystem::path& path,
                      const ScoreDocument& score,
                      const GuitarTabOptions& options = {});

} // namespace transcribe
