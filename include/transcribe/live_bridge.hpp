#pragma once

#include "transcribe/live.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace transcribe::live::bridge {

inline constexpr std::uint32_t protocol_version = 1;

enum class CaptureState {
    Idle,
    Arming,
    Capturing,
    Interrupted,
    Finishing,
    Finished,
    Failed
};

std::string_view capture_state_name(CaptureState state) noexcept;
bool operation_allowed(std::string_view operation) noexcept;

struct Gap {
    std::uint64_t first_frame{};
    std::uint64_t end_frame{};
    std::string message;
};

struct Message {
    std::uint32_t version{protocol_version};
    std::uint64_t sequence{};
    bool resync{};
    std::map<std::string, LiveNote, std::less<>> upserts;
    std::map<std::string, std::uint64_t, std::less<>> retractions;
    std::map<std::string, LiveNote, std::less<>> finalizations;
    std::vector<Gap> gaps;
    std::vector<LiveEvent> events;
    std::uint64_t coalesced_events_dropped{};
    LiveMetrics metrics;
    CaptureState state{CaptureState::Idle};
    std::string status;
};

class Coalescer {
public:
    void append(const EventBatch& batch);
    void append_upsert(const LiveNote& note);
    void append_retraction(std::string id, std::uint64_t revision);
    bool has_pending() const noexcept;
    Message take(const LiveMetrics& metrics, CaptureState state, std::string status);

private:
    void add_event(const LiveEvent& event);
    void clear_pending();

    Message pending_;
    std::map<std::string, std::uint64_t, std::less<>> latest_revisions_;
    std::uint64_t next_sequence_{};
    std::uint64_t coalesced_events_dropped_{};
};

}  // namespace transcribe::live::bridge
