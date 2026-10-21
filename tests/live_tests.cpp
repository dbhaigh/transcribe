#include "transcribe/live.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

using namespace transcribe;
using namespace transcribe::live;

namespace {

struct Truth {
    int midi;
    double onset;
    double offset;
};

// Ideal scripted backend. Audio samples carry their absolute frame index, so the backend knows
// where each window lies and returns the truth clipped to that window.
class FakeBackend final : public ModelBackend {
public:
    FakeBackend(std::vector<Truth> truth, std::uint32_t rate) : truth_(std::move(truth)), rate_(rate) {
        metadata_.definition.id = "test:fake-window";
        metadata_.polyphonic = true;
        metadata_.maximum_polyphony = 16;
        metadata_.minimum_midi = 0;
        metadata_.maximum_midi = 127;
    }
    const ModelMetadata& metadata() const noexcept override { return metadata_; }
    ModelOperation load(const ModelLoadOptions&) override {
        ++loads;
        loaded_ = true;
        return std::monostate{};
    }
    void release() noexcept override { loaded_ = false; }
    InferenceResult<InferenceOutput> infer(const AudioBuffer& audio, const InferenceRequest& request) override {
        if (!loaded_) return InferenceError{InferenceErrorCode::NotLoaded, "not loaded"};
        if (request.cancellation.stop_requested()) return InferenceError{InferenceErrorCode::Cancelled, "cancelled"};
        ++calls;
        if (fail && fail(calls)) return InferenceError{InferenceErrorCode::Inference, "scripted failure"};
        if (delay.count()) std::this_thread::sleep_for(delay);
        const auto first = static_cast<std::uint64_t>(audio.samples.front());
        const auto end = first + audio.samples.size();
        InferenceOutput output;
        for (const auto& note : truth_) {
            const auto on = static_cast<std::uint64_t>(std::llround(note.onset * rate_));
            const auto off = static_cast<std::uint64_t>(std::llround(note.offset * rate_));
            if (on >= end || off <= first) continue;
            const auto a = std::max(on, first), b = std::min(off, end);
            output.notes.push_back({note.midi, double(a - first) / rate_, double(b - a) / rate_, "", 0.8, 0});
        }
        if (perturb) perturb(calls, first, end, output.notes);
        return output;
    }
    ModelOperation warm_up(const InferenceRequest&) override { return std::monostate{}; }
    ModelResources resources() const noexcept override {
        ModelResources resources;
        resources.loaded = loaded_;
        return resources;
    }
    BackendDiagnostics diagnostics() const override { return {}; }

    std::function<bool(std::size_t)> fail;
    std::function<void(std::size_t, std::uint64_t, std::uint64_t, std::vector<Note>&)> perturb;
    std::chrono::milliseconds delay{0};
    std::size_t calls{};
    std::size_t loads{};

private:
    std::vector<Truth> truth_;
    std::uint32_t rate_;
    bool loaded_{};
    ModelMetadata metadata_;
};

constexpr std::uint32_t fake_rate = 8000;

const std::vector<Truth> fake_truth{
    {60, 0.30, 0.80}, {67, 0.50, 5.50}, {64, 1.00, 1.40}, {64, 1.50, 1.90}, {72, 6.00, 6.04}};

LiveOptions window_options(std::uint32_t rate = fake_rate) {
    LiveOptions options;
    options.format = {rate, 1};
    options.backend = LiveBackendKind::OverlappingWindow;
    options.window.temporal_decoding = false;
    options.lag_policy = LagPolicy::Lossless;
    options.threaded = false;
    return options;
}

std::vector<float> frame_index_audio(std::size_t frames, std::size_t first = 0) {
    std::vector<float> audio(frames);
    for (std::size_t i = 0; i < frames; ++i) audio[i] = static_cast<float>(first + i);
    return audio;
}

struct Replay {
    std::vector<LiveEvent> events;
    std::vector<LiveNote> notes;
    LiveMetrics metrics;
    LiveState state{};
    AudioBuffer recording;
};

Replay replay(LiveOptions options, std::unique_ptr<ModelBackend> model, const std::vector<float>& audio,
              std::size_t block, std::uint64_t first_frame = 0) {
    LiveTranscriptionSession session(std::move(options), std::move(model));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    Replay result;
    const auto channels = session.options().format.channels;
    for (std::size_t offset = 0; offset < audio.size(); offset += block * channels) {
        const auto count = std::min(block * channels, audio.size() - offset);
        AudioFrameBlock frames{session.options().format, first_frame + offset / channels,
                               std::span(audio).subspan(offset, count), Nanoseconds{1 + offset}, false};
        REQUIRE(session.push(frames) == PushStatus::Accepted);
        session.pump();
        auto batch = session.poll_events();
        result.events.insert(result.events.end(), batch.events.begin(), batch.events.end());
    }
    session.finish();
    auto batch = session.poll_events();
    result.events.insert(result.events.end(), batch.events.begin(), batch.events.end());
    result.notes = session.snapshot();
    result.metrics = session.metrics();
    result.state = session.state();
    result.recording = session.recording();
    return result;
}

std::uint64_t frame(double seconds, std::uint32_t rate = fake_rate) {
    return static_cast<std::uint64_t>(std::llround(seconds * rate));
}

void require_truth(const std::vector<LiveNote>& notes, const std::vector<Truth>& truth,
                   std::uint32_t rate = fake_rate) {
    REQUIRE(notes.size() == truth.size());
    for (const auto& expected : truth) {
        const auto found = std::count_if(notes.begin(), notes.end(), [&](const LiveNote& note) {
            return note.midi_pitch == expected.midi && note.onset_frame == frame(expected.onset, rate) &&
                note.offset_frame == frame(expected.offset, rate) &&
                note.state == EventLifecycle::Stabilized;
        });
        INFO("midi " << expected.midi << " onset " << expected.onset);
        REQUIRE(found == 1);
    }
}

void require_no_duplicates(const std::vector<LiveNote>& notes) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
        for (std::size_t j = i + 1; j < notes.size(); ++j) {
            if (notes[i].midi_pitch != notes[j].midi_pitch) continue;
            const auto end_i = notes[i].offset_frame.value_or(UINT64_MAX);
            const auto end_j = notes[j].offset_frame.value_or(UINT64_MAX);
            REQUIRE((end_i <= notes[j].onset_frame || end_j <= notes[i].onset_frame));
        }
    }
}

std::size_t count(const std::vector<LiveEvent>& events, LiveEventKind kind) {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [&](const LiveEvent& event) { return event.kind == kind; }));
}

LiveNote make_note(std::string id, int midi, std::uint64_t onset, std::uint64_t offset,
                   EventLifecycle state = EventLifecycle::Stabilized) {
    LiveNote note;
    note.id = std::move(id);
    note.midi_pitch = midi;
    note.sample_rate = fake_rate;
    note.onset_frame = onset;
    note.offset_frame = offset;
    note.state = state;
    return note;
}

std::vector<float> tones(std::uint32_t rate, const std::vector<std::pair<double, double>>& parts) {
    std::vector<float> audio;
    for (const auto& [frequency, seconds] : parts) {
        const auto count = static_cast<std::size_t>(seconds * rate);
        for (std::size_t i = 0; i < count; ++i) {
            const double envelope = std::min({1.0, i / (0.005 * rate), (count - i) / (0.005 * rate)});
            audio.push_back(frequency > 0 ?
                static_cast<float>(0.4 * envelope * std::sin(2 * std::numbers::pi * frequency * i / rate)) : 0.0F);
        }
    }
    return audio;
}

} // namespace

TEST_CASE("live sessions validate options, formats and timestamps", "[live]") {
    auto options = window_options();
    REQUIRE_THROWS_AS(LiveTranscriptionSession(options), std::invalid_argument);
    options.window.window_seconds = 0.2;
    REQUIRE_THROWS_AS(LiveTranscriptionSession(options, std::make_unique<FakeBackend>(fake_truth, fake_rate)),
                      std::invalid_argument);
    LiveOptions yin;
    yin.format = {1000, 1};
    REQUIRE_THROWS_AS(LiveTranscriptionSession(yin), std::invalid_argument);
    yin.format = {44100, 1};
    yin.yin.realtime.latency_ms = 5;
    REQUIRE_THROWS_AS(LiveTranscriptionSession(yin), std::invalid_argument);
    yin.yin.realtime.latency_ms = 40;
    REQUIRE_THROWS_AS(LiveTranscriptionSession(yin, std::make_unique<FakeBackend>(fake_truth, 44100)),
                      std::invalid_argument);

    yin.threaded = false;
    yin.input_queue_blocks = 2;
    LiveTranscriptionSession session(yin);
    std::vector<float> stereo(64, 0.0F), mono(64, 0.0F);
    REQUIRE(session.push({{44100, 2}, 0, stereo, {}, false}) == PushStatus::InvalidFormat);
    REQUIRE(session.push({{48000, 1}, 0, mono, {}, false}) == PushStatus::InvalidFormat);
    REQUIRE(session.push({{44100, 1}, 100, mono, {}, false}) == PushStatus::Accepted);
    REQUIRE(session.push({{44100, 1}, 120, mono, {}, false}) == PushStatus::InvalidTimestamp);
    REQUIRE(session.push({{44100, 1}, 164, mono, {}, false}) == PushStatus::Accepted);
    REQUIRE(session.push({{44100, 1}, 228, mono, {}, false}) == PushStatus::QueueFull);
    REQUIRE_THROWS_AS(session.pump(), std::logic_error);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    REQUIRE(std::holds_alternative<InferenceError>(session.start()));
    session.pump();
    REQUIRE(session.ingested_frames() == 128);  // Timeline origin is the first accepted block.
    REQUIRE(session.push({{44100, 1}, 228, mono, {}, false}) == PushStatus::Accepted);
    session.finish();
    REQUIRE(session.push({{44100, 1}, 400, mono, {}, false}) == PushStatus::Closed);
    const auto metrics = session.metrics();
    REQUIRE(metrics.blocks_rejected == 3);
    REQUIRE(metrics.blocks_refused == 1);
    REQUIRE(metrics.frames_refused == 64);
    REQUIRE(metrics.input_queue_high_water <= metrics.input_queue_capacity);
    REQUIRE(session.state() == LiveState::Finished);
}

TEST_CASE("live ingestion downmixes channels and sanitizes nonfinite samples", "[live]") {
    LiveOptions options;
    options.format = {44100, 2};
    options.threaded = false;
    LiveTranscriptionSession session(options);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    std::vector<float> stereo{1.0F, -1.0F, 0.5F, 0.25F, std::numeric_limits<float>::quiet_NaN(), 0.5F};
    REQUIRE(session.push({options.format, 0, stereo, {}, false}) == PushStatus::Accepted);
    session.finish();
    const auto recording = session.recording();
    REQUIRE(recording.sample_rate == 44100);
    REQUIRE(recording.samples == std::vector<float>{0.0F, 0.375F, 0.25F});
    const auto tail = session.recording_tail(2);
    REQUIRE(tail.sample_rate == 44100);
    REQUIRE(tail.samples == std::vector<float>{0.375F, 0.25F});
    REQUIRE(session.recording_tail(0).samples.empty());
    REQUIRE(session.metrics().nonfinite_samples == 1);
}

TEST_CASE("overlapping windows reconcile into exact unique notes independent of chunk size", "[live]") {
    const auto audio = frame_index_audio(frame(7.0));
    std::optional<Replay> reference;
    for (std::size_t block : {37U, 512U, 2000U, 4096U, 9000U}) {
        INFO("block " << block);
        auto result = replay(window_options(), std::make_unique<FakeBackend>(fake_truth, fake_rate), audio, block);
        REQUIRE(result.state == LiveState::Finished);
        require_truth(result.notes, fake_truth);
        require_no_duplicates(result.notes);
        REQUIRE(count(result.events, LiveEventKind::NoteOn) == fake_truth.size());
        REQUIRE(count(result.events, LiveEventKind::NoteRetracted) == 0);
        REQUIRE(count(result.events, LiveEventKind::AnalysisGap) == 0);
        for (std::size_t i = 1; i < result.events.size(); ++i) {
            REQUIRE(result.events[i].sequence == result.events[i - 1].sequence + 1);
        }
        // The long note was announced while sounding and kept its identity until its offset.
        const auto long_id = std::find_if(result.notes.begin(), result.notes.end(),
            [](const LiveNote& n) { return n.midi_pitch == 67; })->id;
        const auto on = std::find_if(result.events.begin(), result.events.end(), [&](const LiveEvent& e) {
            return e.kind == LiveEventKind::NoteOn && e.note.id == long_id;
        });
        REQUIRE(on != result.events.end());
        REQUIRE_FALSE(on->note.offset_frame);
        REQUIRE(on->timing.evidence_end_frame <= frame(0.75));
        if (!reference) {
            reference = result;
            continue;
        }
        REQUIRE(result.events.size() == reference->events.size());
        for (std::size_t i = 0; i < result.events.size(); ++i) {
            REQUIRE(result.events[i].kind == reference->events[i].kind);
            REQUIRE(result.events[i].note.id == reference->events[i].note.id);
            REQUIRE(result.events[i].note.onset_frame == reference->events[i].note.onset_frame);
            REQUIRE(result.events[i].note.offset_frame == reference->events[i].note.offset_frame);
        }
    }
}

TEST_CASE("live windows correct pitch, retract unreproduced notes and never duplicate them", "[live]") {
    auto backend = std::make_unique<FakeBackend>(fake_truth, fake_rate);
    backend->perturb = [](std::size_t, std::uint64_t first, std::uint64_t end, std::vector<Note>& notes) {
        for (auto& note : notes) {
            const auto onset = first + frame(note.start_seconds);
            if (note.midi_pitch == 60 && end - onset < frame(0.3)) note.midi_pitch = 61;
        }
        if (end == frame(3.0)) notes.push_back({80, 2.5 - double(first) / fake_rate, 0.2, "", 0.9, 0});
    };
    const auto result = replay(window_options(), std::move(backend), frame_index_audio(frame(7.0)), 512);
    require_truth(result.notes, fake_truth);
    require_no_duplicates(result.notes);
    const auto corrected = std::find_if(result.events.begin(), result.events.end(), [](const LiveEvent& e) {
        return e.kind == LiveEventKind::NoteCorrected && e.previous && e.previous->midi_pitch == 61;
    });
    REQUIRE(corrected != result.events.end());
    REQUIRE(corrected->note.midi_pitch == 60);
    REQUIRE(corrected->note.id == corrected->previous->id);
    const auto retracted = std::find_if(result.events.begin(), result.events.end(), [](const LiveEvent& e) {
        return e.kind == LiveEventKind::NoteRetracted;
    });
    REQUIRE(retracted != result.events.end());
    REQUIRE(retracted->note.midi_pitch == 80);
    REQUIRE(retracted->note.state == EventLifecycle::Rejected);
    REQUIRE(count(result.events, LiveEventKind::NoteRetracted) == 1);
    REQUIRE(count(result.events, LiveEventKind::NoteOn) == fake_truth.size() + 1);
}

TEST_CASE("notes whose attack precedes a window are continued, not duplicated", "[live]") {
    // The decoder cannot see an attack outside its window and reports a later onset, past the
    // left guard, for the visible remainder of the note.
    const auto late_onset = [](std::size_t, std::uint64_t first, std::uint64_t, std::vector<Note>& notes) {
        if (first == 0) return;
        for (auto& note : notes) {
            if (note.start_seconds == 0.0 && note.duration_seconds > 0.3) {
                note.start_seconds = 0.3;
                note.duration_seconds -= 0.3;
            }
        }
    };
    for (const std::size_t block : {64U, 997U, 4096U}) {
        auto sustained = std::make_unique<FakeBackend>(std::vector<Truth>{{57, 0.15, 1.8}}, fake_rate);
        sustained->perturb = late_onset;
        const auto single = replay(window_options(), std::move(sustained), frame_index_audio(frame(2.2)), block);
        require_truth(single.notes, {{57, 0.15, 1.8}});

        const std::vector<Truth> repeated{{57, 0.15, 1.0}, {57, 1.0, 1.8}};
        auto twice = std::make_unique<FakeBackend>(repeated, fake_rate);
        twice->perturb = late_onset;
        const auto result = replay(window_options(), std::move(twice), frame_index_audio(frame(2.2)), block);
        require_truth(result.notes, repeated);
    }
}

TEST_CASE("simulated clocks give exact capture, analysis, end-to-end and stabilization latency", "[live]") {
    Nanoseconds now{std::chrono::seconds(10)};
    auto options = window_options();
    options.clock = [&] { return now; };
    options.analysis_hook = [&](std::uint64_t) { now += std::chrono::milliseconds(20); };
    LiveTranscriptionSession session(options,
        std::make_unique<FakeBackend>(std::vector<Truth>{{60, 0.30, 0.80}}, fake_rate));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    const auto audio = frame_index_audio(frame(1.5));
    for (std::size_t offset = 0; offset < audio.size(); offset += frame(0.25)) {
        const auto block = std::span(audio).subspan(offset, frame(0.25));
        REQUIRE(session.push({options.format, offset, block, now, false}) == PushStatus::Accepted);
        now += std::chrono::milliseconds(5);
        session.pump();
    }
    const auto metrics = session.metrics();
    REQUIRE(metrics.capture_latency.count == 6);
    REQUIRE(metrics.capture_latency.maximum_seconds == Catch::Approx(0.005).margin(1e-12));
    REQUIRE(metrics.capture_latency.p50_seconds == Catch::Approx(0.005).margin(1e-12));
    REQUIRE(metrics.analysis_latency.p95_seconds == Catch::Approx(0.020).margin(1e-12));
    REQUIRE(metrics.end_to_end_latency.count == 1);
    REQUIRE(metrics.end_to_end_latency.maximum_seconds == Catch::Approx(0.025).margin(1e-12));
    REQUIRE(metrics.stabilization_delay.maximum_seconds == Catch::Approx(0.050).margin(1e-12));
    REQUIRE(metrics.algorithmic_latency.maximum_seconds == Catch::Approx(0.2).margin(1e-12));
    session.finish();
}

TEST_CASE("missing frames and discontinuities become explicit gaps with zero-filled recordings", "[live]") {
    const std::vector<Truth> truth{{60, 1.0, 5.0}};
    auto options = window_options();
    LiveTranscriptionSession session(options, std::make_unique<FakeBackend>(truth, fake_rate));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    auto first = frame_index_audio(frame(2.0));
    auto second = frame_index_audio(frame(3.0), frame(3.0));
    for (std::size_t offset = 0; offset < first.size(); offset += 1000) {
        REQUIRE(session.push({options.format, offset, std::span(first).subspan(offset, 1000), {}, false}) ==
                PushStatus::Accepted);
        session.pump();
    }
    for (std::size_t offset = 0; offset < second.size(); offset += 1000) {
        REQUIRE(session.push({options.format, frame(3.0) + offset, std::span(second).subspan(offset, 1000), {},
                              offset == 0}) == PushStatus::Accepted);
        session.pump();
    }
    session.finish();
    const auto events = session.poll_events().events;
    const auto gap = std::find_if(events.begin(), events.end(),
        [](const LiveEvent& e) { return e.kind == LiveEventKind::AnalysisGap; });
    REQUIRE(gap != events.end());
    REQUIRE(gap->gap_first_frame == frame(2.0));
    REQUIRE(gap->gap_end_frame == frame(3.0));
    const auto notes = session.snapshot();
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[0].offset_frame == frame(2.0));
    REQUIRE_FALSE(notes[0].onset_truncated);
    REQUIRE(notes[1].onset_frame == frame(3.0));
    REQUIRE(notes[1].offset_frame == frame(5.0));
    REQUIRE(notes[1].onset_truncated);
    const auto recording = session.recording();
    REQUIRE(recording.samples.size() == frame(6.0));
    REQUIRE(recording.samples[frame(2.5)] == 0.0F);
    REQUIRE(recording.samples[frame(4.0)] == static_cast<float>(frame(4.0)));
    REQUIRE(session.metrics().gap_frames == frame(1.0));
    REQUIRE(session.metrics().discontinuities == 1);
}

TEST_CASE("forced analysis lag coalesces windows or skips with an explicit gap", "[live]") {
    const auto audio = frame_index_audio(frame(6.0));
    SECTION("coalescing keeps coverage") {
        auto options = window_options();
        options.lag_policy = LagPolicy::SkipAhead;
        auto result = replay(options, std::make_unique<FakeBackend>(fake_truth, fake_rate), audio, frame(0.6));
        REQUIRE(result.metrics.windows_coalesced > 0);
        REQUIRE(count(result.events, LiveEventKind::AnalysisGap) == 0);
        require_no_duplicates(result.notes);
    }
    SECTION("skipping beyond the lag budget reports the unanalysed range") {
        auto options = window_options();
        options.lag_policy = LagPolicy::SkipAhead;
        options.max_analysis_lag_seconds = 0.5;
        auto result = replay(options, std::make_unique<FakeBackend>(fake_truth, fake_rate), audio, frame(3.0));
        REQUIRE(result.metrics.skipped_frames > 0);
        const auto gap = std::find_if(result.events.begin(), result.events.end(),
            [](const LiveEvent& e) { return e.kind == LiveEventKind::AnalysisGap; });
        REQUIRE(gap != result.events.end());
        REQUIRE(gap->gap_first_frame == 0);
        REQUIRE(gap->gap_end_frame == frame(3.0));
        REQUIRE(result.recording.samples.size() == audio.size());
        require_no_duplicates(result.notes);
    }
    SECTION("incremental YIN skips too") {
        LiveOptions options;
        options.format = {fake_rate, 1};
        options.threaded = false;
        options.max_analysis_lag_seconds = 1.5;
        auto result = replay(options, {}, std::vector<float>(frame(3.0), 0.0F), frame(2.0));
        REQUIRE(result.metrics.skipped_frames == frame(2.0));
        REQUIRE(count(result.events, LiveEventKind::AnalysisGap) == 1);
    }
}

TEST_CASE("model errors recover by reloading or fail explicitly without losing audio", "[live]") {
    const auto audio = frame_index_audio(frame(7.0));
    SECTION("transient failure") {
        auto backend = std::make_unique<FakeBackend>(fake_truth, fake_rate);
        auto* raw = backend.get();
        backend->fail = [](std::size_t call) { return call == 5; };
        auto result = replay(window_options(), std::move(backend), audio, 512);
        REQUIRE(result.state == LiveState::Finished);
        REQUIRE(result.metrics.model_errors == 1);
        REQUIRE(result.metrics.model_recoveries == 1);
        REQUIRE(raw->loads == 2);
        require_truth(result.notes, fake_truth);
    }
    SECTION("persistent failure") {
        auto backend = std::make_unique<FakeBackend>(fake_truth, fake_rate);
        backend->fail = [](std::size_t call) { return call >= 8; };
        auto options = window_options();
        LiveTranscriptionSession session(options, std::move(backend));
        REQUIRE(std::holds_alternative<std::monostate>(session.start()));
        for (std::size_t offset = 0; offset < audio.size(); offset += 1000) {
            REQUIRE(session.push({options.format, offset, std::span(audio).subspan(offset, 1000), {}, false}) ==
                    PushStatus::Accepted);
            session.pump();
        }
        session.finish();
        REQUIRE(session.state() == LiveState::AnalysisFailed);
        REQUIRE(session.last_error().find("scripted failure") != std::string::npos);
        REQUIRE(session.metrics().model_errors == options.max_model_recoveries + 1);
        REQUIRE(session.recording().samples.size() == audio.size());
        for (const auto& note : session.snapshot()) REQUIRE(note.offset_frame);
    }
}

TEST_CASE("device loss closes notes and resumption reports the gap", "[live]") {
    auto options = window_options();
    LiveTranscriptionSession session(options,
        std::make_unique<FakeBackend>(std::vector<Truth>{{60, 0.5, 4.0}}, fake_rate));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    const auto first = frame_index_audio(frame(2.0));
    REQUIRE(session.push({options.format, 0, first, {}, false}) == PushStatus::Accepted);
    session.pump();
    session.report_input_lost();
    session.pump();
    REQUIRE(session.state() == LiveState::InputInterrupted);
    REQUIRE(session.snapshot().at(0).offset_frame == frame(2.0));
    const auto second = frame_index_audio(frame(1.0), frame(2.5));
    REQUIRE(session.push({options.format, frame(2.5), second, {}, false}) == PushStatus::Accepted);
    session.pump();
    REQUIRE(session.state() == LiveState::Running);
    session.finish();
    const auto events = session.poll_events().events;
    REQUIRE(std::any_of(events.begin(), events.end(), [](const LiveEvent& e) {
        return e.kind == LiveEventKind::Status && e.message == "input interrupted";
    }));
    REQUIRE(std::any_of(events.begin(), events.end(), [&](const LiveEvent& e) {
        return e.kind == LiveEventKind::AnalysisGap && e.gap_first_frame == frame(2.0) &&
            e.gap_end_frame == frame(2.5) && e.message == "input resumed";
    }));
}

TEST_CASE("bounded event queues request resynchronization from snapshots", "[live]") {
    auto options = window_options();
    options.event_queue_capacity = 3;
    LiveTranscriptionSession session(options, std::make_unique<FakeBackend>(fake_truth, fake_rate));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    const auto audio = frame_index_audio(frame(7.0));
    for (std::size_t offset = 0; offset < audio.size(); offset += 1000) {
        REQUIRE(session.push({options.format, offset, std::span(audio).subspan(offset, 1000), {}, false}) ==
                PushStatus::Accepted);
        session.pump();
    }
    session.finish();
    const auto batch = session.poll_events();
    REQUIRE(batch.events.size() == 3);
    REQUIRE(batch.resync_required);
    REQUIRE(batch.dropped_events > 0);
    REQUIRE(session.metrics().event_queue_high_water == 3);
    TranscriptionDocument document(fake_rate);
    document.apply(batch);
    REQUIRE(document.requires_resync());
    document.resync(session.snapshot());
    REQUIRE_FALSE(document.requires_resync());
    require_truth(document.notes(), fake_truth);
}

TEST_CASE("documents preserve history and never let live or refined updates overwrite manual edits", "[live]") {
    const auto result = replay(window_options(), std::make_unique<FakeBackend>(fake_truth, fake_rate),
                               frame_index_audio(frame(7.0)), 1000);
    TranscriptionDocument document(fake_rate);
    for (const auto& event : result.events) document.apply(event);
    require_truth(document.notes(), fake_truth);
    const auto original_events = document.original_events().size();
    REQUIRE(original_events == result.events.size());

    auto edited = *std::find_if(result.notes.begin(), result.notes.end(),
                                [](const LiveNote& n) { return n.midi_pitch == 60; });
    edited.midi_pitch = 59;
    document.edit_manual_note(edited, "user fixed pitch");
    auto late = result.events.back();
    late.kind = LiveEventKind::NoteCorrected;
    late.note = edited;
    late.note.midi_pitch = 61;
    late.note.state = EventLifecycle::Stabilized;
    document.apply(late);
    REQUIRE(document.note(edited.id)->midi_pitch == 59);
    REQUIRE(document.history(edited.id).back().superseded_by_manual);
    const auto deleted = std::find_if(result.notes.begin(), result.notes.end(),
                                      [](const LiveNote& n) { return n.midi_pitch == 72; })->id;
    document.delete_manual_note(deleted);
    const auto manual = document.add_manual_note(make_note("", 50, frame(6.5), frame(6.8)));

    ScoreProposal proposal;
    proposal.id = "test-proposal";
    for (auto note : result.notes) {
        note.state = EventLifecycle::Refined;
        if (note.midi_pitch == 64 && note.onset_frame == frame(1.0)) continue;  // Proposed removal.
        if (note.midi_pitch == 60) {
            note.midi_pitch = 59;  // Matches the manual pitch but proposes a later offset.
            note.offset_frame = frame(0.9);
        }
        proposal.notes.push_back(note);
    }
    proposal.notes.push_back(make_note("r", 76, frame(3.0), frame(3.5), EventLifecycle::Refined));
    const auto review = document.review(proposal);
    // Conflicts: modifying the manual pitch, re-adding the deleted note, removing the manual add.
    REQUIRE(review.conflicts == 3);
    const auto before = document.revision();
    auto applied = document.apply(review);
    REQUIRE(applied.withheld_conflicts == 3);
    REQUIRE(applied.applied == 2);
    REQUIRE(document.revision() > before);
    REQUIRE(document.note(edited.id)->offset_frame == frame(0.8));
    REQUIRE(document.note(deleted)->state == EventLifecycle::Rejected);
    REQUIRE(document.note(manual)->state == EventLifecycle::ManuallyCorrected);
    REQUIRE_THROWS_AS(document.apply(review), std::logic_error);

    const auto second = document.review(proposal);
    std::set<std::string> approved;
    for (const auto& change : second.changes) {
        if (change.conflicts_with_manual_edit && change.note_id == edited.id) approved.insert(change.change_id);
    }
    REQUIRE(approved.size() == 1);
    applied = document.apply(second, approved);
    REQUIRE(applied.applied == 1);
    REQUIRE(applied.withheld_conflicts == 2);
    REQUIRE(document.note(edited.id)->offset_frame == frame(0.9));
    REQUIRE(document.note(edited.id)->state == EventLifecycle::Refined);
    const auto& history = document.history(edited.id);
    REQUIRE(history.size() >= 4);
    REQUIRE(history.back().origin == RevisionOrigin::Refinement);
    REQUIRE(std::any_of(history.begin(), history.end(),
                        [](const NoteRevision& r) { return r.origin == RevisionOrigin::Manual; }));
    REQUIRE(document.original_events().size() == original_events + 1);
    REQUIRE(document.to_notes(7.0).size() == document.notes().size());
}

TEST_CASE("live documents preserve manual IDs and detect refinement pitch conflicts", "[live]") {
    TranscriptionDocument document(fake_rate);
    auto manual = make_note("user-note-17", 60, frame(1.0), frame(1.5),
                            EventLifecycle::ManuallyCorrected);
    REQUIRE(document.add_manual_note(manual, "inserted in the live editor",
                                     manual.id) == "user-note-17");

    ScoreProposal proposal;
    proposal.id = "pitch-change";
    proposal.notes.push_back(make_note("refined-note", 61, frame(1.0),
                                       frame(1.5), EventLifecycle::Refined));
    const auto review = document.review(proposal);
    REQUIRE(review.conflicts == 1);
    REQUIRE(review.changes.size() == 1);
    REQUIRE(review.changes.front().kind == ProposedChangeKind::Modify);
    REQUIRE(review.changes.front().note_id == "user-note-17");

    document.delete_manual_note("user-note-17");
    manual.state = EventLifecycle::ManuallyCorrected;
    REQUIRE(document.add_manual_note(manual, "restored by undo",
                                     manual.id) == "user-note-17");
    REQUIRE(document.note("user-note-17")->state ==
            EventLifecycle::ManuallyCorrected);
}

TEST_CASE("incremental YIN emits stabilized notes and retracts short blips", "[live]") {
    constexpr std::uint32_t rate = 22050;
    const auto audio = tones(rate, {{0, 0.2}, {440, 0.5}, {0, 0.2}, {523.25, 0.5}, {0, 0.3}});
    std::vector<int> pitches;
    for (std::size_t block : {128U, 441U, 4096U}) {
        LiveOptions options;
        options.format = {rate, 1};
        options.threaded = false;
        const auto result = replay(options, {}, audio, block);
        REQUIRE(result.notes.size() == 2);
        REQUIRE(result.notes[0].midi_pitch == 69);
        REQUIRE(result.notes[1].midi_pitch == 72);
        for (const auto& note : result.notes) {
            REQUIRE(note.state == EventLifecycle::Stabilized);
            REQUIRE(note.offset_frame);
            REQUIRE(note.source_model == "builtin:yin");
        }
        REQUIRE(std::abs(result.notes[0].onset_seconds() - 0.2) < 0.08);
        REQUIRE(std::abs(result.notes[1].onset_seconds() - 0.9) < 0.08);
        REQUIRE(result.metrics.algorithmic_latency.count == 2);
        REQUIRE(result.metrics.stabilization_delay.count == 2);
    }
    LiveOptions strict;
    strict.format = {rate, 1};
    strict.threaded = false;
    strict.yin.minimum_duration_seconds = 0.6;
    strict.yin.stabilization_seconds = 0.6;
    const auto blips = replay(strict, {}, audio, 512);
    REQUIRE(blips.notes.empty());
    REQUIRE(count(blips.events, LiveEventKind::NoteRetracted) == 2);
}

TEST_CASE("full-context refinement creates a separate proposal", "[live]") {
    constexpr std::uint32_t rate = 22050;
    const AudioBuffer recording{rate, tones(rate, {{0, 0.2}, {440, 0.6}, {0, 0.2}, {659.26, 0.6}, {0, 0.2}})};
    RefinementOptions options;
    options.backend_id = "builtin:yin";
    const auto refined = refine_recording(recording, options);
    REQUIRE(std::holds_alternative<ScoreProposal>(refined));
    const auto& proposal = std::get<ScoreProposal>(refined);
    REQUIRE(proposal.backend_id == "builtin:yin");
    REQUIRE(proposal.notes.size() == proposal.events.size());
    REQUIRE(proposal.notes.size() >= 2);
    for (const auto& note : proposal.notes) REQUIRE(note.state == EventLifecycle::Refined);
    options.backend_id = "missing:model";
    REQUIRE(std::holds_alternative<InferenceError>(refine_recording(recording, options)));
    const auto empty = refine_recording({rate, {}}, {});
    REQUIRE(std::get<ScoreProposal>(empty).notes.empty());
    REQUIRE(std::holds_alternative<InferenceError>(refine_recording({1000, {0.0F}}, {})));
    std::stop_source stop;
    stop.request_stop();
    options.backend_id = "builtin:spectral";
    const auto cancelled = refine_recording(recording, options, stop.get_token());
    REQUIRE(std::get<InferenceError>(cancelled).code == InferenceErrorCode::Cancelled);
}

TEST_CASE("windowed refinement reconciles overlapping windows offline", "[live]") {
    constexpr std::uint32_t rate = 22050;
    const AudioBuffer recording{rate, tones(rate, {{0, 0.2}, {440, 0.6}, {0, 0.2}, {659.26, 0.6}, {0, 0.2}})};
    RefinementOptions options;
    options.backend_id = "builtin:spectral";
    options.windowed = WindowOptions{};
    options.windowed->window_seconds = 0.5;
    options.windowed->hop_seconds = 0.125;
    const auto refined = refine_recording(recording, options);
    REQUIRE(std::holds_alternative<ScoreProposal>(refined));
    const auto& proposal = std::get<ScoreProposal>(refined);
    REQUIRE(proposal.events.empty());
    REQUIRE(proposal.backend_id == "builtin:spectral");
    std::vector<int> pitches;
    for (std::size_t i = 0; i < proposal.notes.size(); ++i) {
        const auto& note = proposal.notes[i];
        REQUIRE(note.id == "refined-" + std::to_string(i + 1));
        REQUIRE(note.state == EventLifecycle::Refined);
        REQUIRE(note.offset_frame.has_value());
        REQUIRE(*note.offset_frame > note.onset_frame);
        REQUIRE(note.detection_method.starts_with("windowed-offline/"));
        pitches.push_back(note.midi_pitch);
    }
    REQUIRE(std::count(pitches.begin(), pitches.end(), 69) == 1);
    REQUIRE(std::count(pitches.begin(), pitches.end(), 76) == 1);
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = refine_recording(recording, options, stop.get_token());
    REQUIRE(std::get<InferenceError>(cancelled).code == InferenceErrorCode::Cancelled);
}

TEST_CASE("threaded sessions cancel promptly and destructors join workers", "[live]") {
    auto backend = std::make_unique<FakeBackend>(fake_truth, fake_rate);
    backend->delay = std::chrono::milliseconds(20);
    auto options = window_options();
    options.threaded = true;
    auto session = std::make_unique<LiveTranscriptionSession>(options, std::move(backend));
    REQUIRE(std::holds_alternative<std::monostate>(session->start()));
    const auto audio = frame_index_audio(frame(3.0));
    REQUIRE(session->push({options.format, 0, audio, {}, false}) == PushStatus::Accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto started = std::chrono::steady_clock::now();
    session->cancel();
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    REQUIRE(session->state() == LiveState::Cancelled);
    REQUIRE(session->push({options.format, frame(3.0), audio, {}, false}) == PushStatus::Closed);
    session->finish();
    REQUIRE(session->state() == LiveState::Cancelled);

    auto second = std::make_unique<FakeBackend>(fake_truth, fake_rate);
    second->delay = std::chrono::milliseconds(20);
    {
        LiveTranscriptionSession scoped(options, std::move(second));
        REQUIRE(std::holds_alternative<std::monostate>(scoped.start()));
        REQUIRE(scoped.push({options.format, 0, audio, {}, false}) == PushStatus::Accepted);
    }
}

TEST_CASE("sustained threaded capture stays bounded and lossless", "[live][stress]") {
    constexpr std::uint32_t rate = 22050;
    std::vector<std::pair<double, double>> parts;
    for (int i = 0; i < 40; ++i) parts.push_back({440.0 * std::pow(2.0, (i % 12) / 12.0), 0.35}), parts.push_back({0, 0.15});
    const auto audio = tones(rate, parts);
    LiveOptions options;
    options.format = {rate, 1};
    options.lag_policy = LagPolicy::Lossless;
    options.input_queue_blocks = 32;
    std::atomic<int> notifications{0};
    options.events_available = [&] { ++notifications; };
    LiveTranscriptionSession session(options);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    TranscriptionDocument document(rate);
    std::size_t retries = 0;
    for (std::size_t offset = 0; offset < audio.size(); offset += 256) {
        const auto block = std::span(audio).subspan(offset, std::min<std::size_t>(256, audio.size() - offset));
        while (session.push({options.format, offset, block, {}, false}) == PushStatus::QueueFull) {
            ++retries;
            std::this_thread::yield();
        }
        if (offset % 8192 == 0) document.apply(session.poll_events());
    }
    session.finish();
    document.apply(session.poll_events());
    const auto metrics = session.metrics();
    REQUIRE(session.state() == LiveState::Finished);
    REQUIRE(metrics.frames_received == audio.size());
    REQUIRE(metrics.recording_frames == audio.size());
    REQUIRE(metrics.skipped_frames == 0);
    REQUIRE(metrics.events_dropped == 0);
    REQUIRE(metrics.input_queue_high_water <= metrics.input_queue_capacity);
    REQUIRE(metrics.rolling_capacity_frames < 3 * rate);
    REQUIRE(notifications.load() > 0);
    REQUIRE_FALSE(document.requires_resync());
    const auto notes = document.notes();
    REQUIRE(notes.size() >= 30);
    REQUIRE(notes.size() <= 45);
    require_no_duplicates(notes);
    (void)retries;
}

TEST_CASE("overloaded threaded analysis skips audio but keeps queues and notes bounded", "[live][stress]") {
    std::vector<Truth> truth;
    for (int i = 0; i < 60; ++i) truth.push_back({60 + i % 12, 0.3 + i * 0.3, 0.5 + i * 0.3});
    auto backend = std::make_unique<FakeBackend>(truth, fake_rate);
    backend->delay = std::chrono::milliseconds(150);
    auto options = window_options();
    options.threaded = true;
    options.lag_policy = LagPolicy::SkipAhead;
    options.max_analysis_lag_seconds = 0.5;
    options.input_queue_blocks = 16;
    options.max_tracked_notes = 8;
    LiveTranscriptionSession session(options, std::move(backend));
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    const auto audio = frame_index_audio(frame(18.5));
    const auto block = frame(0.05);
    for (std::size_t offset = 0; offset < audio.size(); offset += block) {
        // 10x faster than real time; QueueFull blocks are dropped like a real-time producer.
        (void)session.push({options.format, offset,
                            std::span(audio).subspan(offset, std::min(block, audio.size() - offset)), {}, false});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    session.finish();
    const auto metrics = session.metrics();
    REQUIRE(session.state() == LiveState::Finished);
    REQUIRE(metrics.skipped_frames + metrics.windows_coalesced > 0);
    REQUIRE(metrics.input_queue_high_water <= options.input_queue_blocks);
    REQUIRE(metrics.tracked_notes_high_water <= options.max_tracked_notes);
    REQUIRE(metrics.events_dropped == 0);
    require_no_duplicates(session.snapshot());
}
