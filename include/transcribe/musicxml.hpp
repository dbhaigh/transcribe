#pragma once

#include "transcribe/guitar_tab.hpp"
#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <vector>

namespace transcribe {

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& metadata);
void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& metadata, const GuitarTabOptions& tab_options,
                    bool preserve_expressive_timing = false);
void write_midi(const std::filesystem::path& path, const std::vector<Note>& notes,
                const ScoreMetadata& metadata);

} // namespace transcribe
