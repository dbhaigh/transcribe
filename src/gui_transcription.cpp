#include "gui_transcription.hpp"

#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace transcribe::gui {
namespace {

void report_progress(const ProgressCallback& callback, double progress,
                    const char* status) {
    if (callback) {
        callback(std::clamp(progress, 0.0, 1.0), status);
    }
}

std::uint64_t mix_hash(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

class ReviewEvidenceCollector {
public:
    void add(const FrameEvidenceTrack& track) {
        if ((!track.raw_values.empty() &&
             track.raw_values.size() != track.frames.size()) ||
            (!track.spectral_peak_fractions.empty() &&
             track.spectral_peak_fractions.size() != track.frames.size())) {
            throw std::invalid_argument(
                "Review evidence arrays must align with detector frames.");
        }
        auto found = std::find_if(tracks_.begin(), tracks_.end(),
            [&](const auto& entry) {
                return entry.track.method == track.method &&
                    entry.track.evidence_kind == track.evidence_kind &&
                entry.track.window_seconds == track.window_seconds &&
                entry.track.hop_seconds == track.hop_seconds &&
                entry.track.raw_midi_pitch == track.raw_midi_pitch &&
                    entry.track.selected_strength_semantics ==
                        track.selected_strength_semantics &&
                    entry.track.raw_value_semantics == track.raw_value_semantics &&
                    entry.has_raw_values == !track.raw_values.empty() &&
                    entry.has_spectral_peak_fractions ==
                        !track.spectral_peak_fractions.empty();
            });
        if (found == tracks_.end()) {
            Entry entry;
            entry.track.window_seconds = track.window_seconds;
            entry.track.hop_seconds = track.hop_seconds;
            entry.track.method = track.method;
            entry.track.evidence_kind = track.evidence_kind;
            entry.track.raw_midi_pitch = track.raw_midi_pitch;
            entry.track.selected_strength_semantics =
                track.selected_strength_semantics;
            entry.track.raw_value_semantics = track.raw_value_semantics;
            entry.has_raw_values = !track.raw_values.empty();
            entry.has_spectral_peak_fractions =
                !track.spectral_peak_fractions.empty();
            const auto identity = track.method + ":" + track.evidence_kind +
                ":" + std::to_string(track.window_seconds) + ":" +
                std::to_string(track.hop_seconds) +
                ":" + std::to_string(track.raw_midi_pitch) + ":" +
                std::to_string(static_cast<int>(
                    track.selected_strength_semantics)) + ":" +
                std::to_string(static_cast<int>(track.raw_value_semantics)) + ":" +
                std::to_string(entry.has_raw_values) + ":" +
                std::to_string(entry.has_spectral_peak_fractions);
            entry.hash_seed = 14695981039346656037ULL;
            for (const unsigned char value : identity) {
                entry.hash_seed = (entry.hash_seed ^ value) *
                    1099511628211ULL;
            }
            tracks_.push_back(std::move(entry));
            found = std::prev(tracks_.end());
        }
        for (std::size_t index = 0; index < track.frames.size(); ++index) {
            const std::size_t seen = found->seen++;
            std::size_t slot = seen;
            if (seen >= maximum_samples_per_track) {
                truncated_ = true;
                slot = static_cast<std::size_t>(
                    mix_hash(found->hash_seed + seen) % (seen + 1));
                if (slot >= maximum_samples_per_track) continue;
            }
            if (slot == found->track.frames.size()) {
                found->track.frames.push_back(track.frames[index]);
                if (!track.raw_values.empty()) {
                    found->track.raw_values.push_back(track.raw_values[index]);
                }
                if (!track.spectral_peak_fractions.empty()) {
                    found->track.spectral_peak_fractions.push_back(
                        track.spectral_peak_fractions[index]);
                }
            } else {
                found->track.frames[slot] = track.frames[index];
                if (!track.raw_values.empty()) {
                    if (found->track.raw_values.empty()) {
                        found->track.raw_values.resize(found->track.frames.size());
                    }
                    found->track.raw_values[slot] = track.raw_values[index];
                }
                if (!track.spectral_peak_fractions.empty()) {
                    if (found->track.spectral_peak_fractions.empty()) {
                        found->track.spectral_peak_fractions.resize(
                            found->track.frames.size());
                    }
                    found->track.spectral_peak_fractions[slot] =
                        track.spectral_peak_fractions[index];
                }
            }
        }
    }

    std::vector<FrameEvidenceTrack> finish() {
        std::vector<FrameEvidenceTrack> result;
        result.reserve(tracks_.size());
        for (auto& entry : tracks_) {
            std::vector<std::size_t> order(entry.track.frames.size());
            for (std::size_t index = 0; index < order.size(); ++index) {
                order[index] = index;
            }
            std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
                return entry.track.frames[left].time_seconds <
                       entry.track.frames[right].time_seconds;
            });
            FrameEvidenceTrack ordered = entry.track;
            ordered.frames.clear();
            ordered.raw_values.clear();
            ordered.spectral_peak_fractions.clear();
            ordered.frames.reserve(order.size());
            if (!entry.track.raw_values.empty()) {
                ordered.raw_values.reserve(order.size());
            }
            if (!entry.track.spectral_peak_fractions.empty()) {
                ordered.spectral_peak_fractions.reserve(order.size());
            }
            for (const auto index : order) {
                ordered.frames.push_back(entry.track.frames[index]);
                if (!entry.track.raw_values.empty()) {
                    ordered.raw_values.push_back(entry.track.raw_values[index]);
                }
                if (!entry.track.spectral_peak_fractions.empty()) {
                    ordered.spectral_peak_fractions.push_back(
                        entry.track.spectral_peak_fractions[index]);
                }
            }
            result.push_back(std::move(ordered));
        }
        return result;
    }

    bool truncated() const noexcept { return truncated_; }

private:
    struct Entry {
        FrameEvidenceTrack track;
        std::size_t seen{};
        std::uint64_t hash_seed{};
        bool has_raw_values{};
        bool has_spectral_peak_fractions{};
    };
    static constexpr std::size_t maximum_samples_per_track = 2048;
    std::vector<Entry> tracks_;
    bool truncated_{};
};

void attach_review_evidence(ScoreDocument& score,
                            std::span<const FrameEvidenceTrack> tracks,
                            std::string_view model_identifier,
                            bool& truncated) {
    constexpr std::size_t maximum_samples_per_note = 64;
    constexpr std::size_t maximum_alternatives_per_note = 8;
    constexpr std::size_t maximum_total_samples = 100000;
    std::size_t retained_samples = 0;
    for (auto& source : score.source_events) {
        const int target_pitch = static_cast<int>(std::lround(source.midi_pitch));
        std::vector<EventEvidence> selected;
        std::map<int, EventEvidence> alternatives;
        const FrameEvidenceTrack* primary_track = nullptr;
        const FrameEvidenceTrack* contributing_track = nullptr;
        const double first_time = std::max(0.0, source.onset_seconds - 0.08);
        const double last_time = source.offset_seconds + 0.08;
        for (const auto& track : tracks) {
            for (std::size_t index = 0; index < track.frames.size(); ++index) {
                const auto& frame = track.frames[index];
                if (frame.time_seconds < first_time || frame.time_seconds > last_time) {
                    continue;
                }
                if (contributing_track == nullptr) contributing_track = &track;
                const int pitch = track.raw_midi_pitch >= 0
                    ? track.raw_midi_pitch : frame.midi_pitch;
                if (pitch < 0 || pitch > 127) continue;
                EventEvidence evidence;
                evidence.time_seconds = frame.time_seconds;
                evidence.midi_pitch = pitch;
                evidence.raw_strength = std::max(0.0, frame.confidence);
                evidence.raw_voicing = std::clamp(frame.voicing_probability, 0.0, 1.0);
                evidence.raw_midi_pitch = pitch;
                evidence.strength_semantics = track.selected_strength_semantics;
                evidence.raw_value_semantics = track.raw_value_semantics;
                if (!track.raw_values.empty()) {
                    evidence.raw_model_value = track.raw_values[index];
                }
                if (!track.spectral_peak_fractions.empty()) {
                    evidence.spectral_peak_fraction =
                        track.spectral_peak_fractions[index];
                }
                if (pitch == target_pitch) {
                    if (primary_track == nullptr) primary_track = &track;
                    selected.push_back(std::move(evidence));
                } else if (evidence.raw_model_value) {
                    const auto candidate = alternatives.find(pitch);
                    if (candidate == alternatives.end() ||
                        *candidate->second.raw_model_value <
                            *evidence.raw_model_value) {
                        alternatives[pitch] = std::move(evidence);
                    }
                }
            }
        }
        std::stable_sort(selected.begin(), selected.end(),
            [](const auto& left, const auto& right) {
                return left.time_seconds < right.time_seconds;
            });
        selected.erase(std::unique(selected.begin(), selected.end(),
            [](const auto& left, const auto& right) {
                return left.midi_pitch == right.midi_pitch &&
                    std::abs(left.time_seconds - right.time_seconds) < 1e-6;
            }), selected.end());
        if (selected.size() > maximum_samples_per_note) {
            std::vector<EventEvidence> sampled;
            sampled.reserve(maximum_samples_per_note);
            for (std::size_t sample = 0; sample < maximum_samples_per_note; ++sample) {
                const auto index = sample * (selected.size() - 1) /
                                   (maximum_samples_per_note - 1);
                sampled.push_back(std::move(selected[index]));
            }
            selected = std::move(sampled);
            truncated = true;
        }
        std::vector<EventEvidence> candidates;
        candidates.reserve(alternatives.size());
        for (auto& [pitch, evidence] : alternatives) {
            (void)pitch;
            candidates.push_back(std::move(evidence));
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const auto& left, const auto& right) {
                return left.raw_model_value.value_or(
                           -std::numeric_limits<double>::infinity()) >
                       right.raw_model_value.value_or(
                           -std::numeric_limits<double>::infinity());
            });
        if (candidates.size() > maximum_alternatives_per_note) {
            candidates.resize(maximum_alternatives_per_note);
            truncated = true;
        }
        if (retained_samples + selected.size() + candidates.size() >
            maximum_total_samples) {
            truncated = true;
            continue;
        }
        source.evidence = std::move(selected);
        source.evidence.insert(source.evidence.end(),
                               candidates.begin(), candidates.end());
        if (!source.evidence.empty()) {
            source.source_model = std::string(model_identifier);
        const auto* provenance_track = primary_track != nullptr
            ? primary_track : contributing_track;
        if (provenance_track != nullptr) {
            source.detection_method = provenance_track->method;
            source.evidence_kind = provenance_track->evidence_kind;
        }
        }
        retained_samples += source.evidence.size();
    }
    for (auto& note : score.notes) {
        note.raw_evidence.clear();
        for (const auto& source_id : note.source_event_ids) {
            const auto event = std::find_if(score.source_events.begin(),
                score.source_events.end(), [&](const auto& candidate) {
                    return candidate.id == source_id;
                });
            if (event != score.source_events.end()) {
                note.raw_evidence.insert(note.raw_evidence.end(),
                    event->evidence.begin(), event->evidence.end());
            }
        }
    }
}

} // namespace

Result run_transcription(const Request& request, std::stop_token stop_token,
                         const ProgressCallback& progress_callback,
                         const LivePreviewCallback& live_preview_callback,
                         const AudioPreviewCallback& audio_preview_callback) {
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
    ReviewEvidenceCollector review_evidence;
    const auto retain_evidence = [&](const FrameEvidenceTrack& track) {
        if (request.retain_review_evidence) review_evidence.add(track);
    };
    std::vector<Note> notes;
    std::uint32_t audio_sample_rate = 0;
    std::uint64_t audio_frame_count = 0;

    if (streaming_wav) {
        report_progress(progress_callback, 0.0, "Reading audio");
        StreamingOptions options;
        options.chunk_seconds = 2.0;
        options.overlap_seconds = 0.5;
        options.analysis.fast_preview = request.fast_preview;
        options.analysis.instrument = request.instrument;
        if (request.retain_review_evidence) {
            options.analysis.evidence_callback = retain_evidence;
        }
        options.analysis.progress_callback = [&](double progress) {
            report_progress(progress_callback, progress, "Analyzing audio");
        };
        options.audio_callback = [&](const AudioBlockView& block) {
            audio_sample_rate = block.sample_rate;
            audio_frame_count = std::max<std::uint64_t>(
                audio_frame_count, block.total_frames);
            if (audio_preview_callback) {
                const auto preview = make_audio_preview(
                    block.samples, block.sample_rate, 16, 40);
                audio_preview_callback(
                    static_cast<double>(block.first_frame) / block.sample_rate,
                    preview, false);
            }
        };
        options.notes_callback = [&](const std::vector<Note>& updated_notes) {
            if (live_preview_callback) {
                live_preview_callback(0.0, updated_notes, {});
            }
        };
        notes = transcribe_wav_streaming(request.input_path, options).notes;
    } else {
        const auto audio_update_callback =
            [&](double offset, const AudioBuffer& preview_audio) {
                if (audio_preview_callback) {
                    const auto preview = make_audio_preview(
                        preview_audio.samples, preview_audio.sample_rate, 128, 40);
                    audio_preview_callback(offset, preview, true);
                }
            };
        AudioBuffer audio = request.use_asio
            ? capture_asio(
                  request.capture_seconds, stop_token,
                  [&](double progress) {
                      report_progress(progress_callback, progress * 0.35,
                                      "Capturing ASIO audio");
                  },
                  audio_update_callback,
                  [&](const std::vector<Note>& live_notes,
                      const std::vector<NoteEvent>& events) {
                      if (live_preview_callback) {
                          live_preview_callback(0.0, live_notes, events);
                      }
                  },
                  request.realtime_options)
            : read_audio(request.input_path);
        audio_sample_rate = audio.sample_rate;
        audio_frame_count = audio.samples.size();

        if (!request.use_asio && audio_preview_callback) {
            const auto preview =
                make_audio_preview(audio.samples, audio.sample_rate, 512, 48);
            audio_preview_callback(0.0, preview, true);
        }
        report_progress(progress_callback, 0.4, "Audio loaded");
        if (request.model_path.empty()) {
            AnalysisOptions options;
            options.fast_preview = request.fast_preview;
            options.instrument = request.instrument;
            if (request.retain_review_evidence) {
                options.evidence_callback = retain_evidence;
            }
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
            if (request.retain_review_evidence) {
                options.evidence_callback = retain_evidence;
            }
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
    auto interpretation_options = request.interpretation_options;
    if (request.instrument == InstrumentPrior::Guitar) {
        interpretation_options.instrument = "Guitar";
    } else if (request.instrument == InstrumentPrior::Piano) {
        interpretation_options.instrument = "Piano";
    } else if (request.instrument == InstrumentPrior::Voice) {
        interpretation_options.instrument = "Voice";
    }
    const auto interpretation = interpret_notes(notes, interpretation_options);
    if (interpretation.hypotheses.empty()) {
        throw std::runtime_error("Musical interpretation produced no score hypothesis.");
    }
    result.metadata = metadata_for_hypothesis(interpretation.hypotheses.front());
    result.score = make_score_document(
        interpretation, 0,
        request.use_asio ? "live-capture" : request.input_path.string(),
        result.metadata,
        request.guitar_tab_options);
    result.audio_sample_rate = audio_sample_rate;
    result.audio_frame_count = audio_frame_count;
    result.score.source_sample_rate = audio_sample_rate;
    if (request.retain_review_evidence && !request.use_asio) {
        auto tracks = review_evidence.finish();
        result.review_evidence_truncated = review_evidence.truncated();
        const std::string model_identifier = request.model_path.empty()
            ? "builtin:yin" : request.model_path.filename().string();
        attach_review_evidence(result.score, tracks, model_identifier,
                               result.review_evidence_truncated);
    }
    result.notes = score_notes(result.score);
    result.fingering =
        assign_guitar_tab_fingerings(result.notes, request.guitar_tab_options);
    for (const auto& position : result.fingering.positions) {
        if (position.note_index >= result.notes.size() ||
            position.note_index >= result.score.notes.size()) {
            throw std::runtime_error("Guitar tablature assignment returned an invalid note index.");
        }
        auto tab = std::find_if(result.score.tab_notes.begin(),
                                result.score.tab_notes.end(),
            [&](const auto& candidate) {
                return candidate.notation_note_id ==
                    result.score.notes[position.note_index].id;
            });
        if (tab != result.score.tab_notes.end() &&
            position.string_number > 0 && position.fret >= 0) {
            tab->string_number = position.string_number;
            tab->fret = position.fret;
            tab->manually_corrected = position.manually_corrected;
        }
    }
    if (!request.proposal_only) {
#ifdef TRANSCRIBE_ENABLE_VEROVIO
        write_musicxml(request.musicxml_path, result.score,
                       request.preserve_expressive_timing,
                       &result.musicxml_source);
#else
        write_musicxml(request.musicxml_path, result.score,
                       request.preserve_expressive_timing);
#endif
        write_guitar_tab(request.tab_path, result.score,
                         request.guitar_tab_options);
        if (!request.midi_path.empty()) {
            write_midi(request.midi_path, result.score);
        }
    }

    if (live_preview_callback) {
        live_preview_callback(0.0, result.notes, {});
    }
    report_progress(progress_callback, 1.0, "Finished");
    return result;
}

} // namespace transcribe::gui
