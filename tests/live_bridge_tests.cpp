#include "transcribe/live_bridge.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace transcribe::live;
using namespace transcribe::live::bridge;

namespace {

LiveNote note(std::string id, std::uint32_t revision, bool finalized = false) {
    LiveNote result;
    result.id = std::move(id);
    result.midi_pitch = 69;
    result.sample_rate = 22050;
    result.onset_frame = 100;
    result.offset_frame = 200;
    result.revision = revision;
    result.finalized = finalized;
    return result;
}

LiveEvent event(LiveEventKind kind, const LiveNote& value) {
    LiveEvent result;
    result.kind = kind;
    result.note = value;
    return result;
}

}  // namespace

TEST_CASE("live bridge allowlist accepts capture operations and rejects unknown actions",
          "[live-bridge]") {
    REQUIRE(operation_allowed("start-capture"));
    REQUIRE(operation_allowed("stop-capture"));
    REQUIRE(operation_allowed("cancel"));
    REQUIRE(operation_allowed("transcribe"));
    REQUIRE_FALSE(operation_allowed("start-capture-extra"));
    REQUIRE_FALSE(operation_allowed("poll-events"));
    REQUIRE_FALSE(operation_allowed(""));
    REQUIRE(capture_state_name(CaptureState::Idle) == "idle");
    REQUIRE(capture_state_name(CaptureState::Arming) == "arming");
    REQUIRE(capture_state_name(CaptureState::Capturing) == "capturing");
    REQUIRE(capture_state_name(CaptureState::Interrupted) == "interrupted");
    REQUIRE(capture_state_name(CaptureState::Finishing) == "finishing");
    REQUIRE(capture_state_name(CaptureState::Finished) == "finished");
    REQUIRE(capture_state_name(CaptureState::Failed) == "failed");
}

TEST_CASE("live bridge message has versioned revision-keyed change categories", "[live-bridge]") {
    EventBatch batch;
    auto on = note("take/live-1", 0);
    auto stabilized = note("take/live-1", 1);
    auto off = note("take/live-1", 2);
    auto finalized = note("take/live-1", 3, true);
    auto provisional = note("take/live-2", 4);
    auto short_note = note("take/live-3", 0);
    auto retracted = note("take/live-3", 1);
    batch.events = {event(LiveEventKind::NoteOn, on),
                    event(LiveEventKind::NoteStabilized, stabilized),
                    event(LiveEventKind::NoteOff, off),
                    event(LiveEventKind::NoteFinalized, finalized),
                    event(LiveEventKind::NoteOn, provisional),
                    event(LiveEventKind::NoteOn, short_note),
                    event(LiveEventKind::NoteRetracted, retracted)};
    batch.events.push_back({0, LiveEventKind::AnalysisGap, {}, {}, 240, 320, "skip-ahead", {}});

    Coalescer coalescer;
    coalescer.append(batch);
    LiveMetrics metrics;
    metrics.events_dropped = 4;
    metrics.capture_latency.p50_seconds = 0.01;
    metrics.capture_latency.p95_seconds = 0.02;
    metrics.capture_latency.p99_seconds = 0.03;
    const auto message = coalescer.take(metrics, CaptureState::Capturing, "Capturing input");

    REQUIRE(message.version == protocol_version);
    REQUIRE(message.sequence == 1);
    REQUIRE_FALSE(message.resync);
    REQUIRE(message.upserts.size() == 1);
    REQUIRE(message.upserts.at("take/live-2").revision == 4);
    REQUIRE(message.retractions.size() == 1);
    REQUIRE(message.retractions.at("take/live-3") == 1);
    REQUIRE(message.finalizations.size() == 1);
    REQUIRE(message.finalizations.at("take/live-1").revision == 3);
    REQUIRE(message.gaps.size() == 1);
    REQUIRE(message.gaps.front().first_frame == 240);
    REQUIRE(message.gaps.front().end_frame == 320);
    REQUIRE(message.metrics.events_dropped == 4);
    REQUIRE(message.metrics.capture_latency.p50_seconds == 0.01);
    REQUIRE(message.metrics.capture_latency.p95_seconds == 0.02);
    REQUIRE(message.metrics.capture_latency.p99_seconds == 0.03);
    REQUIRE(message.state == CaptureState::Capturing);
    REQUIRE(message.status == "Capturing input");
}

TEST_CASE("live bridge coalesces bursts and ignores stale or equal note revisions", "[live-bridge]") {
    Coalescer coalescer;
    EventBatch burst;
    for (std::uint32_t revision = 0; revision < 5000; ++revision) {
        burst.events.push_back(event(LiveEventKind::NoteCorrected, note("take/live-1", revision)));
    }
    coalescer.append(burst);
    EventBatch gap_burst;
    for (std::uint64_t frame = 0; frame < 100; ++frame) {
        gap_burst.events.push_back(
            {frame, LiveEventKind::AnalysisGap, {}, {}, frame * 10,
             frame * 10 + 5, "skip-ahead", {}});
    }
    coalescer.append(gap_burst);
    auto message = coalescer.take({}, CaptureState::Capturing, {});
    REQUIRE(message.upserts.size() == 1);
    REQUIRE(message.upserts.at("take/live-1").revision == 4999);
    REQUIRE(message.events.size() == 64);
    REQUIRE(message.gaps.size() == 64);
    REQUIRE(message.gaps.front().first_frame == 360);
    REQUIRE(message.gaps.back().first_frame == 990);
    REQUIRE(message.coalesced_events_dropped == 5036);

    EventBatch stale;
    stale.events.push_back(event(LiveEventKind::NoteCorrected, note("take/live-1", 4999)));
    stale.events.push_back(event(LiveEventKind::NoteCorrected, note("take/live-1", 4998)));
    coalescer.append(stale);
    message = coalescer.take({}, CaptureState::Capturing, {});
    REQUIRE(message.upserts.empty());
    REQUIRE(message.events.empty());
}

TEST_CASE("live bridge resync replaces pending changes with the coherent snapshot", "[live-bridge]") {
    Coalescer coalescer;
    EventBatch previous;
    previous.events.push_back(event(LiveEventKind::NoteOn, note("take/old", 0)));
    coalescer.append(previous);

    EventBatch resync;
    resync.resync_required = true;
    resync.resync_snapshot = {note("take/current", 4), note("take/final", 9, true)};
    resync.events.push_back(event(LiveEventKind::NoteOn, note("take/stale-event", 0)));
    resync.events.push_back(
        {0, LiveEventKind::AnalysisGap, {}, {}, 800, 900, "input gap", {}});
    coalescer.append(resync);
    const auto message = coalescer.take({}, CaptureState::Capturing, "Resynchronized");

    REQUIRE(message.resync);
    REQUIRE(message.upserts.size() == 2);
    REQUIRE(message.upserts.contains("take/current"));
    REQUIRE(message.upserts.contains("take/final"));
    REQUIRE_FALSE(message.upserts.contains("take/old"));
    REQUIRE_FALSE(message.upserts.contains("take/stale-event"));
    REQUIRE(message.finalizations.size() == 1);
    REQUIRE(message.finalizations.contains("take/final"));
    REQUIRE(message.events.size() == 1);
    REQUIRE(message.events.front().kind == LiveEventKind::AnalysisGap);
    REQUIRE(message.gaps.size() == 1);
    REQUIRE(message.gaps.front().first_frame == 800);
    REQUIRE(message.coalesced_events_dropped == 1);
}
