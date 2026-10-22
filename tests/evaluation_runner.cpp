#include "transcribe/audio.hpp"
#include "transcribe/transcriber.hpp"
#include "transcribe/decoding.hpp"
#include "transcribe/inference.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
std::string json_string(const std::string& value) {
    std::ostringstream escaped;
    escaped << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        default:
            if (character < 0x20) {
                escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(character) << std::dec;
            } else escaped << character;
        }
    }
    escaped << '"';
    return escaped.str();
}

using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct PeakMemory {
    std::optional<std::uint64_t> bytes;
    const char* source;
};

PeakMemory peak_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return {std::nullopt, "GetProcessMemoryInfo(PeakWorkingSetSize)"};
    }
    return {static_cast<std::uint64_t>(counters.PeakWorkingSetSize),
            "GetProcessMemoryInfo(PeakWorkingSetSize)"};
#elif defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return {std::nullopt, "getrusage(RUSAGE_SELF)"};
    return {static_cast<std::uint64_t>(usage.ru_maxrss), "getrusage(RUSAGE_SELF).ru_maxrss"};
#elif defined(__unix__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return {std::nullopt, "getrusage(RUSAGE_SELF)"};
    return {static_cast<std::uint64_t>(usage.ru_maxrss) * 1024,
            "getrusage(RUSAGE_SELF).ru_maxrss_kib"};
#else
    return {std::nullopt, "unavailable"};
#endif
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

void write_tensors(std::ostream& output, const std::vector<transcribe::TensorContract>& tensors) {
    output << '[';
    for (std::size_t i = 0; i < tensors.size(); ++i) {
        if (i) output << ',';
        output << "{\"name\":" << json_string(tensors[i].name)
               << ",\"element_type\":" << json_string(tensors[i].element_type) << ",\"dimensions\":[";
        for (std::size_t j = 0; j < tensors[i].dimensions.size(); ++j) {
            if (j) output << ',';
            output << tensors[i].dimensions[j];
        }
        output << "]}";
    }
    output << ']';
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
        std::string model_identifier;
        std::string audio_source_id = "file:" + std::filesystem::path(argv[1]).filename().string();
        transcribe::AnalysisOptions analysis;
        transcribe::OnnxOptions onnx;
        transcribe::StreamingOptions streaming;
        transcribe::RealtimeOptions realtime;
        bool temporal = false;
        bool model_interface = false;
        std::size_t session_warmups = 0;
        std::size_t session_repetitions = 1;
        std::vector<double> session_seconds;
        double warmup_seconds = 0;
        transcribe::ModelContract model_contract = transcribe::ModelContract::BasicPitchV1;
        transcribe::TemporalDecodingOptions decoding;
        for (int i = 3; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--temporal") { temporal = true; continue; }
            if (flag == "--model-interface") { model_interface = true; continue; }
            if (flag == "--correct-isolated-octaves") {
                decoding.correct_isolated_octaves = true;
                continue;
            }
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
            else if (flag == "--instrument") {
                if (value == "none") analysis.instrument = transcribe::InstrumentPrior::None;
                else if (value == "guitar") analysis.instrument = transcribe::InstrumentPrior::Guitar;
                else if (value == "bass") analysis.instrument = transcribe::InstrumentPrior::Bass;
                else if (value == "piano") analysis.instrument = transcribe::InstrumentPrior::Piano;
                else if (value == "voice") analysis.instrument = transcribe::InstrumentPrior::Voice;
                else throw std::invalid_argument("Unknown instrument profile");
            }
            else if (flag == "--mode") mode = value;
            else if (flag == "--model") model = value;
            else if (flag == "--model-identifier") model_identifier = value;
            else if (flag == "--session-warmups" || flag == "--session-repetitions") {
                const auto count = number(value);
                if (count < (flag == "--session-repetitions" ? 1 : 0) || count > 100 ||
                    std::floor(count) != count)
                    throw std::invalid_argument("Session counts must be integers in 0..100 (repetitions >=1)");
                if (flag == "--session-warmups") session_warmups = static_cast<std::size_t>(count);
                else session_repetitions = static_cast<std::size_t>(count);
            }
            else if (flag == "--neural-decoder") {
                if (value != "legacy" && value != "events" && value != "speed" && value != "quality")
                    throw std::invalid_argument("Neural decoder must be speed/legacy or quality/events");
                onnx.note_decoder = value == "events" || value == "quality" ?
                    transcribe::NeuralNoteDecoder::MultiHeadEvents :
                    transcribe::NeuralNoteDecoder::Legacy;
            }
            else if (flag == "--polyphonic-decoder") {
                if (value != "legacy" && value != "competing") {
                    throw std::invalid_argument("Polyphonic decoder must be legacy or competing");
                }
                analysis.spectral_decoder = value == "competing" ?
                    transcribe::SpectralDecoder::HarmonicCompetition :
                    transcribe::SpectralDecoder::LegacySalience;
                onnx.aligned_model_heads = value == "competing";
            }
            else if (flag == "--model-objective") {
                if (value == "independent-multi-pitch") {
                    onnx.objective = transcribe::PitchObjective::IndependentMultiPitch;
                } else if (value == "monophonic") {
                    onnx.objective = transcribe::PitchObjective::Monophonic;
                } else throw std::invalid_argument("Unknown model objective");
            }
            else if (flag == "--model-contract") {
                if (value == "basic-pitch-v1") model_contract = transcribe::ModelContract::BasicPitchV1;
                else if (value == "legacy-pitch-v1") model_contract = transcribe::ModelContract::LegacyPitchV1;
                else throw std::invalid_argument("Unsupported ONNX model contract: " + value);
            }
            else if (flag == "--audio-source-id") audio_source_id = value;
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
            else if (flag == "--activation-on") decoding.activation_on = number(value);
            else if (flag == "--activation-off") decoding.activation_off = number(value);
            else if (flag == "--maximum-gap") decoding.maximum_gap_seconds = number(value);
            else if (flag == "--minimum-duration") decoding.minimum_duration_seconds = number(value);
            else if (flag == "--energy-floor") decoding.energy_floor_rms = number(value);
            else if (flag == "--pitch-stability") decoding.pitch_stability_seconds = number(value);
            else if (flag == "--pitch-tolerance") decoding.pitch_tolerance_semitones = number(value);
            else if (flag == "--onset-threshold") decoding.onset_threshold = number(value);
            else if (flag == "--onset-spacing") decoding.minimum_onset_spacing_seconds = number(value);
            else if (flag == "--onset-energy-rise") decoding.onset_energy_rise = number(value);
            else if (flag == "--onset-state-drop") decoding.onset_state_drop = number(value);
            else if (flag == "--model-onsets") decoding.use_model_onsets = number(value) != 0;
            else if (flag == "--no-audio-refinement") {
                throw std::invalid_argument("Use --audio-refinement 0 instead");
            } else if (flag == "--audio-refinement") decoding.refine_with_audio = number(value) != 0;
            else throw std::invalid_argument("Unknown option: " + flag);
        }
        if (backend != "yin" && backend != "spectral" && backend != "onnx") {
            throw std::invalid_argument("Unknown backend: " + backend);
        }
        if (mode != "full" && mode != "streaming" && mode != "realtime") {
            throw std::invalid_argument("Unknown mode: " + mode);
        }
        if (temporal && mode == "realtime") throw std::invalid_argument("Temporal runner is offline only");
        if (onnx.note_decoder == transcribe::NeuralNoteDecoder::MultiHeadEvents &&
            (backend != "onnx" || temporal || mode != "full")) {
            throw std::invalid_argument("Neural event mode requires offline ONNX without a second temporal decoder");
        }
        if (model_interface && (mode != "full" || temporal)) {
            throw std::invalid_argument("Model-interface evaluation currently requires full mode without temporal decoding");
        }
        if (!model_interface && (session_warmups || session_repetitions != 1))
            throw std::invalid_argument("Persistent-session timing requires --model-interface");
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
        transcribe::DecodedTranscription decoded;
        std::optional<transcribe::ModelMetadata> model_metadata;
        transcribe::BackendDiagnostics model_diagnostics;
        transcribe::ModelResources model_resources;
        if (mode == "streaming") {
            streaming.analysis = analysis;
            streaming.audio_callback = [&](const transcribe::AudioBlockView& b) { rate = b.sample_rate; };
            if (!temporal) {
                streaming.notes_callback = [&](const auto& current) {
                    if (!current.empty() && first_update < 0) first_update = elapsed(total_start);
                };
            }
            if (temporal) {
                streaming.audio_callback = [&](const transcribe::AudioBlockView& b) {
                    rate = b.sample_rate;
                    duration = static_cast<double>(b.total_frames) / rate;
                };
                decoded = transcribe::transcribe_note_events_wav_streaming(
                    argv[1], streaming, decoding, audio_source_id);
            } else {
                const auto result = transcribe::transcribe_wav_streaming(argv[1], streaming);
                notes = result.notes;
                duration = result.duration_seconds;
            }
            if (rate == 0 && duration == 0) rate = transcribe::read_wav(argv[1]).sample_rate;
            analysis_seconds = elapsed(total_start); // Includes streaming decode.
        } else {
            const auto audio = transcribe::read_wav(argv[1]);
            decode_seconds = elapsed(total_start);
            rate = audio.sample_rate;
            duration = static_cast<double>(audio.samples.size()) / rate;
            const auto analysis_start = Clock::now();
            if (model_interface) {
                transcribe::ModelRegistry registry;
                const auto checked = [](const transcribe::ModelOperation& result) {
                    if (const auto* error = std::get_if<transcribe::InferenceError>(&result)) {
                        throw std::runtime_error(error->message);
                    }
                };
                const std::string id = backend == "onnx" ? "evaluation:onnx" : "builtin:" + backend;
                if (backend == "onnx") {
                    checked(registry.add_onnx({1, id, model_contract, model,
                        "Explicit evaluation model; provenance supplied by caller", "See model distribution license",
                        onnx.objective}));
                }
                auto created = registry.create(id);
                if (const auto* error = std::get_if<transcribe::InferenceError>(&created)) {
                    throw std::runtime_error(error->message);
                }
                auto instance = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
                checked(instance->load({onnx.execution_provider, onnx.device_id, analysis.fast_preview}));
                if (audio.samples.empty() && backend == "onnx") {
                    inference_skipped = true;
                    std::cerr << "Empty input: ONNX inference not invoked\n";
                } else {
                    transcribe::InferenceRequest request;
                    request.analysis = analysis;
                    request.onnx = onnx;
                    const auto warmup_start = Clock::now();
                    for (std::size_t i = 0; i < session_warmups; ++i) {
                        checked(instance->warm_up(request));
                    }
                    warmup_seconds = elapsed(warmup_start);
                    for (std::size_t i = 0; i < session_repetitions; ++i) {
                        const auto inference_start = Clock::now();
                        auto result = instance->infer(audio, request);
                        session_seconds.push_back(elapsed(inference_start));
                        if (const auto* error = std::get_if<transcribe::InferenceError>(&result)) {
                            throw std::runtime_error(error->message);
                        }
                        auto current = std::move(std::get<transcribe::InferenceOutput>(result).notes);
                        if (i && (current.size() != notes.size() ||
                            !std::equal(current.begin(), current.end(), notes.begin(), [](const auto& a, const auto& b) {
                                return a.midi_pitch == b.midi_pitch && a.start_seconds == b.start_seconds &&
                                    a.duration_seconds == b.duration_seconds && a.confidence == b.confidence;
                            }))) throw std::runtime_error("Persistent-session inference is nondeterministic");
                        notes = std::move(current);
                    }
                }
                model_metadata = instance->metadata();
                model_diagnostics = instance->diagnostics();
                model_resources = instance->resources();
            } else if (temporal && !audio.samples.empty()) {
                decoded = backend == "onnx" ?
                    transcribe::transcribe_note_events_with_onnx(audio, model, onnx, decoding,
                        audio_source_id, model_identifier.empty() ? model.filename().string() :
                                                                 model_identifier) :
                    transcribe::transcribe_note_events(audio, analysis, decoding, audio_source_id);
            } else if (mode == "realtime") {
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
        if (temporal) {
            for (const auto& event : decoded.events) {
                const int pitch = static_cast<int>(std::lround(event.midi_pitch));
                notes.push_back({pitch, event.onset_seconds,
                    event.offset_seconds - event.onset_seconds, transcribe::midi_note_name(pitch), 0.0});
            }
        }
        const double total_seconds = elapsed(total_start);
        const auto memory = peak_memory();
        std::ofstream output(argv[2], std::ios::binary);
        if (!output) throw std::runtime_error("Could not create evaluation output");
        output << std::setprecision(17)
               << "{\"sample_rate\":" << rate << ",\"duration_seconds\":" << duration
               << ",\"inference_skipped\":" << (inference_skipped ? "true" : "false")
               << ",\"peak_resident_bytes\":";
        if (memory.bytes) output << *memory.bytes;
        else output << "null";
        output << ",\"peak_resident_memory_source\":" << json_string(memory.source)
               << ",\"hardware_concurrency\":" << std::thread::hardware_concurrency()
               << ",\"decode_seconds\":";
        if (mode == "streaming") output << "null";
        else output << decode_seconds;
        output << ",\"analysis_seconds\":" << analysis_seconds
        << ",\"session_warmup_seconds\":" << warmup_seconds
        << ",\"session_inference_seconds\":[";
        for (std::size_t i = 0; i < session_seconds.size(); ++i) {
            if (i) output << ',';
            output << session_seconds[i];
        }
        output << ']'
               << ",\"canonical_event_count\":" << decoded.events.size()
               << ",\"raw_track_count\":" << decoded.raw_tracks.size()
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
        output << ",\"model_interface\":";
        if (!model_metadata) output << "null";
        else {
            const auto& m = *model_metadata;
            output << "{\"version\":" << m.definition.contract_version
                   << ",\"id\":" << json_string(m.definition.id)
                   << ",\"analysis_sample_rate\":" << m.analysis_sample_rate
                   << ",\"minimum_midi\":" << m.minimum_midi << ",\"maximum_midi\":" << m.maximum_midi
                   << ",\"window_samples\":" << m.window_samples << ",\"hop_samples\":" << m.hop_samples
                   << ",\"overlap_samples\":" << m.overlap_samples
                   << ",\"output_frame_step_samples\":" << m.output_frame_step_samples
                   << ",\"batch_size\":" << m.batch_size << ",\"incremental\":"
                   << (m.incremental ? "true" : "false")
                   << ",\"activation_semantics\":" << json_string(m.activation_semantics)
                   << ",\"independent_multi_pitch_objective\":"
                   << (m.objective == transcribe::PitchObjective::IndependentMultiPitch ? "true" : "false")
                   << ",\"trimmed_edge_frames\":" << m.trimmed_edge_frames
                   << ",\"output_origin_seconds\":" << m.output_origin_seconds
                   << ",\"temporal_alignment\":" << json_string(m.temporal_alignment)
                   << ",\"active_provider\":"
                   << json_string(transcribe::inference_provider_name(*model_diagnostics.active_provider))
                   << ",\"runtime_version\":" << json_string(model_diagnostics.runtime_version)
                   << ",\"load_seconds\":" << model_resources.load_seconds
                   << ",\"model_file_bytes\":" << model_resources.model_file_bytes
                   << ",\"inputs\":";
            write_tensors(output, m.inputs);
            output << ",\"outputs\":";
            write_tensors(output, m.outputs);
            output << ",\"heads\":[";
            for (std::size_t i = 0; i < m.heads.size(); ++i) {
                if (i) output << ',';
                const auto& head = m.heads[i];
                output << "{\"tensor_name\":" << json_string(head.tensor_name)
                       << ",\"sample_rate\":" << head.sample_rate
                       << ",\"frame_step_samples\":" << head.frame_step_samples
                       << ",\"origin_seconds\":" << head.origin_seconds
                       << ",\"minimum_midi\":" << head.minimum_midi
                       << ",\"bins\":" << head.bins
                       << ",\"bins_per_semitone\":" << head.bins_per_semitone
                       << ",\"available\":" << (head.available ? "true" : "false") << '}';
            }
            output << ']';
            output << '}';
        }
        output << ",\"canonical_events\":[";
        for (std::size_t i = 0; i < decoded.events.size(); ++i) {
            if (i) output << ',';
            const auto& e = decoded.events[i];
            output << "{\"id\":" << json_string(e.id) << ",\"midi_pitch\":" << e.midi_pitch
                   << ",\"onset_seconds\":" << e.onset_seconds
                   << ",\"offset_seconds\":" << e.offset_seconds
                   << ",\"onset_sample\":" << e.onset_sample
                   << ",\"offset_sample\":" << e.offset_sample
                   << ",\"source_sample_rate\":" << e.source_sample_rate
                   << ",\"source_model\":" << json_string(e.source_model)
                   << ",\"detection_method\":" << json_string(e.detection_method)
                   << ",\"evidence_kind\":" << json_string(e.evidence_kind)
                   << ",\"audio_source_id\":" << json_string(e.audio_source_id)
                   << ",\"state\":\""
                   << (e.state == transcribe::EventLifecycle::Refined ? "refined" : "stabilized")
                   << "\",\"calibrated_confidence\":null,\"velocity\":null"
                   << ",\"instrument\":null,\"voice\":null,\"articulation\":null,\"intensity_rms\":";
            if (e.intensity_rms) output << *e.intensity_rms;
            else output << "null";
            const auto track = std::find_if(decoded.raw_tracks.begin(), decoded.raw_tracks.end(),
                [&](const auto& t) {
                    return t.method == e.detection_method &&
                        (t.raw_midi_pitch < 0 || t.raw_midi_pitch == std::lround(e.midi_pitch));
                });
            if (track == decoded.raw_tracks.end()) {
                throw std::logic_error("Canonical event has no evidence track");
            }
            const auto diagnostics = transcribe::summarize_event_evidence(
                e, track->hop_seconds, track->window_seconds);
            output << ",\"diagnostics\":{\"activity_duration_seconds\":"
                   << diagnostics.activity_duration_seconds << ",\"pitch_stability\":";
            if (diagnostics.pitch_stability) output << *diagnostics.pitch_stability;
            else output << "null";
            output << ",\"onset_strength\":";
            if (diagnostics.onset_strength) output << *diagnostics.onset_strength;
            else output << "null";
            output << ",\"maximum_note_state\":";
            if (diagnostics.maximum_note_state) output << *diagnostics.maximum_note_state;
            else output << "null";
            output << '}';
            output << ",\"evidence\":[";
            for (std::size_t j = 0; j < e.evidence.size(); ++j) {
                if (j) output << ',';
                const auto& f = e.evidence[j];
                output << "{\"time_seconds\":" << f.time_seconds << ",\"midi\":" << f.midi_pitch
                       << ",\"raw_strength\":" << f.raw_strength
                       << ",\"raw_voicing\":" << f.raw_voicing
                       << ",\"fractional_midi\":";
                if (f.fractional_midi) output << *f.fractional_midi;
                else output << "null";
                output << ",\"raw_midi_pitch\":" << f.raw_midi_pitch << ",\"raw_model_value\":";
                if (f.raw_model_value) output << *f.raw_model_value;
                else output << "null";
                output << ",\"onset_strength\":";
                if (f.onset_strength) output << *f.onset_strength;
                else output << "null";
                output << ",\"note_state\":";
                if (f.note_state) output << *f.note_state;
                else output << "null";
                output << '}';
            }
            output << "]}";
        }
        output << "],\"notes\":[";
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto& n = notes[i];
            if (i) output << ',';
            output << "{\"midi\":" << n.midi_pitch << ",\"start\":" << n.start_seconds
                   << ",\"duration\":" << n.duration_seconds
                   << ",\"confidence\":";
            if (temporal) output << "null";
            else output << n.confidence;
            output << '}';
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
