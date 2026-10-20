#pragma once

#include "transcribe/transcriber.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace transcribe {

struct GuitarTabPosition {
    std::size_t note_index{};
    int string_number{};
    int fret{};
};

struct GuitarTabOptions {
    int maximum_fret{24};
    int preferred_position{5};
    double position_shift_cost{1.4};
    bool duration_proportional_ascii{};
    std::array<int, 6> tuning{64, 59, 55, 50, 45, 40};
    int capo_fret{};
    std::size_t alternative_fingerings{};
    std::vector<GuitarTabPosition> position_overrides;
};

std::vector<GuitarTabPosition> assign_guitar_tab_positions(
    const std::vector<Note>& notes, const GuitarTabOptions& options = {});
void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& options);

} // namespace transcribe
