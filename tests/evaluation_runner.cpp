#include "transcribe/audio.hpp"
#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

double number(const std::string& value) {
    double result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result)) {
        throw std::invalid_argument("Invalid numeric argument: " + value);
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            throw std::invalid_argument(
                "Usage: transcriber-evaluate input.wav output.json [--backend yin|spectral|onnx]"
                " [--mode full|streaming|realtime] [--model path] [--provider cpu|cuda|directml|rocm]"
                " [--fast] [--serial] [--minimum-confidence value] [--chunk-seconds value]"
                " [--overlap-seconds value] [--latency-ms value]");
        }
        std::string backend = "yin";
        std::string mode = "full";
        std::filesystem::path model;
        transcribe::AnalysisOptions analysis;
        transcribe::OnnxOptions onnx;
        transcribe::StreamingOptions streaming;
        transcribe::RealtimeOptions realtime;
        for (int i = 3; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--fast") {
                analysis.fast_preview = true;
                continue;
            }
            if (flag == "--serial") {
                analysis.parallel = false;
                continue;
            }
            if (++i >= argc) {
                throw std::invalid_argument("Missing value for " + flag);
            }
            const std::string value = argv[i];
            if (flag == "--backend") backend = value;
            else if (flag == "--mode") mode = value;
            else if (flag == "--model") model = value;
            else if (flag == "--provider") {
                if (value == "cpu") onnx.execution_provider = transcribe::OnnxExecutionProvider::Cpu;
                else if (value == "cuda") onnx.execution_provider = transcribe::OnnxExecutionProvider::Cuda;
                else if (value == "directml") onnx.execution_provider = transcribe::OnnxExecutionProvider::DirectML;
                else if (value == "rocm") onnx.execution_provider = transcribe::OnnxExecutionProvider::Rocm;
                else throw std::invalid_argument("Unknown provider: " + value);
            } else if (flag == "--minimum-confidence") {
                analysis.minimum_confidence = number(value);
                if (analysis.minimum_confidence < 0 || analysis.minimum_confidence > 1) {
                    throw std::invalid_argument("Minimum confidence must be in [0,1]");
                }
                onnx.minimum_confidence = analysis.minimum_confidence;
            } else if (flag == "--chunk-seconds") streaming.chunk_seconds = number(value);
            else if (flag == "--overlap-seconds") streaming.overlap_seconds = number(value);
            else if (flag == "--latency-ms") realtime.latency_ms = number(value);
            else throw std::invalid_argument("Unknown option: " + flag);
        }
        if (backend != "yin" && backend != "spectral" && backend != "onnx") {
            throw std::invalid_argument("Unknown backend: " + backend);
        }
        if (mode != "full" && mode != "streaming" && mode != "realtime") {
            throw std::invalid_argument("Unknown mode: " + mode);
        }
        if ((backend == "onnx" && (model.empty() || mode != "full" || analysis.fast_preview)) ||
            (backend != "onnx" && (!model.empty() ||
                onnx.execution_provider != transcribe::OnnxExecutionProvider::Cpu)) ||
            (mode == "realtime" && (backend != "yin" || analysis.fast_preview ||
                                    analysis.minimum_confidence != 0.25))) {
            throw std::invalid_argument("Unsupported backend/mode/model/provider/options combination");
        }
        analysis.polyphonic = backend == "spectral";
        onnx.polyphonic = true;
        onnx.independent_probabilities = true;
        std::vector<transcribe::Note> notes;
        std::vector<transcribe::NoteEvent> events;
        transcribe::AnalysisTimings stages;
        analysis.timing_callback = [&](const transcribe::AnalysisTimings& t) {
            stages.preprocessing_seconds += t.preprocessing_seconds;
            stages.onset_fft_seconds += t.onset_fft_seconds;
            stages.resampling_seconds += t.resampling_seconds;
            stages.pitch_analysis_seconds += t.pitch_analysis_seconds;
        };
        const auto total_start = Clock::now();
        double decode_seconds = 0;
        double analysis_seconds = 0;
        double first_update = -1;
        double duration = 0;
        std::uint32_t rate = 0;
        bool inference_skipped = false;
        if (mode == "streaming") {
            streaming.analysis = analysis;
            streaming.audio_callback = [&](const transcribe::AudioBlockView& b) { rate = b.sample_rate; };
            streaming.notes_callback = [&](const auto& current) {
                if (!current.empty() && first_update < 0) first_update = elapsed(total_start);
            };
            const auto result = transcribe::transcribe_wav_streaming(argv[1], streaming);
            notes = result.notes;
            duration = result.duration_seconds;
            if (rate == 0 && duration == 0) rate = transcribe::read_wav(argv[1]).sample_rate;
            analysis_seconds = elapsed(total_start); // Includes streaming decode.
        } else {
            const auto audio = transcribe::read_wav(argv[1]);
            decode_seconds = elapsed(total_start);
            rate = audio.sample_rate;
            duration = static_cast<double>(audio.samples.size()) / rate;
            const auto analysis_start = Clock::now();
            if (mode == "realtime") {
                realtime.input_sample_rate = rate;
                transcribe::RealtimeTranscriber analyzer(realtime);
                const auto block = std::max<std::size_t>(1, rate / 100);
                for (std::size_t offset = 0; offset < audio.samples.size(); offset += block) {
                    analyzer.push_samples(std::span<const float>(audio.samples).subspan(
                        offset, std::min(block, audio.samples.size() - offset)));
                    auto batch = analyzer.take_events();
                    if (!batch.empty() && first_update < 0) first_update = elapsed(total_start);
                    events.insert(events.end(), batch.begin(), batch.end());
                }
                analyzer.finish();
                auto batch = analyzer.take_events();
                if (!batch.empty() && first_update < 0) first_update = elapsed(total_start);
                events.insert(events.end(), batch.begin(), batch.end());
                notes = analyzer.live_notes();
            } else if (backend == "onnx") {
#ifdef TRANSCRIBE_ENABLE_ONNX
                if (audio.samples.empty()) {
                    inference_skipped = true;
                    std::cerr << "Empty input: ONNX inference not invoked\n";
                } else {
                    notes = transcribe::transcribe_with_onnx(audio, model, onnx);
                }
#else
                throw std::runtime_error("ONNX support is disabled in this build");
#endif
            } else {
                notes = transcribe::transcribe_monophonic(audio, analysis);
            }
            analysis_seconds = elapsed(analysis_start);
        }
        const double total_seconds = elapsed(total_start);
        std::ofstream output(argv[2], std::ios::binary);
        if (!output) throw std::runtime_error("Could not create evaluation output");
        output << std::setprecision(17)
               << "{\"sample_rate\":" << rate << ",\"duration_seconds\":" << duration
               << ",\"inference_skipped\":" << (inference_skipped ? "true" : "false")
               << ",\"hardware_concurrency\":" << std::thread::hardware_concurrency()
               << ",\"decode_seconds\":";
        if (mode == "streaming") output << "null";
        else output << decode_seconds;
        output << ",\"analysis_seconds\":" << analysis_seconds
               << ",\"decode_included_in_analysis\":" << (mode == "streaming" ? "true" : "false")
               << ",\"total_seconds\":" << total_seconds << ",\"first_update_seconds\":";
        if (first_update < 0) output << "null";
        else output << first_update;
        output << ",\"stages\":";
        if (mode != "realtime" && backend != "onnx") {
            output << "{\"preprocessing_seconds\":" << stages.preprocessing_seconds
                   << ",\"onset_fft_seconds\":" << stages.onset_fft_seconds
                   << ",\"resampling_seconds\":" << stages.resampling_seconds
                   << ",\"pitch_analysis_seconds\":" << stages.pitch_analysis_seconds << '}';
        } else output << "null";
        output << ",\"notes\":[";
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto& n = notes[i];
            if (i) output << ',';
            output << "{\"midi\":" << n.midi_pitch << ",\"start\":" << n.start_seconds
                   << ",\"duration\":" << n.duration_seconds
                   << ",\"confidence\":" << n.confidence << '}';
        }
        output << "],\"events\":[";
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            if (i) output << ',';
            output << "{\"type\":\"" << (e.type == transcribe::NoteEventType::Onset ? "onset" : "offset")
                   << "\",\"midi\":" << e.midi_pitch << ",\"time_seconds\":" << e.time_seconds
                   << ",\"detected_at_seconds\":" << e.detected_at_seconds << '}';
        }
        output << "]}\n";
        output.close();
        if (!output) throw std::runtime_error("Failed writing evaluation output");
    } catch (const std::exception& error) {
        std::cerr << "transcriber-evaluate: " << error.what() << '\n';
        return 1;
    }
}
