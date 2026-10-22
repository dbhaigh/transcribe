// Replays a WAV file through LiveTranscriptionSession and writes machine-readable results.
#include "transcribe/audio.hpp"
#include "transcribe/inference.hpp"
#include "transcribe/live.hpp"
#include "../src/asio_transport.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace transcribe;
using namespace transcribe::live;
using Clock = std::chrono::steady_clock;

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

double number(const std::string& value) {
    double result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result)) {
        throw std::invalid_argument("Invalid numeric argument: " + value);
    }
    return result;
}

const char* kind_name(LiveEventKind kind) {
    switch (kind) {
    case LiveEventKind::NoteOn: return "note_on";
    case LiveEventKind::NoteStabilized: return "note_stabilized";
    case LiveEventKind::NoteOff: return "note_off";
    case LiveEventKind::NoteFinalized: return "note_finalized";
    case LiveEventKind::NoteCorrected: return "note_corrected";
    case LiveEventKind::NoteRetracted: return "note_retracted";
    case LiveEventKind::AnalysisGap: return "analysis_gap";
    case LiveEventKind::Status: return "status";
    }
    return "unknown";
}

const char* state_name(LiveState state) {
    switch (state) {
    case LiveState::Created: return "created";
    case LiveState::Running: return "running";
    case LiveState::InputInterrupted: return "input_interrupted";
    case LiveState::AnalysisFailed: return "analysis_failed";
    case LiveState::Finished: return "finished";
    case LiveState::Cancelled: return "cancelled";
    }
    return "unknown";
}

void write_latency(std::ostream& output, const LatencySummary& summary) {
    output << "{\"count\":" << summary.count;
    if (summary.count) {
        output << ",\"mean\":" << summary.mean_seconds << ",\"p50\":" << summary.p50_seconds
               << ",\"p95\":" << summary.p95_seconds << ",\"p99\":" << summary.p99_seconds
               << ",\"maximum\":" << summary.maximum_seconds;
    } else {
        output << ",\"mean\":null,\"p50\":null,\"p95\":null,\"p99\":null,\"maximum\":null";
    }
    output << '}';
}

void write_notes(std::ostream& output, const std::vector<Note>& notes) {
    output << '[';
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i) output << ',';
        output << "{\"midi\":" << notes[i].midi_pitch << ",\"start\":" << notes[i].start_seconds
               << ",\"duration\":" << notes[i].duration_seconds << '}';
    }
    output << ']';
}

std::vector<Note> proposal_notes(const ScoreProposal& proposal, double end_seconds) {
    std::vector<Note> notes;
    for (const auto& note : proposal.notes) {
        const double onset = note.onset_seconds();
        const double offset = note.offset_seconds().value_or(end_seconds);
        if (offset <= onset) continue;
        Note converted;
        converted.midi_pitch = note.midi_pitch;
        converted.start_seconds = onset;
        converted.duration_seconds = offset - onset;
        notes.push_back(converted);
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.start_seconds < b.start_seconds ||
               (a.start_seconds == b.start_seconds && a.midi_pitch < b.midi_pitch);
    });
    return notes;
}

std::unique_ptr<ModelBackend> create_model(const std::string& id, const std::filesystem::path& path) {
    ModelRegistry registry(path);
    auto created = registry.create(id);
    if (const auto* failure = std::get_if<InferenceError>(&created)) {
        throw std::runtime_error("Cannot create model " + id + ": " + failure->message);
    }
    return std::move(std::get<std::unique_ptr<ModelBackend>>(created));
}

std::string registry_id(const std::string& backend) {
    if (backend == "yin") return "builtin:yin";
    if (backend == "spectral") return "builtin:spectral";
    if (backend == "onnx") return "onnx:basic-pitch-v1";
    return backend;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            throw std::invalid_argument(
                "Usage: transcriber-live input.wav output.json [--backend yin|spectral|onnx]"
                " [--model path] [--pace realtime|none] [--policy lossless|skip]"
                " [--block-frames N] [--window-seconds S] [--hop-seconds S]"
                " [--refine-backend yin|spectral|onnx|none] [--refine-mode auto|full|windowed]"
                " [--callback-replay 0|1] [--analysis-delay-ms 0-1000]"
                " [--max-lag-seconds S]");
        }
        const std::filesystem::path input = argv[1];
        const std::filesystem::path output_path = argv[2];
        std::string backend = "yin";
        std::string refine_backend = "none";
        std::string refine_mode = "auto";
        std::string pace = "none";
        std::string policy = "lossless";
        std::filesystem::path model_path;
        std::size_t block_frames = 256;
        bool dump_events = false;
        bool callback_replay = false;
        double analysis_delay_ms = 0;
        LiveOptions options;
        for (int i = 3; i < argc; ++i) {
            const std::string flag = argv[i];
            if (++i >= argc) throw std::invalid_argument("Missing value for " + flag);
            const std::string value = argv[i];
            if (flag == "--backend") backend = value;
            else if (flag == "--model") model_path = value;
            else if (flag == "--pace") pace = value;
            else if (flag == "--policy") policy = value;
            else if (flag == "--refine-backend") refine_backend = value;
            else if (flag == "--refine-mode") refine_mode = value;
            else if (flag == "--dump-events") dump_events = number(value) != 0;
            else if (flag == "--callback-replay") callback_replay = number(value) != 0;
            else if (flag == "--analysis-delay-ms") {
                analysis_delay_ms = number(value);
                if (analysis_delay_ms < 0 || analysis_delay_ms > 1000) {
                    throw std::invalid_argument("Analysis delay must be 0-1000 ms");
                }
            }
            else if (flag == "--block-frames") {
                const double frames = number(value);
                if (frames < 1 || frames > 1 << 20 || frames != std::floor(frames)) {
                    throw std::invalid_argument("Invalid block frame count");
                }
                block_frames = static_cast<std::size_t>(frames);
            } else if (flag == "--window-seconds") options.window.window_seconds = number(value);
            else if (flag == "--hop-seconds") options.window.hop_seconds = number(value);
            else if (flag == "--target-ms") options.target_provisional_seconds = number(value) / 1000;
            else if (flag == "--stabilized-target-ms") options.target_stabilized_seconds = number(value) / 1000;
            else if (flag == "--neural-decoder") {
                if (value != "speed" && value != "quality")
                    throw std::invalid_argument("Neural decoder must be speed or quality");
                options.window.request.onnx.note_decoder = value == "quality" ?
                    NeuralNoteDecoder::MultiHeadEvents : NeuralNoteDecoder::Legacy;
                options.window.temporal_decoding = value != "quality";
            }
            else if (flag == "--max-lag-seconds") options.max_analysis_lag_seconds = number(value);
            else throw std::invalid_argument("Unknown option: " + flag);
        }
        const auto valid_backend = [](const std::string& b) {
            return b == "yin" || b == "spectral" || b == "onnx";
        };
        if (!valid_backend(backend)) throw std::invalid_argument("Unknown backend: " + backend);
        if (refine_backend != "none" && !valid_backend(refine_backend)) {
            throw std::invalid_argument("Unknown refinement backend: " + refine_backend);
        }
        if (refine_mode != "auto" && refine_mode != "full" && refine_mode != "windowed") {
            throw std::invalid_argument("Unknown refinement mode: " + refine_mode);
        }
        // Basic Pitch measured better on its native short windows than on whole recordings.
        if (refine_mode == "auto") refine_mode = refine_backend == "onnx" ? "windowed" : "full";
        if (pace != "realtime" && pace != "none") throw std::invalid_argument("Unknown pace: " + pace);
        if (policy != "lossless" && policy != "skip") throw std::invalid_argument("Unknown policy: " + policy);
        if (callback_replay && policy != "skip") {
            throw std::invalid_argument("Callback replay requires --policy skip (no producer blocking/retry)");
        }
        if ((backend == "onnx" || refine_backend == "onnx") && model_path.empty()) {
            throw std::invalid_argument("The onnx backend requires --model");
        }
        if (options.window.request.onnx.note_decoder == NeuralNoteDecoder::MultiHeadEvents && backend != "onnx")
            throw std::invalid_argument("Neural quality mode requires the ONNX backend");

        const auto info = inspect_wav(input);
        const AudioBuffer audio{info.sample_rate, {}};
        if (audio.sample_rate == 0) throw std::runtime_error("Input has no sample rate");
        const double duration = static_cast<double>(info.frames) / audio.sample_rate;
        block_frames = std::min(block_frames, options.max_block_frames);
        options.format = {audio.sample_rate, 1};
        options.lag_policy = policy == "lossless" ? LagPolicy::Lossless : LagPolicy::SkipAhead;
        options.audio_source_id = "file:" + input.filename().string();
        options.max_recording_seconds = refine_backend == "none" ? 0 : options.max_recording_seconds;
        if (refine_backend != "none" && duration > options.max_recording_seconds)
            throw std::invalid_argument("Refinement exceeds bounded recording retention");
        options.window.request.onnx.polyphonic = true;
        options.window.request.onnx.independent_probabilities = true;
        if (analysis_delay_ms) options.analysis_hook = [analysis_delay_ms](std::uint64_t) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(analysis_delay_ms));
        };
        std::unique_ptr<ModelBackend> model;
        if (backend != "yin") {
            options.backend = LiveBackendKind::OverlappingWindow;
            model = create_model(registry_id(backend), model_path);
        }

        LiveTranscriptionSession session(options, std::move(model));
        const auto started = session.start();
        if (const auto* failure = std::get_if<InferenceError>(&started)) {
            throw std::runtime_error("Live session failed to start: " + failure->message);
        }
        session.begin_input(0);
        TranscriptionDocument document(audio.sample_rate);
        transcribe::detail::LiveCaptureTransport transport;
        transport.session = &session;
        transport.format = options.format;
        transport.capacity = info.frames;
        std::vector<double> callback_times;
        callback_times.reserve((info.frames + block_frames - 1) / block_frames);
        std::map<std::string, std::size_t> kinds;
        std::vector<LiveEvent> log;
        std::uint64_t retries = 0, dropped_blocks = 0, resyncs = 0;
        double first_event_wall = -1;
        const auto consume = [&](const Clock::time_point start) {
            auto batch = session.poll_events();
            if (batch.events.empty() && !batch.resync_required) return;
            if (first_event_wall < 0) {
                for (const auto& event : batch.events) {
                    if (event.kind == LiveEventKind::NoteOn) {
                        first_event_wall = std::chrono::duration<double>(Clock::now() - start).count();
                        break;
                    }
                }
            }
            for (const auto& event : batch.events) ++kinds[kind_name(event.kind)];
            if (dump_events) log.insert(log.end(), batch.events.begin(), batch.events.end());
            document.apply(batch);
            if (document.requires_resync()) {
                ++resyncs;
                document.resync(batch.resync_snapshot);
            }
        };

        const auto wall_start = Clock::now();
        std::uint64_t sequence = 0;
        read_wav_blocks(input, block_frames, [&](const AudioBlockView& source) {
            const auto offset = source.first_frame;
            const auto count = source.samples.size();
            if (pace == "realtime") {
                // Deliver each block when its last frame would have been captured.
                const auto due = wall_start + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(offset + count) / audio.sample_rate));
                std::this_thread::sleep_until(due);
            }
            const AudioFrameBlock block{options.format, offset,
                source.samples,
                pace == "realtime" ? std::chrono::duration_cast<Nanoseconds>(
                    Clock::now().time_since_epoch()) : Nanoseconds{}, false, sequence++};
            PushStatus status;
            if (callback_replay) {
                const auto before = Clock::now();
                const auto refused = transport.refused.load();
                transport.receive(block.interleaved.data(), count, false, block.capture_time);
                callback_times.push_back(std::chrono::duration<double>(Clock::now() - before).count());
                status = transport.rejected.load() ? PushStatus::Closed :
                    transport.refused.load() != refused ? PushStatus::QueueFull : PushStatus::Accepted;
            } else status = session.push(block);
            while (status == PushStatus::QueueFull && policy == "lossless") {
                ++retries;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                status = session.push(block);
            }
            if (status == PushStatus::QueueFull) ++dropped_blocks;
            else if (status != PushStatus::Accepted) {
                throw std::runtime_error("Live session rejected audio block");
            }
            consume(wall_start);
        });
        session.end_input(info.frames);
        session.finish();
        consume(wall_start);
        const double live_wall = std::chrono::duration<double>(Clock::now() - wall_start).count();
        const auto metrics = session.metrics();
        const auto live_notes = document.to_notes(duration);

        std::vector<Note> refined;
        double refine_seconds = 0;
        std::size_t proposed_changes = 0, conflicts = 0, applied = 0;
        std::string refine_error;
        if (refine_backend != "none") {
            RefinementOptions refinement;
            refinement.backend_id = registry_id(refine_backend);
            refinement.model_path = model_path;
            refinement.request.onnx.polyphonic = true;
            refinement.request.onnx.independent_probabilities = true;
            refinement.audio_source_id = options.audio_source_id;
            if (refine_mode == "windowed") refinement.windowed = options.window;
            const auto refine_start = Clock::now();
            auto proposal = refine_recording(session.recording(), refinement);
            refine_seconds = std::chrono::duration<double>(Clock::now() - refine_start).count();
            if (const auto* failure = std::get_if<InferenceError>(&proposal)) {
                refine_error = failure->message;
            } else {
                const auto& value = std::get<ScoreProposal>(proposal);
                refined = proposal_notes(value, duration);
                const auto review = document.review(value);
                proposed_changes = review.changes.size();
                conflicts = review.conflicts;
                applied = document.apply(review).applied;
            }
        }

        std::ofstream output(output_path, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot write " + output_path.string());
        output << std::setprecision(9);
        output << "{\"schema_version\":1,\"input\":" << json_string(input.filename().string())
               << ",\"sample_rate\":" << audio.sample_rate << ",\"duration_seconds\":" << duration
               << ",\"backend\":" << json_string(backend) << ",\"live_mode\":"
               << json_string(backend == "yin" ? "incremental_yin" : "overlapping_window")
               << ",\"pace\":" << json_string(pace) << ",\"policy\":" << json_string(policy)
               << ",\"block_frames\":" << block_frames
               << ",\"callback_replay\":" << (callback_replay ? "true" : "false")
               << ",\"injected_analysis_delay_ms\":" << analysis_delay_ms
               << ",\"window_seconds\":" << options.window.window_seconds
               << ",\"hop_seconds\":" << options.window.hop_seconds
               << ",\"neural_decoder\":" << json_string(
                    options.window.request.onnx.note_decoder == NeuralNoteDecoder::MultiHeadEvents ? "quality" : "speed")
               << ",\"max_analysis_lag_seconds\":" << options.max_analysis_lag_seconds
               << ",\"input_queue_blocks\":" << options.input_queue_blocks
               << ",\"state\":" << json_string(state_name(session.state()))
               << ",\"error\":" << json_string(session.last_error())
               << ",\"live_wall_seconds\":" << live_wall
               << ",\"first_note_on_wall_seconds\":";
        if (first_event_wall < 0) output << "null";
        else output << first_event_wall;
        output << ",\"push_retries\":" << retries << ",\"dropped_blocks\":" << dropped_blocks
               << ",\"document_resyncs\":" << resyncs << ",\"event_counts\":{";
        bool first = true;
        for (const auto& [name, count] : kinds) {
            if (!first) output << ',';
            first = false;
            output << json_string(name) << ':' << count;
        }
        output << "},\"metrics\":{\"capture_latency\":";
        write_latency(output, metrics.capture_latency);
        output << ",\"analysis_latency\":";
        write_latency(output, metrics.analysis_latency);
        output << ",\"end_to_end_latency\":";
        write_latency(output, metrics.end_to_end_latency);
        output << ",\"stabilization_delay\":";
        write_latency(output, metrics.stabilization_delay);
        output << ",\"algorithmic_latency\":";
        write_latency(output, metrics.algorithmic_latency);
        output << ",\"analysis_queue_latency\":";
        write_latency(output, metrics.analysis_queue_latency);
        output << ",\"confirmation_latency\":";
        write_latency(output, metrics.confirmation_latency);
        output << ",\"publication_queue_latency\":";
        write_latency(output, metrics.publication_queue_latency);
        output << ",\"overloaded\":" << (metrics.overloaded ? "true" : "false");
        const auto budget = latency_budget(options, block_frames);
        output << ",\"latency_budget\":{\"capture_block_seconds\":" << budget.capture_block_seconds
               << ",\"ingest_poll_seconds\":" << budget.ingest_poll_seconds
               << ",\"analysis_context_seconds\":" << budget.analysis_context_seconds
               << ",\"update_wait_seconds\":" << budget.update_wait_seconds
               << ",\"stabilization_seconds\":" << budget.stabilization_seconds
               << ",\"provisional_estimate_seconds\":" << budget.provisional_estimate_seconds
               << ",\"stabilized_estimate_seconds\":" << budget.stabilized_estimate_seconds << '}'
               << ",\"provisional_target_seconds\":" << options.target_provisional_seconds
               << ",\"stabilized_target_seconds\":" << options.target_stabilized_seconds
               << ",\"provisional_target_misses\":" << metrics.provisional_target_misses
               << ",\"stabilized_target_misses\":" << metrics.stabilized_target_misses
               << ",\"input_queue_depth\":" << metrics.input_queue_depth
               << ",\"event_queue_depth\":" << metrics.event_queue_depth
               << ",\"processing_lag_frames\":" << metrics.current_lag_frames;
        output << ",\"callback_execution\":";
        LatencySummary callback_summary;
        if (!callback_times.empty()) {
            std::sort(callback_times.begin(), callback_times.end());
            callback_summary.count = callback_times.size();
            for (const auto value : callback_times) callback_summary.mean_seconds += value;
            callback_summary.mean_seconds /= callback_times.size();
            callback_summary.maximum_seconds = callback_times.back();
            callback_summary.p50_seconds = callback_times[(callback_times.size() - 1) / 2];
            callback_summary.p95_seconds = callback_times[
                static_cast<std::size_t>(std::ceil(callback_times.size() * .95)) - 1];
            callback_summary.p99_seconds = callback_times[
                static_cast<std::size_t>(std::ceil(callback_times.size() * .99)) - 1];
        }
        write_latency(output, callback_summary);
        output << ",\"frames_received\":" << metrics.frames_received
               << ",\"blocks_refused\":" << metrics.blocks_refused
               << ",\"gap_frames\":" << metrics.gap_frames
               << ",\"frames_refused\":" << metrics.frames_refused
               << ",\"sequence_gaps\":" << metrics.sequence_gaps
               << ",\"input_queue_capacity\":" << metrics.input_queue_capacity
               << ",\"input_queue_high_water\":" << metrics.input_queue_high_water
               << ",\"event_queue_capacity\":" << metrics.event_queue_capacity
               << ",\"event_queue_high_water\":" << metrics.event_queue_high_water
               << ",\"events_emitted\":" << metrics.events_emitted
               << ",\"events_dropped\":" << metrics.events_dropped
               << ",\"analysis_steps\":" << metrics.analysis_steps
               << ",\"windows_coalesced\":" << metrics.windows_coalesced
               << ",\"skipped_frames\":" << metrics.skipped_frames
               << ",\"maximum_lag_frames\":" << metrics.maximum_lag_frames
               << ",\"rolling_capacity_frames\":" << metrics.rolling_capacity_frames
               << ",\"tracked_notes_high_water\":" << metrics.tracked_notes_high_water
               << ",\"recording_frames\":" << metrics.recording_frames
               << ",\"model_errors\":" << metrics.model_errors
               << ",\"model_recoveries\":" << metrics.model_recoveries
               << ",\"analysed_audio_seconds\":" << metrics.analysed_audio_seconds
               << ",\"analysis_busy_seconds\":" << metrics.analysis_busy_seconds << '}'
               << ",\"live_notes\":";
        write_notes(output, live_notes);
        if (dump_events) {
            output << ",\"events\":[";
            for (std::size_t i = 0; i < log.size(); ++i) {
                const auto& e = log[i];
                if (i) output << ',';
                output << "{\"sequence\":" << e.sequence << ",\"kind\":" << json_string(kind_name(e.kind))
                       << ",\"id\":" << json_string(e.note.id) << ",\"midi\":" << e.note.midi_pitch
                       << ",\"onset_seconds\":" << e.note.onset_seconds() << ",\"offset_seconds\":";
                if (const auto offset = e.note.offset_seconds()) output << *offset;
                else output << "null";
                output << ",\"revision\":" << e.note.revision
                       << ",\"evidence_end_seconds\":"
                       << static_cast<double>(e.timing.evidence_end_frame) / audio.sample_rate
                       << ",\"message\":" << json_string(e.message) << '}';
            }
            output << ']';
        }
        output << ",\"refinement\":";
        if (refine_backend == "none") output << "null";
        else {
            output << "{\"backend\":" << json_string(refine_backend)
                   << ",\"mode\":" << json_string(refine_mode)
                   << ",\"seconds\":" << refine_seconds << ",\"error\":";
            if (refine_error.empty()) output << "null";
            else output << json_string(refine_error);
            output << ",\"proposed_changes\":" << proposed_changes << ",\"conflicts\":" << conflicts
                   << ",\"applied\":" << applied << ",\"notes\":";
            write_notes(output, refined);
            output << '}';
        }
        output << "}\n";
        output.close();
        if (!output) throw std::runtime_error("Failed writing live output");
        return session.state() == LiveState::Finished && refine_error.empty() ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "transcriber-live: " << error.what() << '\n';
        return 1;
    }
}
