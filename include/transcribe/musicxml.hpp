#pragma once

#include "transcribe/guitar_tab.hpp"
#include "transcribe/interpretation.hpp"
#include "transcribe/score.hpp"
#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace transcribe {

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes);
void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& metadata);
void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& metadata, const GuitarTabOptions& tab_options,
                    bool preserve_expressive_timing = false);
void write_musicxml(const std::filesystem::path& path,
                    const ScoreInterpretation& interpretation,
                    std::size_t hypothesis_index = 0,
                    const GuitarTabOptions& tab_options = {},
                    bool preserve_expressive_timing = false);
void write_musicxml(const std::filesystem::path& path,
                    const ScoreDocument& score,
                    bool preserve_expressive_timing = false);
void write_musicxml(const std::filesystem::path& path,
                    const ScoreDocument& score,
                    bool preserve_expressive_timing,
                    std::string* rendered_musicxml);
void write_midi(const std::filesystem::path& path, const std::vector<Note>& notes,
                const ScoreMetadata& metadata);
void write_midi(const std::filesystem::path& path, const ScoreDocument& score);

} // namespace transcribe
