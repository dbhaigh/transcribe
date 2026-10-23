#include "transcribe/live_bridge.hpp"

#include <algorithm>

namespace transcribe::live::bridge {
namespace {

constexpr std::array<std::string_view, 25> allowed_operations{
    "ready", "browse", "transcribe", "start-capture", "stop-capture",
    "export-edits", "play-original", "stop-playback", "cancel",
    "open-output", "accept-proposal", "live-edit", "refine-live",
    "apply-refinement", "discard-refinement", "undo-refinement",
    "audition-event", "new-project", "open-project", "save-project",
    "save-score-copy", "autosave-project", "discard-recovery",
    "pause-analysis", "audition-source"};

bool revisioned_note_event(LiveEventKind kind) noexcept {
    switch (kind) {
    case LiveEventKind::NoteOn:
    case LiveEventKind::NoteStabilized:
    case LiveEventKind::NoteOff:
    case LiveEventKind::NoteCorrected:
    case LiveEventKind::NoteRetracted:
    case LiveEventKind::NoteFinalized:
        return true;
    case LiveEventKind::AnalysisGap:
    case LiveEventKind::Status:
        return false;
    }
    return false;
}

}  // namespace

std::string_view capture_state_name(CaptureState state) noexcept {
    switch (state) {
    case CaptureState::Idle: return "idle";
    case CaptureState::Arming: return "arming";
    case CaptureState::Capturing: return "capturing";
    case CaptureState::Interrupted: return "interrupted";
    case CaptureState::Finishing: return "finishing";
    case CaptureState::Finished: return "finished";
    case CaptureState::Failed: return "failed";
    }
    return "failed";
}

bool operation_allowed(std::string_view operation) noexcept {
    return std::find(allowed_operations.begin(), allowed_operations.end(), operation) !=
        allowed_operations.end();
}

void Coalescer::clear_pending() {
    coalesced_events_dropped_ += pending_.events.size();
    pending_.upserts.clear();
    pending_.retractions.clear();
    pending_.finalizations.clear();
    pending_.gaps.clear();
    pending_.events.clear();
}

void Coalescer::add_event(const LiveEvent& event) {
    constexpr std::size_t max_events = 64;
    if (pending_.events.size() == max_events) {
        pending_.events.erase(pending_.events.begin());
        ++coalesced_events_dropped_;
    }
    pending_.events.push_back(event);
}

void Coalescer::append(const EventBatch& batch) {
    if (batch.resync_required) {
        clear_pending();
        pending_.resync = true;
        for (const auto& note : batch.resync_snapshot) {
            if (note.id.empty()) continue;
            const auto [it, inserted] = latest_revisions_.try_emplace(note.id, note.revision);
            if (!inserted) it->second = std::max(it->second, std::uint64_t(note.revision));
            pending_.upserts[note.id] = note;
            if (note.finalized) pending_.finalizations[note.id] = note;
        }
        for (const auto& event : batch.events) {
            if (event.kind == LiveEventKind::AnalysisGap) {
                constexpr std::size_t max_gaps = 64;
                if (pending_.gaps.size() == max_gaps) {
                    pending_.gaps.erase(pending_.gaps.begin());
                }
                pending_.gaps.push_back(
                    {event.gap_first_frame, event.gap_end_frame, event.message});
                add_event(event);
            } else if (event.kind == LiveEventKind::Status) {
                add_event(event);
            }
        }
        return;
    }

    for (const auto& event : batch.events) {
        if (event.kind == LiveEventKind::AnalysisGap) {
            constexpr std::size_t max_gaps = 64;
            if (pending_.gaps.size() == max_gaps) {
                pending_.gaps.erase(pending_.gaps.begin());
            }
            pending_.gaps.push_back({event.gap_first_frame, event.gap_end_frame, event.message});
            add_event(event);
            continue;
        }
        if (event.kind == LiveEventKind::Status) {
            add_event(event);
            continue;
        }
        if (!revisioned_note_event(event.kind) || event.note.id.empty()) continue;

        const auto id = event.note.id;
        const auto revision = static_cast<std::uint64_t>(event.note.revision);
        if (const auto latest = latest_revisions_.find(id);
            latest != latest_revisions_.end() && revision <= latest->second) {
            continue;
        }
        latest_revisions_[id] = revision;
        switch (event.kind) {
        case LiveEventKind::NoteRetracted:
            pending_.upserts.erase(id);
            pending_.finalizations.erase(id);
            pending_.retractions[id] = revision;
            break;
        case LiveEventKind::NoteFinalized:
            pending_.upserts.erase(id);
            pending_.retractions.erase(id);
            pending_.finalizations[id] = event.note;
            break;
        case LiveEventKind::NoteOn:
        case LiveEventKind::NoteStabilized:
        case LiveEventKind::NoteOff:
        case LiveEventKind::NoteCorrected:
            pending_.retractions.erase(id);
            pending_.finalizations.erase(id);
            pending_.upserts[id] = event.note;
            break;
        case LiveEventKind::AnalysisGap:
        case LiveEventKind::Status:
            break;
        }
        add_event(event);
    }
}

void Coalescer::append_upsert(const LiveNote& note) {
    if (note.id.empty()) return;
    const auto revision = static_cast<std::uint64_t>(note.revision);
    if (const auto latest = latest_revisions_.find(note.id);
        latest != latest_revisions_.end() && revision <= latest->second) {
        return;
    }
    latest_revisions_[note.id] = revision;
    pending_.retractions.erase(note.id);
    if (note.finalized) {
        pending_.upserts.erase(note.id);
        pending_.finalizations[note.id] = note;
    } else {
        pending_.finalizations.erase(note.id);
        pending_.upserts[note.id] = note;
    }
}

void Coalescer::append_retraction(std::string id, std::uint64_t revision) {
    if (id.empty()) return;
    if (const auto latest = latest_revisions_.find(id);
        latest != latest_revisions_.end() && revision <= latest->second) {
        return;
    }
    latest_revisions_[id] = revision;
    pending_.upserts.erase(id);
    pending_.finalizations.erase(id);
    pending_.retractions[std::move(id)] = revision;
}

bool Coalescer::has_pending() const noexcept {
    return pending_.resync || !pending_.upserts.empty() || !pending_.retractions.empty() ||
        !pending_.finalizations.empty() || !pending_.gaps.empty() || !pending_.events.empty();
}

Message Coalescer::take(const LiveMetrics& metrics, CaptureState state, std::string status) {
    pending_.version = protocol_version;
    pending_.sequence = ++next_sequence_;
    pending_.metrics = metrics;
    pending_.state = state;
    pending_.status = std::move(status);
    pending_.coalesced_events_dropped = coalesced_events_dropped_;
    coalesced_events_dropped_ = 0;
    auto result = std::move(pending_);
    pending_ = {};
    return result;
}

}  // namespace transcribe::live::bridge
