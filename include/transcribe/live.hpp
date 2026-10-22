#pragma once

#include "transcribe/decoding.hpp"
#include "transcribe/inference.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace transcribe::live {

using Nanoseconds = std::chrono::nanoseconds;
// Monotonic time source. Tests inject a simulated clock; the default is steady_clock.
using MonotonicClock = std::function<Nanoseconds()>;

enum class ChannelLayout { DiscreteInterleaved, Mono, Stereo };

struct AudioFormat {
    std::uint32_t sample_rate{};
    std::uint16_t channels{1};
    ChannelLayout layout{ChannelLayout::DiscreteInterleaved};
};

// Interleaved float32 frames. first_frame counts device frames (including frames the producer
// could not deliver), so gaps remain explicit. The session's recording timeline starts at the
// first accepted block: every frame reported by the session is first_frame - that origin.
struct AudioFrameBlock {
    AudioFormat format;
    std::uint64_t first_frame{};
    std::span<const float> interleaved;
    // Monotonic time at which the producer received this block (e.g. callback entry).
    Nanoseconds capture_time{};
    // Producer/driver reported unknown loss immediately before this block.
    bool discontinuity{};
    // Optional only for source compatibility. New producers use strictly increasing numbers.
    // QueueFull retries retain the same number; rejected/dropped blocks do not consume it.
    std::optional<std::uint64_t> sequence{};
    std::uint64_t stream_epoch{};
};

enum class PushStatus { Accepted, QueueFull, InvalidFormat, InvalidTimestamp, Closed };

class AudioInputSink {
public:
    virtual ~AudioInputSink() = default;
    virtual PushStatus push(const AudioFrameBlock& block) noexcept = 0;
    virtual void report_input_lost() noexcept = 0;
};

enum class LiveBackendKind {
    // True sample-incremental YIN state machine (existing RealtimeTranscriber).
    IncrementalYin,
    // Repeated offline inference over overlapping windows of a ModelBackend.
    OverlappingWindow
};

enum class LagPolicy {
    // Coalesce windows, then skip unanalysed audio, when analysis falls behind.
    SkipAhead,
    // Never skip: ingestion stops draining and producers observe QueueFull (file replay only).
    Lossless
};

enum class LiveState { Created, Running, InputInterrupted, AnalysisFailed, Finished, Cancelled };

struct IncrementalYinOptions {
    RealtimeOptions realtime;  // input_sample_rate is taken from LiveOptions::format.
    // Audio time a note must remain active after onset before it is stabilized.
    double stabilization_seconds{0.06};
    // Provisional notes closing sooner than this are retracted instead of reported.
    double minimum_duration_seconds{0.055};
};

struct WindowOptions {
    double window_seconds{2.0};
    double hop_seconds{0.25};
    // Onsets this close to a window's left edge may be truncated and never create notes.
    double left_guard_seconds{0.1};
    // Notes ending this close to a window's right edge are treated as still sounding.
    double right_guard_seconds{0.1};
    // Same-pitch onset distance treated as the same note across windows.
    double onset_match_seconds{0.08};
    // A closed note continuing within this gap is extended rather than duplicated.
    double seam_merge_seconds{0.03};
    // Smallest onset/offset change reported as a correction.
    double correction_seconds{0.01};
    // Number of windows that must observe a note before it is stabilized.
    std::size_t confirmations{2};
    // Notes closing before stabilization and shorter than this are retracted.
    double minimum_duration_seconds{0.055};
    // Decode window evidence with the canonical temporal decoder rather than legacy notes.
    bool temporal_decoding{true};
    TemporalDecodingOptions decoding;
    // Request template; cancellation is replaced by the session.
    InferenceRequest request;
};

struct LiveOptions {
    AudioFormat format;
    LiveBackendKind backend{LiveBackendKind::IncrementalYin};
    IncrementalYinOptions yin;
    WindowOptions window;
    ModelLoadOptions model_load;
    // Consecutive model failures tolerated (each followed by reload) before analysis fails.
    std::size_t max_model_recoveries{2};
    std::size_t input_queue_blocks{256};
    std::size_t max_block_frames{4096};
    std::size_t event_queue_capacity{4096};
    std::size_t max_tracked_notes{256};
    std::size_t max_snapshot_notes{20000};
    double max_analysis_lag_seconds{1.0};
    LagPolicy lag_policy{LagPolicy::SkipAhead};
    // Mono recording retained for refinement; 0 disables retention.
    double max_recording_seconds{600.0};
    // false: the caller drives ingestion/analysis with pump() (deterministic tests).
    bool threaded{true};
    std::chrono::microseconds ingest_poll_interval{1000};
    std::string audio_source_id{"live"};
    MonotonicClock clock;
    // Invoked on the analysis worker after events are queued. Must not block (e.g. PostMessage).
    std::function<void()> events_available;
    // Diagnostic/test hook executed inside each timed analysis step on the analysis worker.
    std::function<void(std::uint64_t step)> analysis_hook;
    // Identity is immutable for a session. Use a new epoch/ID for a restarted device timeline.
    std::uint64_t stream_epoch{};
    std::string stream_id;
    // Diagnostic targets only: never silently change pitch/context/stability parameters.
    double target_provisional_seconds{0.2};
    double target_stabilized_seconds{0.3};
};

enum class LiveEventKind {
    NoteOn,          // Provisional note detected.
    NoteStabilized,  // Note confirmed by additional evidence.
    NoteOff,         // Note offset determined (may be later corrected or reopened).
    NoteCorrected,   // Pitch/onset/offset revised, or a closed note reopened; same ID.
    NoteRetracted,   // Provisional note withdrawn; consumers should remove it.
    AnalysisGap,     // Frames [gap_first_frame, gap_end_frame) were not analysed live.
    Status,          // Input/model/lifecycle diagnostic in message.
    NoteFinalized    // Last revision from this stream; offline refinement remains a separate proposal.
};

struct LiveNote {
    std::string id;
    int midi_pitch{-1};
    std::uint32_t sample_rate{};
    std::uint64_t onset_frame{};
    std::optional<std::uint64_t> offset_frame;  // Empty while sounding.
    EventLifecycle state{EventLifecycle::Provisional};
    // Backend heuristic strength or activation; never a probability of correctness.
    double raw_strength{};
    std::string source_model;
    std::string detection_method;
    // Onset hidden by an input gap, interruption or skipped analysis.
    bool onset_truncated{};
    std::uint32_t revision{};
    bool finalized{};

    double onset_seconds() const noexcept;
    std::optional<double> offset_seconds() const;
};

struct EventTiming {
    // Capture time of the block that contained the onset frame; zero when unknown.
    Nanoseconds onset_capture_time{};
    Nanoseconds emitted_time{};
    // Audio analysed (exclusive end frame) when the event was produced.
    std::uint64_t evidence_end_frame{};
};

struct LiveEvent {
    std::uint64_t sequence{};
    LiveEventKind kind{LiveEventKind::Status};
    LiveNote note;
    std::optional<LiveNote> previous;
    std::uint64_t gap_first_frame{};
    std::uint64_t gap_end_frame{};
    std::string message;
    EventTiming timing;
};

struct EventBatch {
    std::vector<LiveEvent> events;
    // Events were dropped because the consumer fell behind; rebuild state from snapshot().
    bool resync_required{};
    std::uint64_t dropped_events{};
    // Atomic-with-events replacement on overflow. It supersedes all events queued at this poll.
    // Use instead of a separate snapshot() call; the latter may include newer revisions.
    std::vector<LiveNote> resync_snapshot;
};

struct LatencySummary {
    std::size_t count{};
    double mean_seconds{};
    double p50_seconds{};
    double p95_seconds{};
    double maximum_seconds{};
    double p99_seconds{};
};

struct LiveMetrics {
    // Block capture timestamp -> ingestion by the ingest worker (queue wait only).
    LatencySummary capture_latency;
    // Wall time of each analysis step (inference + decoding + reconciliation).
    LatencySummary analysis_latency;
    // Onset block capture -> provisional NoteOn queued for the consumer.
    LatencySummary end_to_end_latency;
    // Onset block capture -> NoteStabilized queued for the consumer.
    LatencySummary stabilization_delay;
    // Recording-time distance between a NoteOn's onset and the analysed audio end.
    LatencySummary algorithmic_latency;
    std::uint64_t frames_received{};
    // push() returned QueueFull. Real-time producers drop these frames (later seen as a gap);
    // replay producers may retry them, so retries are counted again.
    std::uint64_t blocks_refused{};
    std::uint64_t frames_refused{};
    // Invalid format/timestamp blocks.
    std::uint64_t blocks_rejected{};
    std::uint64_t discontinuities{};
    std::uint64_t gap_frames{};
    std::uint64_t nonfinite_samples{};
    std::size_t input_queue_capacity{};
    std::size_t input_queue_high_water{};
    std::size_t event_queue_capacity{};
    std::size_t event_queue_high_water{};
    std::uint64_t events_emitted{};
    std::uint64_t events_dropped{};
    std::uint64_t analysis_steps{};
    std::uint64_t windows_coalesced{};
    std::uint64_t skipped_frames{};
    std::uint64_t maximum_lag_frames{};
    std::size_t rolling_capacity_frames{};
    std::size_t tracked_notes_high_water{};
    std::uint64_t tracked_notes_refused{};
    std::uint64_t snapshot_notes_evicted{};
    std::uint64_t recording_frames{};
    bool recording_truncated{};
    std::uint64_t model_errors{};
    std::uint64_t model_recoveries{};
    double analysed_audio_seconds{};
    double analysis_busy_seconds{};
    // Ingestion of the newest evidence block -> start of its analysis step.
    LatencySummary analysis_queue_latency;
    // First NoteOn -> NoteStabilized (distinct from capture -> stabilization above).
    LatencySummary confirmation_latency;
    // Event publication -> poll_events by the consumer; excludes UI dispatch/rendering.
    LatencySummary publication_queue_latency;
    bool overloaded{};
    std::uint64_t input_markers_coalesced{};
    std::size_t input_queue_depth{};
    std::size_t event_queue_depth{};
    std::uint64_t current_lag_frames{};
    std::uint64_t sequence_gaps{};
    std::uint64_t provisional_target_misses{};
    std::uint64_t stabilized_target_misses{};
    double target_provisional_seconds{};
    double target_stabilized_seconds{};
};

struct LatencyBudget {
    double capture_block_seconds{};
    double ingest_poll_seconds{};
    double analysis_context_seconds{};
    double update_wait_seconds{};
    double stabilization_seconds{};
    double provisional_estimate_seconds{};
    double stabilized_estimate_seconds{};
    bool provisional_estimate_within_target{};
    bool stabilized_estimate_within_target{};
    // Model compute, scheduling and consumer/UI delays are measured, not estimated here.
};

LatencyBudget latency_budget(const LiveOptions& options, std::size_t capture_block_frames);

class LiveTranscriptionSession : public AudioInputSink {
public:
    // OverlappingWindow requires a model; it is loaded in start() if not already loaded.
    explicit LiveTranscriptionSession(LiveOptions options,
                                      std::unique_ptr<ModelBackend> model = {});
    ~LiveTranscriptionSession();
    LiveTranscriptionSession(const LiveTranscriptionSession&) = delete;
    LiveTranscriptionSession& operator=(const LiveTranscriptionSession&) = delete;

    ModelOperation start();
    // Optional device timeline declaration. Owner calls before starting the external producer.
    // Without it, the first accepted block remains the origin for compatibility.
    void begin_input(std::uint64_t first_frame);
    // Producer must be quiescent. Seal the known end so a dropped final block becomes a gap.
    void end_input(std::uint64_t end_frame);
    // Real-time safe for one producer: no locks, allocation, blocking or analysis.
    PushStatus push(const AudioFrameBlock& block) noexcept override;
    // Real-time safe: the device stopped delivering audio (e.g. disconnection).
    void report_input_lost() noexcept override;
    // Manual mode only: ingest queued audio and run all ready analysis on the caller.
    void pump();
    // Drain remaining audio, finalize open notes and stop workers.
    // Control calls belong on the owner thread, never inside worker notification/hooks.
    // Quiesce the external producer before destroying the session.
    void finish();
    // Cooperative request only; does not join. Safe from analysis notifications.
    // Owner must later call cancel()/finish() or destroy the quiesced session.
    void request_cancel() noexcept;
    // Capture/recording continue; resume may skip stale analysis with an explicit gap.
    // Only supported with SkipAhead. finish() resumes analysis before draining.
    void pause_analysis(bool paused);
    void cancel() noexcept;

    EventBatch poll_events(std::size_t maximum = std::numeric_limits<std::size_t>::max());
    // Complete snapshot taken under the same publication lock as the drained events.
    // resync_snapshot is populated even without overflow; resync_required retains its meaning.
    EventBatch poll_events_with_snapshot();
    std::vector<LiveNote> snapshot() const;
    LiveMetrics metrics() const;
    LiveState state() const noexcept;
    std::string last_error() const;
    std::uint64_t ingested_frames() const;
    // Mono downmix on the recording timeline; gaps are zero-filled.
    AudioBuffer recording() const;
    // Copies only the newest requested mono frames for bounded-rate UI previews.
    AudioBuffer recording_tail(std::size_t maximum_frames) const;
    const LiveOptions& options() const noexcept;

private:
    EventBatch poll_events_impl(std::size_t maximum, bool include_snapshot);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Full-context refinement and editable document ----

struct RefinementOptions {
    std::string backend_id{"builtin:spectral"};
    std::filesystem::path model_path;
    ModelLoadOptions load;
    InferenceRequest request;
    TemporalDecodingOptions decoding;
    std::string audio_source_id{"live"};
    // Unset: one inference over the whole recording. Set: replay the recording offline (lossless,
    // no latency limit) through the overlapping-window reconciler. Models trained on short
    // windows, such as Basic Pitch, measured better this way; request/decoding above replace the
    // window template's. Proposal events are then empty: notes come from reconciled windows.
    std::optional<WindowOptions> windowed;
};

struct ScoreProposal {
    std::string id;
    std::string backend_id;
    std::uint32_t sample_rate{};
    std::vector<CanonicalNoteEvent> events;
    std::vector<LiveNote> notes;
    ScoreMetadata metadata;
    double inference_seconds{};
};

// Blocking: call from a worker, never a UI or audio thread.
InferenceResult<ScoreProposal> refine_recording(const AudioBuffer& recording,
                                                const RefinementOptions& options,
                                                std::stop_token cancellation = {});

enum class RevisionOrigin { Live, Resync, Refinement, Manual };

struct NoteRevision {
    std::uint64_t document_revision{};
    RevisionOrigin origin{RevisionOrigin::Live};
    LiveNote note;
    std::string reason;
    // Live update recorded in history but not applied because the note was manually edited.
    bool superseded_by_manual{};
};

enum class ProposedChangeKind { Add, Modify, Remove };

struct ProposedChange {
    std::string change_id;
    ProposedChangeKind kind{ProposedChangeKind::Add};
    std::string note_id;
    std::optional<LiveNote> current;
    std::optional<LiveNote> proposed;
    bool conflicts_with_manual_edit{};
};

struct ProposalReview {
    std::string proposal_id;
    std::uint64_t base_revision{};
    std::vector<ProposedChange> changes;
    std::size_t conflicts{};
};

struct ApplyResult {
    std::size_t applied{};
    std::size_t withheld_conflicts{};
    std::uint64_t revision{};
};

// Owned by one consumer thread (typically UI). Not internally synchronized.
class TranscriptionDocument {
public:
    explicit TranscriptionDocument(std::uint32_t sample_rate);

    void apply(const LiveEvent& event);
    void apply(const EventBatch& batch);
    void resync(const std::vector<LiveNote>& snapshot);
    std::string add_manual_note(LiveNote note, const std::string& reason = "manual add",
                                std::string stable_id = {});
    void edit_manual_note(const LiveNote& note, const std::string& reason = "manual edit");
    void delete_manual_note(const std::string& id, const std::string& reason = "manual delete");

    // Read-only proposal comparison; does not modify the document.
    ProposalReview review(const ScoreProposal& proposal,
                          double onset_tolerance_seconds = 0.05) const;
    // Applies non-conflicting changes; conflicting changes only when their change_id is approved.
    // Throws std::logic_error when the document changed after the review was created.
    ApplyResult apply(const ProposalReview& review, const std::set<std::string>& approved_conflicts = {});

    std::vector<LiveNote> notes(bool include_rejected = false) const;
    std::optional<LiveNote> note(const std::string& id) const;
    const std::vector<NoteRevision>& history(const std::string& id) const;
    const std::vector<LiveEvent>& original_events() const noexcept;
    std::uint64_t revision() const noexcept;
    bool requires_resync() const noexcept;
    // Existing exporter representation; open notes end at open_note_end_seconds.
    std::vector<Note> to_notes(double open_note_end_seconds) const;

private:
    struct Record {
        LiveNote current;
        std::vector<NoteRevision> history;
        bool manual{};
    };
    Record* find(const std::string& id);
    const Record* find(const std::string& id) const;
    void record(Record& record, RevisionOrigin origin, const LiveNote& note,
                const std::string& reason, bool superseded = false);
    std::uint32_t sample_rate_{};
    std::uint64_t revision_{};
    std::uint64_t manual_serial_{};
    std::uint64_t refined_serial_{};
    bool requires_resync_{};
    std::vector<Record> records_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<LiveEvent> original_events_;
};

} // namespace transcribe::live
