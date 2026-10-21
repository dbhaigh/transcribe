#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>

#include "gui_transcription.hpp"
#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t app_origin[] = L"https://appassets.transcribe/";
constexpr UINT message_complete = WM_APP + 1;
constexpr UINT message_progress = WM_APP + 2;
constexpr UINT message_live_preview = WM_APP + 3;
constexpr UINT message_audio_preview = WM_APP + 4;
constexpr UINT message_live_session = WM_APP + 5;

enum class LiveEditKind { Add, Update, Delete, Fingering };

struct LiveEditCommand {
    LiveEditKind kind{LiveEditKind::Update};
    std::string note_id;
    int midi_pitch{-1};
    double onset_seconds{};
    double duration_seconds{};
    int string_number{};
    int fret{};
};

struct LiveSessionMessage;

struct AppState {
    HWND window{};
    HMODULE loader{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navigation_token{};
    EventRegistrationToken message_token{};
    std::jthread worker;
    std::mutex live_command_mutex;
    std::deque<LiveEditCommand> live_commands;
    std::mutex live_message_mutex;
    std::unique_ptr<LiveSessionMessage> pending_live_message;
    bool live_message_posted{};
    bool busy{};
    bool active_asio{};
    bool close_when_done{};
    bool shutting_down{};
    std::vector<transcribe::FingeringCorrection> manual_fingering_corrections;
    std::optional<transcribe::gui::Request> last_request;
    std::optional<transcribe::gui::Result> last_result;
    std::optional<transcribe::gui::Request> proposal_request;
    std::optional<transcribe::gui::Result> proposal_result;
    std::optional<transcribe::live::TranscriptionDocument> live_document;
    std::optional<transcribe::live::TranscriptionDocument> proposal_live_document;
    transcribe::AudioBuffer live_recording;
    transcribe::AudioBuffer proposal_live_recording;
    std::string live_backend_id;
    std::string proposal_live_backend_id;
    std::optional<transcribe::live::ScoreProposal> pending_live_proposal;
    std::optional<transcribe::live::ProposalReview> pending_live_review;
    std::optional<transcribe::gui::Result> pre_refinement_result;
    std::optional<transcribe::live::TranscriptionDocument> pre_refinement_document;
    std::uint64_t refinement_result_revision{};
};

enum class RuntimeStartup {
    ready,
    setup_started,
    cancelled,
    failed
};

struct ProgressMessage {
    int percent{};
    std::wstring status;
};

struct LiveMessage {
    double offset{};
    std::vector<transcribe::Note> notes;
    std::vector<transcribe::NoteEvent> events;
};

struct LiveSessionMessage {
    double recording_seconds{};
    std::uint32_t sample_rate{};
    std::vector<transcribe::live::LiveNote> notes;
    std::vector<transcribe::live::LiveEvent> events;
    transcribe::live::LiveMetrics metrics;
    std::unordered_map<std::string, std::pair<int, int>> fingerings;
};

struct AudioPreviewMessage {
    double offset{};
    bool replace{};
    transcribe::AudioPreviewData preview;
};

struct CompletionMessage {
    bool success{};
    bool export_only{};
    bool proposal_only{};
    std::uint64_t accepted_revision{};
    std::string error;
    transcribe::gui::Result result;
    transcribe::gui::Request request;
    bool live_capture{};
    transcribe::AudioBuffer live_recording;
    std::optional<transcribe::live::TranscriptionDocument> live_document;
    std::string live_backend_id;
    bool refinement_proposal{};
    bool refinement_applied{};
    bool refinement_undo{};
    std::size_t withheld_conflicts{};
    std::optional<transcribe::live::ScoreProposal> score_proposal;
    std::optional<transcribe::live::ProposalReview> proposal_review;
};

template <typename Interface>
class RefCountedHandler : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, interface_id())) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

protected:
    virtual ~RefCountedHandler() = default;
    virtual const IID& interface_id() const = 0;

private:
    std::atomic<ULONG> references_{1};
};

class EnvironmentCompleted final
    : public RefCountedHandler<
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    explicit EnvironmentCompleted(AppState& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code,
                                     ICoreWebView2Environment* environment) override;

private:
    const IID& interface_id() const override {
        return IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
    }
    AppState& state_;
};

class ControllerCompleted final
    : public RefCountedHandler<
          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    explicit ControllerCompleted(AppState& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT error_code,
                                     ICoreWebView2Controller* controller) override;

private:
    const IID& interface_id() const override {
        return IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
    }
    AppState& state_;
};

class NavigationStarting final
    : public RefCountedHandler<ICoreWebView2NavigationStartingEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2NavigationStartingEventArgs* args) override;

private:
    const IID& interface_id() const override {
        return IID_ICoreWebView2NavigationStartingEventHandler;
    }
};

class WebMessageReceived final
    : public RefCountedHandler<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    explicit WebMessageReceived(AppState& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                     ICoreWebView2WebMessageReceivedEventArgs* args) override;

private:
    const IID& interface_id() const override {
        return IID_ICoreWebView2WebMessageReceivedEventHandler;
    }
    AppState& state_;
};

std::wstring from_utf8(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::invalid_argument("The interface sent invalid UTF-8 text.");
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) != length) {
        throw std::runtime_error("Could not decode text from the interface.");
    }
    return result;
}

std::wstring from_utf8_lossy(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return L"Text could not be decoded.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) {
        throw std::runtime_error("Could not encode text for the transcription engine.");
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length,
                            nullptr, nullptr) != length) {
        throw std::runtime_error("Could not encode text for the transcription engine.");
    }
    return result;
}

std::wstring json_escape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const wchar_t character : value) {
        switch (character) {
        case L'"':
            result += L"\\\"";
            break;
        case L'\\':
            result += L"\\\\";
            break;
        case L'\b':
            result += L"\\b";
            break;
        case L'\f':
            result += L"\\f";
            break;
        case L'\n':
            result += L"\\n";
            break;
        case L'\r':
            result += L"\\r";
            break;
        case L'\t':
            result += L"\\t";
            break;
        default:
            if (character < 0x20) {
                constexpr wchar_t hex[] = L"0123456789abcdef";
                result += L"\\u00";
                result += hex[(character >> 4) & 0xf];
                result += hex[character & 0xf];
            } else {
                result += character;
            }
            break;
        }
    }
    return result;
}

void post_json(AppState& state, const std::wstring& json) {
    if (!state.webview) {
        return;
    }
    if (FAILED(state.webview->PostWebMessageAsJson(json.c_str()))) {
        OutputDebugStringW(L"Transcriber could not post a message to the WebView2 page.\n");
    }
}

void post_error(AppState& state, std::string_view message) {
    post_json(state, L"{\"type\":\"error\",\"message\":\"" +
                         json_escape(from_utf8_lossy(message)) + L"\"}");
}

void post_live_session(AppState& state,
                       std::unique_ptr<LiveSessionMessage> message) {
    bool should_post = false;
    {
        std::lock_guard lock(state.live_message_mutex);
        state.pending_live_message = std::move(message);
        if (!state.live_message_posted) {
            state.live_message_posted = true;
            should_post = true;
        }
    }
    if (should_post &&
        !PostMessageW(state.window, message_live_session, 0, 0)) {
        std::lock_guard lock(state.live_message_mutex);
        state.live_message_posted = false;
        state.pending_live_message.reset();
    }
}

std::wstring key_name(const transcribe::ScoreMetadata& metadata) {
    static constexpr std::array<const wchar_t*, 15> major_names{
        L"Cb", L"Gb", L"Db", L"Ab", L"Eb", L"Bb", L"F", L"C",
        L"G", L"D", L"A", L"E", L"B", L"F#", L"C#"};
    static constexpr std::array<const wchar_t*, 15> minor_names{
        L"Ab", L"Eb", L"Bb", L"F", L"C", L"G", L"D", L"A",
        L"E", L"B", L"F#", L"C#", L"G#", L"D#", L"A#"};
    const auto index = static_cast<std::size_t>(
        std::clamp(metadata.key_fifths, -7, 7) + 7);
    return std::wstring(metadata.minor_key ? minor_names[index] : major_names[index]) +
           (metadata.minor_key ? L" minor" : L" major");
}

std::wstring notes_json(const std::vector<transcribe::Note>& notes, double offset = 0.0) {
    std::wostringstream json;
    json << L'[';
    for (std::size_t index = 0; index < notes.size(); ++index) {
        if (index != 0) {
            json << L',';
        }
        const auto& note = notes[index];
        json << L"{\"midi\":" << note.midi_pitch
             << L",\"name\":\"" << json_escape(from_utf8(note.name)) << L'"'
             << L",\"start\":" << std::fixed << std::setprecision(5)
             << note.start_seconds + offset
             << L",\"duration\":" << note.duration_seconds
             << L",\"confidence\":" << note.confidence
             << L",\"amplitude\":" << note.amplitude << L'}';
    }
    json << L']';
    return json.str();
}

std::wstring lifecycle_name(transcribe::EventLifecycle state) {
    switch (state) {
    case transcribe::EventLifecycle::Provisional: return L"provisional";
    case transcribe::EventLifecycle::Stabilized: return L"stabilized";
    case transcribe::EventLifecycle::Refined: return L"refined";
    case transcribe::EventLifecycle::ManuallyCorrected: return L"manually_corrected";
    case transcribe::EventLifecycle::Rejected: return L"rejected";
    }
    return L"unknown";
}

std::wstring live_notes_json(
    const std::vector<transcribe::live::LiveNote>& notes,
    const std::unordered_map<std::string, std::pair<int, int>>& fingerings,
    double recording_seconds) {
    std::wostringstream json;
    json << L'[';
    bool first = true;
    for (const auto& note : notes) {
        if (note.state == transcribe::EventLifecycle::Rejected) continue;
        const double start = note.onset_seconds();
        const double end = note.offset_seconds().value_or(recording_seconds);
        if (!std::isfinite(start) || !std::isfinite(end) || end <= start) continue;
        if (!first) json << L',';
        first = false;
        json << L"{\"id\":\"" << json_escape(from_utf8(note.id))
             << L"\",\"midi\":" << note.midi_pitch
             << L",\"name\":\"" << json_escape(from_utf8(
                    transcribe::midi_note_name(note.midi_pitch))) << L'"'
             << L",\"start\":" << std::fixed << std::setprecision(6) << start
             << L",\"duration\":" << std::max(0.001, end - start)
             << L",\"confidence\":null,\"amplitude\":null,\"raw_strength\":"
             << note.raw_strength << L",\"state\":\"" << lifecycle_name(note.state)
             << L"\",\"uncertain\":" << (note.onset_truncated ? L"true" : L"false")
             << L",\"user_authored\":"
             << (note.state == transcribe::EventLifecycle::ManuallyCorrected
                     ? L"true" : L"false")
             << L",\"source_model\":\"" << json_escape(from_utf8(note.source_model))
             << L"\",\"detection_method\":\""
             << json_escape(from_utf8(note.detection_method))
             << L"\",\"measured_start\":" << start << L",\"measured_end\":" << end;
        if (const auto position = fingerings.find(note.id); position != fingerings.end()) {
            json << L",\"string\":" << position->second.first
                 << L",\"fret\":" << position->second.second
                 << L",\"manual_fingering\":true";
        }
        json << L'}';
    }
    json << L']';
    return json.str();
}

std::wstring live_events_json(const std::vector<transcribe::live::LiveEvent>& events,
                              std::uint32_t sample_rate) {
    std::wostringstream json;
    json << L'[';
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i != 0) json << L',';
        const auto& event = events[i];
        const wchar_t* type = L"status";
        switch (event.kind) {
        case transcribe::live::LiveEventKind::NoteOn: type = L"provisional"; break;
        case transcribe::live::LiveEventKind::NoteStabilized: type = L"stabilized"; break;
        case transcribe::live::LiveEventKind::NoteOff: type = L"note_off"; break;
        case transcribe::live::LiveEventKind::NoteCorrected: type = L"corrected"; break;
        case transcribe::live::LiveEventKind::NoteRetracted: type = L"retracted"; break;
        case transcribe::live::LiveEventKind::AnalysisGap: type = L"gap"; break;
        case transcribe::live::LiveEventKind::Status: type = L"status"; break;
        }
        double time = event.note.onset_seconds();
        if (event.kind == transcribe::live::LiveEventKind::NoteOff &&
            event.note.offset_frame && sample_rate != 0) {
            time = static_cast<double>(*event.note.offset_frame) / sample_rate;
        }
        const double latency = std::chrono::duration<double, std::milli>(
            event.timing.emitted_time - event.timing.onset_capture_time).count();
        json << L"{\"type\":\"" << type << L"\",\"midi\":"
             << event.note.midi_pitch << L",\"time\":" << std::fixed
             << std::setprecision(6) << time << L",\"latency_ms\":"
             << std::max(0.0, latency) << L",\"message\":\""
             << json_escape(from_utf8(event.message)) << L"\"}";
    }
    json << L']';
    return json.str();
}

std::wstring live_metrics_json(const transcribe::live::LiveMetrics& metrics) {
    const auto summary = [](const transcribe::live::LatencySummary& value) {
        std::wostringstream output;
        output << L"{\"mean_ms\":" << value.mean_seconds * 1000.0
               << L",\"p95_ms\":" << value.p95_seconds * 1000.0
               << L",\"count\":" << value.count << L'}';
        return output.str();
    };
    std::wostringstream json;
    json << L"{\"capture\":" << summary(metrics.capture_latency)
         << L",\"analysis\":" << summary(metrics.analysis_latency)
         << L",\"end_to_end\":" << summary(metrics.end_to_end_latency)
         << L",\"stabilization\":" << summary(metrics.stabilization_delay)
         << L",\"input_queue_capacity\":" << metrics.input_queue_capacity
         << L",\"input_queue_high_water\":" << metrics.input_queue_high_water
         << L",\"event_queue_capacity\":" << metrics.event_queue_capacity
         << L",\"event_queue_high_water\":" << metrics.event_queue_high_water
         << L",\"blocks_refused\":" << metrics.blocks_refused
         << L",\"events_dropped\":" << metrics.events_dropped
         << L",\"gap_frames\":" << metrics.gap_frames << L'}';
    return json.str();
}

#ifdef TRANSCRIBE_ENABLE_ASIO
std::wstring asio_devices_json() {
    const auto devices = transcribe::enumerate_asio_input_devices();
    std::wostringstream json;
    json << L"{\"type\":\"asio-devices\",\"devices\":[";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (i != 0) json << L',';
        const auto& device = devices[i];
        json << L"{\"id\":" << device.device_id << L",\"name\":\""
             << json_escape(from_utf8(device.name)) << L"\",\"channels\":"
             << device.input_channels << L",\"preferred_sample_rate\":"
             << device.preferred_sample_rate << L",\"default\":"
             << (device.default_input ? L"true" : L"false")
             << L",\"sample_rates\":[";
        for (std::size_t rate = 0; rate < device.sample_rates.size(); ++rate) {
            if (rate != 0) json << L',';
            json << device.sample_rates[rate];
        }
        json << L"]}";
    }
    json << L"]}";
    return json.str();
}
#endif

std::wstring review_note_json(
    const std::optional<transcribe::live::LiveNote>& note) {
    if (!note) return L"null";
    const double start = note->onset_seconds();
    const double end = note->offset_seconds().value_or(start);
    return L"{\"id\":\"" + json_escape(from_utf8(note->id)) +
        L"\",\"midi\":" + std::to_wstring(note->midi_pitch) +
        L",\"name\":\"" + json_escape(from_utf8(
            transcribe::midi_note_name(note->midi_pitch))) +
        L"\",\"start\":" + std::to_wstring(start) +
        L",\"duration\":" + std::to_wstring(std::max(0.0, end - start)) + L"}";
}

std::wstring refinement_review_json(
    const transcribe::live::ScoreProposal& proposal,
    const transcribe::live::ProposalReview& review) {
    std::wostringstream json;
    json << L"{\"type\":\"refinement-proposal\",\"success\":true"
         << L",\"proposal_id\":\"" << json_escape(from_utf8(proposal.id))
         << L"\",\"backend_id\":\"" << json_escape(from_utf8(proposal.backend_id))
         << L"\",\"inference_seconds\":" << proposal.inference_seconds
         << L",\"base_revision\":" << review.base_revision
         << L",\"conflicts\":" << review.conflicts << L",\"changes\":[";
    for (std::size_t i = 0; i < review.changes.size(); ++i) {
        if (i != 0) json << L',';
        const auto& change = review.changes[i];
        const wchar_t* kind = change.kind == transcribe::live::ProposedChangeKind::Add
            ? L"add" : change.kind == transcribe::live::ProposedChangeKind::Modify
                ? L"modify" : L"remove";
        json << L"{\"change_id\":\"" << json_escape(from_utf8(change.change_id))
             << L"\",\"kind\":\"" << kind << L"\",\"note_id\":\""
             << json_escape(from_utf8(change.note_id))
             << L"\",\"conflict\":"
             << (change.conflicts_with_manual_edit ? L"true" : L"false")
             << L",\"current\":" << review_note_json(change.current)
             << L",\"proposed\":" << review_note_json(change.proposed) << L'}';
    }
    json << L"]}";
    return json.str();
}

std::wstring score_notes_json(const transcribe::ScoreDocument& score) {
    std::vector<const transcribe::ScoreNote*> ordered;
    ordered.reserve(score.notes.size());
    for (const auto& note : score.notes) ordered.push_back(&note);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->onset_quarters != right->onset_quarters) {
            return left->onset_quarters < right->onset_quarters;
        }
        if (left->sounding_midi_pitch != right->sounding_midi_pitch) {
            return left->sounding_midi_pitch < right->sounding_midi_pitch;
        }
        return left->id < right->id;
    });
    std::wostringstream json;
    json << L'[';
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (index) json << L',';
        const auto& note = *ordered[index];
        const double start =
            transcribe::score_position_seconds(score, note.onset_quarters);
        const double end = transcribe::score_position_seconds(
            score, note.onset_quarters + note.duration_quarters);
        const transcribe::CanonicalNoteEvent* source_event = nullptr;
        if (!note.source_event_ids.empty()) {
            const auto source = std::find_if(
                score.source_events.begin(), score.source_events.end(),
                [&](const auto& event) {
                    return event.id == note.source_event_ids.front();
                });
            if (source != score.source_events.end()) source_event = &*source;
        }
        json << L"{\"id\":\"" << json_escape(from_utf8(note.id)) << L'"'
             << L",\"source_event_ids\":[";
        for (std::size_t source_index = 0;
             source_index < note.source_event_ids.size(); ++source_index) {
            if (source_index != 0) json << L',';
            json << L'"' << json_escape(
                from_utf8(note.source_event_ids[source_index])) << L'"';
        }
        json << L']'
             << L",\"midi\":" << note.sounding_midi_pitch
             << L",\"name\":\""
             << json_escape(from_utf8(transcribe::midi_note_name(
                    note.sounding_midi_pitch))) << L'"'
             << L",\"start\":" << std::fixed << std::setprecision(7) << start
             << L",\"duration\":" << std::max(0.0, end - start)
             << L",\"measured_start\":" << note.measured_onset_seconds
             << L",\"measured_end\":" << note.measured_offset_seconds
             << L",\"confidence\":";
        if (note.calibrated_confidence) json << *note.calibrated_confidence;
        else json << L"null";
        json << L",\"amplitude\":";
        if (note.intensity_rms) json << *note.intensity_rms;
        else json << L"null";
        json << L",\"velocity\":";
        if (note.velocity) json << *note.velocity;
        else json << L"null";
        json << L",\"detected_midi\":";
        if (source_event) json << source_event->midi_pitch;
        else json << L"null";
        json << L",\"user_authored\":"
             << (note.user_authored ? L"true" : L"false");
        json << L",\"source_model\":\""
             << (source_event
                     ? json_escape(from_utf8(source_event->source_model))
                     : std::wstring{})
             << L"\",\"detection_method\":\""
             << (source_event
                     ? json_escape(from_utf8(source_event->detection_method))
                     : std::wstring{})
             << L"\",\"evidence_kind\":\""
             << (source_event
                     ? json_escape(from_utf8(source_event->evidence_kind))
                     : std::wstring{})
             << L"\",\"raw_evidence_count\":" << note.raw_evidence.size()
             << L",\"raw_evidence_truncated\":"
             << (note.raw_evidence.size() > 64 ? L"true" : L"false")
             << L",\"raw_evidence\":[";
        const std::size_t evidence_samples =
            std::min<std::size_t>(64, note.raw_evidence.size());
        for (std::size_t sample = 0; sample < evidence_samples; ++sample) {
            const std::size_t evidence_index = evidence_samples <= 1
                ? 0
                : sample * (note.raw_evidence.size() - 1) /
                      (evidence_samples - 1);
            if (evidence_index) json << L',';
            const auto& evidence = note.raw_evidence[evidence_index];
            json << L"{\"time\":" << evidence.time_seconds
                 << L",\"midi_pitch\":" << evidence.midi_pitch
                 << L",\"raw_strength\":" << evidence.raw_strength
                 << L",\"raw_voicing\":" << evidence.raw_voicing
                 << L",\"fractional_midi\":";
            if (evidence.fractional_midi) json << *evidence.fractional_midi;
            else json << L"null";
            json << L",\"raw_model_value\":";
            if (evidence.raw_model_value) json << *evidence.raw_model_value;
            else json << L"null";
            json << L",\"raw_midi_pitch\":" << evidence.raw_midi_pitch << L'}';
        }
        json << L"]}";
    }
    json << L']';
    return json.str();
}

std::wstring score_metadata_json(const transcribe::ScoreDocument& score) {
    const auto& metadata = score.metadata;
    std::wostringstream json;
    json << L"{\"tempo\":" << std::fixed << std::setprecision(2)
         << metadata.tempo_bpm
         << L",\"tempo_inferred\":" << (metadata.tempo_inferred ? L"true" : L"false")
         << L",\"key\":\""
         << json_escape(metadata.key_inferred ? key_name(metadata) : L"")
         << L"\",\"key_inferred\":" << (metadata.key_inferred ? L"true" : L"false")
         << L",\"key_fifths\":" << metadata.key_fifths
         << L",\"minor_key\":" << (metadata.minor_key ? L"true" : L"false")
         << L",\"time_numerator\":" << metadata.time_numerator
         << L",\"time_denominator\":" << metadata.time_denominator
         << L",\"meter_inferred\":" << (metadata.meter_inferred ? L"true" : L"false")
         << L",\"swing_ratio\":" << metadata.swing_ratio
         << L",\"swing_inferred\":" << (metadata.swing_inferred ? L"true" : L"false")
         << L",\"score_id\":\"" << json_escape(from_utf8(score.id))
         << L"\",\"score_revision\":" << score.revision << L'}';
    return json.str();
}

std::wstring events_json(const std::vector<transcribe::NoteEvent>& events) {
    std::wostringstream json;
    json << L'[';
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (index != 0) {
            json << L',';
        }
        const auto& event = events[index];
        json << L"{\"type\":\""
             << (event.type == transcribe::NoteEventType::Onset ? L"onset" : L"offset")
             << L"\",\"midi\":" << event.midi_pitch
             << L",\"time\":" << std::fixed << std::setprecision(5)
             << event.time_seconds << L",\"confidence\":" << event.confidence
             << L",\"latency_ms\":"
             << std::max(0.0, event.detected_at_seconds - event.time_seconds) *
                    1000.0
             << L'}';
    }
    json << L']';
    return json.str();
}

std::wstring audio_preview_json(double offset, bool replace,
                                const transcribe::AudioPreviewData& preview) {
    std::wostringstream json;
    json << L"{\"type\":\"audio-preview\",\"offset\":" << std::fixed
         << std::setprecision(4) << offset
         << L",\"replace\":" << (replace ? L"true" : L"false")
         << L",\"duration\":" << preview.duration_seconds
         << L",\"columns\":" << preview.columns
         << L",\"bands\":" << preview.frequency_bands << L",\"waveform\":[";
    for (std::size_t index = 0; index < preview.waveform.size(); ++index) {
        if (index != 0) json << L',';
        json << std::setprecision(3) << preview.waveform[index];
    }
    json << L"],\"spectrogram\":[";
    for (std::size_t index = 0; index < preview.spectrogram.size(); ++index) {
        if (index != 0) json << L',';
        json << std::setprecision(3) << preview.spectrogram[index];
    }
    json << L"]}";
    return json.str();
}

std::wstring fingering_json(const transcribe::FingeringResult& fingering) {
    const auto write_cost = [](std::wostringstream& json,
                               const transcribe::FingeringCostComponents& cost) {
        json << L"{\"total\":" << std::setprecision(4) << cost.total()
             << L",\"fret_height\":" << cost.fret_height
             << L",\"preferred_position\":" << cost.preferred_position
             << L",\"hand_movement\":" << cost.hand_movement
             << L",\"string_preference\":" << cost.string_preference
             << L",\"chord_stretch\":" << cost.chord_stretch
             << L",\"unassigned_notes\":" << cost.unassigned_notes << L'}';
    };
    std::wostringstream json;
    json << L"{\"fully_playable\":"
         << (fingering.fully_playable ? L"true" : L"false")
         << L",\"search_truncated\":"
         << (fingering.search_truncated ? L"true" : L"false")
         << L",\"cost\":";
    write_cost(json, fingering.cost);
    json << L",\"notes\":[";
    for (std::size_t index = 0; index < fingering.notes.size(); ++index) {
        if (index != 0) json << L',';
        const auto& note = fingering.notes[index];
        json << L"{\"note_index\":" << note.position.note_index
             << L",\"string\":" << note.position.string_number
             << L",\"fret\":" << note.position.fret
             << L",\"playable\":" << (note.playable ? L"true" : L"false")
             << L",\"ambiguous\":" << (note.ambiguous ? L"true" : L"false")
             << L",\"manual\":"
             << (note.position.manually_corrected ? L"true" : L"false")
             << L",\"correction_rejected\":"
             << (note.correction_rejected ? L"true" : L"false")
             << L",\"candidates\":[";
        for (std::size_t candidate_index = 0;
             candidate_index < note.candidates.size(); ++candidate_index) {
            if (candidate_index != 0) json << L',';
            const auto& candidate = note.candidates[candidate_index];
            json << L"{\"string\":" << candidate.string_number
                 << L",\"fret\":" << candidate.fret
                 << L",\"sequence_cost\":" << std::setprecision(4)
                 << candidate.sequence_cost << L",\"cost_components\":";
            write_cost(json, candidate.sequence_cost_components);
            json << L'}';
        }
        json << L"]}";
    }
    json << L"],\"alternatives\":[";
    for (std::size_t index = 0; index < fingering.alternatives.size(); ++index) {
        if (index != 0) json << L',';
        const auto& alternative = fingering.alternatives[index];
        json << L"{\"cost\":";
        write_cost(json, alternative.cost);
        json << L",\"positions\":[";
        for (std::size_t position_index = 0;
             position_index < alternative.positions.size(); ++position_index) {
            if (position_index != 0) json << L',';
            const auto& position = alternative.positions[position_index];
            json << L"{\"note_index\":" << position.note_index
                 << L",\"string\":" << position.string_number
                 << L",\"fret\":" << position.fret << L'}';
        }
        json << L"]}";
    }
    json << L"],\"rejected_corrections\":[";
    for (std::size_t index = 0; index < fingering.rejected_corrections.size();
         ++index) {
        if (index != 0) json << L',';
        json << fingering.rejected_corrections[index];
    }
    json << L"]}";
    return json.str();
}

std::wstring result_json(const CompletionMessage& completion) {
    if (!completion.success) {
        return L"{\"type\":\"complete\",\"success\":false,\"error\":\"" +
               json_escape(from_utf8_lossy(completion.error)) +
               L"\",\"live_capture\":" +
               std::wstring(completion.live_capture ? L"true" : L"false") + L"}";
    }

    const auto& metadata = completion.result.metadata;
    std::wostringstream summary;
    summary << L"Detected " << completion.result.notes.size() << L" notes. Tempo: ";
    if (metadata.tempo_inferred) {
        summary << std::fixed << std::setprecision(1) << metadata.tempo_bpm << L" BPM";
    } else {
        summary << L"uncertain; 120 BPM default";
    }
    summary << L". Meter: ";
    if (metadata.meter_inferred) {
        summary << metadata.time_numerator << L'/' << metadata.time_denominator;
    } else {
        summary << L"uncertain; 4/4 default";
    }
    summary << L". Key: ";
    if (metadata.key_inferred) {
        summary << key_name(metadata);
    } else {
        summary << L"uncertain; C major default";
    }
    if (!completion.proposal_only) {
        summary << L".\nMusicXML: " << completion.request.musicxml_path.wstring()
                << L"\nGuitar tab: " << completion.request.tab_path.wstring();
        if (!completion.request.midi_path.empty()) {
            summary << L"\nMIDI: " << completion.request.midi_path.wstring();
        }
    } else {
        summary << L".\nExisting exports are unchanged.";
    }
    if (completion.proposal_only) {
        summary << L"\nThis is a proposal. Accepted score revision "
                << completion.accepted_revision << L" remains unchanged.";
    }

    std::wostringstream json;
    json << L"{\"type\":\""
         << (completion.proposal_only ? L"proposal" : L"complete")
         << L"\",\"success\":true,\"accepted_revision\":"
         << completion.accepted_revision << L",\"notes\":"
         << score_notes_json(completion.result.score)
         << L",\"metadata\":" << score_metadata_json(completion.result.score)
         << L",\"fingering\":" << fingering_json(completion.result.fingering)
         << L",\"summary\":\"" << json_escape(summary.str()) << L"\""
         << L",\"live_capture\":"
         << (completion.live_capture ? L"true" : L"false")
         << L",\"backend_id\":\""
         << json_escape(from_utf8(completion.live_backend_id)) << L"\"";
#ifdef TRANSCRIBE_ENABLE_VEROVIO
    if (!completion.proposal_only) {
        json << L",\"musicxml\":\""
             << json_escape(from_utf8(completion.result.musicxml_source)) << L"\"";
    }
#endif
    json << L'}';
    return json.str();
}

void handle_web_message(AppState& state, const std::string& message);

void resize_webview(AppState& state) {
    if (!state.controller) {
        return;
    }
    RECT bounds{};
    GetClientRect(state.window, &bounds);
    state.controller->put_Bounds(bounds);
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Could not locate the application directory.");
    }
    return std::filesystem::path(path.data()).parent_path();
}

RuntimeStartup prepare_webview_runtime(AppState& state) {
    state.loader = LoadLibraryW(L"WebView2Loader.dll");
    if (state.loader == nullptr) {
        MessageBoxW(nullptr, L"WebView2Loader.dll is missing beside the application executable.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }

    using GetAvailableVersionFunction =
        decltype(&GetAvailableCoreWebView2BrowserVersionString);
    const auto get_available_version =
        reinterpret_cast<GetAvailableVersionFunction>(GetProcAddress(
            state.loader, "GetAvailableCoreWebView2BrowserVersionString"));
    if (get_available_version == nullptr) {
        MessageBoxW(nullptr, L"The WebView2 loader is missing a required entry point.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }

    LPWSTR version = nullptr;
    const HRESULT result = get_available_version(nullptr, &version);
    const bool runtime_available =
        SUCCEEDED(result) && version != nullptr && version[0] != L'\0';
    CoTaskMemFree(version);
    if (runtime_available) {
        return RuntimeStartup::ready;
    }

    const auto bootstrapper =
        executable_directory() / L"MicrosoftEdgeWebView2Setup.exe";
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(bootstrapper, filesystem_error)) {
        MessageBoxW(nullptr,
                    L"The Microsoft Edge WebView2 Runtime is not installed, and its "
                    L"Evergreen installer is missing beside Transcriber.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }

    const int choice = MessageBoxW(
        nullptr,
        L"The Microsoft Edge WebView2 Runtime is required but is not installed.\n\n"
        L"Would you like to start the included Evergreen installer? Internet access "
        L"is required. Restart Transcriber after installation.",
        L"Transcriber", MB_YESNO | MB_ICONINFORMATION);
    if (choice != IDYES) {
        return RuntimeStartup::cancelled;
    }

    SHELLEXECUTEINFOW execute_info{};
    execute_info.cbSize = sizeof(execute_info);
    execute_info.fMask = SEE_MASK_NOASYNC;
    execute_info.lpVerb = L"open";
    execute_info.lpFile = bootstrapper.c_str();
    execute_info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute_info)) {
        const auto error = std::to_wstring(GetLastError());
        const auto message =
            L"Could not start the WebView2 installer (Windows error " + error + L").";
        MessageBoxW(nullptr, message.c_str(), L"Transcriber", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }
    return RuntimeStartup::setup_started;
}

std::filesystem::path webview_user_data_directory() {
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(),
        static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) {
        throw std::runtime_error("The LOCALAPPDATA folder could not be located.");
    }
    auto directory = std::filesystem::path(local_app_data.data()) /
                     L"Transcribe" / L"WebView2";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "Could not create WebView2's user data folder.", directory, error);
    }
    return directory;
}

void initialize_webview(AppState& state) {
    using CreateEnvironmentFunction =
        decltype(&CreateCoreWebView2EnvironmentWithOptions);
    const auto create_environment = reinterpret_cast<CreateEnvironmentFunction>(
        GetProcAddress(state.loader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (create_environment == nullptr) {
        throw std::runtime_error("The WebView2 loader is missing a required entry point.");
    }

    const auto user_data = webview_user_data_directory();
    auto* callback = new EnvironmentCompleted(state);
    const HRESULT result =
        create_environment(nullptr, user_data.c_str(), nullptr, callback);
    callback->Release();
    if (FAILED(result)) {
        throw std::runtime_error("Could not initialize the WebView2 environment.");
    }
}

std::string url_decode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded += ' ';
        } else if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex_value = [](char character) -> int {
                if (character >= '0' && character <= '9') return character - '0';
                if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                return -1;
            };
            const int high = hex_value(value[index + 1]);
            const int low = hex_value(value[index + 2]);
            if (high < 0 || low < 0) {
                throw std::invalid_argument("The interface sent malformed URL-encoded data.");
            }
            decoded += static_cast<char>((high << 4) | low);
            index += 2;
        } else {
            decoded += value[index];
        }
    }
    return decoded;
}

std::unordered_map<std::string, std::string> parse_fields(std::string_view query) {
    std::unordered_map<std::string, std::string> fields;
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto pair = query.substr(0, separator);
        const auto equal = pair.find('=');
        fields[url_decode(pair.substr(0, equal))] =
            equal == std::string_view::npos ? std::string{} :
                                              url_decode(pair.substr(equal + 1));
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return fields;
}

std::string field(const std::unordered_map<std::string, std::string>& fields,
                  const char* name) {
    const auto found = fields.find(name);
    return found == fields.end() ? std::string{} : found->second;
}

std::array<int, 6> guitar_tuning(const std::string& name) {
    if (name == "standard") return {64, 59, 55, 50, 45, 40};
    if (name == "drop-d") return {64, 59, 55, 50, 45, 38};
    if (name == "dadgad") return {62, 57, 55, 50, 45, 38};
    if (name == "open-d") return {62, 57, 54, 50, 45, 38};
    if (name == "open-g") return {62, 59, 55, 50, 43, 38};
    throw std::invalid_argument("Choose a supported guitar tuning.");
}

transcribe::InstrumentPrior instrument_prior(const std::string& name) {
    if (name == "auto") return transcribe::InstrumentPrior::None;
    if (name == "guitar") return transcribe::InstrumentPrior::Guitar;
    if (name == "piano") return transcribe::InstrumentPrior::Piano;
    if (name == "voice") return transcribe::InstrumentPrior::Voice;
    throw std::invalid_argument("Choose a supported instrument prior.");
}

void set_browser_field(AppState& state, const char* name, const std::wstring& value) {
    post_json(state, L"{\"type\":\"field\",\"field\":\"" + from_utf8(name) +
                         L"\",\"value\":\"" + json_escape(value) + L"\"}");
}

void browse_for_path(AppState& state, const std::string& field_name,
                     const std::wstring& current_value) {
    if (field_name != "input" && field_name != "model" &&
        field_name != "musicxml" && field_name != "tab" && field_name != "midi") {
        throw std::invalid_argument("Unknown file picker field.");
    }

    std::array<wchar_t, 32768> filename{};
    if (current_value.size() < filename.size()) {
        std::copy(current_value.begin(), current_value.end(), filename.begin());
    }
    const bool save = field_name == "musicxml" || field_name == "tab" ||
                      field_name == "midi";
    const wchar_t* filter = nullptr;
    const wchar_t* extension = nullptr;
    if (field_name == "input") {
#ifdef TRANSCRIBE_ENABLE_FFMPEG
        filter = L"Audio files (*.wav;*.mp3;*.flac;*.ogg;*.m4a)\0*.wav;*.mp3;*.flac;*.ogg;*.m4a\0All files (*.*)\0*.*\0\0";
#else
        filter = L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0";
#endif
    } else if (field_name == "model") {
        filter = L"ONNX models (*.onnx)\0*.onnx\0All files (*.*)\0*.*\0\0";
    } else if (field_name == "musicxml") {
        filter = L"MusicXML (*.musicxml;*.xml)\0*.musicxml;*.xml\0All files (*.*)\0*.*\0\0";
        extension = L"musicxml";
    } else if (field_name == "tab") {
        filter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
        extension = L"txt";
    } else {
        filter = L"MIDI files (*.mid)\0*.mid\0All files (*.*)\0*.*\0\0";
        extension = L"mid";
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = extension;
    const BOOL accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (accepted) {
        set_browser_field(state, field_name.c_str(), filename.data());
    } else if (CommDlgExtendedError() != 0) {
        post_error(state, "The file dialog could not be opened.");
    }
}

double parse_number(std::string_view text, const char* description);

transcribe::gui::Result build_live_result(
    const transcribe::gui::Request& request,
    const transcribe::live::TranscriptionDocument& live_document,
    std::uint32_t sample_rate, std::uint64_t recording_frames,
    const std::unordered_map<std::string, std::pair<int, int>>& fingerings) {
    if (sample_rate == 0) {
        throw std::invalid_argument("Live recording has an invalid sample rate.");
    }
    std::vector<transcribe::CanonicalNoteEvent> events;
    std::unordered_map<std::string, transcribe::live::LiveNote> original_notes;
    for (const auto& event : live_document.original_events()) {
        if (event.kind == transcribe::live::LiveEventKind::NoteOn) {
            original_notes.try_emplace(event.note.id, event.note);
        }
    }
    const auto current_notes = live_document.notes();
    events.reserve(current_notes.size());
    for (const auto& note : current_notes) {
        const auto onset = note.onset_frame;
        const auto offset = std::max(
            note.offset_frame.value_or(recording_frames), onset + 1);
        transcribe::CanonicalNoteEvent event;
        event.id = note.id;
        event.midi_pitch = note.midi_pitch;
        event.onset_seconds = static_cast<double>(onset) / sample_rate;
        event.offset_seconds = static_cast<double>(offset) / sample_rate;
        event.onset_sample = onset;
        event.offset_sample = offset;
        event.source_sample_rate = sample_rate;
        event.audio_source_id = "live-capture";
        event.source_model = note.source_model;
        event.detection_method = note.detection_method;
        event.evidence_kind = "live-backend-raw-strength";
        event.state = note.state;
        if (const auto original = original_notes.find(note.id);
            original != original_notes.end()) {
            event.evidence.push_back({
                original->second.onset_seconds(), original->second.midi_pitch,
                original->second.raw_strength, 0.0, std::nullopt,
                std::nullopt, original->second.midi_pitch});
        } else {
            event.evidence.push_back({
                event.onset_seconds, note.midi_pitch, note.raw_strength, 0.0,
                std::nullopt, std::nullopt, note.midi_pitch});
        }
        events.push_back(std::move(event));
    }
    if (events.empty()) {
        throw std::runtime_error("No live note events were detected in the recording.");
    }

    auto interpretation_options = request.interpretation_options;
    if (request.instrument == transcribe::InstrumentPrior::Guitar) {
        interpretation_options.instrument = "Guitar";
    } else if (request.instrument == transcribe::InstrumentPrior::Piano) {
        interpretation_options.instrument = "Piano";
    } else if (request.instrument == transcribe::InstrumentPrior::Voice) {
        interpretation_options.instrument = "Voice";
    }
    const auto interpretation =
        transcribe::interpret_note_events(events, interpretation_options);
    if (interpretation.hypotheses.empty()) {
        throw std::runtime_error("Musical interpretation produced no live score hypothesis.");
    }

    transcribe::gui::Result result;
    result.metadata =
        transcribe::metadata_for_hypothesis(interpretation.hypotheses.front());
    result.score = transcribe::make_score_document(
        interpretation, 0, "live-capture", result.metadata,
        request.guitar_tab_options);
    result.score.source_sample_rate = sample_rate;
    for (auto& note : result.score.notes) {
        if (note.source_event_ids.size() != 1) continue;
        const auto& source_id = note.source_event_ids.front();
        const auto source = std::find_if(
            result.score.source_events.begin(), result.score.source_events.end(),
            [&](const auto& candidate) { return candidate.id == source_id; });
        if (source != result.score.source_events.end() &&
            source->state == transcribe::EventLifecycle::ManuallyCorrected) {
            note.user_authored = true;
            note.provenance = {transcribe::ScoreEditOrigin::User, "score-editor",
                               "Live correction", {}};
        }
    }

    result.notes = transcribe::score_notes(result.score);
    auto tab_options = request.guitar_tab_options;
    for (std::size_t index = 0; index < result.score.notes.size(); ++index) {
        const auto& note = result.score.notes[index];
        const auto& live_id = note.source_event_ids.empty()
            ? note.id : note.source_event_ids.front();
        if (const auto position = fingerings.find(live_id);
            position != fingerings.end()) {
            tab_options.position_overrides.push_back({
                index, position->second.first, position->second.second, true});
        }
    }
    result.fingering =
        transcribe::assign_guitar_tab_fingerings(result.notes, tab_options);
    for (const auto& position : result.fingering.positions) {
        if (position.note_index >= result.score.notes.size()) {
            throw std::runtime_error(
                "Guitar tablature assignment returned an invalid live note index.");
        }
        auto tab = std::find_if(
            result.score.tab_notes.begin(), result.score.tab_notes.end(),
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
    return result;
}

void synchronize_live_document(
    transcribe::live::TranscriptionDocument& live_document,
    const transcribe::ScoreDocument& previous,
    const transcribe::ScoreDocument& current) {
    const auto live_id = [](const transcribe::ScoreNote& note) {
        return note.source_event_ids.empty()
            ? note.id : note.source_event_ids.front();
    };
    std::unordered_set<std::string> current_ids;
    for (const auto& note : current.notes) current_ids.insert(live_id(note));
    for (const auto& old_note : previous.notes) {
        const auto id = live_id(old_note);
        if (current_ids.contains(id)) continue;
        if (const auto existing = live_document.note(id);
            existing && existing->state != transcribe::EventLifecycle::Rejected) {
            live_document.delete_manual_note(id, "Deleted from canonical score");
        }
    }
    for (const auto& note : current.notes) {
        if (!note.user_authored) continue;
        const auto id = live_id(note);
        const auto old = std::find_if(previous.notes.begin(), previous.notes.end(),
            [&](const auto& candidate) { return live_id(candidate) == id; });
        const bool changed = old == previous.notes.end() ||
            old->sounding_midi_pitch != note.sounding_midi_pitch ||
            std::abs(old->onset_quarters - note.onset_quarters) > 0.0001 ||
            std::abs(old->duration_quarters - note.duration_quarters) > 0.0001;
        if (!changed) continue;

        const double onset = transcribe::score_position_seconds(
            current, note.onset_quarters);
        const double end = transcribe::score_position_seconds(
            current, note.onset_quarters + note.duration_quarters);
        const auto rate = current.source_sample_rate;
        transcribe::live::LiveNote live_note;
        if (const auto existing = live_document.note(id)) live_note = *existing;
        live_note.id = id;
        live_note.midi_pitch = note.sounding_midi_pitch;
        live_note.sample_rate = rate;
        live_note.onset_frame = static_cast<std::uint64_t>(
            std::llround(std::max(0.0, onset) * rate));
        live_note.offset_frame = std::max(
            live_note.onset_frame + 1,
            static_cast<std::uint64_t>(std::llround(std::max(onset, end) * rate)));
        live_note.state = transcribe::EventLifecycle::ManuallyCorrected;
        if (old == previous.notes.end()) {
            live_note.source_model = "manual";
            live_note.detection_method = "canonical score insertion";
            live_document.add_manual_note(live_note,
                "Canonical score insertion", id);
        } else {
            live_document.edit_manual_note(live_note,
                "Canonical score correction");
        }
    }
}

void post_worker_status(AppState& state, std::string_view text) {
    auto message = std::make_unique<ProgressMessage>();
    message->percent = 0;
    message->status = from_utf8_lossy(text);
    if (PostMessageW(state.window, message_progress, 0,
                     reinterpret_cast<LPARAM>(message.get()))) {
        message.release();
    }
}

void perform_live_capture(
    AppState& state, const transcribe::gui::Request& request,
    std::stop_token stop_token, CompletionMessage& completion,
    const transcribe::gui::ProgressCallback& progress_callback) {
    using namespace transcribe;
    using namespace transcribe::live;
    const auto rate = request.asio_capture_options.sample_rate;
    const auto channels = request.asio_capture_options.channels;
    LiveOptions live_options;
    live_options.format = {rate, channels};
    live_options.yin.realtime = request.realtime_options;
    live_options.yin.realtime.input_sample_rate = rate;
    live_options.yin.stabilization_seconds =
        std::max(0.04, request.realtime_options.latency_ms / 1000.0);
    live_options.max_recording_seconds = request.capture_seconds;
    live_options.audio_source_id = "asio-live";
    live_options.lag_policy = LagPolicy::SkipAhead;

    std::unique_ptr<ModelBackend> backend;
    std::string backend_id = "builtin:yin";
    if (request.live_backend != "yin") {
        backend_id = request.live_backend == "spectral"
            ? "builtin:spectral" : "onnx:basic-pitch-v1";
        ModelRegistry registry(request.model_path);
        auto created = registry.create(backend_id);
        if (const auto* failure = std::get_if<InferenceError>(&created)) {
            throw std::runtime_error(failure->message);
        }
        backend = std::move(std::get<std::unique_ptr<ModelBackend>>(created));
        const auto& metadata = backend->metadata();
        double model_window = 1.0;
        if (metadata.window_samples > 0 && metadata.analysis_sample_rate > 0) {
            model_window = static_cast<double>(metadata.window_samples) /
                           metadata.analysis_sample_rate;
        }
        live_options.backend = LiveBackendKind::OverlappingWindow;
        live_options.window.window_seconds =
            std::clamp(model_window, 0.5, 8.0);
        live_options.window.hop_seconds = std::min(
            std::max(0.02, request.realtime_options.latency_ms / 1000.0),
            live_options.window.window_seconds / 4.0);
        live_options.window.left_guard_seconds =
            std::min(0.08, live_options.window.window_seconds / 8.0);
        live_options.window.right_guard_seconds =
            std::min(0.08, live_options.window.window_seconds / 8.0);
        live_options.window.confirmations = 2;
        live_options.window.request.analysis.instrument = request.instrument;
        live_options.window.request.analysis.fast_preview = request.fast_preview;
        live_options.window.request.analysis.polyphonic = metadata.polyphonic;
        live_options.window.request.onnx.polyphonic = metadata.polyphonic;
        live_options.window.request.onnx.independent_probabilities =
            metadata.polyphonic;
        live_options.window.request.onnx.execution_provider =
            request.model_path.empty() ? OnnxExecutionProvider::Cpu
                                       : OnnxExecutionProvider::Cpu;
        live_options.window.request.retain_evidence = true;
        live_options.window.decoding.minimum_duration_seconds =
            live_options.window.minimum_duration_seconds;
        live_options.model_load.fast_preview = request.fast_preview;
    }

    if (stop_token.stop_requested()) {
        throw std::runtime_error("Capture was stopped before the input device opened.");
    }
    LiveTranscriptionSession session(std::move(live_options), std::move(backend));
    const auto started = session.start();
    if (const auto* failure = std::get_if<InferenceError>(&started)) {
        throw std::runtime_error("Could not start live inference: " + failure->message);
    }
    TranscriptionDocument document(rate);
    std::unordered_map<std::string, std::pair<int, int>> fingerings;
    auto last_message = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(100);
    auto last_preview = last_message;
    const auto update = [&] {
        auto batch = session.poll_events();
        const bool had_events = !batch.events.empty();
        if (batch.resync_required) {
            document.resync(session.snapshot());
        } else {
            document.apply(batch);
        }

        std::deque<LiveEditCommand> commands;
        {
            std::lock_guard lock(state.live_command_mutex);
            commands.swap(state.live_commands);
        }
        bool edited = false;
        for (const auto& command : commands) {
            try {
                if (command.kind == LiveEditKind::Fingering) {
                    const auto note = document.note(command.note_id);
                    if (!note || command.string_number < 1 ||
                        command.string_number > 6 || command.fret < 0 ||
                        command.fret > 24 ||
                        request.guitar_tab_options.tuning[
                            static_cast<std::size_t>(command.string_number - 1)] +
                                request.guitar_tab_options.capo_fret +
                                command.fret != note->midi_pitch) {
                        throw std::invalid_argument(
                            "The selected string and fret do not match the note pitch.");
                    }
                    fingerings[command.note_id] =
                        {command.string_number, command.fret};
                } else if (command.kind == LiveEditKind::Delete) {
                    document.delete_manual_note(command.note_id,
                                                "Live editor deletion");
                } else {
                    if (command.midi_pitch < 0 || command.midi_pitch > 127 ||
                        !std::isfinite(command.onset_seconds) ||
                        !std::isfinite(command.duration_seconds) ||
                        command.onset_seconds < 0 ||
                        command.duration_seconds < 0.05 ||
                        command.onset_seconds + command.duration_seconds >
                            request.capture_seconds) {
                        throw std::invalid_argument(
                            "Live note edits must be within the recording duration.");
                    }
                    LiveNote note;
                    if (command.kind == LiveEditKind::Update) {
                        const auto current = document.note(command.note_id);
                        if (!current) {
                            throw std::invalid_argument(
                                "The live note changed before the edit was applied.");
                        }
                        note = *current;
                    }
                    note.id = command.note_id;
                    note.midi_pitch = command.midi_pitch;
                    note.sample_rate = rate;
                    note.onset_frame = static_cast<std::uint64_t>(
                        std::llround(command.onset_seconds * rate));
                    note.offset_frame = static_cast<std::uint64_t>(
                        std::llround((command.onset_seconds +
                                      command.duration_seconds) * rate));
                    note.state = EventLifecycle::ManuallyCorrected;
                    if (command.kind == LiveEditKind::Add) {
                        note.source_model = backend_id;
                        note.detection_method = "manual live edit";
                        document.add_manual_note(note, "Live editor insertion",
                                                 command.note_id);
                    } else {
                        document.edit_manual_note(note, "Live editor correction");
                    }
                }
                edited = true;
            } catch (const std::exception& error) {
                post_worker_status(state,
                    std::string("Live edit error: ") + error.what());
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (had_events || edited ||
            now - last_message >= std::chrono::milliseconds(100)) {
            auto message = std::make_unique<LiveSessionMessage>();
            message->recording_seconds =
                static_cast<double>(session.ingested_frames()) / rate;
            message->sample_rate = rate;
            message->notes = document.notes();
            message->events = std::move(batch.events);
            message->metrics = session.metrics();
            message->fingerings = fingerings;
            post_live_session(state, std::move(message));
            last_message = now;
        }
        if (now - last_preview >= std::chrono::milliseconds(150)) {
            const auto tail = session.recording_tail(8192);
            if (!tail.samples.empty()) {
                const auto preview = make_audio_preview(
                    tail.samples, tail.sample_rate, 96, 40);
                const double offset = std::max(
                    0.0, static_cast<double>(session.ingested_frames()) / rate -
                             preview.duration_seconds);
                auto message = std::make_unique<AudioPreviewMessage>();
                message->offset = offset;
                message->replace = true;
                message->preview = preview;
                if (PostMessageW(state.window, message_audio_preview, 0,
                                 reinterpret_cast<LPARAM>(message.get()))) {
                    message.release();
                }
            }
            last_preview = now;
        }
    };

    if (progress_callback) progress_callback(0.0, "Connecting to ASIO input");
    const auto capture = capture_asio_live(
        request.capture_seconds, session, stop_token,
        [&](double progress) {
            if (progress_callback) {
                progress_callback(progress * 0.8, "Capturing live audio");
            }
        },
        request.asio_capture_options, update);
    if (progress_callback) progress_callback(0.82, "Stabilizing live note events");
    session.finish();
    update();
    if (session.state() == LiveState::AnalysisFailed) {
        throw std::runtime_error("Live analysis failed: " + session.last_error());
    }
    if (capture.input_lost || capture.blocks_refused > 0 ||
        capture.driver_overflows > 0) {
        std::ostringstream warning;
        warning << "Capture completed with input gaps";
        if (capture.reconnects) warning << " and " << capture.reconnects << " reconnect(s)";
        if (capture.blocks_refused) warning << "; " << capture.blocks_refused
                                            << " audio blocks were refused";
        if (capture.driver_overflows) warning << "; " << capture.driver_overflows
                                               << " driver overflow(s)";
        post_worker_status(state, warning.str());
    }

    completion.live_capture = true;
    completion.live_backend_id = backend_id;
    completion.live_recording = session.recording();
    completion.live_document = std::move(document);
    completion.result = build_live_result(
        request, *completion.live_document, rate, completion.live_recording.samples.size(),
        fingerings);
    if (request.proposal_only) return;
    if (progress_callback) progress_callback(0.9, "Writing live score exports");
#ifdef TRANSCRIBE_ENABLE_VEROVIO
    write_musicxml(request.musicxml_path, completion.result.score,
                   request.preserve_expressive_timing,
                   &completion.result.musicxml_source);
#else
    write_musicxml(request.musicxml_path, completion.result.score,
                   request.preserve_expressive_timing);
#endif
    write_guitar_tab(request.tab_path, completion.result.score,
                     request.guitar_tab_options);
    if (!request.midi_path.empty()) {
        write_midi(request.midi_path, completion.result.score);
    }
    if (progress_callback) progress_callback(1.0, "Live transcription complete");
}

void start_transcription(AppState& state,
                         const std::unordered_map<std::string, std::string>& fields) {
    if (state.busy) {
        return;
    }

    transcribe::gui::Request request;
    request.use_asio = field(fields, "mode") == "asio";
    request.input_path = from_utf8(field(fields, "input"));
    request.proposal_only = state.last_result.has_value();
    request.model_path = from_utf8(field(fields, "model"));
    request.musicxml_path = from_utf8(field(fields, "musicxml"));
    request.tab_path = from_utf8(field(fields, "tab"));
    request.midi_path = from_utf8(field(fields, "midi"));
    request.polyphonic = field(fields, "polyphonic") == "1";
    request.fast_preview = field(fields, "quality") == "fast";
    request.instrument = instrument_prior(field(fields, "instrument"));
    request.guitar_tab_options.tuning = guitar_tuning(field(fields, "tuning"));
    request.guitar_tab_options.manual_corrections =
        state.manual_fingering_corrections;
    request.preserve_expressive_timing = field(fields, "preserve_timing") == "1";
    request.live_backend = field(fields, "live_backend");
    if (request.live_backend.empty()) request.live_backend = "yin";

    const auto tempo_text = field(fields, "tempo");
    if (!tempo_text.empty() && tempo_text != "auto") {
        const double tempo = parse_number(tempo_text, "tempo");
        if (tempo < 40.0 || tempo > 240.0) {
            throw std::invalid_argument("Tempo must be between 40 and 240 BPM.");
        }
        request.interpretation_options.tempo_bpm = tempo;
    }
    const auto key_text = field(fields, "key_fifths");
    if (!key_text.empty() && key_text != "auto") {
        std::size_t consumed = 0;
        const int fifths = std::stoi(key_text, &consumed);
        if (consumed != key_text.size() || fifths < -7 || fifths > 7) {
            throw std::invalid_argument("Key signature must be between -7 and 7 fifths.");
        }
        request.interpretation_options.key_fifths = fifths;
        request.interpretation_options.minor_key = field(fields, "minor") == "1";
    }
    const auto meter_text = field(fields, "meter");
    if (!meter_text.empty() && meter_text != "auto") {
        static constexpr std::array<std::array<int, 2>, 11> supported_meters{{
            {2, 4}, {3, 4}, {4, 4}, {5, 4}, {2, 8}, {3, 8},
            {5, 8}, {6, 8}, {7, 8}, {9, 8}, {12, 8}}};
        const auto supported = std::find_if(
            supported_meters.begin(), supported_meters.end(),
            [&meter_text](const auto& candidate) {
                return meter_text == std::to_string(candidate[0]) + "/" +
                                     std::to_string(candidate[1]);
            });
        if (supported == supported_meters.end()) {
            throw std::invalid_argument("Choose a supported time signature.");
        }
        request.interpretation_options.meter =
            transcribe::MeterConstraint{(*supported)[0], (*supported)[1], {}};
    }
    const auto pickup_text = field(fields, "pickup");
    if (!pickup_text.empty()) {
        const double pickup = parse_number(pickup_text, "pickup");
        if (pickup < 0.0 || pickup > 16.0) {
            throw std::invalid_argument("Pickup length must be between 0 and 16 quarters.");
        }
        if (pickup > 0.0) {
            request.interpretation_options.pickup_quarters = pickup;
        }
    }
    const auto transposition_text = field(fields, "written_transposition");
    if (!transposition_text.empty()) {
        std::size_t consumed = 0;
        const int transposition = std::stoi(transposition_text, &consumed);
        if (consumed != transposition_text.size() ||
            transposition < -48 || transposition > 48) {
            throw std::invalid_argument(
                "Written transposition must be between -48 and 48 semitones.");
        }
        request.interpretation_options.written_transposition_semitones =
            transposition;
    }

    const auto capo_text = field(fields, "capo");
    std::size_t parsed_characters = 0;
    request.guitar_tab_options.capo_fret =
        std::stoi(capo_text, &parsed_characters);
    if (parsed_characters != capo_text.size() ||
        request.guitar_tab_options.capo_fret < 0 ||
        request.guitar_tab_options.capo_fret > 24) {
        throw std::invalid_argument("Capo fret must be between 0 and 24.");
    }
    const auto alternatives_text = field(fields, "alternatives");
    const int alternatives = std::stoi(alternatives_text, &parsed_characters);
    if (parsed_characters != alternatives_text.size() || alternatives < 0 ||
        alternatives > 5) {
        throw std::invalid_argument(
            "Alternative fingerings must be between 0 and 5.");
    }
    request.guitar_tab_options.alternative_fingerings =
        static_cast<std::size_t>(alternatives);

    if (request.use_asio) {
#ifndef TRANSCRIBE_ENABLE_ASIO
        throw std::runtime_error("ASIO support is not enabled in this build.");
#else
        const auto duration_text = field(fields, "duration");
        parsed_characters = 0;
        request.capture_seconds = std::stod(duration_text, &parsed_characters);
        if (parsed_characters != duration_text.size() ||
            !std::isfinite(request.capture_seconds) ||
            request.capture_seconds <= 0.0 || request.capture_seconds > 300.0) {
            throw std::invalid_argument(
                "Enter a capture duration greater than 0 and no more than 300 seconds.");
        }
        const auto latency_text = field(fields, "latency_ms");
        request.realtime_options.latency_ms =
            std::stod(latency_text, &parsed_characters);
        if (parsed_characters != latency_text.size() ||
            !std::isfinite(request.realtime_options.latency_ms) ||
            request.realtime_options.latency_ms < 20.0 ||
            request.realtime_options.latency_ms > 50.0) {
            throw std::invalid_argument(
                "Realtime analysis latency must be between 20 and 50 milliseconds.");
        }
        const auto device_id_text = field(fields, "device_id");
        std::size_t device_id_characters = 0;
        const auto device_id = std::stoul(device_id_text, &device_id_characters);
        if (device_id_text.empty() || device_id_characters != device_id_text.size() ||
            device_id > std::numeric_limits<unsigned int>::max()) {
            throw std::invalid_argument("Choose an available ASIO input device.");
        }
        request.asio_capture_options.device_id =
            static_cast<unsigned int>(device_id);
        const auto sample_rate_text = field(fields, "sample_rate");
        std::size_t sample_rate_characters = 0;
        const auto sample_rate = std::stoul(sample_rate_text, &sample_rate_characters);
        if (sample_rate_text.empty() ||
            sample_rate_characters != sample_rate_text.size() ||
            sample_rate < 2000 || sample_rate > 384000) {
            throw std::invalid_argument("Choose a valid input sample rate.");
        }
        request.asio_capture_options.sample_rate =
            static_cast<std::uint32_t>(sample_rate);
        request.realtime_options.input_sample_rate =
            request.asio_capture_options.sample_rate;
        const auto channels_text = field(fields, "channels");
        std::size_t channel_characters = 0;
        const auto channels = std::stoul(channels_text, &channel_characters);
        if (channels_text.empty() || channel_characters != channels_text.size() ||
            channels == 0 || channels > 32) {
            throw std::invalid_argument("Choose a valid input channel count.");
        }
        request.asio_capture_options.channels =
            static_cast<std::uint16_t>(channels);
        if (request.live_backend != "yin" && request.live_backend != "spectral" &&
            request.live_backend != "onnx") {
            throw std::invalid_argument("Choose YIN, spectral, or ONNX live inference.");
        }
        if (request.live_backend == "onnx" && request.model_path.empty()) {
            throw std::invalid_argument("Choose an ONNX model for live ONNX inference.");
        }
#endif
    } else if (request.input_path.empty()) {
        throw std::invalid_argument("Choose an audio file first.");
    }
    if (request.musicxml_path.empty() || request.tab_path.empty()) {
        throw std::invalid_argument("Choose both MusicXML and guitar tab output paths.");
    }
#ifndef TRANSCRIBE_ENABLE_ONNX
    if (!request.model_path.empty() || request.polyphonic ||
        (request.use_asio && request.live_backend == "onnx")) {
        throw std::runtime_error(
            "ONNX support is not enabled. Configure with -DTRANSCRIBE_ENABLE_ONNX=ON.");
    }
#endif
    if (request.polyphonic && request.model_path.empty()) {
        throw std::invalid_argument("Choose an ONNX model to enable polyphonic inference.");
    }

    state.busy = true;
    state.active_asio = request.use_asio;
    if (request.use_asio) {
        std::lock_guard lock(state.live_command_mutex);
        state.live_commands.clear();
    }
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Starting analysis\"}");
    try {
        state.worker = std::jthread(
            [&state, request = std::move(request)](std::stop_token stop_token) {
            auto completion = std::make_unique<CompletionMessage>();
            completion->request = request;
            try {
                const auto progress_callback = [&state](double progress,
                                                        const std::string& status) {
                    auto message = std::make_unique<ProgressMessage>();
                    message->percent = std::clamp(
                        static_cast<int>(progress * 100.0), 0, 100);
                    message->status = from_utf8(status);
                    if (PostMessageW(state.window, message_progress, 0,
                                     reinterpret_cast<LPARAM>(message.get()))) {
                        message.release();
                    }
                };
                const auto live_callback = [&state](
                                               double offset,
                                               const std::vector<transcribe::Note>& notes,
                                               const std::vector<transcribe::NoteEvent>& events) {
                    auto message = std::make_unique<LiveMessage>();
                    message->offset = offset;
                    message->notes = notes;
                    message->events = events;
                    if (PostMessageW(state.window, message_live_preview, 0,
                                     reinterpret_cast<LPARAM>(message.get()))) {
                        message.release();
                    }
                };
                const auto audio_preview_callback =
                    [&state](double offset,
                             const transcribe::AudioPreviewData& preview,
                             bool replace) {
                        auto message = std::make_unique<AudioPreviewMessage>();
                        message->offset = offset;
                        message->replace = replace;
                        message->preview = preview;
                        if (PostMessageW(state.window, message_audio_preview, 0,
                                         reinterpret_cast<LPARAM>(message.get()))) {
                            message.release();
                        }
                    };
                if (request.use_asio) {
                    perform_live_capture(state, request, stop_token, *completion,
                                         progress_callback);
                } else {
                    completion->result = transcribe::gui::run_transcription(
                        request, stop_token, progress_callback, live_callback,
                        audio_preview_callback);
                }
                completion->success = true;
            } catch (const std::exception& error) {
                completion->error = error.what();
            }
            if (PostMessageW(state.window, message_complete, 0,
                             reinterpret_cast<LPARAM>(completion.get()))) {
                completion.release();
            }
            });
    } catch (...) {
        state.busy = false;
        state.active_asio = false;
        throw;
    }
}

double parse_number(std::string_view text, const char* description) {
    if (text.empty()) {
        throw std::invalid_argument(std::string("Missing ") + description + ".");
    }
    std::size_t parsed = 0;
    const double value = std::stod(std::string(text), &parsed);
    if (parsed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string("Invalid ") + description + ".");
    }
    return value;
}

struct EditedScoreNote {
    std::string id;
    transcribe::Note note;
};

std::vector<EditedScoreNote> parse_edited_notes(std::string_view encoded) {
    std::vector<EditedScoreNote> notes;
    while (!encoded.empty()) {
        const auto end = encoded.find(';');
        const auto record = encoded.substr(0, end);
        std::array<std::string_view, 6> values{};
        std::size_t field_start = 0;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto separator = record.find(',', field_start);
            if (index + 1 < values.size() && separator == std::string_view::npos) {
                throw std::invalid_argument("An edited note has missing values.");
            }
            if (index + 1 == values.size() && separator != std::string_view::npos) {
                throw std::invalid_argument("An edited note has too many values.");
            }
            values[index] = record.substr(
                field_start, separator == std::string_view::npos
                                 ? std::string_view::npos
                                 : separator - field_start);
            field_start = separator == std::string_view::npos ? record.size()
                                                              : separator + 1;
        }
        const std::string id(values[0]);
        const double midi_value = parse_number(values[1], "edited MIDI pitch");
        const double start = parse_number(values[2], "edited note start");
        const double duration = parse_number(values[3], "edited note duration");
        const double confidence = parse_number(values[4], "edited note confidence");
        const double amplitude = parse_number(values[5], "edited note amplitude");
        if (id.empty() || id.find_first_of(";,") != std::string::npos) {
            throw std::invalid_argument("An edited note has an invalid stable ID.");
        }
        if (midi_value < 0.0 || midi_value > 127.0 ||
            std::floor(midi_value) != midi_value || start < 0.0 ||
            duration <= 0.0 || duration > 600.0 ||
            confidence < 0.0 || confidence > 1.0 ||
            amplitude < 0.0 || amplitude > 1.0) {
            throw std::invalid_argument("Edited note values are outside supported ranges.");
        }
        const int midi = static_cast<int>(midi_value);
        notes.push_back({id, {midi, start, duration,
                              transcribe::midi_note_name(midi),
                              confidence, amplitude}});
        if (notes.size() > 10000) {
            throw std::length_error("The score editor supports at most 10,000 notes.");
        }
        if (end == std::string_view::npos) {
            encoded = {};
        } else {
            encoded.remove_prefix(end + 1);
        }
    }
    return notes;
}

std::vector<transcribe::GuitarTabPosition> parse_tab_positions(
    std::string_view encoded) {
    std::vector<transcribe::GuitarTabPosition> positions;
    while (!encoded.empty()) {
        const auto end = encoded.find(';');
        const auto record = encoded.substr(0, end);
        const auto first = record.find(',');
        const auto second =
            first == std::string_view::npos ? first : record.find(',', first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos ||
            record.find(',', second + 1) != std::string_view::npos) {
            throw std::invalid_argument("A guitar string/fret edit is malformed.");
        }
        const double note_index = parse_number(record.substr(0, first), "tab note index");
        const double string_number =
            parse_number(record.substr(first + 1, second - first - 1), "guitar string");
        const double fret = parse_number(record.substr(second + 1), "guitar fret");
        if (note_index < 0.0 || std::floor(note_index) != note_index ||
            string_number < 1.0 || string_number > 6.0 ||
            std::floor(string_number) != string_number || fret < 0.0 ||
            fret > 24.0 || std::floor(fret) != fret) {
            throw std::invalid_argument("A guitar string/fret edit is outside supported ranges.");
        }
        positions.push_back({
            static_cast<std::size_t>(note_index),
            static_cast<int>(string_number),
            static_cast<int>(fret),
            true});
        if (positions.size() > 10000) {
            throw std::length_error("The score editor supports at most 10,000 tab positions.");
        }
        if (end == std::string_view::npos) {
            encoded = {};
        } else {
            encoded.remove_prefix(end + 1);
        }
    }
    return positions;
}

void export_edited_score(
    AppState& state,
    const std::unordered_map<std::string, std::string>& fields) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.last_request || !state.last_result) {
        throw std::runtime_error("Transcribe audio before exporting score edits.");
    }

    auto request = *state.last_request;
    const auto musicxml_path = field(fields, "musicxml");
    const auto tab_path = field(fields, "tab");
    const auto midi_path = field(fields, "midi");
    if (!musicxml_path.empty()) request.musicxml_path = from_utf8(musicxml_path);
    if (!tab_path.empty()) request.tab_path = from_utf8(tab_path);
    if (!midi_path.empty()) request.midi_path = from_utf8(midi_path);
    const auto revision_text = field(fields, "base_revision");
    std::size_t revision_characters = 0;
    const auto base_revision = std::stoull(revision_text, &revision_characters);
    if (revision_characters != revision_text.size() ||
        base_revision != state.last_result->score.revision) {
        throw std::logic_error(
            "The accepted score changed; reload the editor before exporting these edits.");
    }

    transcribe::ScoreEditor editor(state.last_result->score);
    const auto source_score = state.last_result->score;
    const auto edited_notes = parse_edited_notes(field(fields, "notes"));
    std::unordered_set<std::string> edited_ids;
    transcribe::ScoreTransaction transaction;
    transaction.base_revision = base_revision;
    transaction.actor = "score-editor";
    transaction.reason = "Apply notation editor changes";
    transaction.command_id = source_score.id + "-edit-" +
                             std::to_string(base_revision + 1);

    auto metadata = source_score.metadata;
    const auto tempo = parse_number(field(fields, "tempo"), "tempo");
    if (tempo < 40.0 || tempo > 240.0) {
        throw std::invalid_argument("Tempo must be between 40 and 240 BPM.");
    }
    const bool tempo_changed =
        std::abs(tempo - source_score.metadata.tempo_bpm) > 0.5;
    metadata.tempo_bpm = tempo_changed
        ? tempo : source_score.metadata.tempo_bpm;
    if (tempo_changed) metadata.tempo_inferred = true;
    const auto key = field(fields, "key_fifths");
    if (!key.empty() && key != "auto") {
        std::size_t key_characters = 0;
        const int fifths = std::stoi(key, &key_characters);
        if (key_characters != key.size() || fifths < -7 || fifths > 7) {
            throw std::invalid_argument("Key signature must be between -7 and 7 fifths.");
        }
        metadata.key_fifths = fifths;
        metadata.minor_key = field(fields, "minor") == "1";
        metadata.key_inferred = true;
    }
    const auto meter = field(fields, "meter");
    if (meter != "auto") {
        static constexpr std::array<std::array<int, 2>, 11> supported_meters{{
            {2, 4}, {3, 4}, {4, 4}, {5, 4}, {2, 8}, {3, 8},
            {5, 8}, {6, 8}, {7, 8}, {9, 8}, {12, 8}}};
        const auto supported = std::find_if(
            supported_meters.begin(), supported_meters.end(),
            [&meter](const auto& candidate) {
                return meter == std::to_string(candidate[0]) + "/" +
                                std::to_string(candidate[1]);
            });
        if (supported == supported_meters.end()) {
            throw std::invalid_argument("Choose a supported time signature.");
        }
        metadata.time_numerator = (*supported)[0];
        metadata.time_denominator = (*supported)[1];
        metadata.meter_inferred = true;
    }
    const double pickup = parse_number(field(fields, "pickup"), "pickup");
    if (pickup < 0.0 || pickup > 16.0) {
        throw std::invalid_argument("Pickup length must be between 0 and 16 quarters.");
    }
    int transposition = source_score.instrument.written_transposition_semitones;
    const auto transposition_text = field(fields, "written_transposition");
    if (!transposition_text.empty()) {
        std::size_t consumed = 0;
        transposition = std::stoi(transposition_text, &consumed);
        if (consumed != transposition_text.size() ||
            transposition < -48 || transposition > 48) {
            throw std::invalid_argument(
                "Written transposition must be between -48 and 48 semitones.");
        }
    }
    const auto tuning_text = field(fields, "tuning");
    if (!tuning_text.empty()) {
        request.guitar_tab_options.tuning = guitar_tuning(tuning_text);
    }
    const auto capo_text = field(fields, "capo");
    if (!capo_text.empty()) {
        std::size_t consumed = 0;
        const int capo = std::stoi(capo_text, &consumed);
        if (consumed != capo_text.size() || capo < 0 || capo > 24) {
            throw std::invalid_argument("Capo fret must be between 0 and 24.");
        }
        request.guitar_tab_options.capo_fret = capo;
    }
    auto instrument = source_score.instrument;
    instrument.name =
        request.instrument == transcribe::InstrumentPrior::Guitar ? "Guitar" :
        request.instrument == transcribe::InstrumentPrior::Piano ? "Piano" :
        request.instrument == transcribe::InstrumentPrior::Voice ? "Voice" :
        source_score.instrument.name;
    instrument.written_transposition_semitones = transposition;
    instrument.tuning.assign(request.guitar_tab_options.tuning.begin(),
                             request.guitar_tab_options.tuning.end());
    instrument.capo_fret = request.guitar_tab_options.capo_fret;
    transaction.commands.emplace_back(transcribe::UpdateScoreContext{
        metadata, instrument, pickup, tempo_changed});

    for (std::size_t index = 0; index < edited_notes.size(); ++index) {
        const auto& edited = edited_notes[index];
        if (!edited_ids.insert(edited.id).second) {
            throw std::invalid_argument("Edited score contains a duplicate note ID.");
        }
        const double onset_quarters =
            transcribe::score_position_quarters(source_score, edited.note.start_seconds);
        const double end_quarters = transcribe::score_position_quarters(
            source_score, edited.note.start_seconds + edited.note.duration_seconds);
        const double duration_quarters = end_quarters - onset_quarters;
        if (duration_quarters <= 0.0) {
            throw std::invalid_argument("Edited note duration is invalid in score time.");
        }
        const auto current = std::find_if(
            source_score.notes.begin(), source_score.notes.end(),
            [&](const auto& note) { return note.id == edited.id; });
        if (current == source_score.notes.end()) {
            if (!edited.id.starts_with("user-")) {
                throw std::invalid_argument("Edited score contains an unknown note ID.");
            }
            transcribe::ScoreNote note;
            note.id = edited.id;
            note.sounding_midi_pitch = edited.note.midi_pitch;
            note.written_midi_pitch =
                edited.note.midi_pitch + instrument.written_transposition_semitones;
            note.onset_quarters = onset_quarters;
            note.duration_quarters = duration_quarters;
            note.voice = 1;
            note.rhythm.duration_quarters = duration_quarters;
            transaction.commands.emplace_back(transcribe::InsertScoreNote{note});
            continue;
        }
        if (current->sounding_midi_pitch != edited.note.midi_pitch) {
            transaction.commands.emplace_back(transcribe::ChangeScorePitch{
                edited.id, edited.note.midi_pitch});
        }
        const bool timing_changed =
            std::abs(onset_quarters - current->onset_quarters) > 1e-5 ||
            std::abs(duration_quarters - current->duration_quarters) > 1e-5;
        if (timing_changed) {
            auto rhythm = current->rhythm;
            rhythm.denominator = 0;
            rhythm.dots = 0;
            rhythm.tuplet_actual = 0;
            rhythm.tuplet_normal = 0;
            rhythm.duration_quarters = duration_quarters;
            rhythm.fit_error_quarters = 0.0;
            rhythm.symbolic = false;
            transaction.commands.emplace_back(transcribe::ReinterpretScoreRhythm{
                edited.id, onset_quarters, duration_quarters, rhythm});
        }
    }
    for (const auto& note : source_score.notes) {
        if (!edited_ids.contains(note.id)) {
            transaction.commands.emplace_back(
                transcribe::DeleteScoreElement{note.id});
        }
    }
    const auto manual_positions =
        parse_tab_positions(field(fields, "tab_positions"));
    std::unordered_set<std::string> manual_fingering_ids;
    for (const auto& position : manual_positions) {
        if (position.note_index >= edited_notes.size()) {
            throw std::invalid_argument(
                "A manual tab position refers to a missing note.");
        }
        const auto& edited = edited_notes[position.note_index];
        manual_fingering_ids.insert(edited.id);
        transaction.commands.emplace_back(transcribe::ChangeScoreFingering{
            edited.id, position.string_number, position.fret});
    }
    for (const auto& tab : source_score.tab_notes) {
        if (!tab.manually_corrected ||
            !edited_ids.contains(tab.notation_note_id) ||
            manual_fingering_ids.contains(tab.notation_note_id)) {
            continue;
        }
        transaction.commands.emplace_back(transcribe::ChangeScoreFingering{
            tab.notation_note_id, std::nullopt, std::nullopt});
    }
    editor.apply(transaction);

    transcribe::gui::Result result = *state.last_result;
    result.score = editor.document();
    result.metadata = result.score.metadata;
    result.notes = transcribe::score_notes(result.score);
    request.guitar_tab_options.position_overrides.clear();
    request.guitar_tab_options.manual_corrections.clear();
    for (std::size_t index = 0; index < result.score.notes.size(); ++index) {
        const auto& note = result.score.notes[index];
        const auto tab = std::find_if(result.score.tab_notes.begin(),
            result.score.tab_notes.end(), [&](const auto& candidate) {
                return candidate.notation_note_id == note.id;
            });
        if (tab != result.score.tab_notes.end() && tab->manually_corrected &&
            tab->string_number && tab->fret) {
            request.guitar_tab_options.position_overrides.push_back(
                {index, *tab->string_number, *tab->fret, true});
        }
    }
    result.fingering = transcribe::assign_guitar_tab_fingerings(
        result.notes, request.guitar_tab_options);
    for (const auto& position : result.fingering.positions) {
        if (position.note_index >= result.score.notes.size()) continue;
        const auto& note = result.score.notes[position.note_index];
        const auto tab = std::find_if(result.score.tab_notes.begin(),
            result.score.tab_notes.end(), [&](const auto& candidate) {
                return candidate.notation_note_id == note.id;
            });
        if (tab != result.score.tab_notes.end() && position.string_number > 0 &&
            position.fret >= 0) {
            tab->string_number = position.string_number;
            tab->fret = position.fret;
            tab->manually_corrected = position.manually_corrected;
        }
    }
    std::vector<transcribe::FingeringCorrection> saved_corrections;
    for (const auto& tab : result.score.tab_notes) {
        if (!tab.manually_corrected || !tab.string_number || !tab.fret) continue;
        const auto note = std::find_if(result.score.notes.begin(),
            result.score.notes.end(), [&](const auto& candidate) {
                return candidate.id == tab.notation_note_id;
            });
        if (note == result.score.notes.end()) continue;
        const auto start = transcribe::score_position_seconds(
            result.score, note->onset_quarters);
        const auto end = transcribe::score_position_seconds(
            result.score, note->onset_quarters + note->duration_quarters);
        saved_corrections.push_back({note->sounding_midi_pitch, start, end - start,
                                     *tab.string_number, *tab.fret});
    }
    request.guitar_tab_options.manual_corrections = saved_corrections;
    result.score.source_id = source_score.source_id;

    state.busy = true;
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Exporting edited score\"}");
    try {
        state.worker = std::jthread(
            [&state, request = std::move(request), result = std::move(result),
             saved_corrections = std::move(saved_corrections)](std::stop_token) mutable {
                auto message = std::make_unique<CompletionMessage>();
                message->export_only = true;
                message->request = request;
                const auto report_export_progress = [&state](int percent,
                                                             const wchar_t* status) {
                    auto progress = std::make_unique<ProgressMessage>();
                    progress->percent = percent;
                    progress->status = status;
                    if (PostMessageW(state.window, message_progress, 0,
                                     reinterpret_cast<LPARAM>(progress.get()))) {
                        progress.release();
                    }
                };
                try {
                    report_export_progress(20, L"Writing MusicXML");
#ifdef TRANSCRIBE_ENABLE_VEROVIO
                    transcribe::write_musicxml(
                        request.musicxml_path, result.score,
                        request.preserve_expressive_timing,
                        &result.musicxml_source);
#else
                    transcribe::write_musicxml(
                        request.musicxml_path, result.score,
                        request.preserve_expressive_timing);
#endif
                    report_export_progress(55, L"Writing guitar tablature");
                    transcribe::write_guitar_tab(
                        request.tab_path, result.score, request.guitar_tab_options);
                    if (!request.midi_path.empty()) {
                        report_export_progress(80, L"Writing MIDI");
                        transcribe::write_midi(request.midi_path, result.score);
                    }
                    report_export_progress(100, L"Export complete");
                    message->request.guitar_tab_options.manual_corrections =
                        std::move(saved_corrections);
                    message->result = std::move(result);
                    message->success = true;
                } catch (const std::exception& error) {
                    message->error = error.what();
                }
                if (PostMessageW(state.window, message_complete, 0,
                                 reinterpret_cast<LPARAM>(message.get()))) {
                    message.release();
                }
            });
    } catch (...) {
        state.busy = false;
        throw;
    }
}

void accept_proposal(AppState& state) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.proposal_request || !state.proposal_result) {
        throw std::runtime_error("There is no analysis proposal to accept.");
    }

    auto request = *state.proposal_request;
    request.proposal_only = false;
    auto result = *state.proposal_result;
    auto live_document = std::move(state.proposal_live_document);
    auto live_recording = std::move(state.proposal_live_recording);
    auto live_backend_id = std::move(state.proposal_live_backend_id);
    const bool live_capture = live_document.has_value();
    state.proposal_live_document.reset();
    state.proposal_live_recording = {};
    state.proposal_live_backend_id.clear();
    state.busy = true;
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Applying accepted proposal\"}");
    try {
        state.worker = std::jthread(
            [&state, request = std::move(request), result = std::move(result),
             live_document = std::move(live_document),
             live_recording = std::move(live_recording),
             live_backend_id = std::move(live_backend_id), live_capture](
                std::stop_token) mutable {
                auto message = std::make_unique<CompletionMessage>();
                message->export_only = true;
                message->request = request;
                message->live_capture = live_capture;
                const auto report_progress = [&state](int percent,
                                                       const wchar_t* status) {
                    auto progress = std::make_unique<ProgressMessage>();
                    progress->percent = percent;
                    progress->status = status;
                    if (PostMessageW(state.window, message_progress, 0,
                                     reinterpret_cast<LPARAM>(progress.get()))) {
                        progress.release();
                    }
                };
                try {
                    report_progress(20, L"Writing accepted MusicXML");
#ifdef TRANSCRIBE_ENABLE_VEROVIO
                    transcribe::write_musicxml(
                        request.musicxml_path, result.score,
                        request.preserve_expressive_timing,
                        &result.musicxml_source);
#else
                    transcribe::write_musicxml(
                        request.musicxml_path, result.score,
                        request.preserve_expressive_timing);
#endif
                    report_progress(55, L"Writing accepted guitar tablature");
                    transcribe::write_guitar_tab(
                        request.tab_path, result.score, request.guitar_tab_options);
                    if (!request.midi_path.empty()) {
                        report_progress(80, L"Writing accepted MIDI");
                        transcribe::write_midi(request.midi_path, result.score);
                    }
                    report_progress(100, L"Proposal accepted and exported");
                    message->result = std::move(result);
                    message->success = true;
                } catch (const std::exception& error) {
                    message->error = error.what();
                }
                message->live_document = std::move(live_document);
                message->live_recording = std::move(live_recording);
                message->live_backend_id = std::move(live_backend_id);
                if (PostMessageW(state.window, message_complete, 0,
                                 reinterpret_cast<LPARAM>(message.get()))) {
                    message.release();
                }
            });
    } catch (...) {
        state.busy = false;
        throw;
    }
}

void start_live_refinement(AppState& state) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.last_request || !state.last_result || !state.live_document ||
        state.live_recording.samples.empty() || state.live_backend_id.empty()) {
        throw std::runtime_error(
            "Stop a live recording before requesting offline refinement.");
    }
    const auto request = *state.last_request;
    const auto backend_id = state.live_backend_id;
    state.pending_live_proposal.reset();
    state.pending_live_review.reset();
    state.busy = true;
    state.active_asio = false;
    post_json(state,
        L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Preparing offline refinement\"}");
    try {
        state.worker = std::jthread(
            [&state, request, backend_id](std::stop_token stop_token) {
                auto completion = std::make_unique<CompletionMessage>();
                completion->refinement_proposal = true;
                try {
                    transcribe::live::RefinementOptions options;
                    options.backend_id = backend_id;
                    options.model_path = request.model_path;
                    options.audio_source_id = "live-capture";
                    options.load.provider =
                        transcribe::OnnxExecutionProvider::Cpu;
                    options.request.analysis.instrument = request.instrument;
                    options.request.analysis.fast_preview = false;
                    options.request.analysis.polyphonic =
                        backend_id != "builtin:yin";
                    options.request.onnx.polyphonic =
                        backend_id != "builtin:yin";
                    options.request.onnx.independent_probabilities =
                        backend_id != "builtin:yin";
                    options.request.onnx.execution_provider =
                        transcribe::OnnxExecutionProvider::Cpu;
                    options.request.retain_evidence = true;
                    options.request.progress = [&state](double progress) {
                        auto message = std::make_unique<ProgressMessage>();
                        message->percent = std::clamp(
                            static_cast<int>(progress * 100.0), 0, 100);
                        message->status = L"Refining full recording";
                        if (PostMessageW(state.window, message_progress, 0,
                                         reinterpret_cast<LPARAM>(message.get()))) {
                            message.release();
                        }
                    };
                    options.decoding.minimum_duration_seconds = 0.055;
                    if (backend_id != "builtin:yin") {
                        transcribe::live::WindowOptions window;
                        window.window_seconds =
                            backend_id == "builtin:spectral" ? 1.0 : 2.0;
                        window.hop_seconds = 0.25;
                        window.left_guard_seconds = 0.1;
                        window.right_guard_seconds = 0.1;
                        window.confirmations = 2;
                        window.temporal_decoding = true;
                        options.windowed = window;
                    }
                    const auto refined = transcribe::live::refine_recording(
                        state.live_recording, options, stop_token);
                    if (const auto* failure =
                            std::get_if<transcribe::InferenceError>(&refined)) {
                        throw std::runtime_error(failure->message);
                    }
                    completion->score_proposal =
                        std::get<transcribe::live::ScoreProposal>(refined);
                    completion->proposal_review =
                        state.live_document->review(*completion->score_proposal);
                    completion->success = true;
                } catch (const std::exception& error) {
                    completion->error = error.what();
                }
                if (PostMessageW(state.window, message_complete, 0,
                                 reinterpret_cast<LPARAM>(completion.get()))) {
                    completion.release();
                }
            });
    } catch (...) {
        state.busy = false;
        throw;
    }
}

void apply_live_refinement(
    AppState& state,
    const std::unordered_map<std::string, std::string>& fields) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.last_request || !state.last_result || !state.live_document ||
        !state.pending_live_proposal || !state.pending_live_review) {
        throw std::runtime_error("There is no live refinement proposal to apply.");
    }
    if (state.pending_live_review->base_revision !=
        state.live_document->revision()) {
        state.pending_live_review = state.live_document->review(
            *state.pending_live_proposal);
        post_json(state, refinement_review_json(
            *state.pending_live_proposal, *state.pending_live_review));
        return;
    }

    std::set<std::string> approved;
    const auto encoded = field(fields, "approved");
    std::size_t first = 0;
    while (first < encoded.size()) {
        const auto separator = encoded.find(';', first);
        const auto id = encoded.substr(first, separator == std::string::npos
            ? std::string::npos : separator - first);
        const auto change = std::find_if(
            state.pending_live_review->changes.begin(),
            state.pending_live_review->changes.end(),
            [&](const auto& candidate) {
                return candidate.change_id == id &&
                       candidate.conflicts_with_manual_edit;
            });
        if (change == state.pending_live_review->changes.end()) {
            throw std::invalid_argument(
                "The refinement approval refers to an unknown conflict.");
        }
        approved.insert(id);
        if (separator == std::string::npos) break;
        first = separator + 1;
    }

    const auto request = *state.last_request;
    const auto base_revision = state.last_result->score.revision;
    state.busy = true;
    state.active_asio = false;
    post_json(state,
        L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Applying selected refinement changes\"}");
    try {
        state.worker = std::jthread(
            [&state, request, base_revision, approved = std::move(approved)](
                std::stop_token) mutable {
                auto completion = std::make_unique<CompletionMessage>();
                completion->export_only = true;
                completion->live_capture = true;
                completion->refinement_applied = true;
                completion->request = request;
                try {
                    auto document = *state.live_document;
                    const auto review = *state.pending_live_review;
                    const auto proposal = *state.pending_live_proposal;
                    const auto applied = document.apply(review, approved);
                    if (applied.applied == 0) {
                        throw std::runtime_error(
                            "No non-conflicting or explicitly approved refinement changes were selected.");
                    }
                    std::unordered_map<std::string, std::pair<int, int>> fingerings;
                    for (const auto& tab : state.last_result->score.tab_notes) {
                        if (tab.manually_corrected && tab.string_number && tab.fret) {
                            const auto note = std::find_if(
                                state.last_result->score.notes.begin(),
                                state.last_result->score.notes.end(),
                                [&](const auto& candidate) {
                                    return candidate.id == tab.notation_note_id;
                                });
                            if (note == state.last_result->score.notes.end()) continue;
                            const auto& live_id = note->source_event_ids.empty()
                                ? note->id : note->source_event_ids.front();
                            fingerings[live_id] =
                                {*tab.string_number, *tab.fret};
                        }
                    }
                    completion->result = build_live_result(
                        request, document, request.asio_capture_options.sample_rate,
                        state.live_recording.samples.size(), fingerings);
                    completion->result.score.revision = base_revision + 1;
                    for (auto& note : completion->result.score.notes) {
                        note.revision = completion->result.score.revision;
                    }
                    for (auto& rest : completion->result.score.rests) {
                        rest.revision = completion->result.score.revision;
                    }
                    for (auto& tab : completion->result.score.tab_notes) {
                        tab.revision = completion->result.score.revision;
                    }
                    completion->result.score.history.push_back({
                        completion->result.score.revision,
                        transcribe::ScoreEditOrigin::Refinement,
                        proposal.backend_id,
                        "Apply live offline refinement",
                        proposal.id});
                    transcribe::validate_score_document(
                        completion->result.score);
                    completion->withheld_conflicts = applied.withheld_conflicts;
                    completion->live_document = std::move(document);
                    completion->live_recording = state.live_recording;
                    completion->live_backend_id = state.live_backend_id;
                    const auto progress = [&state](int percent,
                                                    const wchar_t* status) {
                        auto message = std::make_unique<ProgressMessage>();
                        message->percent = percent;
                        message->status = status;
                        if (PostMessageW(state.window, message_progress, 0,
                                         reinterpret_cast<LPARAM>(message.get()))) {
                            message.release();
                        }
                    };
                    progress(30, L"Writing refined MusicXML");
#ifdef TRANSCRIBE_ENABLE_VEROVIO
                    transcribe::write_musicxml(
                        request.musicxml_path, completion->result.score,
                        request.preserve_expressive_timing,
                        &completion->result.musicxml_source);
#else
                    transcribe::write_musicxml(
                        request.musicxml_path, completion->result.score,
                        request.preserve_expressive_timing);
#endif
                    progress(60, L"Writing refined guitar tablature");
                    transcribe::write_guitar_tab(
                        request.tab_path, completion->result.score,
                        request.guitar_tab_options);
                    if (!request.midi_path.empty()) {
                        progress(80, L"Writing refined MIDI");
                        transcribe::write_midi(
                            request.midi_path, completion->result.score);
                    }
                    completion->success = true;
                } catch (const std::exception& error) {
                    completion->error = error.what();
                }
                if (PostMessageW(state.window, message_complete, 0,
                                 reinterpret_cast<LPARAM>(completion.get()))) {
                    completion.release();
                }
            });
    } catch (...) {
        state.busy = false;
        throw;
    }
}

void discard_live_refinement(AppState& state) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    state.pending_live_proposal.reset();
    state.pending_live_review.reset();
    post_json(state, L"{\"type\":\"refinement-discarded\"}");
}

void undo_live_refinement(AppState& state) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.pre_refinement_result || !state.pre_refinement_document ||
        !state.last_result || !state.last_request) {
        throw std::runtime_error("There is no applied refinement to undo.");
    }
    if (state.last_result->score.revision != state.refinement_result_revision) {
        throw std::logic_error(
            "The score changed after refinement; export those edits before undoing.");
    }
    const auto request = *state.last_request;
    auto prior_result = *state.pre_refinement_result;
    auto prior_document = *state.pre_refinement_document;
    state.busy = true;
    state.active_asio = false;
    post_json(state,
        L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Restoring the pre-refinement score\"}");
    try {
        state.worker = std::jthread(
            [&state, request, prior_result = std::move(prior_result),
             prior_document = std::move(prior_document)](std::stop_token) mutable {
                auto completion = std::make_unique<CompletionMessage>();
                completion->export_only = true;
                completion->live_capture = true;
                completion->refinement_undo = true;
                completion->request = request;
                try {
#ifdef TRANSCRIBE_ENABLE_VEROVIO
                    transcribe::write_musicxml(
                        request.musicxml_path, prior_result.score,
                        request.preserve_expressive_timing,
                        &prior_result.musicxml_source);
#else
                    transcribe::write_musicxml(
                        request.musicxml_path, prior_result.score,
                        request.preserve_expressive_timing);
#endif
                    transcribe::write_guitar_tab(
                        request.tab_path, prior_result.score,
                        request.guitar_tab_options);
                    if (!request.midi_path.empty()) {
                        transcribe::write_midi(
                            request.midi_path, prior_result.score);
                    }
                    completion->success = true;
                } catch (const std::exception& error) {
                    completion->error = error.what();
                }
                completion->result = std::move(prior_result);
                completion->live_document = std::move(prior_document);
                completion->live_recording = state.live_recording;
                completion->live_backend_id = state.live_backend_id;
                if (PostMessageW(state.window, message_complete, 0,
                                 reinterpret_cast<LPARAM>(completion.get()))) {
                    completion.release();
                }
            });
    } catch (...) {
        state.busy = false;
        throw;
    }
}

void handle_web_message(AppState& state, const std::string& message) {
    constexpr std::size_t maximum_message_bytes = 16 * 1024 * 1024;
    if (message.empty() || message.size() > maximum_message_bytes) {
        throw std::invalid_argument("The interface message is empty or exceeds the size limit.");
    }
    const auto separator = message.find('|');
    const std::string action = message.substr(0, separator);
    static constexpr std::array<std::string_view, 14> allowed_actions{
        "ready", "browse", "transcribe", "export-edits", "play-original",
        "stop-playback", "cancel", "open-output", "accept-proposal", "live-edit",
        "refine-live", "apply-refinement", "discard-refinement", "undo-refinement"};
    if (std::find(allowed_actions.begin(), allowed_actions.end(), action) ==
        allowed_actions.end()) {
        throw std::invalid_argument("The interface requested an unsupported operation.");
    }
    const std::string query = separator == std::string::npos
                                  ? std::string{}
                                  : message.substr(separator + 1);
    const auto fields = parse_fields(query);

    if (action == "ready") {
#ifdef TRANSCRIBE_ENABLE_ASIO
        constexpr bool asio_enabled = true;
#else
        constexpr bool asio_enabled = false;
#endif
#ifdef TRANSCRIBE_ENABLE_ONNX
        constexpr bool onnx_enabled = true;
#else
        constexpr bool onnx_enabled = false;
#endif
#ifdef TRANSCRIBE_ENABLE_FFMPEG
        constexpr bool ffmpeg_enabled = true;
#else
        constexpr bool ffmpeg_enabled = false;
#endif
#ifdef TRANSCRIBE_ENABLE_VEROVIO
        constexpr bool verovio_enabled = true;
#else
        constexpr bool verovio_enabled = false;
#endif
        post_json(state, L"{\"type\":\"ready\",\"asio\":" +
                             std::wstring(asio_enabled ? L"true" : L"false") +
                             L",\"onnx\":" +
                             std::wstring(onnx_enabled ? L"true" : L"false") +
                             L",\"ffmpeg\":" +
                             std::wstring(ffmpeg_enabled ? L"true" : L"false") +
                             L",\"verovio\":" +
                             std::wstring(verovio_enabled ? L"true" : L"false") + L"}");
#ifdef TRANSCRIBE_ENABLE_ASIO
        post_json(state, asio_devices_json());
#endif
    } else if (action == "browse") {
        const std::string field_name = field(fields, "field");
        browse_for_path(state, field_name, from_utf8(field(fields, "current")));
    } else if (action == "transcribe") {
        start_transcription(state, fields);
    } else if (action == "export-edits") {
        export_edited_score(state, fields);
    } else if (action == "accept-proposal") {
        accept_proposal(state);
    } else if (action == "live-edit") {
        if (!state.busy || !state.active_asio) {
            throw std::runtime_error("Live note editing is available only during capture.");
        }
        LiveEditCommand command;
        command.note_id = field(fields, "note_id");
        const auto kind = field(fields, "kind");
        if (kind == "add") command.kind = LiveEditKind::Add;
        else if (kind == "update") command.kind = LiveEditKind::Update;
        else if (kind == "delete") command.kind = LiveEditKind::Delete;
        else if (kind == "fingering") command.kind = LiveEditKind::Fingering;
        else throw std::invalid_argument("Unsupported live note edit.");
        if (command.note_id.empty() || command.note_id.size() > 256) {
            throw std::invalid_argument("A live edit requires a stable note ID.");
        }
        if (command.kind == LiveEditKind::Add ||
            command.kind == LiveEditKind::Update) {
            const auto pitch = field(fields, "midi");
            std::size_t pitch_characters = 0;
            command.midi_pitch = std::stoi(pitch, &pitch_characters);
            if (pitch_characters != pitch.size()) {
                throw std::invalid_argument("Invalid MIDI pitch in live edit.");
            }
            command.onset_seconds = parse_number(field(fields, "start"), "note onset");
            command.duration_seconds =
                parse_number(field(fields, "duration"), "note duration");
        } else if (command.kind == LiveEditKind::Fingering) {
            const auto string_text = field(fields, "string");
            const auto fret_text = field(fields, "fret");
            std::size_t consumed = 0;
            command.string_number = std::stoi(string_text, &consumed);
            if (consumed != string_text.size()) {
                throw std::invalid_argument("Invalid string number in live fingering.");
            }
            consumed = 0;
            command.fret = std::stoi(fret_text, &consumed);
            if (consumed != fret_text.size()) {
                throw std::invalid_argument("Invalid fret in live fingering.");
            }
        }
        std::lock_guard lock(state.live_command_mutex);
        if (state.live_commands.size() >= 256) {
            throw std::runtime_error(
                "The live edit queue is full; wait for pending edits to apply.");
        }
        state.live_commands.push_back(std::move(command));
    } else if (action == "refine-live") {
        start_live_refinement(state);
    } else if (action == "apply-refinement") {
        apply_live_refinement(state, fields);
    } else if (action == "discard-refinement") {
        discard_live_refinement(state);
    } else if (action == "undo-refinement") {
        undo_live_refinement(state);
    } else if (action == "play-original") {
        if (!state.last_request || state.last_request->use_asio ||
            state.last_request->input_path.empty()) {
            throw std::runtime_error("Transcribe a file before playing its original audio.");
        }
        auto extension = state.last_request->input_path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t character) {
                           return static_cast<wchar_t>(std::towlower(character));
                       });
        if (extension != L".wav") {
            throw std::runtime_error("Original-audio playback currently supports WAV files.");
        }
        if (!PlaySoundW(state.last_request->input_path.c_str(), nullptr,
                        SND_ASYNC | SND_FILENAME | SND_NODEFAULT)) {
            throw std::runtime_error("Windows could not play the selected WAV file.");
        }
    } else if (action == "stop-playback") {
        PlaySoundW(nullptr, nullptr, 0);
    } else if (action == "cancel") {
        if (state.busy && state.active_asio && state.worker.joinable()) {
            state.worker.request_stop();
            post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Stopping capture...\"}");
        }
    } else if (action == "open-output") {
        const auto output_path = std::filesystem::path(
            from_utf8(field(fields, "path")));
        auto directory = output_path.parent_path();
        if (directory.empty()) {
            directory = std::filesystem::current_path();
        }
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(state.window, L"open", directory.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL));
        if (result <= 32) {
            post_error(state, "Could not open the output folder.");
        }
    }
}

HRESULT EnvironmentCompleted::Invoke(HRESULT error_code,
                                     ICoreWebView2Environment* environment) {
    if (state_.shutting_down || !IsWindow(state_.window)) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (FAILED(error_code) || environment == nullptr) {
        MessageBoxW(state_.window,
                    L"WebView2 could not start. Install the Microsoft Edge WebView2 Runtime.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return FAILED(error_code) ? error_code : E_FAIL;
    }

    state_.environment = environment;
    auto* callback = new ControllerCompleted(state_);
    const HRESULT result =
        state_.environment->CreateCoreWebView2Controller(state_.window, callback);
    callback->Release();
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"WebView2 could not create its browser control.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
    }
    return result;
}

HRESULT ControllerCompleted::Invoke(HRESULT error_code,
                                    ICoreWebView2Controller* controller) {
    if (state_.shutting_down || !IsWindow(state_.window)) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (FAILED(error_code) || controller == nullptr) {
        MessageBoxW(state_.window, L"WebView2 could not create its browser control.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return FAILED(error_code) ? error_code : E_FAIL;
    }

    state_.controller = controller;
    HRESULT result = state_.controller->get_CoreWebView2(&state_.webview);
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not access the WebView2 browser control.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }
    resize_webview(state_);

    ComPtr<ICoreWebView2Controller4> controller4;
    if (SUCCEEDED(state_.controller->QueryInterface(
            IID_ICoreWebView2Controller4,
            reinterpret_cast<void**>(controller4.GetAddressOf())))) {
        controller4->put_AllowExternalDrop(FALSE);
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(state_.webview->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    auto* navigation_handler = new NavigationStarting();
    result = state_.webview->add_NavigationStarting(
        navigation_handler, &state_.navigation_token);
    navigation_handler->Release();
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not secure WebView2 navigation.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }

    auto* message_handler = new WebMessageReceived(state_);
    result = state_.webview->add_WebMessageReceived(
        message_handler, &state_.message_token);
    message_handler->Release();
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not initialize the WebView2 message bridge.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }

    ComPtr<ICoreWebView2_3> webview3;
    result = state_.webview->QueryInterface(
        IID_ICoreWebView2_3,
        reinterpret_cast<void**>(webview3.GetAddressOf()));
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"The installed WebView2 Runtime is too old.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }
    const auto ui_directory = executable_directory() / L"ui";
    result = webview3->SetVirtualHostNameToFolderMapping(
        L"appassets.transcribe", ui_directory.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not load the local user interface assets.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }
    result = state_.webview->Navigate(L"https://appassets.transcribe/index.html");
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not open the local user interface.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
    }
    return result;
}

HRESULT NavigationStarting::Invoke(ICoreWebView2*,
                                   ICoreWebView2NavigationStartingEventArgs* args) {
    if (args == nullptr) {
        return E_POINTER;
    }
    LPWSTR uri = nullptr;
    const HRESULT result = args->get_Uri(&uri);
    if (FAILED(result)) {
        return result;
    }
    if (uri != nullptr) {
        const bool allowed = std::wstring_view(uri).starts_with(app_origin);
        CoTaskMemFree(uri);
        if (!allowed) {
            return args->put_Cancel(TRUE);
        }
    }
    return S_OK;
}

HRESULT WebMessageReceived::Invoke(
    ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (args == nullptr) {
        return E_POINTER;
    }
    LPWSTR source = nullptr;
    if (FAILED(args->get_Source(&source)) || source == nullptr) {
        return S_OK;
    }
    const bool trusted = std::wstring_view(source).starts_with(app_origin);
    CoTaskMemFree(source);
    if (!trusted) {
        return S_OK;
    }

    LPWSTR message = nullptr;
    const HRESULT message_result = args->TryGetWebMessageAsString(&message);
    if (FAILED(message_result) || message == nullptr) {
        post_error(state_, "The interface sent a message that was not a text command.");
        return S_OK;
    }
    std::string encoded_message;
    try {
        encoded_message = to_utf8(message);
    } catch (const std::exception& error) {
        CoTaskMemFree(message);
        post_error(state_, error.what());
        return S_OK;
    }
    CoTaskMemFree(message);
    try {
        handle_web_message(state_, encoded_message);
    } catch (const std::exception& error) {
        post_error(state_, error.what());
    }
    return S_OK;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE:
        try {
            initialize_webview(*state);
        } catch (const std::exception& error) {
            MessageBoxW(window, from_utf8(error.what()).c_str(), L"Transcriber",
                        MB_OK | MB_ICONERROR);
            return -1;
        }
        return 0;
    case WM_SIZE:
        resize_webview(*state);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = 760;
        limits->ptMinTrackSize.y = 650;
        return 0;
    }
    case message_progress: {
        std::unique_ptr<ProgressMessage> progress(
            reinterpret_cast<ProgressMessage*>(lparam));
        std::wostringstream json;
        json << L"{\"type\":\"progress\",\"progress\":" << progress->percent
             << L",\"status\":\"" << json_escape(progress->status) << L"\"}";
        post_json(*state, json.str());
        return 0;
    }
    case message_live_preview: {
        std::unique_ptr<LiveMessage> live(reinterpret_cast<LiveMessage*>(lparam));
        post_json(*state, L"{\"type\":\"live\",\"notes\":" +
                             notes_json(live->notes, live->offset) +
                             L",\"events\":" + events_json(live->events) + L"}");
        return 0;
    }
    case message_live_session: {
        std::unique_ptr<LiveSessionMessage> live;
        {
            std::lock_guard lock(state->live_message_mutex);
            live = std::move(state->pending_live_message);
            state->live_message_posted = false;
        }
        if (live) {
            post_json(*state, L"{\"type\":\"live\",\"live_session\":true,\"notes\":" +
                live_notes_json(live->notes, live->fingerings,
                                live->recording_seconds) +
                L",\"events\":" + live_events_json(live->events, live->sample_rate) +
                L",\"metrics\":" + live_metrics_json(live->metrics) +
                L",\"recording_seconds\":" +
                std::to_wstring(live->recording_seconds) + L"}");
        }
        return 0;
    }
    case message_audio_preview: {
        std::unique_ptr<AudioPreviewMessage> preview(
            reinterpret_cast<AudioPreviewMessage*>(lparam));
        post_json(*state, audio_preview_json(
                              preview->offset, preview->replace, preview->preview));
        return 0;
    }
    case message_complete: {
        std::unique_ptr<CompletionMessage> completion(
            reinterpret_cast<CompletionMessage*>(lparam));
        if (state->worker.joinable()) {
            state->worker.join();
        }
        state->busy = false;
        state->active_asio = false;
        if (completion->refinement_proposal) {
            if (completion->success && completion->score_proposal &&
                completion->proposal_review) {
                state->pending_live_proposal =
                    std::move(completion->score_proposal);
                state->pending_live_review =
                    std::move(completion->proposal_review);
                post_json(*state, refinement_review_json(
                    *state->pending_live_proposal,
                    *state->pending_live_review));
            } else {
                post_json(*state,
                    L"{\"type\":\"refinement-proposal\",\"success\":false,\"error\":\"" +
                    json_escape(from_utf8_lossy(completion->error)) + L"\"}");
            }
        } else if (completion->export_only) {
            if (completion->success) {
                if (completion->refinement_applied) {
                    state->pre_refinement_result = state->last_result;
                    state->pre_refinement_document = state->live_document;
                    state->refinement_result_revision =
                        completion->result.score.revision;
                } else if (!completion->refinement_undo) {
                    state->pre_refinement_result.reset();
                    state->pre_refinement_document.reset();
                    state->refinement_result_revision = 0;
                }
                if (!completion->live_capture && state->last_result &&
                    state->live_document &&
                    state->last_result->score.source_id == "live-capture" &&
                    completion->result.score.source_id == "live-capture") {
                    try {
                        synchronize_live_document(
                            *state->live_document,
                            state->last_result->score,
                            completion->result.score);
                    } catch (const std::exception& error) {
                        post_error(*state,
                            std::string("Score export succeeded, but live edit history could not be synchronized; offline refinement is unavailable. ") +
                                error.what());
                        state->live_document.reset();
                        state->live_recording = {};
                    }
                }
                state->last_result = completion->result;
                state->last_request = completion->request;
                state->manual_fingering_corrections =
                    completion->request.guitar_tab_options.manual_corrections;
                if (completion->live_capture) {
                    state->live_document = std::move(completion->live_document);
                    state->live_recording = std::move(completion->live_recording);
                    state->live_backend_id = completion->live_backend_id;
                } else if (completion->result.score.source_id != "live-capture") {
                    state->live_document.reset();
                    state->live_recording = {};
                    state->live_backend_id.clear();
                }
                if (completion->refinement_undo) {
                    state->pre_refinement_result.reset();
                    state->pre_refinement_document.reset();
                    state->refinement_result_revision = 0;
                }
                state->proposal_request.reset();
                state->proposal_result.reset();
                state->proposal_live_document.reset();
                state->proposal_live_recording = {};
                state->proposal_live_backend_id.clear();
            } else if (completion->live_capture &&
                       !completion->refinement_applied &&
                       !completion->refinement_undo) {
                state->proposal_live_document =
                    std::move(completion->live_document);
                state->proposal_live_recording =
                    std::move(completion->live_recording);
                state->proposal_live_backend_id =
                    completion->live_backend_id;
            }
            std::wstring export_message =
                std::wstring(L"{\"type\":\"exported\",\"success\":") +
                (completion->success ? L"true" : L"false");
            if (completion->success) {
                export_message += L",\"score_revision\":" +
                    std::to_wstring(completion->result.score.revision) +
                    L",\"score_id\":\"" +
                    json_escape(from_utf8(completion->result.score.id)) +
                    L"\",\"notes\":" + score_notes_json(completion->result.score) +
                    L",\"metadata\":" + score_metadata_json(completion->result.score) +
                    L",\"fingering\":" +
                    fingering_json(completion->result.fingering) +
                    L",\"live_capture\":" +
                    std::wstring(completion->live_capture ? L"true" : L"false") +
                    L",\"refinement_applied\":" +
                    std::wstring(completion->refinement_applied ? L"true" : L"false") +
                    L",\"refinement_undo\":" +
                    std::wstring(completion->refinement_undo ? L"true" : L"false") +
                    L",\"withheld_conflicts\":" +
                    std::to_wstring(completion->withheld_conflicts) +
                    L",\"can_undo_refinement\":" +
                    std::wstring(state->pre_refinement_result
                                     ? L"true" : L"false");
#ifdef TRANSCRIBE_ENABLE_VEROVIO
                export_message += L",\"musicxml\":\"" +
                    json_escape(from_utf8(completion->result.musicxml_source)) + L"\"";
#endif
                export_message += L"}";
            } else {
                export_message += L",\"error\":\"" +
                                  json_escape(from_utf8_lossy(completion->error)) +
                                  L"\"}";
            }
            post_json(*state, export_message);
            if (completion->success && state->pending_live_proposal &&
                state->pending_live_review && state->live_document &&
                (completion->refinement_applied || completion->refinement_undo)) {
                state->pending_live_review = state->live_document->review(
                    *state->pending_live_proposal);
                if (!state->pending_live_review->changes.empty()) {
                    post_json(*state, refinement_review_json(
                        *state->pending_live_proposal,
                        *state->pending_live_review));
                } else {
                    state->pending_live_proposal.reset();
                    state->pending_live_review.reset();
                    post_json(*state, L"{\"type\":\"refinement-discarded\"}");
                }
            }
        } else {
            if (completion->success) {
                if (state->last_result) {
                    completion->proposal_only = true;
                    completion->accepted_revision =
                        state->last_result->score.revision;
                    state->proposal_request = completion->request;
                    state->proposal_result = completion->result;
                    if (completion->live_capture) {
                        state->proposal_live_document =
                            std::move(completion->live_document);
                        state->proposal_live_recording =
                            std::move(completion->live_recording);
                        state->proposal_live_backend_id =
                            completion->live_backend_id;
                    } else {
                        state->proposal_live_document.reset();
                        state->proposal_live_recording = {};
                        state->proposal_live_backend_id.clear();
                    }
                } else {
                    state->last_request = completion->request;
                    state->last_result = completion->result;
                    if (completion->live_capture) {
                        state->live_document =
                            std::move(completion->live_document);
                        state->live_recording =
                            std::move(completion->live_recording);
                        state->live_backend_id = completion->live_backend_id;
                    } else if (completion->result.score.source_id !=
                               "live-capture") {
                        state->live_document.reset();
                        state->live_recording = {};
                        state->live_backend_id.clear();
                    }
                }
            } else if (completion->live_capture) {
                state->live_document = std::move(completion->live_document);
                state->live_recording = std::move(completion->live_recording);
                state->live_backend_id = completion->live_backend_id;
            }
            post_json(*state, result_json(*completion));
        }
        if (state->close_when_done) {
            DestroyWindow(window);
        }
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wparam);
        std::array<wchar_t, 32768> path{};
        if (DragQueryFileW(drop, 0, path.data(),
                           static_cast<UINT>(path.size())) > 0) {
            if (!state->busy) {
#ifndef TRANSCRIBE_ENABLE_FFMPEG
                auto extension = std::filesystem::path(path.data()).extension().wstring();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](wchar_t character) {
                                   return static_cast<wchar_t>(std::towlower(character));
                               });
                if (extension != L".wav") {
                    post_error(*state, "This build accepts WAV files only.");
                    DragFinish(drop);
                    return 0;
                }
#endif
                set_browser_field(*state, "input", path.data());
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        if (state->busy && state->worker.joinable()) {
            state->close_when_done = true;
            state->worker.request_stop();
            post_json(*state,
                      L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Stopping before closing...\"}");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->shutting_down = true;
        PlaySoundW(nullptr, nullptr, 0);
        if (state->worker.joinable()) {
            state->worker.request_stop();
            state->worker.join();
        }
        if (state->controller) {
            state->controller->Close();
        }
        state->webview.Reset();
        state->controller.Reset();
        state->environment.Reset();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        MessageBoxW(nullptr, L"Could not initialize Windows COM services.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return 1;
    }

    AppState state;
    const RuntimeStartup runtime_startup = prepare_webview_runtime(state);
    if (runtime_startup != RuntimeStartup::ready) {
        if (state.loader != nullptr) {
            FreeLibrary(state.loader);
        }
        CoUninitialize();
        return runtime_startup == RuntimeStartup::failed ? 1 : 0;
    }

    const wchar_t class_name[] = L"TranscriberWebViewWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(RGB(11, 16, 21));
    window_class.lpszClassName = class_name;
    if (RegisterClassExW(&window_class) == 0) {
        DeleteObject(window_class.hbrBackground);
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not register the application window.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return 1;
    }

    RECT bounds{0, 0, 1240, 900};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(
        0, class_name, L"Transcriber", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        DeleteObject(window_class.hbrBackground);
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not create the application window.",
                    L"Transcriber", MB_OK | MB_ICONERROR);
        return 1;
    }

    DragAcceptFiles(window, TRUE);
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.loader != nullptr) {
        FreeLibrary(state.loader);
    }
    DeleteObject(window_class.hbrBackground);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
