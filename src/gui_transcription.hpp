#pragma once

#include "transcribe/guitar_tab.hpp"
#include "transcribe/interpretation.hpp"
#include "transcribe/asio_capture.hpp"
#include "transcribe/score.hpp"
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
    bool fast_preview{};
    bool proposal_only{};
    InstrumentPrior instrument{InstrumentPrior::None};
    double capture_seconds{10.0};
    RealtimeOptions realtime_options;
    AsioCaptureOptions asio_capture_options;
    std::string live_backend{"yin"};
    GuitarTabOptions guitar_tab_options;
    InterpretationOptions interpretation_options;
    bool preserve_expressive_timing{};
    std::filesystem::path input_path;
    std::filesystem::path model_path;
    std::filesystem::path musicxml_path;
    std::filesystem::path tab_path;
    std::filesystem::path midi_path;
};

struct Result {
    std::vector<Note> notes;
    FingeringResult fingering;
    ScoreMetadata metadata;
    ScoreDocument score;
#ifdef TRANSCRIBE_ENABLE_VEROVIO
    std::string musicxml_source;
#endif
};

using ProgressCallback = std::function<void(double, const std::string&)>;
using LivePreviewCallback =
    std::function<void(double, const std::vector<Note>&,
                       const std::vector<NoteEvent>&)>;
using AudioPreviewCallback =
    std::function<void(double, const AudioPreviewData&, bool)>;

Result run_transcription(const Request& request, std::stop_token stop_token,
                         const ProgressCallback& progress_callback,
                         const LivePreviewCallback& live_preview_callback,
                         const AudioPreviewCallback& audio_preview_callback = {});

} // namespace transcribe::gui
