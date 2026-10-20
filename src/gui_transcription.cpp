#include "gui_transcription.hpp"

#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace transcribe::gui {
namespace {

void report_progress(const ProgressCallback& callback, double progress,
                    const char* status) {
    if (callback) {
        callback(std::clamp(progress, 0.0, 1.0), status);
    }
}

} // namespace

Result run_transcription(const Request& request, std::stop_token stop_token,
                         const ProgressCallback& progress_callback,
                         const LivePreviewCallback& live_preview_callback) {
    if (stop_token.stop_requested()) {
        throw std::runtime_error("Transcription was cancelled.");
    }

    auto extension = request.input_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    const bool streaming_wav = !request.use_asio && request.model_path.empty() &&
                               extension == ".wav";
    std::vector<Note> notes;

    if (streaming_wav) {
        report_progress(progress_callback, 0.0, "Reading audio");
        StreamingOptions options;
        options.analysis.progress_callback = [&](double progress) {
            report_progress(progress_callback, progress, "Analyzing audio");
        };
        notes = transcribe_wav_streaming(request.input_path, options).notes;
    } else {
        AudioBuffer audio = request.use_asio
            ? capture_asio(
                  request.capture_seconds, stop_token,
                  [&](double progress) {
                      report_progress(progress_callback, progress * 0.35,
                                      "Capturing ASIO audio");
                  },
                  [&](double offset, const AudioBuffer& snapshot) {
                      if (live_preview_callback) {
                          const auto live_notes = transcribe_monophonic(snapshot);
                          live_preview_callback(offset, live_notes);
                      }
                  })
            : read_audio(request.input_path);

        report_progress(progress_callback, 0.4, "Audio loaded");
        if (request.model_path.empty()) {
            AnalysisOptions options;
            options.progress_callback = [&](double progress) {
                report_progress(progress_callback, 0.4 + progress * 0.6,
                                "Analyzing audio");
            };
            notes = transcribe_monophonic(audio, options);
        } else {
#ifdef TRANSCRIBE_ENABLE_ONNX
            OnnxOptions options;
            options.polyphonic = request.polyphonic;
            options.independent_probabilities = request.polyphonic;
            options.progress_callback = [&](double progress) {
                report_progress(progress_callback, 0.4 + progress * 0.6,
                                "Running ONNX model");
            };
            notes = transcribe_with_onnx(audio, request.model_path, options);
#else
            throw std::runtime_error("ONNX Runtime support is disabled.");
#endif
        }
    }

    if (notes.empty()) {
        throw std::runtime_error("No stable pitches were detected in the input.");
    }

    Result result;
    result.metadata = infer_score_metadata(notes);
    result.notes = std::move(notes);
    write_musicxml(request.musicxml_path, result.notes, result.metadata);
    write_guitar_tab(request.tab_path, result.notes);
    if (!request.midi_path.empty()) {
        write_midi(request.midi_path, result.notes, result.metadata);
    }

    if (live_preview_callback) {
        live_preview_callback(0.0, result.notes);
    }
    report_progress(progress_callback, 1.0, "Finished");
    return result;
}

} // namespace transcribe::gui
