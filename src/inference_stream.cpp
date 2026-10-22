#include "transcribe/inference.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace transcribe {

InferenceResult<FileInferenceResult> infer_wav_file(
    ModelBackend& backend, const std::filesystem::path& path,
    const InferenceRequest& request, const FileInferenceOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    try {
        const auto contract = backend.metadata().definition.contract;
        if (contract != ModelContract::Yin && contract != ModelContract::Spectral &&
            contract != ModelContract::BasicPitchV1) {
            return InferenceError{InferenceErrorCode::Unsupported,
                "Bounded file inference supports built-in trackers and BasicPitchV1 only; "
                "LegacyPitchV1 requires a complete waveform"};
        }
        if (!backend.resources().loaded) {
            return InferenceError{InferenceErrorCode::NotLoaded, "Load the backend before file inference"};
        }
        if (!std::isfinite(options.chunk_seconds) || options.chunk_seconds < 2 ||
            options.chunk_seconds > 30 || !std::isfinite(options.overlap_seconds) ||
            options.overlap_seconds < .5 || options.overlap_seconds >= options.chunk_seconds ||
            options.maximum_audio_bytes == 0 || options.maximum_pending_notes == 0 ||
            options.maximum_retained_notes == 0 || request.retain_evidence) {
            throw std::invalid_argument(
                "File inference requires 2-30 s chunks, .5 s <= overlap < chunk, positive memory/note "
                "limits and retain_evidence=false (use absolute-time evidence callbacks)");
        }
        FileInferenceResult result;
        AudioBuffer chunk;
        std::size_t capacity = 0, overlap = 0, stride = 0, origin = 0, read = 0, total = 0;
        bool finished = false;
        std::vector<Note> pending;
        double progress = 0;
        const auto check = [&] {
            if (request.cancellation.stop_requested()) throw std::runtime_error("File inference cancelled");
        };
        const auto publish = [&](double before) {
            std::vector<Note> ready;
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->start_seconds + it->duration_seconds < before) {
                    ready.push_back(std::move(*it));
                    it = pending.erase(it);
                } else ++it;
            }
            std::sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) {
                return a.start_seconds != b.start_seconds ? a.start_seconds < b.start_seconds :
                    a.midi_pitch < b.midi_pitch;
            });
            if (options.retain_notes) {
                if (ready.size() > options.maximum_retained_notes - result.notes.size()) {
                    throw std::length_error("File inference retained-note limit exceeded; use callback-only output");
                }
                result.notes.insert(result.notes.end(), ready.begin(), ready.end());
            }
            if (!ready.empty() && options.notes_callback) options.notes_callback(ready);
            check();
        };
        const auto process = [&](bool last) {
            check();
            const double offset = static_cast<double>(origin) / result.sample_rate;
            const double length = static_cast<double>(chunk.samples.size()) / result.sample_rate;
            const double left = origin ? static_cast<double>(overlap) / (2 * result.sample_rate) : 0;
            const double right = last ? length : length -
                static_cast<double>(overlap) / (2 * result.sample_rate);
            auto current = request;
            current.progress = [&](double value) {
                check();
                const double position = (origin + (left + (right - left) * value) *
                    result.sample_rate) / total;
                progress = std::max(progress, std::clamp(position, 0.0, 1.0));
                if (request.progress) request.progress(progress);
            };
            const auto wrap = [&](const auto& callback) ->
                std::function<void(const FrameEvidenceTrack&)> {
                if (!callback) return {};
                return [callback, offset](const auto& track) {
                    auto absolute = track;
                    for (auto& frame : absolute.frames) frame.time_seconds += offset;
                    callback(absolute);
                };
            };
            current.analysis.evidence_callback = wrap(request.analysis.evidence_callback);
            current.onnx.evidence_callback = wrap(request.onnx.evidence_callback);
            auto inferred = backend.infer(chunk, current);
            if (const auto* error = std::get_if<InferenceError>(&inferred)) {
                throw *error;
            }
            ++result.processed_windows;
            if (result.processed_windows == 1) result.first_window_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            for (auto note : std::get<InferenceOutput>(inferred).notes) {
                const double local = note.start_seconds;
                if (local >= right && !last) continue;
                note.start_seconds += offset;
                const auto prior = std::find_if(pending.rbegin(), pending.rend(), [&](const auto& p) {
                    return p.midi_pitch == note.midi_pitch &&
                        p.start_seconds < note.start_seconds + note.duration_seconds &&
                        p.start_seconds + p.duration_seconds > note.start_seconds;
                });
                if (prior != pending.rend() && local < left) {
                    prior->duration_seconds = std::max(prior->start_seconds + prior->duration_seconds,
                        note.start_seconds + note.duration_seconds) - prior->start_seconds;
                } else if (local >= left) {
                    if (pending.size() >= options.maximum_pending_notes) {
                        throw std::length_error("File inference pending-note limit exceeded");
                    }
                    pending.push_back(std::move(note));
                }
            }
            publish(last ? std::numeric_limits<double>::infinity() :
                offset + static_cast<double>(stride) / result.sample_rate);
        };
        check();
        read_wav_blocks(path, 4096, [&](const AudioBlockView& block) {
            check();
            if (!result.sample_rate) {
                result.sample_rate = block.sample_rate;
                total = block.total_frames;
                const double samples = options.chunk_seconds * block.sample_rate;
                if (samples > static_cast<double>(options.maximum_audio_bytes / sizeof(float))) {
                    throw std::length_error("File inference audio buffer exceeds maximum_audio_bytes");
                }
                capacity = static_cast<std::size_t>(std::llround(samples));
                overlap = static_cast<std::size_t>(std::llround(options.overlap_seconds * block.sample_rate));
                if (capacity <= overlap) throw std::invalid_argument("Rounded file windows have no stride");
                stride = capacity - overlap;
                chunk.sample_rate = block.sample_rate;
                chunk.samples.reserve(capacity);
            }
            if (block.first_frame != read || block.sample_rate != result.sample_rate ||
                block.total_frames != total) throw std::runtime_error("Discontinuous WAV file stream");
            if (options.audio_callback) options.audio_callback(block);
            read += block.samples.size();
            std::size_t consumed = 0;
            while (consumed < block.samples.size()) {
                check();
                const auto count = std::min(capacity - chunk.samples.size(),
                    block.samples.size() - consumed);
                chunk.samples.insert(chunk.samples.end(), block.samples.begin() + consumed,
                    block.samples.begin() + consumed + count);
                consumed += count;
                result.peak_buffered_audio_bytes = std::max(result.peak_buffered_audio_bytes,
                    chunk.samples.size() * sizeof(float));
                if (chunk.samples.size() == capacity) {
                    const bool last = origin + chunk.samples.size() == total;
                    process(last);
                    if (last) { finished = true; break; }
                    chunk.samples.erase(chunk.samples.begin(), chunk.samples.begin() + stride);
                    origin += stride;
                }
            }
        });
        if (read != total) throw std::runtime_error("Incomplete WAV input");
        if (!finished && !chunk.samples.empty()) process(true);
        check();
        if (request.progress) request.progress(1);
        check();
        result.source_frames = read;
        std::sort(result.notes.begin(), result.notes.end(), [](const auto& a, const auto& b) {
            return a.start_seconds != b.start_seconds ? a.start_seconds < b.start_seconds :
                a.midi_pitch < b.midi_pitch;
        });
        return result;
    } catch (const InferenceError& error) {
        return error;
    } catch (const std::invalid_argument& error) {
        return InferenceError{InferenceErrorCode::InvalidArgument, error.what()};
    } catch (const std::exception& error) {
        return InferenceError{request.cancellation.stop_requested() ? InferenceErrorCode::Cancelled :
            InferenceErrorCode::Inference, error.what()};
    }
}
} // namespace transcribe
