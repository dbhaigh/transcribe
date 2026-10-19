#pragma once

#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <vector>

namespace transcribe {

struct GuitarTabOptions {
    int maximum_fret{24};
    int preferred_position{5};
    double position_shift_cost{1.4};
    bool duration_proportional_ascii{};
};

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& options);

} // namespace transcribe
