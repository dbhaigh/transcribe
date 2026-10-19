#pragma once

#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <vector>

namespace transcribe {

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& metadata);
void write_midi(const std::filesystem::path& path, const std::vector<Note>& notes,
                const ScoreMetadata& metadata);

} // namespace transcribe
