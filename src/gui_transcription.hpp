#pragma once

#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace transcribe::gui {

struct Request {
    bool use_asio{};
    bool polyphonic{};
    double capture_seconds{10.0};
    std::filesystem::path input_path;
    std::filesystem::path model_path;
    std::filesystem::path musicxml_path;
    std::filesystem::path tab_path;
    std::filesystem::path midi_path;
};

struct Result {
    std::vector<Note> notes;
    ScoreMetadata metadata;
};

using ProgressCallback = std::function<void(double, const std::string&)>;
using LivePreviewCallback =
    std::function<void(double, const std::vector<Note>&)>;

Result run_transcription(const Request& request, std::stop_token stop_token,
                         const ProgressCallback& progress_callback,
                         const LivePreviewCallback& live_preview_callback);

} // namespace transcribe::gui
