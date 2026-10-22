#include "../src/asio_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <thread>

using namespace transcribe;

TEST_CASE("discontinuity storms coalesce bounded markers without losing the final timeline", "[asio][stress]") {
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    options.input_queue_blocks = 1200;
    options.max_block_frames = 1;
    live::LiveTranscriptionSession session(options);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    session.begin_input(0);
    const std::array<float, 1> sample{};
    for (std::size_t i = 0; i < 1100; ++i) {
        REQUIRE(session.push({options.format, i * 2, sample, {}, false}) == live::PushStatus::Accepted);
    }
    session.finish();
    REQUIRE(session.metrics().input_markers_coalesced > 0);
    REQUIRE(session.metrics().skipped_frames > 0);
    REQUIRE(session.metrics().overloaded);
    REQUIRE(session.ingested_frames() == 2199);
    REQUIRE(session.metrics().gap_frames == 1099);
}

TEST_CASE("declared input timelines retain leading and trailing missing frames", "[asio][callback]") {
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    live::LiveTranscriptionSession session(options);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    session.begin_input(100);
    const std::array<float, 8> samples{1, 1, 1, 1, 1, 1, 1, 1};
    REQUIRE(session.push({options.format, 99, samples, {}, false}) == live::PushStatus::InvalidTimestamp);
    REQUIRE(session.push({options.format, 108, samples, {}, false}) == live::PushStatus::Accepted);
    session.end_input(124);
    session.finish();
    REQUIRE(session.ingested_frames() == 24);
    REQUIRE(session.metrics().gap_frames == 16);
    const auto audio = session.recording();
    REQUIRE(audio.samples.size() == 24);
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        REQUIRE(audio.samples[i] == (i >= 8 && i < 16 ? 1.0f : 0.0f));
    }
}

TEST_CASE("capture rings account for a two-hour timeline without growing storage", "[asio][stress]") {
    constexpr std::size_t total = 8000 * 7200;
    detail::CaptureTransport transport(total, 1027);
    std::array<float, 512> input{}, recorded{}, analysed{};
    std::size_t count = 0;
    while (count < total) {
        const auto frames = std::min(input.size(), total - count);
        const auto status = transport.receive(input.data(), frames, false);
        if (status != (count + frames == total ? 1 : 0)) break;
        if (transport.ring.read(recorded) != frames ||
            transport.analysis_ring.read(analysed) != frames) break;
        count += frames;
    }
    REQUIRE(count == total);
    REQUIRE(transport.captured.load() == total);
    REQUIRE_FALSE(transport.overflow.load());
    REQUIRE(transport.ring.readable() == 0);
    REQUIRE(transport.analysis_ring.readable() == 0);
}

TEST_CASE("capture transport publishes both rings atomically and reports failures", "[asio][callback]") {
    detail::CaptureTransport transport(100, 7);
    const std::array<float, 5> input{1, 2, 3, 4, 5};
    std::array<float, 8> output{};
    REQUIRE(transport.receive(input.data(), 5, false) == 0);
    REQUIRE(transport.ring.read(output) == 5);
    REQUIRE(transport.receive(input.data(), 5, false) == 2);
    REQUIRE(transport.overflow.load());
    REQUIRE(transport.captured.load() == 5);
    REQUIRE(transport.ring.readable() == 0);
    REQUIRE(transport.analysis_ring.read(output) == 5);
    for (std::size_t i = 0; i < input.size(); ++i)
        REQUIRE(output[i] == input[i]);
    detail::CaptureTransport stopped(3, 7);
    REQUIRE(stopped.receive(input.data(), 5, false) == 1);
    REQUIRE(stopped.captured.load() == 3);
    REQUIRE(stopped.ring.read(output) == 3);
    REQUIRE(stopped.analysis_ring.read(output) == 3);
    REQUIRE(stopped.receive(input.data(), 5, false) == 1);
    detail::CaptureTransport missing(100, 7);
    REQUIRE(missing.receive(nullptr, 5, false) == 2);
    REQUIRE(missing.input_failed.load());
    REQUIRE(missing.captured.load() == 0);
    detail::CaptureTransport overflow(100, 7);
    REQUIRE(overflow.receive(input.data(), 5, true) == 2);
    REQUIRE(overflow.input_failed.load());
    overflow.analysis_failed.store(true);
    REQUIRE(overflow.receive(input.data(), 5, false) == 2);
}

TEST_CASE("audio ring arbitrary capacities wrap repeatedly under concurrent SPSC traffic", "[asio][stress]") {
    for (const std::size_t capacity : {1, 7, 257}) {
        AudioRingBuffer ring(capacity);
        constexpr std::size_t total = 250000;
        std::atomic<bool> correct{true};
        std::thread producer([&] {
            std::array<float, 17> samples{};
            std::size_t sent = 0;
            while (sent < total) {
                const auto count = std::min(samples.size(), total - sent);
                for (std::size_t i = 0; i < count; ++i)
                    samples[i] = static_cast<float>(sent + i);
                sent += ring.write(std::span(samples).first(count));
                if (ring.writable() > capacity)
                    correct.store(false);
                std::this_thread::yield();
            }
        });
        std::array<float, 23> samples{};
        std::size_t received = 0;
        while (received < total) {
            const auto count = ring.read(samples);
            for (std::size_t i = 0; i < count; ++i) {
                if (samples[i] != static_cast<float>(received + i))
                    correct.store(false);
            }
            received += count;
            if (ring.readable() > capacity)
                correct.store(false);
            std::this_thread::yield();
        }
        producer.join();
        REQUIRE(correct.load());
        REQUIRE(received == total);
        REQUIRE(ring.read(samples) == 0);
        REQUIRE(ring.writable() == capacity);
    }
}

TEST_CASE("live capture transport counts refused null and final frames without shifting time",
          "[asio][callback]") {
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    options.input_queue_blocks = 1;
    options.max_block_frames = 8;
    live::LiveTranscriptionSession session(options);
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    detail::LiveCaptureTransport transport;
    transport.session = &session;
    transport.format = options.format;
    transport.capacity = 35;
    const std::array<float, 8> samples{.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f};
    REQUIRE(transport.receive(samples.data(), 8, false, std::chrono::seconds(1)) == 0);
    REQUIRE(transport.receive(samples.data(), 8, false, std::chrono::seconds(2)) == 0);
    REQUIRE(transport.refused.load() == 1);
    session.pump();
    REQUIRE(transport.receive(nullptr, 8, true, std::chrono::seconds(3)) == 0);
    REQUIRE(transport.next_frame.load() == 24);
    REQUIRE(transport.receive(samples.data(), 8, false, std::chrono::seconds(4)) == 0);
    session.pump();
    REQUIRE(transport.receive(samples.data(), 8, false, std::chrono::seconds(5)) == 1);
    session.finish();
    REQUIRE(transport.next_frame.load() == 35);
    REQUIRE(transport.delivered_frames.load() == 19);
    REQUIRE(transport.missing_frames.load() == 8);
    REQUIRE(transport.overflows.load() == 1);
    REQUIRE(session.ingested_frames() == 35);
    REQUIRE(session.metrics().gap_frames == 16);
    const auto recording = session.recording();
    REQUIRE(recording.samples.size() == 35);
    for (std::size_t i = 8; i < 24; ++i)
        REQUIRE(recording.samples[i] == 0);
    REQUIRE(session.metrics().overloaded);
}

TEST_CASE("repeated callback sessions stop and cancel without retained producer state", "[asio][stress]") {
    std::array<float, 128> samples{};
    for (int iteration = 0; iteration < 100; ++iteration) {
        live::LiveOptions options;
        options.format = {8000, 1};
        options.input_queue_blocks = 3;
        options.max_recording_seconds = 1;
        live::LiveTranscriptionSession session(options);
        REQUIRE(std::holds_alternative<std::monostate>(session.start()));
        detail::LiveCaptureTransport transport;
        transport.session = &session;
        transport.format = options.format;
        transport.capacity = samples.size() * 10;
        std::thread producer([&] {
            for (int block = 0; block < 10; ++block) {
                if (transport.receive(samples.data(), samples.size(), false, {}) != 0)
                    break;
            }
        });
        if (iteration % 2)
            session.cancel();
        else
            session.finish();
        producer.join();
        REQUIRE(session.state() == (iteration % 2 ? live::LiveState::Cancelled : live::LiveState::Finished));
        REQUIRE(session.push({options.format, 2000, samples, {}, false}) == live::PushStatus::Closed);
        REQUIRE(session.metrics().input_queue_high_water <= options.input_queue_blocks);
    }
}

TEST_CASE("live worker exceptions become an explicit failed state and owner shutdown joins", "[asio][live]") {
    for (bool from_notification : {false, true}) {
        live::LiveOptions options;
        options.format = {8000, 1};
        const auto injected = [] { throw std::runtime_error("Injected worker exception"); };
        if (from_notification)
            options.events_available = injected;
        else
            options.analysis_hook = [&](std::uint64_t) { injected(); };
        live::LiveTranscriptionSession session(options);
        REQUIRE(std::holds_alternative<std::monostate>(session.start()));
        std::array<float, 4096> samples{};
        // report_input_lost emits a Status event even for silence, invoking the notifier.
        session.report_input_lost();
        REQUIRE(session.push({options.format, 0, samples, {}, false}) == live::PushStatus::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (session.state() != live::LiveState::AnalysisFailed &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(session.state() == live::LiveState::AnalysisFailed);
        session.finish();
        REQUIRE(session.state() == live::LiveState::AnalysisFailed);
        REQUIRE(session.last_error() == "Injected worker exception");
        REQUIRE(session.push({options.format, samples.size(), samples, {}, false}) ==
                live::PushStatus::Closed);
    }
}

TEST_CASE("live admission rejects regressing clocks and overflowing device frame counters",
          "[asio][callback]") {
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    live::LiveTranscriptionSession session(options);
    std::array<float, 4> samples{};
    REQUIRE(std::holds_alternative<std::monostate>(session.start()));
    REQUIRE(session.push({options.format, 0, samples, std::chrono::seconds(2), false}) ==
            live::PushStatus::Accepted);
    REQUIRE(session.push({options.format, 4, samples, std::chrono::seconds(1), false}) ==
            live::PushStatus::InvalidTimestamp);
    REQUIRE(session.push({options.format, 4, samples, live::Nanoseconds(-1), false}) ==
            live::PushStatus::InvalidTimestamp);
    REQUIRE(session.push({options.format, std::numeric_limits<std::uint64_t>::max() - 2, samples,
                          std::chrono::seconds(3), false}) == live::PushStatus::InvalidTimestamp);
    session.finish();
    REQUIRE(session.metrics().blocks_rejected == 3);
}
