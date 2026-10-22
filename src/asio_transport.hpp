#pragma once

#include "transcribe/asio_capture.hpp"

#include <algorithm>
#include <atomic>

namespace transcribe::detail {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// The device is the only producer. Owners must quiesce callbacks before destroying this state.
struct CaptureTransport {
    CaptureTransport(std::size_t sample_capacity, std::size_t ring_capacity)
        : ring(ring_capacity), analysis_ring(ring_capacity), capacity(sample_capacity) {}

    int receive(const float *samples, std::size_t frames, bool driver_overflow) noexcept {
        if (analysis_failed.load(std::memory_order_acquire) || overflow.load(std::memory_order_acquire) ||
            input_failed.load(std::memory_order_acquire))
            return 2;
        if (!samples || driver_overflow) {
            input_failed.store(true, std::memory_order_release);
            return 2;
        }
        const auto first = captured.load(std::memory_order_relaxed);
        if (first >= capacity)
            return 1;
        const auto count = std::min(frames, capacity - first);
        // Do not partially publish to one ring if the other is full.
        if (ring.writable() < count || analysis_ring.writable() < count) {
            overflow.store(true, std::memory_order_release);
            return 2;
        }
        const auto block = std::span(samples, count);
        ring.write(block);
        analysis_ring.write(block);
        captured.store(first + count, std::memory_order_release);
        return first + count == capacity ? 1 : 0;
    }

    AudioRingBuffer ring, analysis_ring;
    const std::size_t capacity;
    std::atomic<std::size_t> captured{0};
    std::atomic<bool> overflow{false}, input_failed{false}, analysis_failed{false};
};

struct LiveCaptureTransport {
    live::AudioInputSink *session{};
    live::AudioFormat format;
    std::uint64_t capacity{};
    std::atomic<std::uint64_t> next_frame{0}, refused{0}, overflows{0};
    std::atomic<std::uint64_t> delivered_frames{0}, missing_frames{0};
    std::atomic<bool> rejected{false};
    bool pending_discontinuity{};
    std::uint64_t sequence{};
    std::uint64_t stream_epoch{};

    int receive(const float *input, std::size_t frames, bool overflow, live::Nanoseconds captured) noexcept {
        if (rejected.load(std::memory_order_acquire)) return 2;
        const auto first = next_frame.load(std::memory_order_relaxed);
        if (first >= capacity)
            return 1;
        const auto count = std::min<std::uint64_t>(frames, capacity - first);
        const auto block_sequence = sequence++;
        if (overflow)
            overflows.fetch_add(1, std::memory_order_relaxed);
        if (!input) {
            missing_frames.fetch_add(count, std::memory_order_relaxed);
            pending_discontinuity = true;
        } else {
            const live::AudioFrameBlock block{
                format, first, std::span(input, static_cast<std::size_t>(count) * format.channels), captured,
                overflow || pending_discontinuity, block_sequence, stream_epoch};
            const auto pushed = session->push(block);
            if (pushed == live::PushStatus::QueueFull) {
                refused.fetch_add(1, std::memory_order_relaxed);
                pending_discontinuity = true;
            } else if (pushed != live::PushStatus::Accepted) {
                rejected.store(true, std::memory_order_release);
                return 2;
            } else {
                delivered_frames.fetch_add(count, std::memory_order_relaxed);
                pending_discontinuity = false;
            }
        }
        next_frame.store(first + count, std::memory_order_release);
        return first + count >= capacity ? 1 : 0;
    }
};
} // namespace transcribe::detail
