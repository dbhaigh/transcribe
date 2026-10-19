#pragma once

#include "transcribe/audio.hpp"

#include <string>
#include <vector>
#include <filesystem>

namespace transcribe {

struct Note {
    int midi_pitch{};
    double start_seconds{};
    double duration_seconds{};
    std::string name;
};

std::vector<Note> transcribe_monophonic(const AudioBuffer& audio);
std::vector<Note> transcribe_with_onnx(const AudioBuffer& audio,
                                       const std::filesystem::path& model_path);
std::string midi_note_name(int midi_pitch);

} // namespace transcribe
