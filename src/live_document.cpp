#include "transcribe/live.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace transcribe::live {
namespace {

std::uint64_t frame_distance(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

bool overlaps(const LiveNote& a, const LiveNote& b) {
    const auto a_end = a.offset_frame.value_or(std::numeric_limits<std::uint64_t>::max());
    const auto b_end = b.offset_frame.value_or(std::numeric_limits<std::uint64_t>::max());
    return a.onset_frame < b_end && b.onset_frame < a_end;
}

InferenceResult<ScoreProposal> refine_windowed(const AudioBuffer& recording,
                                               const RefinementOptions& options,
                                               std::unique_ptr<ModelBackend> model,
                                               std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return InferenceError{InferenceErrorCode::Cancelled, "Refinement cancelled"};
    }
    LiveOptions live;
    live.format = {recording.sample_rate, 1};
    live.backend = LiveBackendKind::OverlappingWindow;
    live.window = *options.windowed;
    live.window.request = options.request;
    live.window.decoding = options.decoding;
    live.model_load = options.load;
    live.lag_policy = LagPolicy::Lossless;
    live.threaded = false;
    live.max_recording_seconds = 0;
    live.max_tracked_notes = std::numeric_limits<std::size_t>::max() / 2;
    live.max_snapshot_notes = std::numeric_limits<std::size_t>::max() / 2;
    live.audio_source_id = options.audio_source_id;
    const auto id = model->metadata().definition.id;
    const auto started = std::chrono::steady_clock::now();
    LiveTranscriptionSession session(std::move(live), std::move(model));
    const auto started_session = session.start();
    if (const auto* failure = std::get_if<InferenceError>(&started_session)) return *failure;
    constexpr std::size_t block = 4096;
    for (std::size_t first = 0; first < recording.samples.size(); first += block) {
        if (cancellation.stop_requested()) {
            session.cancel();
            return InferenceError{InferenceErrorCode::Cancelled, "Refinement cancelled"};
        }
        const auto count = std::min(block, recording.samples.size() - first);
        AudioFrameBlock frames;
        frames.format = session.options().format;
        frames.first_frame = first;
        frames.interleaved = std::span<const float>(recording.samples.data() + first, count);
        for (;;) {
            const auto status = session.push(frames);
            if (status == PushStatus::Accepted) break;
            if (status != PushStatus::QueueFull) {
                return InferenceError{InferenceErrorCode::InvalidArgument,
                                      "Windowed refinement rejected the recording"};
            }
            session.pump();
            (void)session.poll_events();
        }
        session.pump();
        (void)session.poll_events();
    }
    session.finish();
    if (session.state() != LiveState::Finished) {
        return InferenceError{InferenceErrorCode::Inference,
                              "Windowed refinement failed: " + session.last_error()};
    }
    ScoreProposal proposal;
    proposal.backend_id = id;
    proposal.sample_rate = recording.sample_rate;
    std::vector<Note> notes;
    for (auto note : session.snapshot()) {
        if (!note.offset_frame) note.offset_frame = recording.samples.size();
        note.offset_frame = std::max(*note.offset_frame, note.onset_frame + 1);
        note.id = "refined-" + std::to_string(proposal.notes.size() + 1);
        note.state = EventLifecycle::Refined;
        note.detection_method = "windowed-offline/" + note.detection_method;
        const double onset = note.onset_seconds();
        notes.push_back({note.midi_pitch, onset, *note.offset_seconds() - onset,
                         midi_note_name(note.midi_pitch), std::clamp(note.raw_strength, 0.0, 1.0), 0.0});
        proposal.notes.push_back(std::move(note));
    }
    proposal.inference_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    proposal.id = "proposal-" + id + "-windowed-" + std::to_string(recording.samples.size()) + "-" +
        std::to_string(proposal.notes.size());
    proposal.metadata = infer_score_metadata(notes);
    return proposal;
}

} // namespace

InferenceResult<ScoreProposal> refine_recording(const AudioBuffer& recording,
                                                const RefinementOptions& options,
                                                std::stop_token cancellation) {
    if (recording.sample_rate < 2000) {
        return InferenceError{InferenceErrorCode::InvalidArgument,
                              "Refinement requires audio sampled at >=2000 Hz"};
    }
    if (recording.samples.empty()) {
        ScoreProposal empty;
        empty.id = "proposal-" + options.backend_id + "-empty";
        empty.backend_id = options.backend_id;
        empty.sample_rate = recording.sample_rate;
        return empty;
    }
    ModelRegistry registry(options.model_path);
    auto created = registry.create(options.backend_id);
    if (auto* failure = std::get_if<InferenceError>(&created)) return *failure;
    auto model = std::move(std::get<std::unique_ptr<ModelBackend>>(created));
    if (options.windowed) return refine_windowed(recording, options, std::move(model), cancellation);
    const auto loaded = model->load(options.load);
    if (const auto* failure = std::get_if<InferenceError>(&loaded)) return *failure;
    auto request = options.request;
    request.retain_evidence = true;
    request.cancellation = cancellation;
    request.onnx.execution_provider = options.load.provider;
    request.onnx.device_id = options.load.device_id;
    const auto started = std::chrono::steady_clock::now();
    auto inferred = model->infer(recording, request);
    if (const auto* failure = std::get_if<InferenceError>(&inferred)) {
        model->release();
        return *failure;
    }
    try {
        const auto& output = std::get<InferenceOutput>(inferred);
        const auto& id = model->metadata().definition.id;
        auto decoded = decode_note_events(recording, output.evidence, options.decoding,
                                          options.audio_source_id, id);
        ScoreProposal proposal;
        proposal.backend_id = id;
        proposal.sample_rate = recording.sample_rate;
        proposal.inference_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        proposal.id = "proposal-" + id + "-" + std::to_string(recording.samples.size()) + "-" +
            std::to_string(decoded.events.size());
        std::vector<Note> notes;
        for (std::size_t i = 0; i < decoded.events.size(); ++i) {
            const auto& event = decoded.events[i];
            LiveNote note;
            note.id = "refined-" + std::to_string(i + 1);
            note.midi_pitch = static_cast<int>(std::lround(event.midi_pitch));
            note.sample_rate = recording.sample_rate;
            note.onset_frame = event.onset_sample;
            note.offset_frame = std::max(event.offset_sample, event.onset_sample + 1);
            note.state = EventLifecycle::Refined;
            for (const auto& evidence : event.evidence) {
                note.raw_strength = std::max(note.raw_strength, evidence.raw_strength);
            }
            note.source_model = id;
            note.detection_method = "full-context/" + event.detection_method;
            proposal.notes.push_back(note);
            notes.push_back({note.midi_pitch, event.onset_seconds,
                             event.offset_seconds - event.onset_seconds,
                             midi_note_name(note.midi_pitch), std::clamp(note.raw_strength, 0.0, 1.0),
                             event.intensity_rms.value_or(0.0)});
        }
        proposal.metadata = infer_score_metadata(notes);
        proposal.events = std::move(decoded.events);
        model->release();
        return proposal;
    } catch (const std::exception& error) {
        model->release();
        return InferenceError{InferenceErrorCode::Inference,
                              std::string("Refinement decoding failed: ") + error.what()};
    }
}

TranscriptionDocument::TranscriptionDocument(std::uint32_t sample_rate) : sample_rate_(sample_rate) {
    if (sample_rate == 0) throw std::invalid_argument("Document sample rate must be positive");
}

TranscriptionDocument::Record* TranscriptionDocument::find(const std::string& id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &records_[it->second];
}

const TranscriptionDocument::Record* TranscriptionDocument::find(const std::string& id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &records_[it->second];
}

void TranscriptionDocument::record(Record& target, RevisionOrigin origin, const LiveNote& note,
                                   const std::string& reason, bool superseded) {
    ++revision_;
    target.history.push_back({revision_, origin, note, reason, superseded});
}

void TranscriptionDocument::apply(const LiveEvent& event) {
    original_events_.push_back(event);
    switch (event.kind) {
    case LiveEventKind::AnalysisGap:
    case LiveEventKind::Status:
        return;
    default:
        break;
    }
    if (event.note.id.empty()) return;
    auto* target = find(event.note.id);
    if (!target) {
        if (event.kind == LiveEventKind::NoteRetracted) return;
        index_.emplace(event.note.id, records_.size());
        records_.push_back({event.note, {}, false});
        record(records_.back(), RevisionOrigin::Live, event.note, "live event");
        return;
    }
    if (target->manual) {
        // Manual edits win; the live update stays in history for review.
        record(*target, RevisionOrigin::Live, event.note, "live event after manual edit", true);
        return;
    }
    if (event.note.revision <= target->current.revision || target->current.finalized) return;
    target->current = event.note;
    if (event.kind == LiveEventKind::NoteRetracted) target->current.state = EventLifecycle::Rejected;
    record(*target, RevisionOrigin::Live, target->current,
           event.message.empty() ? "live event" : event.message);
}

void TranscriptionDocument::apply(const EventBatch& batch) {
    if (batch.resync_required) requires_resync_ = true;
    for (const auto& event : batch.events) apply(event);
}

void TranscriptionDocument::resync(const std::vector<LiveNote>& snapshot) {
    std::uint64_t earliest = std::numeric_limits<std::uint64_t>::max();
    std::set<std::string> present;
    for (const auto& note : snapshot) {
        earliest = std::min(earliest, note.onset_frame);
        present.insert(note.id);
        auto* target = find(note.id);
        if (!target) {
            index_.emplace(note.id, records_.size());
            records_.push_back({note, {}, false});
            record(records_.back(), RevisionOrigin::Resync, note, "snapshot resync");
        } else if (target->manual) {
            record(*target, RevisionOrigin::Resync, note, "snapshot after manual edit", true);
        } else if (!target->current.finalized && target->current.revision < note.revision) {
            target->current = note;
            record(*target, RevisionOrigin::Resync, note, "snapshot resync");
        }
    }
    for (auto& target : records_) {
        // Notes absent from the snapshot were retracted (older notes may have been evicted).
        if (target.manual || target.current.state == EventLifecycle::Rejected ||
            present.contains(target.current.id) || target.current.onset_frame < earliest) continue;
        target.current.state = EventLifecycle::Rejected;
        record(target, RevisionOrigin::Resync, target.current, "absent from snapshot");
    }
    requires_resync_ = false;
}

std::string TranscriptionDocument::add_manual_note(LiveNote note,
                                                   const std::string& reason,
                                                   std::string stable_id) {
    if (stable_id.empty()) stable_id = "manual-" + std::to_string(++manual_serial_);
    note.id = std::move(stable_id);
    note.sample_rate = note.sample_rate ? note.sample_rate : sample_rate_;
    note.state = EventLifecycle::ManuallyCorrected;
    if (auto* existing = find(note.id)) {
        if (!existing->manual ||
            existing->current.state != EventLifecycle::Rejected) {
            throw std::invalid_argument("Duplicate note id: " + note.id);
        }
        existing->current = note;
        record(*existing, RevisionOrigin::Manual, note, reason);
        return note.id;
    }
    index_.emplace(note.id, records_.size());
    records_.push_back({note, {}, true});
    record(records_.back(), RevisionOrigin::Manual, note, reason);
    return note.id;
}

void TranscriptionDocument::edit_manual_note(const LiveNote& note, const std::string& reason) {
    auto* target = find(note.id);
    if (!target) throw std::invalid_argument("Unknown note id: " + note.id);
    target->current = note;
    target->current.state = EventLifecycle::ManuallyCorrected;
    target->current.revision = target->current.revision + 1;
    target->manual = true;
    record(*target, RevisionOrigin::Manual, target->current, reason);
}

void TranscriptionDocument::delete_manual_note(const std::string& id, const std::string& reason) {
    auto* target = find(id);
    if (!target) throw std::invalid_argument("Unknown note id: " + id);
    target->current.state = EventLifecycle::Rejected;
    target->manual = true;
    record(*target, RevisionOrigin::Manual, target->current, reason);
}

ProposalReview TranscriptionDocument::review(const ScoreProposal& proposal,
                                             double onset_tolerance_seconds) const {
    ProposalReview result;
    result.proposal_id = proposal.id;
    result.base_revision = revision_;
    const auto tolerance = static_cast<std::uint64_t>(
        std::llround(std::max(0.0, onset_tolerance_seconds) * sample_rate_));
    const auto timing_tolerance = std::max<std::uint64_t>(1, sample_rate_ / 200);
    std::vector<bool> matched(records_.size(), false);
    std::size_t serial = 0;
    const auto add_change = [&](ProposedChange change) {
        change.change_id = proposal.id + ":" + std::to_string(++serial);
        result.conflicts += change.conflicts_with_manual_edit ? 1 : 0;
        result.changes.push_back(std::move(change));
    };
    for (const auto& proposed : proposal.notes) {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < records_.size(); ++i) {
            const auto& current = records_[i].current;
            if (matched[i] || current.state == EventLifecycle::Rejected ||
                current.midi_pitch != proposed.midi_pitch ||
                frame_distance(current.onset_frame, proposed.onset_frame) > tolerance) continue;
            if (!best || frame_distance(current.onset_frame, proposed.onset_frame) <
                    frame_distance(records_[*best].current.onset_frame, proposed.onset_frame)) {
                best = i;
            }
        }
        if (!best) {
            for (std::size_t i = 0; i < records_.size(); ++i) {
                const auto& record = records_[i];
                const auto& current = record.current;
                if (matched[i] || !record.manual ||
                    current.state == EventLifecycle::Rejected ||
                    frame_distance(current.onset_frame, proposed.onset_frame) > tolerance) {
                    continue;
                }
                if (!best || frame_distance(current.onset_frame, proposed.onset_frame) <
                        frame_distance(records_[*best].current.onset_frame,
                                       proposed.onset_frame)) {
                    best = i;
                }
            }
        }
        if (best) {
            matched[*best] = true;
            const auto& current = records_[*best].current;
            const bool same_offset = current.offset_frame && proposed.offset_frame &&
                frame_distance(*current.offset_frame, *proposed.offset_frame) <= timing_tolerance;
            if (current.midi_pitch != proposed.midi_pitch ||
                frame_distance(current.onset_frame, proposed.onset_frame) > timing_tolerance ||
                !same_offset) {
                add_change({{}, ProposedChangeKind::Modify, current.id, current, proposed,
                            records_[*best].manual});
            }
            continue;
        }
        // Re-adding a note the user deleted (same pitch, overlapping) needs explicit approval.
        const bool conflicts = std::any_of(records_.begin(), records_.end(), [&](const Record& r) {
            return r.manual && r.current.state == EventLifecycle::Rejected &&
                r.current.midi_pitch == proposed.midi_pitch && overlaps(r.current, proposed);
        });
        add_change({{}, ProposedChangeKind::Add, {}, std::nullopt, proposed, conflicts});
    }
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const auto& current = records_[i].current;
        if (matched[i] || current.state == EventLifecycle::Rejected) continue;
        add_change({{}, ProposedChangeKind::Remove, current.id, current, std::nullopt,
                    records_[i].manual});
    }
    return result;
}

ApplyResult TranscriptionDocument::apply(const ProposalReview& review,
                                         const std::set<std::string>& approved_conflicts) {
    if (review.base_revision != revision_) {
        throw std::logic_error("The document changed after the proposal review; review it again");
    }
    ApplyResult result;
    for (const auto& change : review.changes) {
        if (change.conflicts_with_manual_edit && !approved_conflicts.contains(change.change_id)) {
            ++result.withheld_conflicts;
            continue;
        }
        const auto reason = "refinement " + review.proposal_id +
            (change.conflicts_with_manual_edit ? " (approved over manual edit)" : "");
        if (change.kind == ProposedChangeKind::Add) {
            auto note = *change.proposed;
            note.id = "refined-" + std::to_string(++refined_serial_);
            note.state = EventLifecycle::Refined;
            index_.emplace(note.id, records_.size());
            records_.push_back({note, {}, false});
            record(records_.back(), RevisionOrigin::Refinement, note, reason);
        } else if (auto* target = find(change.note_id)) {
            if (change.kind == ProposedChangeKind::Modify) {
                auto note = *change.proposed;
                note.id = target->current.id;
                note.revision = target->current.revision + 1;
                note.state = EventLifecycle::Refined;
                target->current = note;
            } else {
                target->current.state = EventLifecycle::Rejected;
            }
            target->manual = false;
            record(*target, RevisionOrigin::Refinement, target->current, reason);
        }
        ++result.applied;
    }
    result.revision = revision_;
    return result;
}

std::vector<LiveNote> TranscriptionDocument::notes(bool include_rejected) const {
    std::vector<LiveNote> result;
    for (const auto& target : records_) {
        if (include_rejected || target.current.state != EventLifecycle::Rejected) {
            result.push_back(target.current);
        }
    }
    std::sort(result.begin(), result.end(), [](const LiveNote& a, const LiveNote& b) {
        return a.onset_frame != b.onset_frame ? a.onset_frame < b.onset_frame : a.midi_pitch < b.midi_pitch;
    });
    return result;
}

std::optional<LiveNote> TranscriptionDocument::note(const std::string& id) const {
    const auto* target = find(id);
    return target ? std::optional<LiveNote>(target->current) : std::nullopt;
}

const std::vector<NoteRevision>& TranscriptionDocument::history(const std::string& id) const {
    static const std::vector<NoteRevision> empty;
    const auto* target = find(id);
    return target ? target->history : empty;
}

const std::vector<LiveEvent>& TranscriptionDocument::original_events() const noexcept {
    return original_events_;
}

std::uint64_t TranscriptionDocument::revision() const noexcept { return revision_; }

bool TranscriptionDocument::requires_resync() const noexcept { return requires_resync_; }

std::vector<Note> TranscriptionDocument::to_notes(double open_note_end_seconds) const {
    std::vector<Note> result;
    for (const auto& note : notes()) {
        const double start = note.onset_seconds();
        const double end = note.offset_seconds().value_or(open_note_end_seconds);
        if (end <= start) continue;
        result.push_back({note.midi_pitch, start, end - start, midi_note_name(note.midi_pitch),
                          std::clamp(note.raw_strength, 0.0, 1.0), 0.0});
    }
    return result;
}

} // namespace transcribe::live
