#include "transcribe/audio.hpp"
#include "transcribe/transcriber.hpp"
#include "transcribe/inference.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

double cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("GetProcessTimes failed");
    }
    const auto seconds = [](FILETIME time) {
        return (static_cast<double>(time.dwHighDateTime) * 4294967296.0 +
                time.dwLowDateTime) / 10000000.0;
    };
    return seconds(kernel) + seconds(user);
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
           usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#endif
}

std::size_t peak_resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        throw std::runtime_error("GetProcessMemoryInfo failed");
    }
    return memory.PeakWorkingSetSize;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

void write_notes(std::ostream& output, const std::vector<transcribe::Note>& notes) {
    output << '[';
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i) output << ',';
        const auto& n = notes[i];
        output << "{\"midi\":" << n.midi_pitch << ",\"start\":" << n.start_seconds
               << ",\"duration\":" << n.duration_seconds << ",\"confidence\":" << n.confidence << '}';
    }
    output << ']';
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 8 && argc != 10) {
            throw std::invalid_argument(
                "Usage: transcriber-performance input.wav output.json yin|spectral "
                "full|streaming|model serial|parallel repetitions chunk-seconds [model.onnx|- cpu|cuda|directml|rocm]");
        }
        const std::string backend = argv[3], mode = argv[4], scheduling = argv[5];
        const std::string repetitions_text = argv[6], chunk_text = argv[7];
        std::size_t consumed = 0;
        const int repetitions = std::stoi(repetitions_text, &consumed);
        if (consumed != repetitions_text.size() || repetitions < 2 || repetitions > 100) {
            throw std::invalid_argument("Repetitions must be 2-100");
        }
        const double chunk = std::stod(chunk_text, &consumed);
        if (consumed != chunk_text.size() || !std::isfinite(chunk) ||
            chunk < 1 || chunk > 300 ||
            (backend != "yin" && backend != "spectral" && !(backend == "onnx" && mode == "model")) ||
            (mode != "full" && mode != "streaming" && mode != "model") ||
            ((mode == "model") != (argc == 10)) ||
            (scheduling != "serial" && scheduling != "parallel")) {
            throw std::invalid_argument("Invalid performance configuration");
        }
        // Full-buffer decode is outside analysis timing; streaming includes decode.
        transcribe::AudioBuffer audio;
        if (mode != "streaming") audio = transcribe::read_wav(argv[1]);
        std::unique_ptr<transcribe::ModelBackend> instance;
        transcribe::OnnxExecutionProvider provider = transcribe::OnnxExecutionProvider::Cpu;
        if (mode == "model") {
            const std::string name = argv[9];
            if (name == "cuda") provider = transcribe::OnnxExecutionProvider::Cuda;
            else if (name == "directml") provider = transcribe::OnnxExecutionProvider::DirectML;
            else if (name == "rocm") provider = transcribe::OnnxExecutionProvider::Rocm;
            else if (name != "cpu") throw std::invalid_argument("Unknown requested provider");
            transcribe::ModelRegistry registry;
            const auto id = backend == "onnx" ? "benchmark:basic-pitch" : "builtin:" + backend;
            if (backend == "onnx") {
                const auto registered = registry.add_onnx({1, id, transcribe::ModelContract::BasicPitchV1,
                    argv[8], "Spotify Basic Pitch; see model license", "Apache-2.0"});
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&registered)) {
                    throw std::runtime_error(failure->message);
                }
            }
            auto created = registry.create(id);
            if (const auto* failure = std::get_if<transcribe::InferenceError>(&created)) {
                throw std::runtime_error(failure->message);
            }
            instance = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
            const auto loaded = instance->load({provider, 0, false});
            if (const auto* failure = std::get_if<transcribe::InferenceError>(&loaded)) {
                throw std::runtime_error(failure->message);
            }
        }
        std::ofstream output(argv[2], std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create performance report");
        output << std::setprecision(17) << "{\"runs\":[";
        std::vector<transcribe::Note> first_notes;
        for (int iteration = 0; iteration < repetitions; ++iteration) {
            transcribe::AnalysisTimings stages;
            transcribe::AnalysisOptions options;
            options.polyphonic = backend == "spectral";
            options.parallel = scheduling == "parallel";
            options.timing_callback = [&](const transcribe::AnalysisTimings& t) {
                stages.preprocessing_seconds += t.preprocessing_seconds;
                stages.onset_fft_seconds += t.onset_fft_seconds;
                stages.resampling_seconds += t.resampling_seconds;
                stages.pitch_analysis_seconds += t.pitch_analysis_seconds;
                stages.spectrogram_cache_hit = t.spectrogram_cache_hit;
            };
            const double cpu_start = cpu_seconds();
            const auto start = Clock::now();
            std::vector<transcribe::Note> notes;
            double duration = 0;
            if (instance) {
                if (backend == "onnx" && audio.samples.empty()) {
                    duration = 0; // Same explicit empty-input skip as the quality evaluator.
                } else {
                    transcribe::InferenceRequest request;
                    request.analysis = options;
                    request.onnx.polyphonic = true;
                    request.onnx.independent_probabilities = true;
                    request.onnx.execution_provider = provider;
                    auto result = instance->infer(audio, request);
                    if (const auto* failure = std::get_if<transcribe::InferenceError>(&result)) {
                        throw std::runtime_error(failure->message);
                    }
                    notes = std::move(std::get<transcribe::InferenceOutput>(result).notes);
                    duration = static_cast<double>(audio.samples.size()) / audio.sample_rate;
                }
            } else if (mode == "streaming") {
                transcribe::StreamingOptions streaming;
                streaming.analysis = options;
                streaming.chunk_seconds = chunk;
                const auto result = transcribe::transcribe_wav_streaming(argv[1], streaming);
                notes = result.notes;
                duration = result.duration_seconds;
            } else {
                notes = transcribe::transcribe_monophonic(audio, options);
                duration = static_cast<double>(audio.samples.size()) / audio.sample_rate;
            }
            const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            const double cpu = cpu_seconds() - cpu_start;
            if (iteration == 0) first_notes = notes;
            else {
                if (notes.size() != first_notes.size()) throw std::runtime_error("Warm notes differ");
                for (std::size_t i = 0; i < notes.size(); ++i) {
                    const auto& a = notes[i];
                    const auto& b = first_notes[i];
                    if (a.midi_pitch != b.midi_pitch || a.start_seconds != b.start_seconds ||
                        a.duration_seconds != b.duration_seconds || a.confidence != b.confidence) {
                        throw std::runtime_error("Warm notes differ");
                    }
                }
            }
            if (iteration) output << ',';
            output << "{\"wall_seconds\":" << elapsed << ",\"cpu_seconds\":" << cpu
                   << ",\"cpu_core_equivalents\":";
            if (elapsed > 0) output << cpu / elapsed;
            else output << "null";
            output << ",\"audio_seconds\":" << duration
                   << ",\"peak_resident_bytes\":" << peak_resident_bytes()
                   << ",\"stages\":{\"preprocessing_seconds\":" << stages.preprocessing_seconds
                   << ",\"onset_fft_seconds\":" << stages.onset_fft_seconds
                   << ",\"resampling_seconds\":" << stages.resampling_seconds
                   << ",\"pitch_analysis_seconds\":" << stages.pitch_analysis_seconds
                   << ",\"spectrogram_cache_hit\":" << (stages.spectrogram_cache_hit ? "true" : "false")
                   << "}}";
        }
        output << "],\"notes\":";
        write_notes(output, first_notes);
        if (instance) {
            const auto resources = instance->resources();
            const auto& metadata = instance->metadata();
            const auto diagnostics = instance->diagnostics();
            output << ",\"model_interface\":{\"contract_version\":1,\"provider\":\""
                   << transcribe::inference_provider_name(provider)
                   << "\",\"cpu_fallback_disabled\":" << (diagnostics.cpu_fallback_disabled ? "true" : "false")
                   << ",\"load_seconds\":" << resources.load_seconds
                   << ",\"model_file_bytes\":" << resources.model_file_bytes
                   << ",\"live_sessions\":" << resources.live_sessions
                   << ",\"completed_inferences\":" << resources.completed_inferences
                   << ",\"runtime_version\":\"" << diagnostics.runtime_version
                   << "\",\"incremental\":false,\"batch_size\":" << metadata.batch_size
                   << ",\"analysis_sample_rate\":" << metadata.analysis_sample_rate
                   << ",\"window_samples\":" << metadata.window_samples
                   << ",\"hop_samples\":" << metadata.hop_samples
                   << ",\"overlap_samples\":" << metadata.overlap_samples
                   << ",\"providers\":[";
            bool comma = false;
            for (const auto& p : transcribe::inference_providers()) {
                if (comma) output << ',';
                comma = true;
                output << "{\"provider\":\"" << transcribe::inference_provider_name(p.provider)
                       << "\",\"compiled\":" << (p.compiled ? "true" : "false")
                       << ",\"runtime_available\":" << (p.runtime_available ? "true" : "false") << '}';
            }
            output << "]}";
            instance->release();
            output << ",\"live_sessions_after_release\":" << instance->resources().live_sessions;
        }
        output << "}\n";
        output.close();
        if (!output) throw std::runtime_error("Failed writing performance report");
    } catch (const std::exception& error) {
        std::cerr << "transcriber-performance: " << error.what() << '\n';
        return 1;
    }
}
