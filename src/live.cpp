#include "transcribe/live.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace transcribe::live {
namespace {

constexpr std::uint64_t no_frame = std::numeric_limits<std::uint64_t>::max();

std::uint64_t distance(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

class Reservoir {
public:
    Reservoir() { values_.reserve(capacity); }
    void add(double value) {
        if (!std::isfinite(value)) return;
        ++count_;
        sum_ += value;
        maximum_ = count_ == 1 ? value : std::max(maximum_, value);
        if (values_.size() < capacity) values_.push_back(value);
        else values_[next_++ % capacity] = value;
    }
    LatencySummary summary() const {
        LatencySummary result;
        result.count = count_;
        if (count_ == 0) return result;
        result.mean_seconds = sum_ / static_cast<double>(count_);
        result.maximum_seconds = maximum_;
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        const auto rank = [&](double q) {
            const auto index = static_cast<std::size_t>(std::ceil(q * sorted.size()));
            return sorted[std::clamp<std::size_t>(index, 1, sorted.size()) - 1];
        };
        result.p50_seconds = rank(0.5);
        result.p95_seconds = rank(0.95);
        return result;
    }

private:
    static constexpr std::size_t capacity = 4096;
    std::vector<double> values_;
    std::size_t next_{};
    std::size_t count_{};
    double sum_{};
    double maximum_{};
};

bool finite_nonnegative(double value) { return std::isfinite(value) && value >= 0; }

void validate(const LiveOptions& options, bool has_model) {
    const auto& format = options.format;
    if (format.sample_rate < 2000 || format.sample_rate > 768000 || format.channels == 0 ||
        format.channels > 64) {
        throw std::invalid_argument("Live audio format requires 2000-768000 Hz and 1-64 channels");
    }
    if (options.input_queue_blocks == 0 || options.max_block_frames == 0 ||
        options.event_queue_capacity == 0 || options.max_tracked_notes == 0 ||
        options.max_snapshot_notes == 0) {
        throw std::invalid_argument("Live queue and note capacities must be positive");
    }
    if (!std::isfinite(options.max_analysis_lag_seconds) || options.max_analysis_lag_seconds <= 0 ||
        !finite_nonnegative(options.max_recording_seconds) ||
        options.ingest_poll_interval.count() <= 0) {
        throw std::invalid_argument("Live lag, recording and polling limits must be positive and finite");
    }
    if (options.backend == LiveBackendKind::IncrementalYin) {
        if (has_model) {
            throw std::invalid_argument("IncrementalYin uses the built-in tracker; pass a model only for OverlappingWindow");
        }
        if (!finite_nonnegative(options.yin.stabilization_seconds) ||
            !finite_nonnegative(options.yin.minimum_duration_seconds)) {
            throw std::invalid_argument("YIN stabilization and minimum duration must be finite and non-negative");
        }
        auto realtime = options.yin.realtime;
        realtime.input_sample_rate = format.sample_rate;
        RealtimeTranscriber validation(realtime);
        return;
    }
    if (!has_model) {
        throw std::invalid_argument("OverlappingWindow requires a ModelBackend");
    }
    const auto& w = options.window;
    if (!std::isfinite(w.window_seconds) || !std::isfinite(w.hop_seconds) || w.hop_seconds <= 0 ||
        !finite_nonnegative(w.left_guard_seconds) || !finite_nonnegative(w.right_guard_seconds) ||
        w.window_seconds < w.left_guard_seconds + w.right_guard_seconds + w.hop_seconds ||
        w.window_seconds > 60 || !finite_nonnegative(w.onset_match_seconds) ||
        !finite_nonnegative(w.seam_merge_seconds) || !finite_nonnegative(w.correction_seconds) ||
        !finite_nonnegative(w.minimum_duration_seconds) || w.confirmations == 0) {
        throw std::invalid_argument(
            "Window options require 0 < hop, guards >= 0, window >= guards + hop (<= 60 s), "
            "non-negative tolerances and at least one confirmation");
    }
}

} // namespace

double LiveNote::onset_seconds() const noexcept {
    return sample_rate ? static_cast<double>(onset_frame) / sample_rate : 0.0;
}

std::optional<double> LiveNote::offset_seconds() const {
    if (!offset_frame || !sample_rate) return std::nullopt;
    return static_cast<double>(*offset_frame) / sample_rate;
}

struct LiveTranscriptionSession::Impl {
    struct Slot {
        std::vector<float> samples;
        std::size_t frames{};
        std::uint64_t first_frame{};
        Nanoseconds capture_time{};
        bool discontinuity{};
        bool continuation{};
    };
    enum class MarkerKind { Gap, Interrupt };
    struct Marker {
        MarkerKind kind{MarkerKind::Gap};
        std::uint64_t end_frame{};
        std::uint64_t resume_frame{};
        std::string message;
    };
    struct Tracked {
        LiveNote note;
        std::uint64_t serial{};
        std::size_t confirmations{};
        std::uint64_t active_until{};
        std::uint64_t onset_context{};
        std::uint64_t offset_context{};
        bool seen{};
        Nanoseconds onset_capture{};
    };
    struct Candidate {
        int midi{};
        std::uint64_t onset{};
        std::uint64_t offset{};
        double strength{};
        bool truncated{};
        bool open{};
        bool used{};
    };

    Impl(LiveOptions opts, std::unique_ptr<ModelBackend> backend)
        : options(std::move(opts)), model(std::move(backend)) {
        validate(options, model != nullptr);
        clock = options.clock ? options.clock : [] {
            return std::chrono::duration_cast<Nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch());
        };
        rate = options.format.sample_rate;
        channels = options.format.channels;
        slots.resize(options.input_queue_blocks);
        for (auto& slot : slots) slot.samples.resize(options.max_block_frames * channels);
        const bool window_mode = options.backend == LiveBackendKind::OverlappingWindow;
        const double span = window_mode ? options.window.window_seconds : 0.5;
        ring.assign(static_cast<std::size_t>(frames(span + 2 * (window_mode ? options.window.hop_seconds : 0.0) +
                                                          options.max_analysis_lag_seconds + 1.0)) +
                        options.max_block_frames, 0.0F);
        recording_limit = static_cast<std::size_t>(frames(options.max_recording_seconds));
        hop = std::max<std::uint64_t>(1, frames(options.window.hop_seconds));
        window_frames = std::max(hop, frames(options.window.window_seconds));
        left_guard = frames(options.window.left_guard_seconds);
        right_guard = frames(options.window.right_guard_seconds);
        max_jump = std::max<std::uint64_t>(hop,
            (window_frames - std::min(window_frames, left_guard + right_guard)) / hop * hop);
        lag_limit = std::max<std::uint64_t>(1, frames(options.max_analysis_lag_seconds));
        if (window_mode) {
            model_id = model->metadata().definition.id;
            detection_method = options.window.temporal_decoding ?
                "overlapping-window/temporal-decoder" : "overlapping-window/backend-notes";
        } else {
            model_id = "builtin:yin";
            detection_method = "incremental-yin";
            reset_yin();
        }
        next_end = hop;
    }

    std::uint64_t frames(double seconds) const {
        return static_cast<std::uint64_t>(std::llround(std::max(0.0, seconds) * rate));
    }
    double seconds(std::uint64_t value) const { return static_cast<double>(value) / rate; }
    double seconds(Nanoseconds value) const { return std::chrono::duration<double>(value).count(); }

    // ---------------- producer (real-time thread) ----------------
    PushStatus push(const AudioFrameBlock& block) noexcept {
        if (closed.load(std::memory_order_acquire)) return PushStatus::Closed;
        if (block.format.sample_rate != rate || block.format.channels != channels ||
            block.interleaved.size() % channels != 0) {
            blocks_rejected.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::InvalidFormat;
        }
        const std::size_t count = block.interleaved.size() / channels;
        if ((producer_has_frame && block.first_frame < producer_next_frame) ||
            block.first_frame > no_frame - count) {
            blocks_rejected.fetch_add(1, std::memory_order_relaxed);
            return PushStatus::InvalidTimestamp;
        }
        if (count == 0) return PushStatus::Accepted;
        const std::size_t needed = (count + options.max_block_frames - 1) / options.max_block_frames;
        const auto write = write_index.load(std::memory_order_relaxed);
        const auto read = read_index.load(std::memory_order_acquire);
        if (slots.size() - (write - read) < needed) {
            blocks_refused.fetch_add(1, std::memory_order_relaxed);
            frames_refused.fetch_add(count, std::memory_order_relaxed);
            return PushStatus::QueueFull;
        }
        for (std::size_t part = 0; part < needed; ++part) {
            auto& slot = slots[(write + part) % slots.size()];
            const std::size_t first = part * options.max_block_frames;
            slot.frames = std::min(options.max_block_frames, count - first);
            std::copy_n(block.interleaved.data() + first * channels, slot.frames * channels,
                        slot.samples.data());
            slot.first_frame = block.first_frame + first;
            slot.capture_time = block.capture_time;
            slot.discontinuity = part == 0 && block.discontinuity;
            slot.continuation = part > 0;
        }
        write_index.store(write + needed, std::memory_order_release);
        const auto used = write + needed - read;
        auto high = queue_high_water.load(std::memory_order_relaxed);
        while (used > high && !queue_high_water.compare_exchange_weak(high, used,
                                                                       std::memory_order_relaxed)) {}
        producer_has_frame = true;
        producer_next_frame = block.first_frame + count;
        return PushStatus::Accepted;
    }

    // ---------------- ingest worker ----------------
    std::size_t ingest_available(bool may_wait) {
        std::size_t processed = 0;
        while (!stop.stop_requested()) {
            const auto read = read_index.load(std::memory_order_relaxed);
            if (read == write_index.load(std::memory_order_acquire)) break;
            if (!ingest_slot(slots[read % slots.size()], may_wait)) break;
            read_index.store(read + 1, std::memory_order_release);
            ++processed;
        }
        if (input_lost.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(data_mutex);
            markers.push_back({MarkerKind::Interrupt, ring_end, ring_end, "input interrupted"});
            interrupted = true;
            ++processed;
        }
        if (processed) data_cv.notify_all();
        return processed;
    }

    bool ingest_slot(Slot& slot, bool may_wait) {
        const auto ingested_at = clock();
        std::unique_lock lock(data_mutex);
        if (!have_origin) {
            origin = slot.first_frame;
            have_origin = true;
        }
        const auto frame = slot.first_frame - origin;
        if (!slot.continuation && (frame != ring_end || slot.discontinuity || interrupted)) {
            std::string message = interrupted ? "input resumed" :
                frame > ring_end ? "input frames missing" : "input discontinuity";
            markers.push_back({MarkerKind::Gap, ring_end, frame, std::move(message)});
            {
                std::lock_guard metrics_lock(metrics_mutex);
                ++counters.discontinuities;
                counters.gap_frames += frame - ring_end;
            }
            if (recording_limit && recorded.size() < recording_limit) {
                const auto target = std::min<std::uint64_t>(frame, recording_limit);
                if (target > recorded.size()) recorded.resize(static_cast<std::size_t>(target), 0.0F);
                if (frame > recording_limit) recording_truncated = true;
            }
            ring_end = frame;
            interrupted = false;
            slot.discontinuity = false;
            data_cv.notify_all();
        }
        if (options.lag_policy == LagPolicy::Lossless) {
            while (ring_end + slot.frames > keep_from.load(std::memory_order_acquire) + ring.size()) {
                if (!may_wait || stop.stop_requested()) return false;
                data_cv.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
        std::uint64_t nonfinite = 0;
        const float scale = 1.0F / static_cast<float>(channels);
        for (std::size_t i = 0; i < slot.frames; ++i) {
            float sum = 0;
            for (std::size_t c = 0; c < channels; ++c) {
                const float sample = slot.samples[i * channels + c];
                if (std::isfinite(sample)) sum += sample;
                else ++nonfinite;
            }
            const float mono = channels == 1 ? sum : sum * scale;
            ring[static_cast<std::size_t>((ring_end + i) % ring.size())] = mono;
            if (recording_limit) {
                if (recorded.size() < recording_limit) recorded.push_back(mono);
                else recording_truncated = true;
            }
        }
        capture_map.emplace_back(ring_end, slot.capture_time);
        const auto oldest = ring_end > ring.size() ? ring_end - ring.size() : 0;
        while (capture_map.size() > 1 &&
               (capture_map.size() > 65536 || capture_map[1].first <= oldest)) {
            capture_map.pop_front();
        }
        ring_end += slot.frames;
        lock.unlock();
        std::lock_guard metrics_lock(metrics_mutex);
        counters.frames_received += slot.frames;
        counters.nonfinite_samples += nonfinite;
        if (slot.capture_time.count() != 0) capture_latency.add(seconds(ingested_at - slot.capture_time));
        return true;
    }

    // ---------------- events ----------------
    void emit(LiveEventKind kind, const Tracked* tracked, std::optional<LiveNote> previous = {},
              std::string message = {}, std::uint64_t gap_first = 0, std::uint64_t gap_end = 0) {
        LiveEvent event;
        event.kind = kind;
        if (tracked) {
            event.note = tracked->note;
            event.timing.onset_capture_time = tracked->onset_capture;
        }
        event.previous = std::move(previous);
        event.message = std::move(message);
        event.gap_first_frame = gap_first;
        event.gap_end_frame = gap_end;
        event.timing.emitted_time = clock();
        event.timing.evidence_end_frame = evidence_end;
        const auto onset_capture = event.timing.onset_capture_time;
        const auto emitted = event.timing.emitted_time;
        const auto onset = event.note.onset_frame;
        std::uint64_t evicted = 0;
        {
            std::lock_guard lock(events_mutex);
            event.sequence = ++sequence;
            if (tracked) {
                if (kind == LiveEventKind::NoteRetracted) notes.erase(tracked->serial);
                else notes[tracked->serial] = tracked->note;
                while (notes.size() > options.max_snapshot_notes) {
                    notes.erase(notes.begin());
                    ++evicted;
                }
            }
            if (events.size() >= options.event_queue_capacity) {
                events.pop_front();
                resync_required = true;
                ++dropped_since_poll;
                ++events_dropped;
            }
            events.push_back(std::move(event));
            events_high_water = std::max(events_high_water, events.size());
            ++events_emitted;
        }
        pending_notification = true;
        std::lock_guard metrics_lock(metrics_mutex);
        counters.snapshot_notes_evicted += evicted;
        if (kind == LiveEventKind::NoteOn) {
            if (onset_capture.count() != 0) end_to_end.add(seconds(emitted - onset_capture));
            algorithmic.add(seconds(evidence_end > onset ? evidence_end - onset : 0));
        } else if (kind == LiveEventKind::NoteStabilized && onset_capture.count() != 0) {
            stabilization.add(seconds(emitted - onset_capture));
        }
    }

    void notify_consumer() {
        if (pending_notification && options.events_available) {
            pending_notification = false;
            options.events_available();
        }
    }

    Nanoseconds capture_time_for(std::uint64_t frame) {
        std::lock_guard lock(data_mutex);
        auto it = std::upper_bound(capture_map.begin(), capture_map.end(), frame,
            [](std::uint64_t value, const auto& entry) { return value < entry.first; });
        if (it == capture_map.begin()) return {};
        return std::prev(it)->second;
    }

    Tracked make_note(int midi, std::uint64_t onset, double strength, bool truncated) {
        Tracked tracked;
        tracked.serial = ++serial;
        tracked.note.id = "live-" + std::to_string(tracked.serial);
        tracked.note.midi_pitch = midi;
        tracked.note.sample_rate = rate;
        tracked.note.onset_frame = onset;
        tracked.note.raw_strength = strength;
        tracked.note.source_model = model_id;
        tracked.note.detection_method = detection_method;
        tracked.note.onset_truncated = truncated;
        tracked.onset_capture = capture_time_for(onset);
        tracked.active_until = onset;
        return tracked;
    }

    void stabilize(Tracked& tracked) {
        if (tracked.note.state != EventLifecycle::Provisional) return;
        auto previous = tracked.note;
        tracked.note.state = EventLifecycle::Stabilized;
        ++tracked.note.revision;
        emit(LiveEventKind::NoteStabilized, &tracked, previous);
    }

    void retract(Tracked& tracked, const std::string& reason) {
        auto previous = tracked.note;
        tracked.note.state = EventLifecycle::Rejected;
        ++tracked.note.revision;
        emit(LiveEventKind::NoteRetracted, &tracked, previous, reason);
        retired.emplace_back(previous.midi_pitch, previous.onset_frame);
        if (retired.size() > 256) retired.pop_front();
    }

    void close(Tracked& tracked, std::uint64_t frame) {
        auto previous = tracked.note;
        tracked.note.offset_frame = std::max(frame, tracked.note.onset_frame + 1);
        ++tracked.note.revision;
        emit(LiveEventKind::NoteOff, &tracked, previous);
    }

    // Segment-final decision for a note that can receive no further evidence.
    void settle(Tracked& tracked, std::uint64_t minimum_frames, bool allow_stabilize) {
        if (tracked.note.state != EventLifecycle::Provisional) return;
        const auto end = tracked.note.offset_frame.value_or(tracked.active_until);
        if (allow_stabilize && end >= tracked.note.onset_frame + minimum_frames) stabilize(tracked);
        else retract(tracked, "insufficient evidence before segment end");
    }

    // ---------------- incremental YIN ----------------
    void reset_yin() {
        auto realtime = options.yin.realtime;
        realtime.input_sample_rate = rate;
        yin = std::make_unique<RealtimeTranscriber>(realtime);
        yin_active.reset();
    }

    void yin_close(std::uint64_t frame) {
        auto& tracked = *yin_active;
        const auto end = std::max(frame, tracked.note.onset_frame + 1);
        tracked.active_until = end;
        if (tracked.note.state == EventLifecycle::Provisional &&
            end - tracked.note.onset_frame < frames(options.yin.minimum_duration_seconds)) {
            retract(tracked, "shorter than minimum duration before stabilization");
        } else {
            stabilize(tracked);
            close(tracked, end);
        }
        yin_active.reset();
    }

    void yin_events() {
        for (const auto& event : yin->take_events()) {
            const auto offset = static_cast<std::uint64_t>(std::max<long long>(
                0, std::llround(event.time_seconds * rate)));
            const auto frame = std::min(segment_start + offset, std::max(evidence_end, segment_start));
            if (event.type == NoteEventType::Onset) {
                if (yin_active) yin_close(frame);
                yin_active = make_note(event.midi_pitch, frame, event.confidence,
                                       segment_after_gap && frame == segment_start);
                {
                    std::lock_guard lock(metrics_mutex);
                    counters.tracked_notes_high_water = std::max<std::size_t>(counters.tracked_notes_high_water, 1);
                }
                emit(LiveEventKind::NoteOn, &*yin_active);
            } else if (yin_active && yin_active->note.midi_pitch == event.midi_pitch) {
                yin_close(frame);
            }
        }
        if (yin_active && yin_active->note.state == EventLifecycle::Provisional &&
            evidence_end >= yin_active->note.onset_frame + frames(options.yin.stabilization_seconds)) {
            stabilize(*yin_active);
        }
    }

    bool copy_audio(std::uint64_t first, std::uint64_t end, std::vector<float>& output) {
        output.resize(static_cast<std::size_t>(end - first));
        std::lock_guard lock(data_mutex);
        if (end > ring_end || first + ring.size() < ring_end) return false;
        for (std::uint64_t f = first; f < end; ++f) {
            output[static_cast<std::size_t>(f - first)] = ring[static_cast<std::size_t>(f % ring.size())];
        }
        return true;
    }

    bool yin_step(std::uint64_t limit) {
        if (position >= limit) return false;
        const auto end = std::min(limit, position + std::max<std::uint64_t>(256, options.max_block_frames));
        if (!copy_audio(position, end, scratch.samples)) {
            skip_to(limit, "rolling window overwritten before analysis");
            return true;
        }
        timed_step([&] {
            yin->push_samples(scratch.samples);
            position = end;
            evidence_end = end;
            analysed_frames.store(end, std::memory_order_relaxed);
            yin_events();
        });
        publish_keep_from();
        return true;
    }

    // ---------------- overlapping windows ----------------
    std::vector<Candidate> infer_window(std::uint64_t first, std::uint64_t end, bool& ok) {
        ok = false;
        scratch.sample_rate = rate;
        if (!copy_audio(first, end, scratch.samples)) {
            skip_to(end, "rolling window overwritten before analysis");
            ok = true;
            return {};
        }
        auto request = options.window.request;
        request.cancellation = stop.get_token();
        request.retain_evidence = options.window.temporal_decoding;
        request.onnx.execution_provider = options.model_load.provider;
        request.onnx.device_id = options.model_load.device_id;
        auto result = model->infer(scratch, request);
        if (const auto* failure = std::get_if<InferenceError>(&result)) {
            if (failure->code != InferenceErrorCode::Cancelled || !stop.stop_requested()) {
                model_failure(failure->message);
            }
            return {};
        }
        auto& output = std::get<InferenceOutput>(result);
        std::vector<Candidate> candidates;
        if (options.window.temporal_decoding) {
            const auto decoded = decode_note_events(scratch, output.evidence, options.window.decoding,
                                                    options.audio_source_id, model_id);
            for (const auto& event : decoded.events) {
                double strength = 0;
                for (const auto& evidence : event.evidence) strength = std::max(strength, evidence.raw_strength);
                candidates.push_back({static_cast<int>(std::lround(event.midi_pitch)),
                                      first + event.onset_sample, first + event.offset_sample, strength});
            }
        } else {
            for (const auto& note : output.notes) {
                const auto onset = first + frames(note.start_seconds);
                candidates.push_back({note.midi_pitch, onset,
                                      onset + frames(note.duration_seconds), note.confidence});
            }
        }
        for (auto& candidate : candidates) {
            candidate.onset = std::min(candidate.onset, end - 1);
            candidate.offset = std::clamp(candidate.offset, candidate.onset + 1, end);
        }
        consecutive_failures = 0;
        ok = true;
        return candidates;
    }

    void update(Tracked& tracked, const Candidate& c, std::uint64_t end, bool final_window) {
        tracked.seen = true;
        ++tracked.confirmations;
        tracked.note.raw_strength = std::max(tracked.note.raw_strength, c.strength);
        const auto previous = tracked.note;
        const auto cap = window_frames / 2;
        const auto context = [&](std::uint64_t frame) {
            return final_window ? cap : std::min(end - std::min(end, frame), cap);
        };
        const auto correction = frames(options.window.correction_seconds);
        bool changed = false, reopened = false, closed = false;
        if (!c.truncated && context(c.onset) > tracked.onset_context) {
            if (distance(c.onset, tracked.note.onset_frame) >= std::max<std::uint64_t>(1, correction)) {
                tracked.note.onset_frame = c.onset;
                tracked.onset_capture = capture_time_for(c.onset);
                changed = true;
            }
            tracked.onset_context = context(c.onset);
        }
        if (c.open) {
            tracked.active_until = std::max(tracked.active_until, end - std::min(end, right_guard));
            if (tracked.note.offset_frame) {
                tracked.note.offset_frame.reset();
                tracked.offset_context = 0;
                changed = reopened = true;
            }
        } else {
            tracked.active_until = std::max(tracked.active_until, c.offset);
            if (!tracked.note.offset_frame) {
                tracked.note.offset_frame = c.offset;
                tracked.offset_context = context(c.offset);
                closed = true;
            } else if (context(c.offset) > tracked.offset_context) {
                if (distance(c.offset, *tracked.note.offset_frame) >= std::max<std::uint64_t>(1, correction)) {
                    tracked.note.offset_frame = c.offset;
                    changed = true;
                }
                tracked.offset_context = context(c.offset);
            }
        }
        if (tracked.note.offset_frame && *tracked.note.offset_frame <= tracked.note.onset_frame) {
            tracked.note.offset_frame = tracked.note.onset_frame + 1;
        }
        if (closed || changed) {
            ++tracked.note.revision;
            emit(closed ? LiveEventKind::NoteOff : LiveEventKind::NoteCorrected, &tracked, previous,
                 reopened ? "reopened by later evidence" : closed ? "" : "timing refined with more context");
        }
        if (tracked.confirmations >= options.window.confirmations) stabilize(tracked);
    }

    void reconcile(std::vector<Candidate>& candidates, std::uint64_t first, std::uint64_t end,
                   bool final_window) {
        const bool segment_head = first == segment_start;
        const auto trusted_start = segment_head ? first : first + left_guard;
        const auto trusted_end = final_window ? end : end - std::min(end - first, right_guard);
        const auto match = frames(options.window.onset_match_seconds);
        const auto seam = frames(options.window.seam_merge_seconds);
        for (auto& c : candidates) {
            c.truncated = !segment_head && c.onset < first + left_guard;
            c.open = !final_window && c.offset + right_guard > end;
        }
        for (auto& tracked : this->tracked) tracked.seen = false;

        struct Pair { std::uint64_t cost; std::size_t candidate, note; bool hidden_onset; };
        std::vector<Pair> pairs;
        for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
            const auto& c = candidates[ci];
            for (std::size_t ti = 0; ti < tracked.size(); ++ti) {
                const auto& note = tracked[ti].note;
                if (note.midi_pitch != c.midi) continue;
                const auto note_end = note.offset_frame.value_or(no_frame);
                const auto onset_distance = distance(c.onset, note.onset_frame);
                const bool overlaps = c.offset >= note.onset_frame &&
                    (note_end == no_frame || c.onset <= note_end + seam);
                if (onset_distance <= match) {
                    pairs.push_back({onset_distance, ci, ti, false});
                } else if (c.truncated && overlaps) {
                    pairs.push_back({match + 1 + onset_distance, ci, ti, false});
                } else if (overlaps && note.onset_frame < trusted_start) {
                    // The note's attack precedes this window, so the decoder may place a later
                    // onset inside it; continue the existing note instead of duplicating it.
                    pairs.push_back({2 * (match + 1) + onset_distance, ci, ti, true});
                }
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(),
                         [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
        std::vector<bool> note_matched(tracked.size(), false);
        for (const auto& pair : pairs) {
            if (candidates[pair.candidate].used || note_matched[pair.note]) continue;
            candidates[pair.candidate].used = true;
            if (pair.hidden_onset) candidates[pair.candidate].truncated = true;
            note_matched[pair.note] = true;
            update(tracked[pair.note], candidates[pair.candidate], end, final_window);
        }

        std::vector<bool> remove(tracked.size(), false);
        for (std::size_t ti = 0; ti < tracked.size(); ++ti) {
            if (note_matched[ti]) continue;
            auto& t = tracked[ti];
            const auto onset = t.note.onset_frame;
            const bool in_view = onset >= trusted_start && onset < trusted_end;
            if (t.note.state == EventLifecycle::Provisional && in_view) {
                Candidate* replacement = nullptr;
                for (auto& c : candidates) {
                    if (c.used || c.truncated || c.midi == t.note.midi_pitch ||
                        distance(c.onset, onset) > match) continue;
                    if (!replacement || distance(c.onset, onset) < distance(replacement->onset, onset)) {
                        replacement = &c;
                    }
                }
                if (replacement) {
                    replacement->used = true;
                    const auto previous = t.note;
                    t.note.midi_pitch = replacement->midi;
                    t.note.offset_frame = replacement->open ? std::nullopt :
                        std::optional<std::uint64_t>(replacement->offset);
                    t.active_until = replacement->open ? end - std::min(end, right_guard) : replacement->offset;
                    t.confirmations = 1;
                    t.seen = true;
                    ++t.note.revision;
                    emit(LiveEventKind::NoteCorrected, &t, previous, "pitch corrected by later window");
                    if (t.confirmations >= options.window.confirmations) stabilize(t);
                } else {
                    retract(t, "not reproduced by a later overlapping window");
                    remove[ti] = true;
                }
            } else if (!t.note.offset_frame && onset < trusted_end) {
                close(t, t.active_until);
            }
        }

        for (auto& c : candidates) {
            if (c.used) continue;
            const bool after_gap_head = segment_head && segment_after_gap &&
                c.onset < segment_start + left_guard;
            if (c.truncated) continue;
            const bool blocked = std::any_of(retired.begin(), retired.end(), [&](const auto& r) {
                return r.first == c.midi && distance(r.second, c.onset) <= match;
            });
            if (blocked) continue;
            if (tracked.size() >= options.max_tracked_notes) {
                std::lock_guard lock(metrics_mutex);
                ++counters.tracked_notes_refused;
                continue;
            }
            auto note = make_note(c.midi, c.onset, c.strength, after_gap_head);
            note.confirmations = 1;
            note.seen = true;
            note.onset_context = final_window ? window_frames / 2 :
                std::min(end - c.onset, window_frames / 2);
            tracked.push_back(std::move(note));
            remove.push_back(false);
            auto& t = tracked.back();
            emit(LiveEventKind::NoteOn, &t);
            if (c.open) {
                t.active_until = end - std::min(end, right_guard);
            } else {
                t.active_until = c.offset;
                t.offset_context = final_window ? window_frames / 2 :
                    std::min(end - c.offset, window_frames / 2);
                close(t, c.offset);
            }
            if (t.confirmations >= options.window.confirmations) stabilize(t);
        }

        const auto next_first = final_window ? no_frame :
            std::max(segment_start, end + hop > window_frames ? end + hop - window_frames : 0);
        for (std::size_t ti = 0; ti < tracked.size(); ++ti) {
            auto& t = tracked[ti];
            if (remove[ti] || !t.note.offset_frame || final_window) continue;
            if (*t.note.offset_frame + seam < next_first) {
                if (t.note.state == EventLifecycle::Provisional) {
                    retract(t, "too few overlapping-window confirmations");
                }
                remove[ti] = true;
            }
        }
        std::size_t write = 0;
        for (std::size_t ti = 0; ti < tracked.size(); ++ti) {
            if (!remove[ti]) {
                if (write != ti) tracked[write] = std::move(tracked[ti]);
                ++write;
            }
        }
        tracked.resize(write);
        std::erase_if(retired, [&](const auto& r) {
            return final_window || r.second + match < next_first;
        });
        std::lock_guard lock(metrics_mutex);
        counters.tracked_notes_high_water = std::max(counters.tracked_notes_high_water, tracked.size());
    }

    bool run_window(std::uint64_t first, std::uint64_t end, bool final_window) {
        bool ok = false;
        timed_step([&] {
            auto candidates = infer_window(first, end, ok);
            if (!ok || failed || segment_start > first) return;
            evidence_end = end;
            reconcile(candidates, first, end, final_window);
        });
        return ok;
    }

    bool window_step(std::uint64_t limit) {
        if (next_end > limit) return false;
        auto target = next_end;
        if (options.lag_policy == LagPolicy::SkipAhead) {
            const auto latest = next_end + (limit - next_end) / hop * hop;
            const auto reach = std::max(next_end, last_end + max_jump);
            target = std::min(latest, next_end + (reach - next_end) / hop * hop);
        }
        const auto first = std::max(segment_start, target > window_frames ? target - window_frames : 0);
        if (!run_window(first, target, false)) return true;
        if (failed || last_end > target || segment_start > first) return true;
        {
            std::lock_guard lock(metrics_mutex);
            counters.windows_coalesced += (target - next_end) / hop;
        }
        last_end = target;
        next_end = target + hop;
        analysed_frames.store(target, std::memory_order_relaxed);
        publish_keep_from();
        return true;
    }

    // ---------------- segments, lag and failures ----------------
    void publish_keep_from() {
        std::uint64_t keep = failed ? no_frame / 2 :
            options.backend == LiveBackendKind::IncrementalYin ? position :
            std::max(segment_start, last_end > window_frames + hop ? last_end - window_frames - hop : 0);
        keep_from.store(keep, std::memory_order_release);
        data_cv.notify_all();
    }

    template<class Function>
    void timed_step(Function&& function) {
        const auto started = clock();
        if (options.analysis_hook) options.analysis_hook(step);
        ++step;
        try {
            function();
        } catch (const std::exception& error) {
            model_failure(error.what());
        }
        const auto elapsed = seconds(clock() - started);
        std::lock_guard lock(metrics_mutex);
        ++counters.analysis_steps;
        counters.analysis_busy_seconds += elapsed;
        analysis_latency.add(elapsed);
    }

    void close_open_notes(std::uint64_t end, bool allow_stabilize) {
        const auto minimum = options.backend == LiveBackendKind::IncrementalYin ?
            frames(options.yin.minimum_duration_seconds) : frames(options.window.minimum_duration_seconds);
        if (options.backend == LiveBackendKind::IncrementalYin) {
            if (yin_active) yin_close(std::min(end, std::max(evidence_end, yin_active->note.onset_frame + 1)));
            return;
        }
        for (auto& t : tracked) {
            if (!t.note.offset_frame) {
                if (t.note.state == EventLifecycle::Provisional &&
                    t.active_until < t.note.onset_frame + minimum) {
                    retract(t, "segment ended before stabilization");
                    continue;
                }
                close(t, std::min(std::max(t.active_until, t.note.onset_frame + 1), std::max(end, t.note.onset_frame + 1)));
            }
            settle(t, minimum, allow_stabilize && t.seen);
        }
        tracked.clear();
        retired.clear();
    }

    // Finish the current segment at end, analysing remaining audio when analyse is true.
    void end_segment(std::uint64_t end, bool analyse) {
        if (!segment_open) return;
        if (!failed && analyse) {
            if (options.backend == LiveBackendKind::IncrementalYin) {
                while (!failed && position < end && yin_step(end)) {}
                if (!failed) {
                    timed_step([&] {
                        yin->finish();
                        evidence_end = std::max(evidence_end, end);
                        yin_events();
                    });
                }
            } else {
                const auto first = std::max(segment_start, end > window_frames ? end - window_frames : 0);
                if (end > first + frames(0.05)) {
                    while (!failed && !stop.stop_requested() && !run_window(first, end, true)) {}
                }
            }
        }
        if (!failed) close_open_notes(end, true);
        segment_open = false;
    }

    void begin_segment(std::uint64_t start, bool after_gap) {
        segment_start = start;
        position = start;
        last_end = start;
        next_end = start + hop;
        evidence_end = start;
        segment_after_gap = after_gap;
        segment_open = true;
        analysed_frames.store(start, std::memory_order_relaxed);
        if (options.backend == LiveBackendKind::IncrementalYin) reset_yin();
        publish_keep_from();
    }

    void skip_to(std::uint64_t target, const std::string& reason) {
        const auto analysed = options.backend == LiveBackendKind::IncrementalYin ? position : last_end;
        end_segment(analysed, false);
        {
            std::lock_guard lock(metrics_mutex);
            counters.skipped_frames += target - std::min(target, analysed);
        }
        emit(LiveEventKind::AnalysisGap, nullptr, std::nullopt, reason, analysed, target);
        begin_segment(target, true);
    }

    void model_failure(const std::string& message) {
        {
            std::lock_guard lock(error_mutex);
            error = message;
        }
        {
            std::lock_guard lock(metrics_mutex);
            ++counters.model_errors;
        }
        emit(LiveEventKind::Status, nullptr, std::nullopt, "analysis error: " + message);
        if (++consecutive_failures > options.max_model_recoveries) {
            fail_analysis("analysis failed after " + std::to_string(consecutive_failures) +
                          " consecutive errors: " + message);
            return;
        }
        if (options.backend == LiveBackendKind::IncrementalYin) {
            const auto resume = position;
            close_open_notes(resume, false);
            begin_segment(resume, true);
        } else {
            model->release();
            const auto loaded = model->load(options.model_load);
            if (const auto* failure = std::get_if<InferenceError>(&loaded)) {
                model_failure("model reload failed: " + failure->message);
                return;
            }
        }
        std::lock_guard lock(metrics_mutex);
        ++counters.model_recoveries;
    }

    void fail_analysis(const std::string& message) {
        if (failed) return;
        const auto end = options.backend == LiveBackendKind::IncrementalYin ? position : last_end;
        failed = true;
        close_open_notes(std::max(end, evidence_end), false);
        segment_open = false;
        {
            std::lock_guard lock(error_mutex);
            error = message;
        }
        state.store(LiveState::AnalysisFailed, std::memory_order_release);
        emit(LiveEventKind::Status, nullptr, std::nullopt, message);
        publish_keep_from();
    }

    bool analyse_ready() {
        bool progress = false;
        while (!failed && !stop.stop_requested()) {
            std::uint64_t available;
            std::optional<Marker> marker;
            {
                std::lock_guard lock(data_mutex);
                available = ring_end;
                if (!markers.empty()) marker = markers.front();
            }
            const auto limit = marker ? marker->end_frame : available;
            const auto analysed = options.backend == LiveBackendKind::IncrementalYin ? position : last_end;
            if (segment_open && limit > analysed) {
                std::lock_guard lock(metrics_mutex);
                counters.maximum_lag_frames = std::max(counters.maximum_lag_frames, limit - analysed);
            }
            if (segment_open && options.lag_policy == LagPolicy::SkipAhead && limit > analysed &&
                limit - analysed > lag_limit) {
                skip_to(limit, "analysis fell behind live input");
                progress = true;
                continue;
            }
            bool stepped = false;
            if (segment_open) {
                stepped = options.backend == LiveBackendKind::IncrementalYin ?
                    yin_step(limit) : window_step(limit);
            }
            if (stepped) {
                progress = true;
                notify_consumer();
                continue;
            }
            if (!marker) break;
            end_segment(marker->end_frame, true);
            if (failed) break;
            {
                std::lock_guard lock(data_mutex);
                markers.pop_front();
            }
            if (marker->kind == MarkerKind::Interrupt) {
                state.store(LiveState::InputInterrupted, std::memory_order_release);
                emit(LiveEventKind::Status, nullptr, std::nullopt, marker->message);
                segment_start = marker->end_frame;
            } else {
                auto expected = LiveState::InputInterrupted;
                state.compare_exchange_strong(expected, LiveState::Running);
                if (marker->resume_frame > marker->end_frame) {
                    emit(LiveEventKind::AnalysisGap, nullptr, std::nullopt, marker->message,
                         marker->end_frame, marker->resume_frame);
                } else {
                    emit(LiveEventKind::Status, nullptr, std::nullopt, marker->message);
                }
                begin_segment(marker->resume_frame, true);
            }
            progress = true;
            notify_consumer();
        }
        notify_consumer();
        return progress;
    }

    void finalize() {
        while (analyse_ready()) {}
        if (!failed && !stop.stop_requested()) {
            std::uint64_t end;
            {
                std::lock_guard lock(data_mutex);
                end = ring_end;
            }
            end_segment(end, true);
            emit(LiveEventKind::Status, nullptr, std::nullopt, "finished");
        }
        notify_consumer();
    }

    void ingest_loop() {
        while (!stop.stop_requested()) {
            const bool exiting = ingest_exit.load(std::memory_order_acquire);
            if (ingest_available(true) == 0) {
                if (exiting) break;
                std::this_thread::sleep_for(options.ingest_poll_interval);
            }
        }
    }

    void analysis_loop() {
        while (!stop.stop_requested()) {
            const bool exiting = analysis_exit.load(std::memory_order_acquire);
            const bool progress = analyse_ready();
            if (exiting && !progress) {
                finalize();
                break;
            }
            if (!progress) {
                std::unique_lock lock(data_mutex);
                data_cv.wait_for(lock, std::chrono::milliseconds(5));
            }
        }
    }

    void join() noexcept {
        try {
            if (ingest_thread.joinable()) ingest_thread.join();
            if (analysis_thread.joinable()) analysis_thread.join();
        } catch (...) {
        }
    }

    LiveOptions options;
    std::unique_ptr<ModelBackend> model;
    MonotonicClock clock;
    std::uint32_t rate{};
    std::uint16_t channels{};
    std::uint64_t hop{}, window_frames{}, left_guard{}, right_guard{}, max_jump{}, lag_limit{};
    std::string model_id, detection_method;

    std::vector<Slot> slots;
    std::atomic<std::size_t> write_index{0}, read_index{0}, queue_high_water{0};
    bool producer_has_frame{};
    std::uint64_t producer_next_frame{};
    std::atomic<bool> closed{false}, input_lost{false}, ingest_exit{false}, analysis_exit{false};
    std::atomic<std::uint64_t> blocks_refused{0}, frames_refused{0}, blocks_rejected{0};

    mutable std::mutex data_mutex;
    std::condition_variable data_cv;
    std::vector<float> ring;
    std::uint64_t ring_end{};
    std::vector<float> recorded;
    std::size_t recording_limit{};
    bool recording_truncated{};
    std::deque<std::pair<std::uint64_t, Nanoseconds>> capture_map;
    std::deque<Marker> markers;
    bool have_origin{}, interrupted{};
    std::uint64_t origin{};
    std::atomic<std::uint64_t> keep_from{0};

    mutable std::mutex metrics_mutex;
    Reservoir capture_latency, analysis_latency, end_to_end, stabilization, algorithmic;
    LiveMetrics counters;

    mutable std::mutex events_mutex;
    std::deque<LiveEvent> events;
    std::map<std::uint64_t, LiveNote> notes;
    std::uint64_t sequence{}, dropped_since_poll{}, events_dropped{}, events_emitted{};
    std::size_t events_high_water{};
    bool resync_required{};

    std::atomic<LiveState> state{LiveState::Created};
    mutable std::mutex error_mutex;
    std::string error;
    std::stop_source stop;
    std::thread ingest_thread, analysis_thread;
    std::mutex control_mutex;

    // Analysis-thread state.
    std::uint64_t segment_start{}, position{}, last_end{}, next_end{}, evidence_end{}, step{}, serial{};
    bool segment_open{true}, segment_after_gap{}, failed{}, pending_notification{};
    std::size_t consecutive_failures{};
    std::atomic<std::uint64_t> analysed_frames{0};
    std::unique_ptr<RealtimeTranscriber> yin;
    std::optional<Tracked> yin_active;
    std::vector<Tracked> tracked;
    std::deque<std::pair<int, std::uint64_t>> retired;
    AudioBuffer scratch;
};

LiveTranscriptionSession::LiveTranscriptionSession(LiveOptions options,
                                                   std::unique_ptr<ModelBackend> model)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(model))) {}

LiveTranscriptionSession::~LiveTranscriptionSession() {
    if (impl_->ingest_thread.joinable() || impl_->analysis_thread.joinable()) cancel();
}

ModelOperation LiveTranscriptionSession::start() {
    std::lock_guard control(impl_->control_mutex);
    if (impl_->state.load() != LiveState::Created) {
        return InferenceError{InferenceErrorCode::InvalidArgument, "Live session was already started"};
    }
    if (impl_->model && !impl_->model->resources().loaded) {
        const auto loaded = impl_->model->load(impl_->options.model_load);
        if (const auto* failure = std::get_if<InferenceError>(&loaded)) {
            std::lock_guard lock(impl_->error_mutex);
            impl_->error = failure->message;
            return *failure;
        }
    }
    impl_->state.store(LiveState::Running);
    if (impl_->options.threaded) {
        impl_->ingest_thread = std::thread([this] { impl_->ingest_loop(); });
        impl_->analysis_thread = std::thread([this] { impl_->analysis_loop(); });
    }
    return std::monostate{};
}

PushStatus LiveTranscriptionSession::push(const AudioFrameBlock& block) noexcept {
    return impl_->push(block);
}

void LiveTranscriptionSession::report_input_lost() noexcept {
    impl_->input_lost.store(true, std::memory_order_release);
}

void LiveTranscriptionSession::pump() {
    if (impl_->options.threaded) throw std::logic_error("pump() is only available when threaded is false");
    const auto state = impl_->state.load();
    if (state == LiveState::Created) throw std::logic_error("Call start() before pump()");
    if (state == LiveState::Finished || state == LiveState::Cancelled) return;
    for (;;) {
        const auto ingested = impl_->ingest_available(false);
        const auto analysed = impl_->analyse_ready();
        if (ingested == 0 && !analysed) break;
    }
}

void LiveTranscriptionSession::finish() {
    std::lock_guard control(impl_->control_mutex);
    const auto state = impl_->state.load();
    if (state == LiveState::Finished || state == LiveState::Cancelled) return;
    if (state == LiveState::Created) throw std::logic_error("Call start() before finish()");
    impl_->closed.store(true, std::memory_order_release);
    if (impl_->options.threaded) {
        impl_->ingest_exit.store(true, std::memory_order_release);
        if (impl_->ingest_thread.joinable()) impl_->ingest_thread.join();
        impl_->analysis_exit.store(true, std::memory_order_release);
        impl_->data_cv.notify_all();
        if (impl_->analysis_thread.joinable()) impl_->analysis_thread.join();
    } else {
        for (;;) {
            const auto ingested = impl_->ingest_available(false);
            const auto analysed = impl_->analyse_ready();
            if (ingested == 0 && !analysed) break;
        }
        impl_->finalize();
    }
    if (!impl_->failed && !impl_->stop.stop_requested()) impl_->state.store(LiveState::Finished);
}

void LiveTranscriptionSession::cancel() noexcept {
    {
        const auto state = impl_->state.load();
        if (state == LiveState::Finished || state == LiveState::Cancelled) return;
    }
    // Stop first so a concurrent finish() aborts inference instead of holding the control lock.
    impl_->closed.store(true, std::memory_order_release);
    impl_->stop.request_stop();
    impl_->data_cv.notify_all();
    std::lock_guard control(impl_->control_mutex);
    const auto state = impl_->state.load();
    if (state == LiveState::Finished || state == LiveState::Cancelled) return;
    impl_->join();
    impl_->state.store(LiveState::Cancelled);
    try {
        impl_->emit(LiveEventKind::Status, nullptr, std::nullopt, "cancelled");
    } catch (...) {
    }
}

EventBatch LiveTranscriptionSession::poll_events(std::size_t maximum) {
    EventBatch batch;
    std::lock_guard lock(impl_->events_mutex);
    const auto count = std::min(maximum, impl_->events.size());
    batch.events.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        batch.events.push_back(std::move(impl_->events.front()));
        impl_->events.pop_front();
    }
    batch.resync_required = impl_->resync_required;
    batch.dropped_events = impl_->dropped_since_poll;
    impl_->resync_required = false;
    impl_->dropped_since_poll = 0;
    return batch;
}

std::vector<LiveNote> LiveTranscriptionSession::snapshot() const {
    std::lock_guard lock(impl_->events_mutex);
    std::vector<LiveNote> result;
    result.reserve(impl_->notes.size());
    for (const auto& [serial, note] : impl_->notes) result.push_back(note);
    return result;
}

LiveMetrics LiveTranscriptionSession::metrics() const {
    LiveMetrics result;
    {
        std::lock_guard lock(impl_->metrics_mutex);
        result = impl_->counters;
        result.capture_latency = impl_->capture_latency.summary();
        result.analysis_latency = impl_->analysis_latency.summary();
        result.end_to_end_latency = impl_->end_to_end.summary();
        result.stabilization_delay = impl_->stabilization.summary();
        result.algorithmic_latency = impl_->algorithmic.summary();
    }
    {
        std::lock_guard lock(impl_->events_mutex);
        result.event_queue_capacity = impl_->options.event_queue_capacity;
        result.event_queue_high_water = impl_->events_high_water;
        result.events_emitted = impl_->events_emitted;
        result.events_dropped = impl_->events_dropped;
    }
    {
        std::lock_guard lock(impl_->data_mutex);
        result.rolling_capacity_frames = impl_->ring.size();
        result.recording_frames = impl_->recorded.size();
        result.recording_truncated = impl_->recording_truncated;
    }
    result.blocks_refused = impl_->blocks_refused.load(std::memory_order_relaxed);
    result.frames_refused = impl_->frames_refused.load(std::memory_order_relaxed);
    result.blocks_rejected = impl_->blocks_rejected.load(std::memory_order_relaxed);
    result.input_queue_capacity = impl_->slots.size();
    result.input_queue_high_water = impl_->queue_high_water.load(std::memory_order_relaxed);
    result.analysed_audio_seconds = impl_->seconds(impl_->analysed_frames.load(std::memory_order_relaxed));
    return result;
}

LiveState LiveTranscriptionSession::state() const noexcept { return impl_->state.load(); }

std::string LiveTranscriptionSession::last_error() const {
    std::lock_guard lock(impl_->error_mutex);
    return impl_->error;
}

std::uint64_t LiveTranscriptionSession::ingested_frames() const {
    std::lock_guard lock(impl_->data_mutex);
    return impl_->ring_end;
}

AudioBuffer LiveTranscriptionSession::recording() const {
    std::lock_guard lock(impl_->data_mutex);
    return {impl_->rate, impl_->recorded};
}

AudioBuffer LiveTranscriptionSession::recording_tail(std::size_t maximum_frames) const {
    std::lock_guard lock(impl_->data_mutex);
    const auto count = std::min(maximum_frames, impl_->recorded.size());
    const auto first = impl_->recorded.size() - count;
    return {impl_->rate, std::vector<float>(
                             impl_->recorded.begin() +
                                 static_cast<std::ptrdiff_t>(first),
                             impl_->recorded.end())};
}

const LiveOptions& LiveTranscriptionSession::options() const noexcept { return impl_->options; }

} // namespace transcribe::live
