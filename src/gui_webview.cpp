#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>

#include "gui_transcription.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/project.hpp"
#include "transcribe/review.hpp"
#include "transcribe/guitar_analysis.hpp"
#include "transcribe/preprocessing.hpp"
#include "transcribe/live_bridge.hpp"
#include "transcribe/output.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
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
constexpr UINT message_project_autosave = WM_APP + 6;
constexpr UINT message_live_events_ready = WM_APP + 7;
constexpr UINT message_live_status = WM_APP + 8;
constexpr UINT_PTR live_session_timer = 0x4752;

class ProjectAutosaver;
struct LiveBridgeState;

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

struct ProgressMessage;
struct LiveMessage;
struct AudioPreviewMessage;

struct AppState {
    HWND window{};
    HMODULE loader{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navigation_token{};
    EventRegistrationToken message_token{};
    std::jthread worker;
    std::unique_ptr<ProjectAutosaver> autosaver;
    std::mutex live_command_mutex;
    std::deque<std::vector<LiveEditCommand>> live_commands;
    std::mutex live_message_mutex;
    std::unique_ptr<ProgressMessage> pending_progress;
    std::unique_ptr<LiveMessage> pending_preview_notes;
    std::unique_ptr<AudioPreviewMessage> pending_audio_preview;
    std::unique_ptr<struct LiveStatusMessage> pending_live_status;
    bool progress_posted{};
    bool preview_notes_posted{};
    bool audio_preview_posted{};
    bool live_status_posted{};
    bool live_session_timer_active{};
    std::shared_ptr<LiveBridgeState> live_bridge;
    bool busy{};
    std::uint64_t next_transcription_job{};
    std::uint64_t active_transcription_job{};
    bool active_asio{};
    std::atomic<bool> analysis_paused{false};
    bool close_when_done{};
    bool shutting_down{};
    bool review_audio_open{};
    bool original_playback{};
    std::uint64_t playback_token{};
    bool transcription_cancelled{};
    bool live_audio_asset_written{};
    std::filesystem::path project_path;
    std::optional<transcribe::ProjectDocument> project;
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
    std::uint64_t job{};
    int percent{-1};
    std::wstring status;
};

struct LiveMessage {
    std::uint64_t job{};
    double offset{};
    std::vector<transcribe::Note> notes;
    std::vector<transcribe::NoteEvent> events;
};

struct LiveStatusMessage {
    std::uint64_t job{};
    transcribe::live::bridge::CaptureState state{
        transcribe::live::bridge::CaptureState::Idle};
    std::string status;
};

struct LiveBridgeState {
    explicit LiveBridgeState(std::uint32_t sample_rate)
        : document(sample_rate), sample_rate(sample_rate) {}

    std::mutex mutex;
    std::condition_variable drained;
    std::shared_ptr<transcribe::live::LiveTranscriptionSession> session;
    transcribe::live::TranscriptionDocument document;
    transcribe::live::bridge::Coalescer coalescer;
    std::unordered_map<std::string, std::pair<int, int>> fingerings;
    std::unordered_set<std::string> manual_note_ids;
    std::vector<transcribe::live::LiveNote> manual_upserts;
    std::vector<std::pair<std::string, std::uint64_t>> manual_retractions;
    std::atomic<bool> notification_posted{false};
    std::atomic<std::int64_t> notification_posted_at_ns{0};
    std::atomic<bool> notification_failed{false};
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> shutdown_requested{false};
    std::deque<double> ui_dispatch_samples_ms;
    std::uint64_t ui_dispatch_sample_count{};
    bool final_batch_applied{};
    std::uint64_t job{};
    std::uint32_t sample_rate{};
    double driver_input_latency_ms{-1};
    double callback_period_ms{};
    transcribe::live::bridge::CaptureState capture_state{
        transcribe::live::bridge::CaptureState::Arming};
    std::string status{"Preparing ASIO input"};
};

struct AudioPreviewMessage {
    std::uint64_t job{};
    double offset{};
    bool replace{};
    transcribe::AudioPreviewData preview;
};

struct CompletionMessage {
    std::uint64_t job{};
    bool success{};
    bool export_only{};
    bool cancelled{};
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
    bool project_save{};
    bool project_open{};
    bool project_recovery{};
    bool score_only_copy{};
    bool has_project{};
    std::filesystem::path project_path;
    transcribe::ProjectDocument project;
    std::uint64_t edit_generation{};
    std::string project_warning;
};

template<class Message>
void post_coalesced(AppState& state, UINT kind, std::unique_ptr<Message> message,
                    std::unique_ptr<Message>& pending, bool& posted) {
    std::lock_guard lock(state.live_message_mutex);
    pending = std::move(message);
    if (!posted) {
        if (!PostMessageW(state.window, kind, 1, 0)) {
            pending.reset();
            throw std::runtime_error("Could not post transcription update to the interface.");
        }
        posted = true;
    }
}

void post_live_events_ready(
    AppState& state, const std::shared_ptr<LiveBridgeState>& bridge) noexcept {
    if (bridge->notification_posted.exchange(true, std::memory_order_acq_rel)) return;
    auto expected = std::int64_t{0};
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bridge->notification_posted_at_ns.compare_exchange_strong(
        expected, now_ns, std::memory_order_release, std::memory_order_relaxed);
    if (!PostMessageW(state.window, message_live_events_ready, 0, 0)) {
        bridge->notification_failed.store(true, std::memory_order_release);
    }
}

void post_live_capture_state(
    AppState& state, const std::shared_ptr<LiveBridgeState>& bridge,
    transcribe::live::bridge::CaptureState capture_state, std::string status) {
    {
        std::lock_guard lock(bridge->mutex);
        bridge->capture_state = capture_state;
        bridge->status = status;
    }
    auto message = std::make_unique<LiveStatusMessage>();
    message->job = bridge->job;
    message->state = capture_state;
    message->status = std::move(status);
    post_coalesced(state, message_live_status, std::move(message),
                   state.pending_live_status, state.live_status_posted);
}

struct AutosaveCompletion {
    bool success{};
    std::string error;
    std::uint64_t revision{};
    std::uint64_t edit_generation{};
    std::wstring summary;
};

std::wstring project_summary(const transcribe::ProjectDocument& project);
std::wstring project_summary_json(
    const transcribe::ProjectDocument& project,
    const std::filesystem::path& path);

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

void post_live_status_json(
    AppState& state, std::uint64_t job,
    transcribe::live::bridge::CaptureState capture_state,
    std::string_view status) {
    std::wostringstream json;
    json << L"{\"type\":\"live-status\",\"version\":"
         << transcribe::live::bridge::protocol_version
         << L",\"job\":" << job << L",\"state\":\""
         << from_utf8(std::string(
                transcribe::live::bridge::capture_state_name(capture_state)))
         << L"\",\"status\":\"" << json_escape(from_utf8_lossy(status)) << L"\"}";
    post_json(state, json.str());
}

std::filesystem::path project_autosave_directory() {
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(),
        static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) {
        throw std::runtime_error("The LOCALAPPDATA folder could not be located.");
    }
    const auto directory =
        std::filesystem::path(local_app_data.data()) /
        L"Transcribe" / L"Projects" / L"Autosave";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "Could not create the project autosave folder.", directory, error);
    }
    return directory;
}

std::filesystem::path project_autosave_path() {
    return project_autosave_directory() / L"recovery.transcriber";
}

class ProjectAutosaver {
public:
    ProjectAutosaver(HWND window, std::filesystem::path path)
        : window_(window), path_(std::move(path)),
          worker_([this](std::stop_token stop) { run(stop); }) {}

    ~ProjectAutosaver() {
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void submit(transcribe::ProjectDocument project,
                std::uint64_t edit_generation) {
        {
            std::lock_guard lock(mutex_);
            pending_ = Task{std::move(project), false, edit_generation};
        }
        condition_.notify_one();
    }

    void clear() {
        {
            std::lock_guard lock(mutex_);
            pending_ = Task{std::nullopt, true, 0};
        }
        condition_.notify_one();
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    struct Task {
        std::optional<transcribe::ProjectDocument> project;
        bool remove{};
        std::uint64_t edit_generation{};
    };

    void run(std::stop_token stop) {
        for (;;) {
            std::optional<Task> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [this] { return pending_.has_value(); });
                if (!pending_ && stop.stop_requested()) return;
                task = std::move(pending_);
                pending_.reset();
            }
            if (!task) continue;
            auto result = std::make_unique<AutosaveCompletion>();
            result->edit_generation = task->edit_generation;
            try {
                if (task->remove) {
                    std::error_code error;
                    std::filesystem::remove(path_, error);
                    if (error) {
                        throw std::filesystem::filesystem_error(
                            "Could not clear the project recovery file.", path_, error);
                    }
                } else if (task->project) {
                    result->revision = task->project->score.revision;
                    transcribe::save_project_file_atomic(path_, *task->project);
                    result->summary = project_summary(*task->project);
                }
                result->success = true;
            } catch (const std::exception& error) {
                result->error = error.what();
            }
            if (PostMessageW(window_, message_project_autosave, 0,
                             reinterpret_cast<LPARAM>(result.get()))) {
                result.release();
            }
            if (stop.stop_requested()) {
                std::lock_guard lock(mutex_);
                if (!pending_) return;
            }
        }
    }

    HWND window_{};
    std::filesystem::path path_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<Task> pending_;
    std::jthread worker_;
};

void post_error(AppState& state, std::string_view message) {
    post_json(state, L"{\"type\":\"error\",\"message\":\"" +
                         json_escape(from_utf8_lossy(message)) + L"\"}");
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
             << L",\"calibrated_confidence\":null"
             << L",\"raw_strength\":" << note.confidence
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

std::wstring live_note_json(
    const transcribe::live::LiveNote& note,
    const std::unordered_map<std::string, std::pair<int, int>>& fingerings,
    double recording_seconds) {
    if (note.state == transcribe::EventLifecycle::Rejected) return {};
    const double start = note.onset_seconds();
    const double end = note.offset_seconds().value_or(recording_seconds);
    if (!std::isfinite(start) || !std::isfinite(end) || end <= start) return {};
    std::wostringstream json;
    json << L"{\"id\":\"" << json_escape(from_utf8(note.id))
         << L"\",\"midi\":" << note.midi_pitch
         << L",\"name\":\"" << json_escape(from_utf8(
                transcribe::midi_note_name(note.midi_pitch))) << L'"'
         << L",\"start\":" << std::fixed << std::setprecision(6) << start
         << L",\"duration\":" << std::max(0.001, end - start)
         << L",\"confidence\":null,\"calibrated_confidence\":null"
         << L",\"amplitude\":null,\"raw_strength\":"
         << note.raw_strength << L",\"finalized\":" << (note.finalized ? L"true" : L"false")
         << L",\"revision\":" << note.revision << L",\"state\":\"" << lifecycle_name(note.state)
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
        case transcribe::live::LiveEventKind::NoteFinalized: type = L"finalized"; break;
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
        json << L"{\"type\":\"" << type << L"\",\"id\":\""
             << json_escape(from_utf8(event.note.id)) << L"\",\"revision\":"
             << event.note.revision << L",\"sequence\":" << event.sequence
             << L",\"finalized\":" << (event.note.finalized ? L"true" : L"false")
             << L",\"midi\":"
             << event.note.midi_pitch << L",\"time\":" << std::fixed
             << std::setprecision(6) << time << L",\"latency_ms\":"
             << std::max(0.0, latency) << L",\"message\":\""
             << json_escape(from_utf8(event.message)) << L'"';
        if (event.kind == transcribe::live::LiveEventKind::AnalysisGap) {
            json << L",\"first_frame\":" << event.gap_first_frame
                 << L",\"end_frame\":" << event.gap_end_frame;
        }
        json << L'}';
    }
    json << L']';
    return json.str();
}

transcribe::live::LatencySummary summarize_ui_dispatch(
    const std::deque<double>& samples_ms, std::uint64_t sample_count) {
    transcribe::live::LatencySummary summary;
    summary.count = static_cast<std::size_t>(sample_count);
    if (samples_ms.empty()) return summary;
    std::vector<double> sorted(samples_ms.begin(), samples_ms.end());
    std::sort(sorted.begin(), sorted.end());
    const auto rank = [&](double quantile) {
        const auto index = static_cast<std::size_t>(
            std::ceil(quantile * static_cast<double>(sorted.size())));
        return sorted[std::clamp<std::size_t>(index, 1, sorted.size()) - 1];
    };
    double sum_ms = 0.0;
    for (const auto sample : samples_ms) {
        sum_ms += sample;
        summary.maximum_seconds = std::max(
            summary.maximum_seconds, sample / 1000.0);
    }
    summary.mean_seconds = sum_ms / static_cast<double>(samples_ms.size()) / 1000.0;
    summary.p50_seconds = rank(0.50) / 1000.0;
    summary.p95_seconds = rank(0.95) / 1000.0;
    summary.p99_seconds = rank(0.99) / 1000.0;
    return summary;
}

std::wstring live_metrics_json(
    const transcribe::live::LiveMetrics& metrics,
    const transcribe::live::LatencySummary& ui_dispatch) {
    const auto summary = [](const transcribe::live::LatencySummary& value) {
        std::wostringstream output;
        output << L"{\"mean_ms\":" << value.mean_seconds * 1000.0
               << L",\"p50_ms\":" << value.p50_seconds * 1000.0
               << L",\"p95_ms\":" << value.p95_seconds * 1000.0
               << L",\"p99_ms\":" << value.p99_seconds * 1000.0
               << L",\"maximum_ms\":" << value.maximum_seconds * 1000.0
               << L",\"count\":" << value.count << L'}';
        return output.str();
    };
    std::wostringstream json;
    json << L"{\"capture\":" << summary(metrics.capture_latency)
         << L",\"analysis\":" << summary(metrics.analysis_latency)
         << L",\"end_to_end\":" << summary(metrics.end_to_end_latency)
         << L",\"stabilization\":" << summary(metrics.stabilization_delay)
         << L",\"analysis_queue\":" << summary(metrics.analysis_queue_latency)
         << L",\"confirmation\":" << summary(metrics.confirmation_latency)
         << L",\"publication_queue\":" << summary(metrics.publication_queue_latency)
         << L",\"analysis_window\":" << summary(metrics.algorithmic_latency)
         << L",\"ui_dispatch\":" << summary(ui_dispatch)
         << L",\"overloaded\":" << (metrics.overloaded ? L"true" : L"false")
         << L",\"input_queue_capacity\":" << metrics.input_queue_capacity
         << L",\"input_queue_depth\":" << metrics.input_queue_depth
         << L",\"input_queue_high_water\":" << metrics.input_queue_high_water
         << L",\"event_queue_depth\":" << metrics.event_queue_depth
         << L",\"event_queue_capacity\":" << metrics.event_queue_capacity
         << L",\"event_queue_high_water\":" << metrics.event_queue_high_water
         << L",\"processing_lag_frames\":" << metrics.current_lag_frames
         << L",\"windows_coalesced\":" << metrics.windows_coalesced
         << L",\"skipped_frames\":" << metrics.skipped_frames
         << L",\"sequence_gaps\":" << metrics.sequence_gaps
         << L",\"discontinuities\":" << metrics.discontinuities
         << L",\"tracked_notes_refused\":" << metrics.tracked_notes_refused
         << L",\"provisional_target_misses\":" << metrics.provisional_target_misses
         << L",\"stabilized_target_misses\":" << metrics.stabilized_target_misses
         << L",\"target_provisional_seconds\":" << metrics.target_provisional_seconds
         << L",\"target_stabilized_seconds\":" << metrics.target_stabilized_seconds
         << L",\"blocks_refused\":" << metrics.blocks_refused
         << L",\"events_dropped\":" << metrics.events_dropped
         << L",\"gap_frames\":" << metrics.gap_frames << L'}';
    return json.str();
}

std::wstring live_bridge_message_json(
    const transcribe::live::bridge::Message& message,
    const std::unordered_map<std::string, std::pair<int, int>>& fingerings,
    double recording_seconds, std::uint32_t sample_rate,
    const transcribe::live::LatencySummary& ui_dispatch,
    double driver_input_latency_ms, double callback_period_ms,
    std::uint64_t job) {
    std::wostringstream json;
    const double safe_sample_rate = sample_rate == 0
        ? 1.0 : static_cast<double>(sample_rate);
    json << L"{\"type\":\"live-event-batch\",\"version\":" << message.version
         << L",\"job\":" << job
         << L",\"sequence\":" << message.sequence
         << L",\"resync\":" << (message.resync ? L"true" : L"false")
         << L",\"sample_rate\":" << sample_rate
         << L",\"upserts\":{";
    bool first = true;
    for (const auto& [id, note] : message.upserts) {
        const auto serialized = live_note_json(note, fingerings, recording_seconds);
        if (serialized.empty()) continue;
        if (!first) json << L',';
        first = false;
        json << L'"' << json_escape(from_utf8(id)) << L"\":{\"revision\":"
             << note.revision << L",\"note\":" << serialized << L'}';
    }
    json << L"},\"retractions\":[";
    first = true;
    for (const auto& [id, revision] : message.retractions) {
        if (!first) json << L',';
        first = false;
        json << L"{\"id\":\"" << json_escape(from_utf8(id))
             << L"\",\"revision\":" << revision << L'}';
    }
    json << L"],\"finalizations\":[";
    first = true;
    for (const auto& [id, note] : message.finalizations) {
        const auto serialized = live_note_json(note, fingerings, recording_seconds);
        if (serialized.empty()) continue;
        if (!first) json << L',';
        first = false;
        json << L"{\"id\":\"" << json_escape(from_utf8(id))
             << L"\",\"revision\":" << note.revision
             << L",\"note\":" << serialized << L'}';
    }
    json << L"],\"gaps\":[";
    for (std::size_t i = 0; i < message.gaps.size(); ++i) {
        if (i != 0) json << L',';
        const auto& gap = message.gaps[i];
        json << L"{\"first_frame\":" << gap.first_frame
             << L",\"end_frame\":" << gap.end_frame
             << L",\"start_seconds\":"
             << static_cast<double>(gap.first_frame) / safe_sample_rate
             << L",\"end_seconds\":"
             << static_cast<double>(gap.end_frame) / safe_sample_rate
             << L",\"message\":\"" << json_escape(from_utf8(gap.message)) << L"\"}";
    }
    json << L"],\"events\":" << live_events_json(message.events, sample_rate)
         << L",\"coalesced_events_dropped\":"
         << message.coalesced_events_dropped
         << L",\"metrics\":" << live_metrics_json(message.metrics, ui_dispatch)
         << L",\"status\":{\"state\":\""
    << from_utf8(std::string(
           transcribe::live::bridge::capture_state_name(message.state)))
         << L"\",\"message\":\"" << json_escape(from_utf8(message.status)) << L"\"}"
         << L",\"recording_seconds\":" << recording_seconds
         << L",\"driver_input_latency_ms\":"
         << (driver_input_latency_ms < 0 ? L"null" : std::to_wstring(driver_input_latency_ms))
         << L",\"callback_period_ms\":" << callback_period_ms
         << L",\"fingerings\":{";
    first = true;
    for (const auto& [id, position] : fingerings) {
        if (!first) json << L',';
        first = false;
        json << L'"' << json_escape(from_utf8(id)) << L"\":{\"string\":"
             << position.first << L",\"fret\":" << position.second << L'}';
    }
    json << L"}}";
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
    std::unordered_map<std::string, std::pair<const transcribe::CanonicalNoteEvent*, bool>> sources;
    std::vector<const transcribe::CanonicalNoteEvent*> intervals;
    for (const auto& source : score.source_events) {
        sources.emplace(source.id, std::make_pair(&source, false));
        intervals.push_back(&source);
    }
    std::sort(intervals.begin(), intervals.end(), [](const auto* left, const auto* right) {
        return left->onset_seconds < right->onset_seconds;
    });
    double previous_offset = -1.0;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        const auto& event = *intervals[i];
        sources.at(event.id).second = event.onset_seconds >= previous_offset &&
            (i + 1 == intervals.size() || event.offset_seconds <= intervals[i + 1]->onset_seconds);
        previous_offset = std::max(previous_offset, event.offset_seconds);
    }
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
        bool isolated_source = false;
        if (!note.source_event_ids.empty()) {
            if (const auto source = sources.find(note.source_event_ids.front()); source != sources.end()) {
                source_event = source->second.first;
                isolated_source = source->second.second;
            }
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
             << L",\"voice\":" << note.voice
             << L",\"spelling_step\":\"" << json_escape(from_utf8(note.spelling_step)) << L'"'
             << L",\"spelling_alter\":" << note.spelling_alter
             << L",\"spelling_octave\":" << note.spelling_octave
             << L",\"accidental_required\":" << (note.accidental_required ? L"true" : L"false")
             << L",\"tie_start\":" << (note.tie_start ? L"true" : L"false")
             << L",\"tie_stop\":" << (note.tie_stop ? L"true" : L"false")
             << L",\"finalized\":true"
             << L",\"rhythm_denominator\":" << note.rhythm.denominator
             << L",\"rhythm_dots\":" << note.rhythm.dots
             << L",\"tuplet_actual\":" << note.rhythm.tuplet_actual
             << L",\"measured_start\":" << note.measured_onset_seconds
             << L",\"measured_end\":" << note.measured_offset_seconds
             << L",\"confidence\":";
        if (note.calibrated_confidence) json << *note.calibrated_confidence;
        else json << L"null";
        if (source_event && score.instrument.name == "Guitar" &&
            !source_event->evidence.empty()) {
            transcribe::GuitarTechniqueOptions technique_options;
            technique_options.enabled = true;
            technique_options.isolated_pitch_contour =
                source_event->detection_method == "yin" && isolated_source;
            const auto technique = transcribe::analyze_guitar_technique(
                *source_event, transcribe::InstrumentPrior::Guitar, technique_options);
            json << L",\"guitar_technique\":{\"gesture\":\""
                 << json_escape(from_utf8(std::string(transcribe::guitar_pitch_gesture_name(technique.gesture))))
                 << L"\",\"heuristic_support\":" << technique.heuristic_support
                 << L",\"calibrated_technique_confidence\":null,\"physical_technique_unresolved\":true"
                 << L",\"explanation\":\"" << json_escape(from_utf8(technique.explanation))
                 << L"\"}";
        }
        json << L",\"calibrated_confidence\":";
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
             << L",\"pitch_alternatives\":[";
        const int detected_pitch = source_event
            ? static_cast<int>(std::lround(source_event->midi_pitch))
            : note.sounding_midi_pitch;
        std::map<int, double> alternative_values;
        for (const auto& evidence : note.raw_evidence) {
            if (evidence.midi_pitch == detected_pitch ||
                !evidence.raw_model_value) continue;
            const auto found = alternative_values.find(evidence.midi_pitch);
            if (found == alternative_values.end() ||
                *evidence.raw_model_value > found->second) {
                alternative_values[evidence.midi_pitch] =
                    *evidence.raw_model_value;
            }
        }
        std::vector<std::pair<int, double>> alternatives(
            alternative_values.begin(), alternative_values.end());
        std::stable_sort(alternatives.begin(), alternatives.end(),
            [](const auto& left, const auto& right) {
                if (left.second != right.second) return left.second > right.second;
                return left.first < right.first;
            });
        if (alternatives.size() > 8) alternatives.resize(8);
        for (std::size_t alternative_index = 0;
             alternative_index < alternatives.size(); ++alternative_index) {
            if (alternative_index) json << L',';
            const auto [midi, value] = alternatives[alternative_index];
            json << L"{\"midi\":" << midi << L",\"raw_value\":" << value
                 << L",\"semantics\":\"raw activation; not a probability\"}";
        }
        json << L"],\"raw_evidence\":[";
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
            json << L",\"raw_midi_pitch\":" << evidence.raw_midi_pitch
                 << L",\"strength_semantics\":\""
                 << json_escape(from_utf8(evidence_semantics_name(
                        evidence.strength_semantics)))
                 << L"\",\"raw_value_semantics\":\""
                 << json_escape(from_utf8(evidence_semantics_name(
                        evidence.raw_value_semantics)))
                 << L"\",\"spectral_peak_fraction\":";
            if (evidence.spectral_peak_fraction) {
                json << *evidence.spectral_peak_fraction;
            } else {
                json << L"null";
            }
            json << L",\"onset_strength\":";
            if (evidence.onset_strength) json << *evidence.onset_strength;
            else json << L"null";
            json << L",\"note_state\":";
            if (evidence.note_state) json << *evidence.note_state;
            else json << L"null";
            json << L'}';
        }
        json << L"]}";
    }
    json << L']';
    return json.str();
}

std::wstring review_dimension_name(transcribe::ReviewDimension dimension) {
    switch (dimension) {
    case transcribe::ReviewDimension::Pitch: return L"pitch";
    case transcribe::ReviewDimension::Onset: return L"onset";
    case transcribe::ReviewDimension::Offset: return L"offset";
    case transcribe::ReviewDimension::Fingering: return L"fingering";
    case transcribe::ReviewDimension::Rhythm: return L"rhythm";
    case transcribe::ReviewDimension::Distribution: return L"distribution";
    }
    return L"unknown";
}

std::wstring review_uncertainty_name(transcribe::ReviewUncertainty uncertainty) {
    switch (uncertainty) {
    case transcribe::ReviewUncertainty::NotAssessed: return L"not-assessed";
    case transcribe::ReviewUncertainty::NoConflictObserved: return L"no-conflict-observed";
    case transcribe::ReviewUncertainty::Ambiguous: return L"ambiguous";
    }
    return L"not-assessed";
}

std::wstring review_suggestion_name(transcribe::ReviewSuggestionKind kind) {
    switch (kind) {
    case transcribe::ReviewSuggestionKind::AlternativePitch: return L"alternative-pitch";
    case transcribe::ReviewSuggestionKind::IsolatedOctave: return L"isolated-octave";
    case transcribe::ReviewSuggestionKind::SplitRepeatedNote: return L"split-repeated-note";
    }
    return L"unknown";
}

std::wstring score_review_json(const transcribe::ScoreDocument& score) {
    const auto review = transcribe::review_score(score);
    std::wostringstream json;
    json << L"{\"out_of_distribution_assessed\":false,\"calibrated_notes\":"
         << review.calibrated_notes << L",\"notes_without_evidence\":"
         << review.notes_without_evidence << L",\"limitation\":\""
         << json_escape(from_utf8(review.calibration_limitation))
         << L"\",\"decisions\":[";
    for (std::size_t index = 0; index < score.review_decisions.size(); ++index) {
        if (index) json << L',';
        const auto& decision = score.review_decisions[index];
        json << L"{\"suggestion_id\":\""
             << json_escape(from_utf8(decision.suggestion_id))
             << L"\",\"note_id\":\""
             << json_escape(from_utf8(decision.note_id))
             << L"\",\"outcome\":\""
             << (decision.outcome == transcribe::ReviewDecisionOutcome::Accepted
                     ? L"accepted" : L"rejected")
             << L"\"}";
    }
    json << L"],\"notes\":[";
    for (std::size_t note_index = 0; note_index < review.notes.size(); ++note_index) {
        if (note_index) json << L',';
        const auto& note = review.notes[note_index];
        json << L"{\"note_id\":\"" << json_escape(from_utf8(note.note_id))
             << L"\",\"evidence_samples\":" << note.raw_evidence_samples
             << L",\"calibrated_confidence_available\":"
             << (note.calibrated_confidence_available ? L"true" : L"false")
             << L",\"assessments\":[";
        for (std::size_t index = 0; index < note.assessments.size(); ++index) {
            if (index) json << L',';
            const auto& assessment = note.assessments[index];
            json << L"{\"dimension\":\""
                 << review_dimension_name(assessment.dimension)
                 << L"\",\"state\":\""
                 << review_uncertainty_name(assessment.uncertainty)
                 << L"\",\"explanation\":\""
                 << json_escape(from_utf8(assessment.explanation)) << L"\"}";
        }
        json << L"],\"findings\":[";
        for (std::size_t index = 0; index < note.findings.size(); ++index) {
            if (index) json << L',';
            const auto& finding = note.findings[index];
            json << L"{\"id\":\"" << json_escape(from_utf8(finding.id))
                 << L"\",\"category\":\""
                 << json_escape(from_utf8(finding.category))
                 << L"\",\"explanation\":\""
                 << json_escape(from_utf8(finding.explanation))
                 << L"\",\"ranking_score\":" << finding.ranking_score << L'}';
        }
        json << L"],\"suggestions\":[";
        std::size_t suggestion_count = 0;
        for (const auto& suggestion : note.suggestions) {
            const bool already_decided = std::any_of(
                score.review_decisions.begin(), score.review_decisions.end(),
                [&](const auto& decision) {
                    return decision.suggestion_id == suggestion.id;
                });
            if (already_decided) continue;
            if (suggestion_count++) json << L',';
            json << L"{\"id\":\"" << json_escape(from_utf8(suggestion.id))
                 << L"\",\"kind\":\"" << review_suggestion_name(suggestion.kind)
                 << L"\",\"suggested_midi_pitch\":";
            if (suggestion.suggested_midi_pitch) {
                json << *suggestion.suggested_midi_pitch;
            } else {
                json << L"null";
            }
            json << L",\"split_time_seconds\":";
            if (suggestion.split_time_seconds) {
                json << *suggestion.split_time_seconds;
            } else {
                json << L"null";
            }
            json << L",\"ranking_score\":" << suggestion.ranking_score
                 << L",\"ranking_basis\":\""
                 << json_escape(from_utf8(suggestion.ranking_basis))
                 << L"\",\"explanation\":\""
                 << json_escape(from_utf8(suggestion.explanation)) << L"\"}";
        }
        json << L"]}";
    }
    json << L"]}";
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
         << L"\",\"score_revision\":" << score.revision
         << L",\"source_sample_rate\":" << score.source_sample_rate
         << L",\"tempo_map\":[";
    for (std::size_t i = 0; i < score.tempo_map.size(); ++i) {
        if (i) json << L',';
        const auto& segment = score.tempo_map[i];
        json << std::setprecision(9)
             << L"{\"start_seconds\":" << segment.start_seconds
             << L",\"start_quarters\":" << segment.start_quarters
             << L",\"beats_per_minute\":" << segment.beats_per_minute << L'}';
    }
    json << L"],\"rests\":[";
    for (std::size_t i = 0; i < score.rests.size(); ++i) {
        if (i) json << L',';
        const auto& rest = score.rests[i];
        json << L"{\"voice\":" << rest.voice
             << L",\"start\":" << transcribe::score_position_seconds(score, rest.onset_quarters)
             << L",\"denominator\":" << rest.rhythm.denominator
             << L",\"dots\":" << rest.rhythm.dots << L'}';
    }
    json << L"]}";
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
         << L",\"bands\":" << preview.frequency_bands
         << L",\"minimum_frequency_hz\":" << preview.minimum_frequency_hz
         << L",\"maximum_frequency_hz\":" << preview.maximum_frequency_hz
         << L",\"waveform\":[";
    for (std::size_t index = 0; index < preview.waveform.size(); ++index) {
        if (index != 0) json << L',';
        json << std::setprecision(3) << preview.waveform[index];
    }
    json << L"],\"waveform_minimum\":[";
    for (std::size_t index = 0; index < preview.waveform_minimum.size(); ++index) {
        if (index != 0) json << L',';
        json << std::setprecision(3) << preview.waveform_minimum[index];
    }
    json << L"],\"waveform_maximum\":[";
    for (std::size_t index = 0; index < preview.waveform_maximum.size(); ++index) {
        if (index != 0) json << L',';
        json << std::setprecision(3) << preview.waveform_maximum[index];
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
             << L",\"repeated_strings\":" << cost.repeated_strings
             << L",\"string_crossings\":" << cost.string_crossings
             << L",\"awkward_transitions\":" << cost.awkward_transitions
             << L",\"unassigned_notes\":" << cost.unassigned_notes << L'}';
    };
    std::wostringstream json;
    json << L"{\"fully_playable\":"
         << (fingering.fully_playable ? L"true" : L"false")
         << L",\"search_truncated\":"
         << (fingering.search_truncated ? L"true" : L"false")
         << L",\"alternatives_complete\":" << (fingering.alternatives_complete ? L"true" : L"false")
         << L",\"visited_search_nodes\":" << fingering.visited_search_nodes
         << L",\"explanation\":\"" << json_escape(from_utf8(fingering.explanation)) << L"\""
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
             << L",\"performed_position_unknown\":true,\"calibrated_fingering_confidence\":null"
             << L",\"manual\":"
             << (note.position.manually_corrected ? L"true" : L"false")
             << L",\"explanation\":\"" << json_escape(from_utf8(note.explanation)) << L"\""
             << L",\"suggested_finger\":";
        if (note.position.suggested_finger) json << *note.position.suggested_finger;
        else json << L"null";
        json
             << L",\"correction_rejected\":"
             << (note.correction_rejected ? L"true" : L"false")
             << L",\"playable_positions\":[";
        for (std::size_t i = 0; i < note.playable_positions.size(); ++i) {
            if (i != 0) json << L',';
            const auto& position = note.playable_positions[i];
            json << L"{\"string\":" << position.string_number
                 << L",\"fret\":" << position.fret << L'}';
        }
        json << L"],\"candidates\":[";
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
               L"\",\"cancelled\":" +
               std::wstring(completion.cancelled ? L"true" : L"false") +
               L",\"live_capture\":" +
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
                << L"\nGuitar tab: " << completion.request.tab_path.wstring()
                << L"\nScore JSON: " << completion.request.json_path.wstring();
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
         << L",\"preprocessing_diagnostic\":\""
         << json_escape(from_utf8(completion.result.preprocessing_diagnostic)) << L"\""
         << L",\"live_capture\":"
         << (completion.live_capture ? L"true" : L"false")
         << L",\"backend_id\":\""
         << json_escape(from_utf8(completion.live_backend_id)) << L"\"";
    if (completion.request.retain_review_evidence) {
        json << L",\"review\":" << score_review_json(completion.result.score)
             << L",\"review_evidence_truncated\":"
             << (completion.result.review_evidence_truncated ? L"true" : L"false");
    }
    if (completion.has_project) {
        json << L",\"project\":" <<
            project_summary_json(completion.project, completion.project_path)
             << L",\"project_opened\":"
             << (completion.project_open ? L"true" : L"false");
    }
    if (!completion.project_warning.empty()) {
        json << L",\"project_warning\":\""
             << json_escape(from_utf8_lossy(completion.project_warning)) << L"\"";
    }
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
    state.autosaver = std::make_unique<ProjectAutosaver>(
        state.window, project_autosave_path());
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
    if (name == "auto" || name == "none") return transcribe::InstrumentPrior::None;
    if (name == "guitar") return transcribe::InstrumentPrior::Guitar;
    if (name == "bass") return transcribe::InstrumentPrior::Bass;
    if (name == "piano") return transcribe::InstrumentPrior::Piano;
    if (name == "voice") return transcribe::InstrumentPrior::Voice;
    throw std::invalid_argument("Choose a supported instrument prior.");
}

void set_browser_field(AppState& state, const char* name, const std::wstring& value) {
    post_json(state, L"{\"type\":\"field\",\"field\":\"" + from_utf8(name) +
                         L"\",\"value\":\"" + json_escape(value) + L"\"}");
}

std::string path_utf8(const std::filesystem::path& path) {
    return to_utf8(path.wstring());
}

void set_project_audio_reference(
    transcribe::ProjectDocument& project,
    const std::filesystem::path& source_path,
    const std::filesystem::path& project_path) {
    if (source_path.empty()) {
        project.audio.policy = transcribe::ProjectAudioPolicy::ScoreOnly;
        project.audio.reference_path.clear();
        project.audio.relative_to_project = false;
        return;
    }
    project.audio.policy = transcribe::ProjectAudioPolicy::ExternalReference;
    std::error_code error;
    auto absolute_source = std::filesystem::absolute(source_path, error);
    if (error) absolute_source = source_path;
    if (project.audio.original_reference.empty())
        project.audio.original_reference = path_utf8(absolute_source);
    if (project_path.empty()) {
        project.audio.reference_path = path_utf8(absolute_source);
        project.audio.relative_to_project = false;
        return;
    }
    const auto project_directory = project_path.has_parent_path()
        ? project_path.parent_path() : std::filesystem::current_path();
    auto relative = std::filesystem::relative(
        absolute_source, project_directory, error);
    if (!error && !relative.empty() && *relative.begin() != L"..") {
        project.audio.reference_path = path_utf8(relative);
        project.audio.relative_to_project = true;
    } else {
        project.audio.reference_path = path_utf8(absolute_source);
        project.audio.relative_to_project = false;
    }
}

std::string instrument_prior_name(transcribe::InstrumentPrior prior) {
    switch (prior) {
    case transcribe::InstrumentPrior::Guitar: return "guitar";
    case transcribe::InstrumentPrior::Piano: return "piano";
    case transcribe::InstrumentPrior::Voice: return "voice";
    case transcribe::InstrumentPrior::Bass: return "bass";
    case transcribe::InstrumentPrior::None: return "auto";
    }
    return "auto";
}

void update_export_status(transcribe::ProjectDocument& project,
                          std::string format,
                          const std::filesystem::path& path,
                          bool success, std::string error = {}) {
    if (path.empty()) return;
    const auto found = std::find_if(project.exports.begin(), project.exports.end(),
        [&](const auto& item) { return item.format == format; });
    const transcribe::ProjectExportStatus status{
        std::move(format), path_utf8(path), project.score.revision,
        success, std::move(error)};
    if (found == project.exports.end()) {
        project.exports.push_back(status);
    } else {
        *found = status;
    }
}

void queue_project_autosave(AppState& state,
                            const transcribe::ProjectDocument& project,
                            std::uint64_t edit_generation) {
    if (!state.autosaver) {
        throw std::runtime_error("Project autosave is not available.");
    }
    auto snapshot = project;
    const auto source_project_path = state.project_path.empty()
        ? state.autosaver->path() : state.project_path;
    auto audio_path = transcribe::resolve_project_audio_path(
        snapshot, source_project_path);
    if (state.last_request && !state.last_request->input_path.empty()) {
        std::error_code error;
        if (audio_path.empty() ||
            !std::filesystem::is_regular_file(audio_path, error) || error) {
            audio_path = state.last_request->input_path;
        }
    }
    if (!audio_path.empty()) {
        set_project_audio_reference(
            snapshot, audio_path, state.autosaver->path());
    }
    state.autosaver->submit(std::move(snapshot), edit_generation);
}

void capture_project_settings(transcribe::ProjectDocument& project,
                              const transcribe::gui::Request& request) {
    auto& analysis = project.analysis;
    switch (request.interpretation_options.quantization_mode) {
    case transcribe::QuantizationMode::Legacy: analysis.quantization = "legacy"; break;
    case transcribe::QuantizationMode::EvidenceAware: analysis.quantization = "evidence"; break;
    case transcribe::QuantizationMode::Expressive: analysis.quantization = "expressive"; break;
    }
    analysis.quantization_strength = request.interpretation_options.quantization_strength;
    analysis.minimum_rhythm_evidence = request.interpretation_options.minimum_rhythm_evidence;
    analysis.retain_review_evidence = request.retain_review_evidence;
    analysis.bounded_inference = request.bounded_inference;
    analysis.maximum_fret = request.guitar_tab_options.maximum_fret;
    analysis.preferred_position = request.guitar_tab_options.preferred_position;
    analysis.fingering_alternatives = request.guitar_tab_options.search.maximum_alternatives;
    if (!request.model_path.empty())
        analysis.model_reference = path_utf8(std::filesystem::absolute(request.model_path));
}

void capture_project_alternatives(transcribe::ProjectDocument& project,
                                  const transcribe::FingeringResult& fingering) {
    project.fingering_alternatives.clear();
    project.fingering_alternatives_truncated = fingering.search_truncated;
    std::size_t positions = 0;
    for (const auto& item : fingering.alternatives) {
        if (project.fingering_alternatives.size() >= 16 ||
            positions + item.positions.size() > 100000) {
            project.fingering_alternatives_truncated = true;
            break;
        }
        transcribe::ProjectFingeringAlternative saved;
        saved.cost = item.cost.total();
        saved.cost_components = {item.cost.fret_height, item.cost.preferred_position,
            item.cost.hand_movement, item.cost.string_preference, item.cost.chord_stretch,
            item.cost.unassigned_notes, item.cost.repeated_strings, item.cost.string_crossings,
            item.cost.awkward_transitions};
        for (const auto& position : item.positions) {
            if (position.string_number > 0 && position.fret >= 0 &&
                position.note_index < project.score.notes.size())
                saved.positions.push_back({project.score.notes[position.note_index].id,
                    position.string_number, position.fret});
        }
        positions += saved.positions.size();
        project.fingering_alternatives.push_back(std::move(saved));
    }
}

transcribe::ProjectDocument make_project_document(
    const transcribe::gui::Request& request,
    const transcribe::gui::Result& result,
    const transcribe::AudioBuffer* live_audio,
    std::string& audio_warning) {
    transcribe::ProjectDocument project;
    project.id = result.score.id + "-project";
    project.title = request.use_asio || request.input_path.empty()
        ? "Live recording" : path_utf8(request.input_path.filename());
    if (project.title.empty()) project.title = "Untitled project";
    project.score = result.score;
    project.audio.sample_rate = result.audio_sample_rate;
    project.audio.channels = result.audio_channels;
    project.audio.frame_count = result.audio_frame_count;
    if (request.use_asio && live_audio != nullptr &&
        !live_audio->samples.empty()) {
        try {
            const auto directory = project_autosave_directory();
            const auto asset_directory = directory / L"assets";
            std::error_code error;
            std::filesystem::create_directories(asset_directory, error);
            if (error) {
                throw std::filesystem::filesystem_error(
                    "Could not create the recovery audio folder.",
                    asset_directory, error);
            }
            const auto asset_name = from_utf8(project.id) + L".wav";
            transcribe::write_audio_wav_atomic(
                asset_directory / asset_name, *live_audio);
            project.audio.policy = transcribe::ProjectAudioPolicy::ExternalReference;
            project.audio.reference_path =
                "assets/" + path_utf8(std::filesystem::path(asset_name));
            project.audio.relative_to_project = true;
            project.audio.sample_rate = live_audio->sample_rate;
            project.audio.channels = 1;
            project.audio.frame_count = live_audio->samples.size();
        } catch (const std::exception& error) {
            project.audio.policy = transcribe::ProjectAudioPolicy::ScoreOnly;
            project.audio.reference_path.clear();
            project.audio.relative_to_project = false;
            audio_warning = std::string(
                "Live score saved without its audio asset: ") + error.what();
        }
    } else if (!request.use_asio && !request.input_path.empty()) {
        set_project_audio_reference(project, request.input_path, {});
    }

    if (request.model_path.empty()) {
        project.analysis.backend_identifier = request.use_asio
            ? "builtin:" + request.live_backend : "builtin:yin";
        project.analysis.model_identifier = project.analysis.backend_identifier;
    } else {
        project.analysis.backend_identifier = "onnx-runtime";
        project.analysis.model_identifier =
            path_utf8(request.model_path.filename());
        project.analysis.model_reference = path_utf8(request.model_path);
    }
    project.analysis.instrument_prior =
        instrument_prior_name(request.instrument);
    project.analysis.live_backend = request.live_backend;
    project.analysis.tempo_automatic = request.tempo_automatic;
    project.analysis.meter_automatic = request.meter_automatic;
    project.analysis.key_automatic = request.key_automatic;
    project.analysis.polyphonic = request.polyphonic;
    project.analysis.fast_preview = request.fast_preview;
    project.analysis.preserve_expressive_timing =
        request.preserve_expressive_timing;
    project.analysis.latency_ms = request.realtime_options.latency_ms;
    capture_project_settings(project, request);
    capture_project_alternatives(project, result.fingering);
    update_export_status(project, "musicxml", request.musicxml_path, true);
    update_export_status(project, "tablature", request.tab_path, true);
    update_export_status(project, "midi", request.midi_path, true);
    update_export_status(project, "json", request.json_path, true);
    transcribe::validate_project_document(project);
    return project;
}

std::wstring project_summary(const transcribe::ProjectDocument& project) {
    std::wostringstream summary;
    summary << L"Duration: ";
    if (project.audio.sample_rate > 0 && project.audio.frame_count > 0) {
        summary << std::fixed << std::setprecision(2)
                << static_cast<double>(project.audio.frame_count) /
                       project.audio.sample_rate
                << L" s";
    } else {
        summary << L"unknown";
    }
    summary << L"\nInstrument: "
            << from_utf8(project.score.instrument.name.empty()
                             ? project.analysis.instrument_prior
                             : project.score.instrument.name)
            << L"\nTuning (MIDI): ";
    for (std::size_t index = 0;
         index < project.score.instrument.tuning.size(); ++index) {
        if (index) summary << L", ";
        summary << project.score.instrument.tuning[index];
    }
    summary << L" · capo " << project.score.instrument.capo_fret
            << L"\nBackend: "
            << from_utf8(project.analysis.backend_identifier)
            << L"\nModel: "
            << from_utf8(project.analysis.model_identifier)
            << L"\nScore revision: " << project.score.revision
            << L"\nAudio policy: "
            << (project.audio.policy ==
                        transcribe::ProjectAudioPolicy::ExternalReference
                    ? L"external reference (local)" :
                project.audio.policy == transcribe::ProjectAudioPolicy::ProjectCopy
                    ? L"project-owned copy (move recording and project together)" : L"score only")
            << L"\nExports:";
    if (project.exports.empty()) summary << L" none";
    for (const auto& item : project.exports) {
        summary << L"\n  " << from_utf8(item.format) << L": "
                << (item.success ? L"available" : L"failed")
                << (item.score_revision == project.score.revision
                        ? L" · current" : L" · stale")
                << L" · " << from_utf8(item.path);
        if (!item.error.empty()) {
            summary << L" · " << from_utf8_lossy(item.error);
        }
    }
    if (project.fingering_alternatives_truncated)
        summary << L"\nFingering search/storage budget reached; saved alternatives are incomplete.";
    return summary.str();
}

std::wstring project_summary_json(const transcribe::ProjectDocument& project,
                                  const std::filesystem::path& path) {
    std::wstring tuning_preset = L"custom";
    const auto& tuning = project.score.instrument.tuning;
    static const std::array<std::pair<std::wstring_view, std::array<int, 6>>, 5>
        known_tunings{{
            {L"standard", {64, 59, 55, 50, 45, 40}},
            {L"drop-d", {64, 59, 55, 50, 45, 38}},
            {L"dadgad", {62, 57, 55, 50, 45, 38}},
            {L"open-d", {62, 57, 54, 50, 45, 38}},
            {L"open-g", {62, 59, 55, 50, 43, 38}},
        }};
    if (tuning.size() == 6) {
        for (const auto& [name, pitches] : known_tunings) {
            if (std::equal(tuning.begin(), tuning.end(), pitches.begin())) {
                tuning_preset = std::wstring(name);
                break;
            }
        }
    }

    std::wostringstream json;
    json << L"{\"project_path\":\"" << json_escape(path.wstring())
         << L"\",\"title\":\"" << json_escape(from_utf8(project.title))
         << L"\",\"score_revision\":" << project.score.revision
         << L",\"summary\":\"" << json_escape(project_summary(project))
         << L"\",\"audio_path\":\"";
    const auto audio_path = transcribe::resolve_project_audio_path(project, path);
    json << json_escape(audio_path.wstring()) << L"\",\"settings\":{"
         << L"\"tempo_automatic\":"
         << (project.analysis.tempo_automatic ? L"true" : L"false")
         << L",\"meter_automatic\":"
         << (project.analysis.meter_automatic ? L"true" : L"false")
         << L",\"key_automatic\":"
         << (project.analysis.key_automatic ? L"true" : L"false")
         << L",\"tempo_bpm\":" << project.score.metadata.tempo_bpm
         << L",\"meter_numerator\":" << project.score.metadata.time_numerator
         << L",\"meter_denominator\":" << project.score.metadata.time_denominator
         << L",\"key_fifths\":" << project.score.metadata.key_fifths
         << L",\"minor_key\":"
         << (project.score.metadata.minor_key ? L"true" : L"false")
         << L",\"pickup_quarters\":" << project.score.pickup_quarters
         << L",\"written_transposition\":"
         << project.score.instrument.written_transposition_semitones
         << L",\"tuning_preset\":\"" << tuning_preset
         << L"\",\"tuning\":[";
    for (std::size_t index = 0; index < tuning.size(); ++index) {
        if (index) json << L',';
        json << tuning[index];
    }
    json << L"],\"capo_fret\":" << project.score.instrument.capo_fret
         << L",\"instrument_prior\":\""
         << json_escape(from_utf8(project.analysis.instrument_prior))
         << L"\",\"model_path\":\""
         << json_escape(from_utf8(project.analysis.model_reference))
         << L"\",\"live_backend\":\""
         << json_escape(from_utf8(project.analysis.live_backend))
         << L"\",\"polyphonic\":"
         << (project.analysis.polyphonic ? L"true" : L"false")
         << L",\"fast_preview\":"
         << (project.analysis.fast_preview ? L"true" : L"false")
         << L",\"preserve_timing\":"
         << (project.analysis.preserve_expressive_timing ? L"true" : L"false")
         << L",\"latency_ms\":" << project.analysis.latency_ms
         << L",\"quantization\":\"" << from_utf8(project.analysis.quantization)
         << L"\",\"quantization_strength\":" << project.analysis.quantization_strength
         << L",\"minimum_rhythm_evidence\":" << project.analysis.minimum_rhythm_evidence
         << L",\"retain_review_evidence\":" << (project.analysis.retain_review_evidence ? L"true" : L"false")
         << L",\"bounded_inference\":" << (project.analysis.bounded_inference ? L"true" : L"false")
         << L"}";
    const auto annotation = std::find_if(project.annotations.begin(), project.annotations.end(),
        [](const auto& item) { return item.id == "project-notes"; });
    json << L",\"annotation_text\":\""
         << (annotation == project.annotations.end() ? L"" : json_escape(from_utf8(annotation->text)))
         << L"\",\"audio_policy\":\""
         << (project.audio.policy == transcribe::ProjectAudioPolicy::ProjectCopy ? L"project-copy" :
             project.audio.policy == transcribe::ProjectAudioPolicy::ScoreOnly ? L"score-only" : L"external-reference")
         << L"\""
         << L",\"exports\":{";
    bool first = true;
    for (const auto& status : project.exports) {
        if (status.format != "musicxml" && status.format != "tablature" &&
            status.format != "midi") {
            continue;
        }
        if (!first) json << L',';
        first = false;
        const auto field_name = status.format == "tablature"
            ? L"tab" : from_utf8(status.format);
        json << L'"' << field_name << L"\":\""
             << json_escape(from_utf8(status.path)) << L'"';
    }
    json << L"}}";
    return json.str();
}

std::filesystem::path project_path_from_utf8(std::string_view value) {
    return std::filesystem::path(from_utf8(value));
}

transcribe::gui::Request request_for_project(
    const transcribe::ProjectDocument& project,
    const std::filesystem::path& project_path) {
    transcribe::gui::Request request;
    request.input_path =
        transcribe::resolve_project_audio_path(project, project_path);
    request.model_path =
        project_path_from_utf8(project.analysis.model_reference);
    if (!request.model_path.empty() && request.model_path.is_relative())
        request.model_path = std::filesystem::absolute(project_path).parent_path() / request.model_path;
    request.instrument = instrument_prior(project.analysis.instrument_prior);
    request.tempo_automatic = project.analysis.tempo_automatic;
    request.meter_automatic = project.analysis.meter_automatic;
    request.key_automatic = project.analysis.key_automatic;
    request.polyphonic = project.analysis.polyphonic;
    request.fast_preview = project.analysis.fast_preview;
    request.live_backend = project.analysis.live_backend;
    request.preserve_expressive_timing =
        project.analysis.preserve_expressive_timing;
    request.realtime_options.latency_ms = project.analysis.latency_ms;
    request.interpretation_options.quantization_mode =
        transcribe::parse_quantization_mode(project.analysis.quantization);
    request.interpretation_options.quantization_strength = project.analysis.quantization_strength;
    request.interpretation_options.minimum_rhythm_evidence = project.analysis.minimum_rhythm_evidence;
    request.bounded_inference = project.analysis.bounded_inference;
    request.guitar_tab_options.search.maximum_alternatives = project.analysis.fingering_alternatives;
    request.guitar_tab_options.capo_fret = project.score.instrument.capo_fret;
    request.guitar_tab_options.search.physical_fret_count = project.score.instrument.physical_fret_count;
    request.guitar_tab_options.string_tuning = project.score.instrument.tuning;
    request.guitar_tab_options.maximum_fret = project.analysis.maximum_fret;
    request.guitar_tab_options.preferred_position = project.analysis.preferred_position;
    for (std::size_t index = 0; index < project.score.notes.size(); ++index) {
        const auto& note = project.score.notes[index];
        const auto tab = std::find_if(project.score.tab_notes.begin(),
            project.score.tab_notes.end(), [&](const auto& candidate) {
                return candidate.notation_note_id == note.id;
            });
        if (tab != project.score.tab_notes.end() && tab->manually_corrected &&
            tab->string_number && tab->fret) {
            request.guitar_tab_options.position_overrides.push_back({
                index, *tab->string_number, *tab->fret, true});
        }
    }
    request.retain_review_evidence = project.analysis.retain_review_evidence;

    const auto output_for = [&](std::string_view format) {
        const auto found = std::find_if(project.exports.begin(), project.exports.end(),
            [&](const auto& item) { return item.format == format; });
        return found == project.exports.end()
            ? std::filesystem::path{} : project_path_from_utf8(found->path);
    };
    request.musicxml_path = output_for("musicxml");
    request.tab_path = output_for("tablature");
    request.midi_path = output_for("midi");
    request.json_path = output_for("json");
    const auto base = project_path.empty()
        ? std::filesystem::current_path() / from_utf8(project.title)
        : project_path.parent_path() / project_path.stem();
    if (request.musicxml_path.empty()) {
        request.musicxml_path = base;
        request.musicxml_path.replace_extension(L".musicxml");
    }
    if (request.tab_path.empty()) {
        request.tab_path = base;
        request.tab_path.replace_extension(L".tab.txt");
    }
    if (request.json_path.empty()) {
        request.json_path = base;
        request.json_path.replace_extension(L".score.json");
    }
    return request;
}

transcribe::gui::Result result_for_project(
    const transcribe::ProjectDocument& project,
    const transcribe::gui::Request& request) {
    transcribe::gui::Result result;
    result.score = project.score;
    result.metadata = project.score.metadata;
    result.notes = transcribe::score_notes(project.score);
    result.audio_sample_rate = project.audio.sample_rate;
    result.audio_channels = project.audio.channels;
    result.audio_frame_count = project.audio.frame_count;
    result.fingering = transcribe::assign_guitar_tab_fingerings(
        result.notes, request.guitar_tab_options);
    std::unordered_map<std::string, const transcribe::ScoreTabNote*> saved_positions;
    for (const auto& tab : project.score.tab_notes)
        saved_positions.emplace(tab.notation_note_id, &tab);
    result.fingering.fully_playable = true;
    for (std::size_t index = 0; index < project.score.notes.size(); ++index) {
        const auto tab = saved_positions.find(project.score.notes[index].id);
        transcribe::GuitarTabPosition position{index, 0, -1, false, std::nullopt};
        if (tab != saved_positions.end() && tab->second->string_number && tab->second->fret)
            position = {index, *tab->second->string_number, *tab->second->fret,
                tab->second->manually_corrected, std::nullopt};
        result.fingering.positions[index] = position;
        result.fingering.notes[index].position = position;
        result.fingering.notes[index].playable = position.string_number > 0;
        result.fingering.fully_playable &= position.string_number > 0;
    }
    result.fingering.explanation =
        "Saved canonical score assignments restored. Alternatives remain suggestions, not performed positions.";
    if (!project.fingering_alternatives.empty()) {
        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t index = 0; index < project.score.notes.size(); ++index)
            indices.emplace(project.score.notes[index].id, index);
        result.fingering.alternatives.clear();
        for (const auto& saved : project.fingering_alternatives) {
            transcribe::FingeringAlternative alternative;
            const auto& c = saved.cost_components;
            alternative.cost = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]};
            for (std::size_t index = 0; index < project.score.notes.size(); ++index)
                alternative.positions.push_back({index, 0, -1, false, std::nullopt});
            for (const auto& position : saved.positions) {
                const auto index = indices.at(position.note_id);
                alternative.positions[index] = {index, position.string_number,
                    position.fret, false, std::nullopt};
            }
            result.fingering.alternatives.push_back(std::move(alternative));
        }
        result.fingering.alternatives_complete = false;
    }
    result.fingering.search_truncated |= project.fingering_alternatives_truncated;
    return result;
}

void browse_for_path(AppState& state, const std::string& field_name,
                     const std::wstring& current_value) {
    if (field_name != "input" && field_name != "isolated" && field_name != "model" &&
        field_name != "musicxml" && field_name != "tab" && field_name != "midi" &&
        field_name != "json" &&
        field_name != "project-open" && field_name != "project-save" &&
        field_name != "project-copy" && field_name != "project-portable" &&
        field_name != "project-relink") {
        throw std::invalid_argument("Unknown file picker field.");
    }

    std::array<wchar_t, 32768> filename{};
    if (current_value.size() < filename.size()) {
        std::copy(current_value.begin(), current_value.end(), filename.begin());
    }
    const bool save = field_name == "musicxml" || field_name == "tab" ||
                      field_name == "midi" || field_name == "json" ||
                      field_name == "project-save" ||
                      field_name == "project-copy" || field_name == "project-portable";
    const wchar_t* filter = nullptr;
    const wchar_t* extension = nullptr;
    if (field_name == "isolated") {
        filter = L"Aligned wave audio (*.wav)\0*.wav\0\0";
    } else if (field_name == "input" || field_name == "project-relink") {
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
    } else if (field_name == "midi") {
        filter = L"MIDI files (*.mid)\0*.mid\0All files (*.*)\0*.*\0\0";
        extension = L"mid";
    } else if (field_name == "json") {
        filter = L"Score JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        extension = L"json";
    } else if (field_name == "project-open") {
        filter = L"Transcriber projects (*.transcriber;*.json)\0*.transcriber;*.json\0All files (*.*)\0*.*\0\0";
    } else {
        filter = L"Transcriber project (*.transcriber)\0*.transcriber\0All files (*.*)\0*.*\0\0";
        extension = L"transcriber";
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
                std::nullopt, original->second.midi_pitch,
                transcribe::EvidenceSemantics::Unspecified,
                transcribe::EvidenceSemantics::Unspecified, std::nullopt});
        } else {
            event.evidence.push_back({
                event.onset_seconds, note.midi_pitch, note.raw_strength, 0.0,
                std::nullopt, std::nullopt, note.midi_pitch,
                transcribe::EvidenceSemantics::Unspecified,
                transcribe::EvidenceSemantics::Unspecified, std::nullopt});
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
    } else if (request.instrument == transcribe::InstrumentPrior::Bass) {
        interpretation_options.instrument = "Bass";
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
    result.audio_sample_rate = sample_rate;
    result.audio_frame_count = recording_frames;
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
    message->status = from_utf8_lossy(text);
    if (PostMessageW(state.window, message_progress, 0,
                     reinterpret_cast<LPARAM>(message.get()))) {
        message.release();
    }
}

void perform_live_capture(
    AppState& state, const transcribe::gui::Request& request,
    std::stop_token stop_token, CompletionMessage& completion,
    const transcribe::gui::ProgressCallback& progress_callback,
    const std::shared_ptr<LiveBridgeState>& bridge) {
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
    live_options.events_available = [&state, bridge] {
        post_live_events_ready(state, bridge);
    };

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
        live_options.window.hop_seconds = request.live_hop_ms / 1000.0;
        live_options.window.left_guard_seconds =
            std::min(0.08, live_options.window.window_seconds / 8.0);
        live_options.window.right_guard_seconds =
            std::min(0.08, live_options.window.window_seconds / 8.0);
        if (live_options.window.window_seconds <
            live_options.window.hop_seconds +
                live_options.window.left_guard_seconds +
                live_options.window.right_guard_seconds) {
            throw std::invalid_argument(
                "The live hop interval is too large for this model's analysis window.");
        }
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
        if (bridge->cancel_requested.load(std::memory_order_acquire)) {
            completion.cancelled = true;
            throw std::runtime_error("Live capture cancelled.");
        }
        throw std::runtime_error("Capture was stopped before the input device opened.");
    }
    auto session = std::make_shared<LiveTranscriptionSession>(
        std::move(live_options), std::move(backend));
    {
        std::lock_guard lock(bridge->mutex);
        bridge->session = session;
    }
    const auto started = session->start();
    if (const auto* failure = std::get_if<InferenceError>(&started)) {
        throw std::runtime_error("Could not start live inference: " + failure->message);
    }
    std::stop_callback cancel_capture(stop_token, [session, bridge] {
        if (bridge->cancel_requested.load(std::memory_order_acquire)) {
            session->request_cancel();
        }
    });
    auto last_preview = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds(100);
    auto capture_options = request.asio_capture_options;
    capture_options.stream_opened = [&](std::uint32_t buffer_frames, std::uint64_t driver_frames) {
        {
            std::lock_guard lock(bridge->mutex);
            bridge->callback_period_ms = 1000.0 * buffer_frames / rate;
            bridge->driver_input_latency_ms = driver_frames
                ? 1000.0 * driver_frames / rate : -1;
        }
        post_live_capture_state(state, bridge,
            transcribe::live::bridge::CaptureState::Capturing,
            "ASIO stream opened");
    };
    const auto update = [&] {
        if (bridge->notification_failed.load(std::memory_order_acquire)) {
            throw std::runtime_error("The interface could not receive live events.");
        }
        session->pause_analysis(state.analysis_paused.load(std::memory_order_acquire));

        std::deque<std::vector<LiveEditCommand>> transactions;
        {
            std::lock_guard lock(state.live_command_mutex);
            transactions.swap(state.live_commands);
        }
        bool edited = false;
        {
            std::lock_guard bridge_lock(bridge->mutex);
            auto &document = bridge->document;
            auto &fingerings = bridge->fingerings;
            for (const auto &commands : transactions) {
                auto before = document;
                auto prior_fingerings = fingerings;
                const auto manual_upserts_before = bridge->manual_upserts.size();
                const auto manual_retractions_before = bridge->manual_retractions.size();
                const bool previously_edited = edited;
                try {
                    for (const auto &command : commands) {
                        if (command.kind == LiveEditKind::Fingering) {
                            if (command.string_number == 0 && command.fret == -1) {
                                fingerings.erase(command.note_id);
                                edited = true;
                                continue;
                            }
                            const auto note = document.note(command.note_id);
                            if (!note || command.string_number < 1 ||
                                static_cast<std::size_t>(command.string_number) >
                                    transcribe::configured_tuning(request.guitar_tab_options)
                                        .size() ||
                                command.fret < 0 ||
                                command.fret > request.guitar_tab_options.maximum_fret ||
                                (request.guitar_tab_options.search.physical_fret_count &&
                                 command.fret + request.guitar_tab_options.capo_fret >
                                     *request.guitar_tab_options.search.physical_fret_count) ||
                                transcribe::configured_tuning(
                                    request.guitar_tab_options)[static_cast<std::size_t>(
                                    command.string_number - 1)] +
                                        request.guitar_tab_options.capo_fret + command.fret !=
                                    note->midi_pitch) {
                                throw std::invalid_argument(
                                    "The selected string and fret do not match the note pitch.");
                            }
                            fingerings[command.note_id] = {command.string_number, command.fret};
                        } else if (command.kind == LiveEditKind::Delete) {
                            document.delete_manual_note(command.note_id, "Live editor deletion");
                            fingerings.erase(command.note_id);
                        } else {
                            if (command.midi_pitch < 0 || command.midi_pitch > 127 ||
                                !std::isfinite(command.onset_seconds) ||
                                !std::isfinite(command.duration_seconds) ||
                                command.onset_seconds < 0 || command.duration_seconds < 0.05 ||
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
                            note.onset_frame =
                                transcribe::recording_sample_position(command.onset_seconds, rate);
                            note.offset_frame = transcribe::recording_sample_position(
                                command.onset_seconds + command.duration_seconds, rate);
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
                    }
                    std::array<std::vector<std::pair<std::uint64_t, std::uint64_t>>, 6> occupied;
                    for (const auto &[id, position] : fingerings) {
                        const auto note = document.note(id);
                        if (!note || note->state == EventLifecycle::Rejected)
                            continue;
                        const auto [string, fret] = position;
                        if (string < 1 ||
                            static_cast<std::size_t>(string) >
                                transcribe::configured_tuning(request.guitar_tab_options).size() ||
                            fret < 0 || fret > request.guitar_tab_options.maximum_fret ||
                            (request.guitar_tab_options.search.physical_fret_count &&
                             fret + request.guitar_tab_options.capo_fret >
                                 *request.guitar_tab_options.search.physical_fret_count) ||
                            transcribe::configured_tuning(
                                request.guitar_tab_options)[static_cast<std::size_t>(string - 1)] +
                                    request.guitar_tab_options.capo_fret + fret !=
                                note->midi_pitch) {
                            throw std::invalid_argument(
                                "Live fingering no longer preserves pitch.");
                        }
                        occupied[static_cast<std::size_t>(string - 1)].push_back(
                            {note->onset_frame, note->offset_frame.value_or(
                                                    std::numeric_limits<std::uint64_t>::max())});
                    }
                    for (auto &string : occupied) {
                        std::sort(string.begin(), string.end());
                        for (std::size_t i = 1; i < string.size(); ++i) {
                            if (string[i].first < string[i - 1].second) {
                                throw std::invalid_argument(
                                    "Live edit assigns overlapping notes to the same string.");
                            }
                        }
                    }
                    for (const auto &command : commands) {
                        if (command.kind == LiveEditKind::Fingering)
                            continue;
                        bridge->manual_note_ids.insert(command.note_id);
                        if (const auto note = document.note(command.note_id)) {
                            if (note->state == EventLifecycle::Rejected) {
                                bridge->manual_retractions.emplace_back(note->id, note->revision);
                            } else {
                                bridge->manual_upserts.push_back(*note);
                            }
                        }
                    }
                } catch (const std::exception &error) {
                    document = std::move(before);
                    fingerings = std::move(prior_fingerings);
                    bridge->manual_upserts.resize(manual_upserts_before);
                    bridge->manual_retractions.resize(manual_retractions_before);
                    edited = previously_edited;
                    post_worker_status(state, std::string("Live edit error: ") + error.what());
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (edited)
            post_live_events_ready(state, bridge);
        if (now - last_preview >= std::chrono::milliseconds(150)) {
            const auto tail =
                session->recording_tail(static_cast<std::size_t>(rate) * 3);
            if (!tail.samples.empty()) {
                const auto preview = make_audio_preview(tail.samples, tail.sample_rate, 128, 40);
                const double offset =
                    std::max(0.0, static_cast<double>(session->ingested_frames()) / rate -
                                      preview.duration_seconds);
                auto message = std::make_unique<AudioPreviewMessage>();
                message->job = completion.job;
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

    post_worker_status(state, "Connecting to ASIO input");
    const auto capture = capture_asio_live(
        request.capture_seconds, *session, stop_token,
        [&](double progress) {
            if (progress_callback) {
                progress_callback(progress, "Capturing live audio");
            }
        },
        capture_options, update);
    const auto discard_queued_edits = [&state] {
        std::lock_guard lock(state.live_command_mutex);
        state.live_commands.clear();
    };
    if (bridge->cancel_requested.load(std::memory_order_acquire)) {
        discard_queued_edits();
        post_live_capture_state(state, bridge,
            transcribe::live::bridge::CaptureState::Finishing,
            "Discarding cancelled live capture");
        session->cancel();
    } else {
        post_worker_status(state, "Stabilizing live note events");
        update();
        post_live_capture_state(state, bridge,
            transcribe::live::bridge::CaptureState::Finishing,
            "Finalizing live note events");
        session->finish();
        if (bridge->cancel_requested.load(std::memory_order_acquire) &&
            session->state() != LiveState::Finished) {
            discard_queued_edits();
            session->cancel();
        }
    }
    post_live_events_ready(state, bridge);
    {
        std::unique_lock lock(bridge->mutex);
        while (!bridge->final_batch_applied &&
               !bridge->notification_failed.load(std::memory_order_acquire) &&
               !bridge->shutdown_requested.load(std::memory_order_acquire)) {
            bridge->drained.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!bridge->final_batch_applied) {
            throw std::runtime_error(
                bridge->notification_failed.load(std::memory_order_acquire)
                    ? "Could not notify the interface to drain final live events."
                    : "The interface closed before final live events were applied.");
        }
    }
    if (session->state() == LiveState::AnalysisFailed) {
        throw std::runtime_error("Live analysis failed: " + session->last_error());
    }
    bool cancelled = false;
    {
        std::lock_guard lock(bridge->mutex);
        cancelled = bridge->cancel_requested.load(std::memory_order_acquire) &&
                    session->state() != LiveState::Finished;
        if (!cancelled) {
            bridge->capture_state =
                transcribe::live::bridge::CaptureState::Finished;
            bridge->status = "Capture finished";
        } else {
            bridge->capture_state =
                transcribe::live::bridge::CaptureState::Finished;
            bridge->status =
                "Capture cancelled; captured notes were not accepted";
        }
    }
    if (cancelled) {
        completion.cancelled = true;
        post_live_capture_state(state, bridge,
            transcribe::live::bridge::CaptureState::Finished,
            "Capture cancelled; captured notes were not accepted");
        throw std::runtime_error("Live capture cancelled.");
    }
    post_live_capture_state(state, bridge,
        transcribe::live::bridge::CaptureState::Finished, "Capture finished");
    if (capture.input_lost || capture.reconnects > 0 || capture.blocks_refused > 0 || capture.null_input_frames > 0 ||
        capture.driver_overflows > 0) {
        std::ostringstream warning;
        warning << "Capture completed with input gaps";
        if (capture.reconnects) warning << " and " << capture.reconnects << " reconnect(s)";
        if (capture.blocks_refused) warning << "; " << capture.blocks_refused
                                            << " audio blocks were refused";
        if (capture.driver_overflows) warning << "; " << capture.driver_overflows
                                               << " driver overflow(s)";
        if (capture.null_input_frames) warning << "; " << capture.null_input_frames << " null-input frames";
        post_worker_status(state, warning.str());
    }

    completion.live_capture = true;
    completion.live_backend_id = backend_id;
    completion.live_recording = session->recording();
    std::unordered_map<std::string, std::pair<int, int>> fingerings;
    {
        std::lock_guard lock(bridge->mutex);
        completion.live_document = bridge->document;
        fingerings = bridge->fingerings;
    }
    completion.result = build_live_result(
        request, *completion.live_document, rate, completion.live_recording.samples.size(),
        fingerings);
    if (request.proposal_only) return;
    if (progress_callback) progress_callback(0.0, "Writing live score exports");
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
    write_output_atomic(
        request.json_path, serialize_score_document(completion.result.score) + '\n');
    if (progress_callback) progress_callback(1.0, "Live transcription complete");
}

void start_transcription(AppState& state,
                         const std::unordered_map<std::string, std::string>& fields) {
    if (state.busy) {
        return;
    }

    transcribe::gui::Request request;
    request.use_asio = field(fields, "mode") == "asio";
    request.retain_review_evidence =
        !request.use_asio && field(fields, "review_mode") == "1";
    request.bounded_inference = field(fields, "bounded_inference") == "1";
    if (request.bounded_inference && request.use_asio) {
        throw std::invalid_argument("Bounded neural windows apply to WAV files, not ASIO capture.");
    }
    request.input_path = from_utf8(field(fields, "input"));
    if (field(fields, "isolation_enabled") == "1") {
        if (request.use_asio || field(fields, "isolation_reviewed") != "1") {
            throw std::invalid_argument("Audition and confirm the isolated file before using offline preprocessing.");
        }
        request.isolated_audio_path = from_utf8(field(fields, "isolated"));
        if (request.isolated_audio_path.empty()) {
            throw std::invalid_argument("Choose an aligned isolated WAV or disable preprocessing.");
        }
        request.preprocessing_fallback = field(fields, "preprocessing_fallback") == "1";
    }
    request.proposal_only = state.last_result.has_value();
    request.model_path = from_utf8(field(fields, "model"));
    request.musicxml_path = from_utf8(field(fields, "musicxml"));
    request.tab_path = from_utf8(field(fields, "tab"));
    request.midi_path = from_utf8(field(fields, "midi"));
    request.json_path = from_utf8(field(fields, "json"));
    request.polyphonic = field(fields, "polyphonic") == "1";
    request.fast_preview = field(fields, "quality") == "fast";
    request.instrument = instrument_prior(field(fields, "instrument"));
    const auto tuning_text = field(fields, "tuning");
    if (tuning_text == "custom" && state.project) {
        request.guitar_tab_options.string_tuning = state.project->score.instrument.tuning;
        request.guitar_tab_options.search.physical_fret_count = state.project->score.instrument.physical_fret_count;
    } else {
        request.guitar_tab_options.tuning = guitar_tuning(tuning_text);
    }
    request.guitar_tab_options.manual_corrections =
        state.manual_fingering_corrections;
    request.preserve_expressive_timing = field(fields, "preserve_timing") == "1";
    const auto quantization = field(fields, "quantization");
    if (!quantization.empty()) request.interpretation_options.quantization_mode =
        transcribe::parse_quantization_mode(quantization);
    const auto strength = field(fields, "quantization_strength");
    if (!strength.empty()) {
        const double value = parse_number(strength, "quantization strength");
        if (value < 0.0 || value > 1.0) {
            throw std::invalid_argument("Quantization strength must be between 0 and 1.");
        }
        request.interpretation_options.quantization_strength = value;
    }
    const auto rhythm_evidence = field(fields, "minimum_rhythm_evidence");
    if (!rhythm_evidence.empty()) {
        const double value = parse_number(rhythm_evidence, "minimum rhythm evidence");
        if (value < 0.0 || value > 1.0) {
            throw std::invalid_argument("Minimum rhythm evidence must be between 0 and 1.");
        }
        request.interpretation_options.minimum_rhythm_evidence = value;
    }
    request.live_backend = field(fields, "live_backend");
    if (request.live_backend.empty()) request.live_backend = "yin";

    const auto tempo_text = field(fields, "tempo");
    request.tempo_automatic = tempo_text.empty() || tempo_text == "auto";
    if (!tempo_text.empty() && tempo_text != "auto") {
        const double tempo = parse_number(tempo_text, "tempo");
        if (tempo < 40.0 || tempo > 240.0) {
            throw std::invalid_argument("Tempo must be between 40 and 240 BPM.");
        }
        request.interpretation_options.tempo_bpm = tempo;
    }
    const auto key_text = field(fields, "key_fifths");
    request.key_automatic = key_text.empty() || key_text == "auto";
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
    request.meter_automatic = meter_text.empty() || meter_text == "auto";
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
    if (request.guitar_tab_options.search.physical_fret_count) {
        const int frets = *request.guitar_tab_options.search.physical_fret_count;
        if (frets < request.guitar_tab_options.capo_fret) {
            throw std::invalid_argument("Capo exceeds the project's physical fret count.");
        }
        request.guitar_tab_options.maximum_fret = frets - request.guitar_tab_options.capo_fret;
        request.guitar_tab_options.preferred_position = std::min(
            request.guitar_tab_options.preferred_position, request.guitar_tab_options.maximum_fret);
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
        if (request.live_backend != "yin" && fields.contains("live_hop_ms")) {
            request.live_hop_ms = parse_number(
                field(fields, "live_hop_ms"), "live hop interval");
            if (request.live_hop_ms < 10.0 || request.live_hop_ms > 1800.0) {
                throw std::invalid_argument(
                    "Live hop interval must be between 10 and 1800 milliseconds.");
            }
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
    if (request.musicxml_path.empty() || request.tab_path.empty() ||
        request.json_path.empty()) {
        throw std::invalid_argument(
            "Choose MusicXML, guitar tab, and score JSON output paths.");
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
    state.analysis_paused.store(false, std::memory_order_release);
    const auto job = ++state.next_transcription_job;
    state.active_transcription_job = job;
    state.transcription_cancelled = false;
    std::shared_ptr<LiveBridgeState> live_bridge;
    if (request.use_asio) {
        live_bridge = std::make_shared<LiveBridgeState>(
            request.asio_capture_options.sample_rate);
        live_bridge->job = job;
        state.live_bridge = live_bridge;
        post_live_status_json(state, job,
            transcribe::live::bridge::CaptureState::Arming,
            "Preparing ASIO input");
        std::lock_guard lock(state.live_command_mutex);
        state.live_commands.clear();
    } else {
        state.live_bridge.reset();
    }
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Starting analysis\"}");
    try {
        state.worker = std::jthread(
            [&state, job, request = std::move(request), live_bridge](std::stop_token stop_token) {
            auto completion = std::make_unique<CompletionMessage>();
            completion->job = job;
            completion->request = request;
            try {
                const auto progress_callback = [&state, job](double progress,
                                                        const std::string& status) {
                    auto message = std::make_unique<ProgressMessage>();
                    message->job = job;
                    message->percent = std::clamp(
                        static_cast<int>(progress * 100.0), 0, 100);
                    message->status = from_utf8(status);
                    post_coalesced(state, message_progress, std::move(message),
                        state.pending_progress, state.progress_posted);
                };
                const auto live_callback = [&state, job](
                                               double offset,
                                               const std::vector<transcribe::Note>& notes,
                                               const std::vector<transcribe::NoteEvent>& events) {
                    auto message = std::make_unique<LiveMessage>();
                    message->job = job;
                    message->offset = offset;
                    message->notes = notes;
                    message->events = events;
                    post_coalesced(state, message_live_preview, std::move(message),
                        state.pending_preview_notes, state.preview_notes_posted);
                };
                const auto audio_preview_callback =
                    [&state, job](double offset,
                             const transcribe::AudioPreviewData& preview,
                             bool replace) {
                        auto message = std::make_unique<AudioPreviewMessage>();
                        message->job = job;
                        message->offset = offset;
                        message->replace = replace;
                        message->preview = preview;
                        post_coalesced(state, message_audio_preview, std::move(message),
                            state.pending_audio_preview, state.audio_preview_posted);
                    };
                if (request.use_asio) {
                    perform_live_capture(state, request, stop_token, *completion,
                                         progress_callback, live_bridge);
                } else {
                    completion->result = transcribe::gui::run_transcription(
                        request, stop_token, progress_callback, live_callback,
                        audio_preview_callback);
                    if (stop_token.stop_requested()) {
                        throw std::runtime_error("Transcription cancelled.");
                    }
                }
                completion->success = true;
                if (!request.proposal_only) {
                    completion->project = make_project_document(
                        request, completion->result,
                        completion->live_capture
                            ? &completion->live_recording : nullptr,
                        completion->project_warning);
                    completion->has_project = true;
                }
            } catch (const std::exception& error) {
                completion->error = error.what();
                if (request.use_asio && live_bridge && completion->cancelled) {
                    try {
                        post_live_capture_state(state, live_bridge,
                            transcribe::live::bridge::CaptureState::Finished,
                            "Capture canceled; captured notes were not accepted");
                    } catch (const std::exception& status_error) {
                        completion->error += std::string("; status delivery failed: ") +
                                             status_error.what();
                    }
                } else if (request.use_asio && live_bridge &&
                           !completion->cancelled) {
                    try {
                        post_live_capture_state(state, live_bridge,
                            transcribe::live::bridge::CaptureState::Failed,
                            error.what());
                    } catch (const std::exception& status_error) {
                        completion->error += std::string("; status delivery failed: ") +
                                             status_error.what();
                    }
                }
            }
            if (PostMessageW(state.window, message_complete, 0,
                             reinterpret_cast<LPARAM>(completion.get()))) {
                completion.release();
            }
            });
    } catch (const std::exception& error) {
        if (live_bridge) {
            {
                std::lock_guard lock(live_bridge->mutex);
                live_bridge->capture_state =
                    transcribe::live::bridge::CaptureState::Failed;
                live_bridge->status = error.what();
            }
            post_live_status_json(state, job,
                transcribe::live::bridge::CaptureState::Failed, error.what());
            state.live_bridge.reset();
        }
        state.busy = false;
        state.active_transcription_job = 0;
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

void start_project_open(AppState& state, std::filesystem::path path) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (path.empty()) {
        throw std::invalid_argument("Choose a project file to open.");
    }
    state.busy = true;
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,"
                      L"\"status\":\"Opening project\"}");
    try {
        state.worker = std::jthread(
            [&state, path = std::move(path)](std::stop_token) {
                auto completion = std::make_unique<CompletionMessage>();
                completion->project_open = true;
                completion->project_path = path;
                try {
                    const auto autosave = project_autosave_path();
                    const bool is_recovery =
                        std::filesystem::absolute(path).lexically_normal() ==
                        std::filesystem::absolute(autosave).lexically_normal();
                    completion->project = transcribe::load_project_file(path);
                    completion->has_project = true;
                    auto request = request_for_project(completion->project, path);
                    auto result = result_for_project(completion->project, request);
                    completion->request = std::move(request);
                    completion->result = std::move(result);
                    const auto audio_path =
                        transcribe::resolve_project_audio_path(
                            completion->project, path);
                    if (!audio_path.empty()) {
                        std::error_code error;
                        if (!std::filesystem::is_regular_file(audio_path, error) ||
                            error) {
                            completion->project_warning =
                                "The referenced source audio is unavailable. "
                                "The score and its evidence were opened without audio. "
                                "Use Locate audio to relink the original recording, then Save.";
                        }
                    }
                    if (is_recovery) {
                        completion->project_recovery = true;
                        completion->project_path =
                            completion->project.last_saved_path.empty()
                                ? std::filesystem::path{}
                                : project_path_from_utf8(
                                      completion->project.last_saved_path);
                        if (!audio_path.empty()) {
                            completion->project.audio.policy = transcribe::ProjectAudioPolicy::ExternalReference;
                            completion->project.audio.reference_path =
                                path_utf8(audio_path);
                            completion->project.audio.relative_to_project = false;
                        }
                    } else {
                        completion->project.last_saved_path = path_utf8(path);
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

void create_new_project(AppState& state) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    GUID guid{};
    const auto guid_result = CoCreateGuid(&guid);
    if (FAILED(guid_result)) {
        throw std::system_error(static_cast<int>(guid_result),
                                std::system_category(),
                                "Could not create a project identity");
    }
    std::array<wchar_t, 40> guid_text{};
    if (StringFromGUID2(guid, guid_text.data(),
                        static_cast<int>(guid_text.size())) == 0) {
        throw std::runtime_error("Could not format a new project identity.");
    }
    std::wstring project_id = L"project-";
    project_id.append(guid_text.data());
    std::erase(project_id, L'{');
    std::erase(project_id, L'}');

    transcribe::ProjectDocument project;
    project.id = to_utf8(project_id);
    project.title = "Untitled project";
    project.score = transcribe::make_empty_score_document(
        "score-" + project.id);
    transcribe::gui::Request request;
    transcribe::gui::Result result;
    result.score = project.score;
    result.metadata = project.score.metadata;
    result.notes = transcribe::score_notes(project.score);
    result.fingering = transcribe::assign_guitar_tab_fingerings(
        result.notes, request.guitar_tab_options);

    state.project_path.clear();
    state.project = project;
    state.last_request = request;
    state.last_result = result;
    state.proposal_request.reset();
    state.proposal_result.reset();
    state.live_document.reset();
    state.live_recording = {};
    state.live_backend_id.clear();
    state.manual_fingering_corrections.clear();
    state.pre_refinement_result.reset();
    state.pre_refinement_document.reset();
    if (state.autosaver) state.autosaver->submit(project, 0);

    post_json(state, L"{\"type\":\"project-reset\",\"project\":" +
        project_summary_json(project, {}) + L",\"notes\":" +
        score_notes_json(project.score) + L",\"metadata\":" +
        score_metadata_json(project.score) + L",\"fingering\":" +
        fingering_json(result.fingering) + L"}");
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

std::vector<transcribe::RecordReviewDecision> parse_review_decisions(
    std::string_view encoded) {
    std::vector<transcribe::RecordReviewDecision> decisions;
    std::unordered_set<std::string> suggestion_ids;
    while (!encoded.empty()) {
        const auto end = encoded.find(';');
        const auto record = encoded.substr(0, end);
        const auto first = record.find(',');
        const auto second = first == std::string_view::npos
            ? first : record.find(',', first + 1);
        if (first == std::string_view::npos ||
            second == std::string_view::npos ||
            record.find(',', second + 1) != std::string_view::npos) {
            throw std::invalid_argument("A review decision is malformed.");
        }
        std::string suggestion_id(record.substr(0, first));
        std::string note_id(record.substr(first + 1, second - first - 1));
        const auto outcome = record.substr(second + 1);
        if (suggestion_id.empty() || note_id.empty() ||
            suggestion_id.size() > 256 || note_id.size() > 256 ||
            !suggestion_ids.insert(suggestion_id).second ||
            (outcome != "accepted" && outcome != "rejected")) {
            throw std::invalid_argument("A review decision has invalid or duplicate values.");
        }
        decisions.push_back({
            std::move(suggestion_id), std::move(note_id),
            outcome == "accepted"
                ? transcribe::ReviewDecisionOutcome::Accepted
                : transcribe::ReviewDecisionOutcome::Rejected,
            outcome == "accepted" ? "Accepted review suggestion"
                                  : "Dismissed review suggestion"});
        if (decisions.size() > 10000) {
            throw std::length_error("Too many review decisions in one edit.");
        }
        if (end == std::string_view::npos) {
            encoded = {};
        } else {
            encoded.remove_prefix(end + 1);
        }
    }
    return decisions;
}

std::vector<transcribe::GuitarTabPosition> parse_tab_positions(
    std::string_view encoded) {
    std::vector<transcribe::GuitarTabPosition> positions;
    std::unordered_set<std::size_t> indices;
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
            string_number < 1.0 || string_number > 12.0 ||
            std::floor(string_number) != string_number || fret < 0.0 ||
            fret > 36.0 || std::floor(fret) != fret || note_index > 9999) {
            throw std::invalid_argument("A guitar string/fret edit is outside supported ranges.");
        }
        positions.push_back({
            static_cast<std::size_t>(note_index),
            static_cast<int>(string_number),
            static_cast<int>(fret),
            true});
        if (!indices.insert(static_cast<std::size_t>(note_index)).second) {
            throw std::invalid_argument("A guitar string/fret edit repeats a note index.");
        }
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

enum class ScoreWriteMode { Export, SaveProject, SaveCopy, Autosave };

void stop_native_playback(AppState& state, bool ended = false);

void export_edited_score(
    AppState& state,
    const std::unordered_map<std::string, std::string>& fields,
    ScoreWriteMode mode) {
    if (state.busy) {
        throw std::runtime_error("Wait for the current operation to finish.");
    }
    if (!state.last_request || !state.last_result) {
        throw std::runtime_error("Transcribe audio before exporting score edits.");
    }

    auto request = *state.last_request;
    if (fields.contains("quantization"))
        request.interpretation_options.quantization_mode =
            transcribe::parse_quantization_mode(field(fields, "quantization"));
    if (fields.contains("quantization_strength"))
        request.interpretation_options.quantization_strength =
            parse_number(field(fields, "quantization_strength"), "quantization strength");
    if (fields.contains("minimum_rhythm_evidence"))
        request.interpretation_options.minimum_rhythm_evidence =
            parse_number(field(fields, "minimum_rhythm_evidence"), "minimum rhythm evidence");
    if (fields.contains("retain_review_evidence"))
        request.retain_review_evidence = field(fields, "retain_review_evidence") == "1";
    if (fields.contains("bounded_inference"))
        request.bounded_inference = field(fields, "bounded_inference") == "1";
    const bool copy_audio = mode == ScoreWriteMode::SaveProject &&
        field(fields, "copy_audio") == "1";
    if (fields.contains("model")) {
        request.model_path = from_utf8(field(fields, "model"));
    }
    if (fields.contains("instrument")) {
        request.instrument = instrument_prior(field(fields, "instrument"));
    }
    if (fields.contains("quality")) {
        const auto quality = field(fields, "quality");
        if (quality != "fast" && quality != "quality") {
            throw std::invalid_argument("Choose a supported analysis quality mode.");
        }
        request.fast_preview = quality == "fast";
    }
    if (fields.contains("live_backend")) {
        const auto backend = field(fields, "live_backend");
        if (backend != "yin" && backend != "spectral" && backend != "onnx") {
            throw std::invalid_argument("Choose a supported live inference backend.");
        }
        request.live_backend = backend;
    }
    if (fields.contains("polyphonic")) {
        request.polyphonic = field(fields, "polyphonic") == "1";
    }
    if (fields.contains("preserve_timing")) {
        request.preserve_expressive_timing =
            field(fields, "preserve_timing") == "1";
    }
    if (fields.contains("latency_ms")) {
        const double latency = parse_number(
            field(fields, "latency_ms"), "analysis latency");
        if (latency < 20.0 || latency > 50.0) {
            throw std::invalid_argument(
                "Realtime analysis latency must be between 20 and 50 milliseconds.");
        }
        request.realtime_options.latency_ms = latency;
    }
    std::filesystem::path project_destination;
    if (mode == ScoreWriteMode::Export) {
        const auto musicxml_path = field(fields, "musicxml");
        const auto tab_path = field(fields, "tab");
        const auto midi_path = field(fields, "midi");
        const auto json_path = field(fields, "json");
        if (!musicxml_path.empty()) request.musicxml_path = from_utf8(musicxml_path);
        if (!tab_path.empty()) request.tab_path = from_utf8(tab_path);
        if (!midi_path.empty()) request.midi_path = from_utf8(midi_path);
        if (!json_path.empty()) request.json_path = from_utf8(json_path);
    } else if (mode == ScoreWriteMode::SaveProject) {
        const auto requested_path = field(fields, "project_path");
        project_destination = requested_path.empty()
            ? state.project_path : std::filesystem::path(from_utf8(requested_path));
        if (project_destination.empty()) {
            throw std::invalid_argument("Choose a project file path before saving.");
        }
    } else if (mode == ScoreWriteMode::SaveCopy) {
        project_destination =
            std::filesystem::path(from_utf8(field(fields, "project_path")));
        if (project_destination.empty()) {
            throw std::invalid_argument("Choose a path for the score-only project copy.");
        }
    }
    if (!request.isolated_audio_path.empty()) {
        const std::array outputs{
            request.musicxml_path, request.tab_path, request.midi_path,
            request.json_path, project_destination};
        transcribe::protect_preprocessing_inputs(
            {request.input_path, request.isolated_audio_path, "", false}, outputs);
    }
    std::uint64_t edit_generation = 0;
    const auto generation_text = field(fields, "generation");
    if (!generation_text.empty()) {
        std::size_t generation_characters = 0;
        edit_generation = std::stoull(generation_text, &generation_characters);
        if (generation_characters != generation_text.size()) {
            throw std::invalid_argument("Project edit generation is invalid.");
        }
    }
    const auto revision_text = field(fields, "base_revision");
    std::size_t revision_characters = 0;
    const auto base_revision = std::stoull(revision_text, &revision_characters);
    if (revision_characters != revision_text.size() ||
        base_revision != state.last_result->score.revision ||
        (!field(fields, "score_id").empty() &&
         field(fields, "score_id") != state.last_result->score.id)) {
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
    for (auto& decision : parse_review_decisions(
             field(fields, "review_decisions"))) {
        transaction.commands.emplace_back(std::move(decision));
    }

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
    if (!tuning_text.empty() && tuning_text != "custom") {
        request.guitar_tab_options.tuning = guitar_tuning(tuning_text);
        request.guitar_tab_options.string_tuning.clear();
    } else if (tuning_text == "custom") {
        const auto tuning_pitches = field(fields, "tuning_pitches");
        if (!tuning_pitches.empty()) {
            std::vector<int> pitches;
            std::size_t start = 0;
            while (start <= tuning_pitches.size()) {
                const auto end = tuning_pitches.find(',', start);
                const auto pitch_text = tuning_pitches.substr(
                    start, end == std::string::npos
                        ? std::string::npos : end - start);
                std::size_t consumed = 0;
                const int pitch = std::stoi(pitch_text, &consumed);
                if (consumed != pitch_text.size() || pitch < 0 || pitch > 127 ||
                    pitches.size() >= 12) {
                    throw std::invalid_argument(
                        "Custom tuning pitches must be MIDI notes from 0 to 127, with at most 12 strings.");
                }
                pitches.push_back(pitch);
                if (end == std::string::npos) break;
                start = end + 1;
            }
            if (pitches.empty()) {
                throw std::invalid_argument("Custom tuning must contain at least one string.");
            }
            request.guitar_tab_options.string_tuning = std::move(pitches);
        }
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
        request.instrument == transcribe::InstrumentPrior::Bass ? "Bass" :
        source_score.instrument.name;
    instrument.written_transposition_semitones = transposition;
    const auto configured = transcribe::configured_tuning(request.guitar_tab_options);
    instrument.tuning.assign(configured.begin(), configured.end());
    instrument.capo_fret = request.guitar_tab_options.capo_fret;
    transaction.commands.emplace_back(transcribe::UpdateScoreContext{
        metadata, instrument, pickup, tempo_changed});
    auto editing_timebase = source_score;
    if (tempo_changed) {
        editing_timebase.metadata = metadata;
        editing_timebase.tempo_map = {{0, 0, 0, metadata.tempo_bpm, 1, false}};
    }

    for (std::size_t index = 0; index < edited_notes.size(); ++index) {
        const auto& edited = edited_notes[index];
        if (!edited_ids.insert(edited.id).second) {
            throw std::invalid_argument("Edited score contains a duplicate note ID.");
        }
        const double onset_quarters =
            transcribe::score_position_quarters(editing_timebase, edited.note.start_seconds);
        const double end_quarters = transcribe::score_position_quarters(
            editing_timebase, edited.note.start_seconds + edited.note.duration_seconds);
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
    const bool explicit_assignments = fields.contains("tab_assignments");
    if (explicit_assignments) {
        const auto assignments = parse_tab_positions(field(fields, "tab_assignments"));
        for (std::size_t index = 0; index < edited_notes.size(); ++index) {
            const auto position = std::find_if(assignments.begin(), assignments.end(),
                [index](const auto& item) { return item.note_index == index; });
            transaction.commands.emplace_back(transcribe::ChangeScoreFingering{
                edited_notes[index].id,
                position == assignments.end() ? std::nullopt : std::optional(position->string_number),
                position == assignments.end() ? std::nullopt : std::optional(position->fret)});
        }
        for (const auto& position : assignments) {
            if (position.note_index >= edited_notes.size()) {
                throw std::invalid_argument("Tab assignment refers to a missing note.");
            }
        }
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
            (!explicit_assignments || manual_fingering_ids.contains(note.id)) &&
            tab->string_number && tab->fret) {
            request.guitar_tab_options.position_overrides.push_back(
                {index, *tab->string_number, *tab->fret, true});
        }
    }
    if (!explicit_assignments) {
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
    } else {
        result.fingering = {};
        result.fingering.fully_playable = true;
        result.fingering.explanation = "Accepted editor assignments; performed fingering unknown.";
        for (std::size_t index = 0; index < result.score.notes.size(); ++index) {
            const auto& note = result.score.notes[index];
            auto tab = std::find_if(result.score.tab_notes.begin(), result.score.tab_notes.end(),
                [&](const auto& item) { return item.notation_note_id == note.id; });
            transcribe::GuitarTabPosition position{index, 0, -1, false, std::nullopt};
            if (tab != result.score.tab_notes.end()) {
                tab->manually_corrected = manual_fingering_ids.contains(note.id);
                if (tab->string_number && tab->fret) {
                    position.string_number = *tab->string_number;
                    position.fret = *tab->fret;
                    position.manually_corrected = tab->manually_corrected;
                }
            }
            result.fingering.positions.push_back(position);
            transcribe::FingeringNoteResult detail;
            detail.position = position;
            detail.playable = position.string_number > 0;
            if (!detail.playable) {
                detail.explanation = "Unresolved editor assignment; select a pitch-compatible position.";
                result.fingering.fully_playable = false;
            }
            result.fingering.notes.push_back(std::move(detail));
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

    std::string project_creation_warning;
    transcribe::ProjectDocument project = state.project
        ? *state.project
        : make_project_document(request, *state.last_result, nullptr,
                                project_creation_warning);
    const bool alternative_context_changed =
        source_score.instrument.tuning != result.score.instrument.tuning ||
        source_score.instrument.capo_fret != result.score.instrument.capo_fret ||
        source_score.notes.size() != result.score.notes.size() ||
        !std::equal(source_score.notes.begin(), source_score.notes.end(),
            result.score.notes.begin(), result.score.notes.end(), [](const auto& a, const auto& b) {
                return a.id == b.id && a.sounding_midi_pitch == b.sounding_midi_pitch &&
                    a.onset_quarters == b.onset_quarters && a.duration_quarters == b.duration_quarters;
            });
    if (alternative_context_changed) {
        project.fingering_alternatives.clear();
        project.fingering_alternatives_truncated = false;
    }
    project.score = result.score;
    for (auto& annotation : project.annotations) {
        if (!annotation.note_id.empty() && !edited_ids.contains(annotation.note_id))
            annotation.note_id.clear();
    }
    if (fields.contains("annotation_text")) {
        const auto annotation = std::find_if(project.annotations.begin(), project.annotations.end(),
            [](const auto& item) { return item.id == "project-notes"; });
        const auto text = field(fields, "annotation_text");
        if (annotation != project.annotations.end()) annotation->text = text;
        else if (!text.empty()) project.annotations.push_back({"project-notes", {}, text});
    }
    project.audio.sample_rate = result.audio_sample_rate;
    project.audio.channels = result.audio_channels;
    project.audio.frame_count = result.audio_frame_count;
    project.analysis.backend_identifier = request.model_path.empty()
        ? (result.score.source_id == "live-capture"
               ? "builtin:" + request.live_backend : "builtin:yin")
        : "onnx-runtime";
    project.analysis.model_identifier = request.model_path.empty()
        ? project.analysis.backend_identifier
        : path_utf8(request.model_path.filename());
    project.analysis.model_reference = path_utf8(request.model_path);
    project.analysis.instrument_prior = instrument_prior_name(request.instrument);
    project.analysis.live_backend = request.live_backend;
    if (fields.contains("tempo_auto")) {
        request.tempo_automatic = field(fields, "tempo_auto") == "1";
    }
    if (fields.contains("meter_auto")) {
        request.meter_automatic = field(fields, "meter_auto") == "1";
    }
    if (fields.contains("key_auto")) {
        request.key_automatic = field(fields, "key_auto") == "1";
    }
    project.analysis.tempo_automatic = request.tempo_automatic;
    project.analysis.meter_automatic = request.meter_automatic;
    project.analysis.key_automatic = request.key_automatic;
    project.analysis.polyphonic = request.polyphonic;
    project.analysis.fast_preview = request.fast_preview;
    project.analysis.preserve_expressive_timing =
        request.preserve_expressive_timing;
    project.analysis.latency_ms = request.realtime_options.latency_ms;
    capture_project_settings(project, request);
    if (!field(fields, "relink_audio").empty()) {
        if (mode != ScoreWriteMode::Autosave)
            throw std::invalid_argument("Relink is a separate project operation.");
        transcribe::relink_project_audio(project, state.project_path,
            std::filesystem::path(from_utf8(field(fields, "relink_audio"))));
        request.input_path = transcribe::resolve_project_audio_path(project, state.project_path);
        request.use_asio = false;
        request.isolated_audio_path.clear();
        stop_native_playback(state);
    }

    if (mode == ScoreWriteMode::Autosave) {
        transcribe::validate_project_document(project);
        state.last_result = result;
        state.last_request = request;
        state.project = project;
        queue_project_autosave(state, project, edit_generation);
        post_json(state, L"{\"type\":\"project-autosaving\","
            L"\"score_revision\":" + std::to_wstring(result.score.revision) +
            L",\"generation\":" + std::to_wstring(edit_generation) + L"}");
        if (!field(fields, "relink_audio").empty())
            post_json(state, L"{\"type\":\"project-relinked\",\"project\":" +
                project_summary_json(project, state.project_path) + L"}");
        return;
    }

    std::filesystem::path audio_copy_source;
    if (mode == ScoreWriteMode::SaveProject ||
        mode == ScoreWriteMode::SaveCopy) {
        const bool score_only = mode == ScoreWriteMode::SaveCopy;
        const auto current_audio_path = transcribe::resolve_project_audio_path(
            project, state.project_path.empty()
                ? (state.autosaver ? state.autosaver->path()
                                   : std::filesystem::path{})
                : state.project_path);
        const std::array project_outputs{project_destination};
        transcribe::protect_preprocessing_inputs(
            {current_audio_path, request.model_path, "", false}, project_outputs);
        audio_copy_source = !request.input_path.empty() ? request.input_path : current_audio_path;
        if (score_only) {
            project.audio.policy = transcribe::ProjectAudioPolicy::ScoreOnly;
            project.audio.reference_path.clear();
            project.audio.relative_to_project = false;
            project.audio.original_reference.clear();
        } else if (request.use_asio && !state.live_recording.samples.empty()) {
            project.audio.policy =
                transcribe::ProjectAudioPolicy::ExternalReference;
            project.audio.reference_path = path_utf8(
                project_destination.stem().wstring() + L".audio.wav");
            project.audio.relative_to_project = true;
        } else if (project.audio.policy == transcribe::ProjectAudioPolicy::ProjectCopy &&
                   project_destination == state.project_path && !copy_audio) {
            // A normal save keeps the existing immutable, project-owned asset.
        } else if (!request.input_path.empty()) {
            set_project_audio_reference(
                project, request.input_path, project_destination);
        } else if (!current_audio_path.empty()) {
            set_project_audio_reference(
                project, current_audio_path, project_destination);
        } else {
            project.audio.policy = transcribe::ProjectAudioPolicy::ScoreOnly;
            project.audio.reference_path.clear();
            project.audio.relative_to_project = false;
        }
        project.title = path_utf8(project_destination.stem());
        if (project.title.empty()) project.title = "Transcriber project";
        project.last_saved_path = path_utf8(project_destination);
        transcribe::validate_project_document(project);
    }

    if (mode == ScoreWriteMode::Export) {
        update_export_status(project, "musicxml", request.musicxml_path, true);
        update_export_status(project, "tablature", request.tab_path, true);
        update_export_status(project, "midi", request.midi_path, true);
        update_export_status(project, "json", request.json_path, true);
    }
    transcribe::validate_project_document(project);

    state.busy = true;
    post_json(state, mode == ScoreWriteMode::Export
        ? L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Exporting edited score\"}"
        : L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Saving project\"}");
    try {
        state.worker = std::jthread(
            [&state, request = std::move(request), result = std::move(result),
             project = std::move(project), project_destination,
             copy_audio, audio_copy_source,
             saved_corrections = std::move(saved_corrections),
             mode, edit_generation](std::stop_token) mutable {
                auto message = std::make_unique<CompletionMessage>();
                message->export_only = true;
                message->project_save = mode == ScoreWriteMode::SaveProject;
                message->score_only_copy = mode == ScoreWriteMode::SaveCopy;
                message->edit_generation = edit_generation;
                message->request = request;
                message->project_path = project_destination;
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
                std::string current_format;
                std::filesystem::path current_output;
                try {
                    if (mode == ScoreWriteMode::SaveProject ||
                        mode == ScoreWriteMode::SaveCopy) {
                        if (mode == ScoreWriteMode::SaveProject &&
                            request.use_asio &&
                            !state.live_recording.samples.empty()) {
                            report_export_progress(25, L"Saving linked audio");
                            project = transcribe::save_project_with_live_audio(
                                project_destination, std::move(project), state.live_recording);
                        } else if (copy_audio) {
                            report_export_progress(30, L"Copying recording into project directory");
                            project = transcribe::save_project_with_audio_copy(
                                project_destination, std::move(project), audio_copy_source);
                            request.input_path = transcribe::resolve_project_audio_path(
                                project, project_destination);
                            request.use_asio = false;
                            message->request = request;
                        } else {
                            report_export_progress(70, L"Saving project");
                            transcribe::save_project_file_atomic(project_destination, project);
                        }
                    } else {
                        current_format = "musicxml";
                        current_output = request.musicxml_path;
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
                        update_export_status(
                            project, current_format, current_output, true);
                        current_format = "tablature";
                        current_output = request.tab_path;
                        report_export_progress(55, L"Writing guitar tablature");
                        transcribe::write_guitar_tab(
                            request.tab_path, result.score,
                            request.guitar_tab_options);
                        update_export_status(
                            project, current_format, current_output, true);
                        if (!request.midi_path.empty()) {
                            current_format = "midi";
                            current_output = request.midi_path;
                            report_export_progress(80, L"Writing MIDI");
                            transcribe::write_midi(request.midi_path, result.score);
                            update_export_status(
                                project, current_format, current_output, true);
                        }
                        current_format = "json";
                        current_output = request.json_path;
                        report_export_progress(95, L"Writing accepted score JSON");
                        transcribe::write_output_atomic(
                            request.json_path,
                            transcribe::serialize_score_document(result.score) + '\n');
                        update_export_status(
                            project, current_format, current_output, true);
                        report_export_progress(100, L"Export complete");
                    }
                    message->request.guitar_tab_options.manual_corrections =
                        std::move(saved_corrections);
                    message->result = std::move(result);
                    message->project = std::move(project);
                    message->has_project = true;
                    message->success = true;
                } catch (const std::exception& error) {
                    message->error = error.what();
                    if (!current_format.empty()) {
                        update_export_status(project, current_format,
                                             current_output, false, error.what());
                        message->project = std::move(project);
                        message->has_project = true;
                    }
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
                    report_progress(90, L"Writing accepted score JSON");
                    transcribe::write_output_atomic(
                        request.json_path,
                        transcribe::serialize_score_document(result.score) + '\n');
                    message->project = make_project_document(
                        request, result,
                        live_capture ? &live_recording : nullptr,
                        message->project_warning);
                    message->has_project = true;
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
                        message->status = L"Refining recording";
                        if (PostMessageW(state.window, message_progress, 0,
                                         reinterpret_cast<LPARAM>(message.get()))) {
                            message.release();
                        }
                    };
                    options.decoding.minimum_duration_seconds = 0.055;
                    if (backend_id.starts_with("onnx:")) {
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
                    progress(90, L"Writing accepted score JSON");
                    transcribe::write_output_atomic(
                        request.json_path,
                        transcribe::serialize_score_document(
                            completion->result.score) + '\n');
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
                    transcribe::write_output_atomic(
                        request.json_path,
                        transcribe::serialize_score_document(prior_result.score) + '\n');
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

constexpr UINT_PTR playback_timer = 0x5450;

void stop_native_playback(AppState& state, bool ended) {
    KillTimer(state.window, playback_timer);
    PlaySoundW(nullptr, nullptr, 0);
    const bool notify = state.original_playback;
    state.original_playback = false;
    if (state.review_audio_open) {
        const auto error = mciSendStringW(L"close transcriber_review_audio", nullptr, 0, nullptr);
        state.review_audio_open = false;
        if (error) post_error(state, "Windows could not close WAV playback.");
    }
    if (notify) {
        post_json(state, L"{\"type\":\"playback-state\",\"state\":\"" +
            std::wstring(ended ? L"ended" : L"stopped") + L"\",\"token\":" +
            std::to_wstring(state.playback_token) + L"}");
    }
}

void poll_native_playback(AppState& state) {
    if (!state.original_playback) return;
    std::array<wchar_t, 64> position{}, mode{};
    const auto position_error = mciSendStringW(
        L"status transcriber_review_audio position", position.data(),
        static_cast<UINT>(position.size()), nullptr);
    const auto mode_error = mciSendStringW(
        L"status transcriber_review_audio mode", mode.data(),
        static_cast<UINT>(mode.size()), nullptr);
    if (position_error || mode_error) {
        stop_native_playback(state);
        post_error(state, "Windows could not query WAV playback position.");
        return;
    }
    if (std::wstring_view(mode.data()) != L"playing") {
        stop_native_playback(state, true);
        return;
    }
    post_json(state, L"{\"type\":\"playback-state\",\"state\":\"playing\",\"token\":" +
        std::to_wstring(state.playback_token) + L",\"seconds\":" +
        std::to_wstring(std::wcstod(position.data(), nullptr) / 1000.0) + L"}");
}

void handle_web_message(AppState& state, const std::string& message) {
    constexpr std::size_t maximum_message_bytes = 16 * 1024 * 1024;
    if (message.empty() || message.size() > maximum_message_bytes) {
        throw std::invalid_argument("The interface message is empty or exceeds the size limit.");
    }
    const auto separator = message.find('|');
    const std::string action = message.substr(0, separator);
    if (!transcribe::live::bridge::operation_allowed(action)) {
        throw std::invalid_argument("The interface requested an unsupported operation.");
    }
    const std::string query = separator == std::string::npos
                                  ? std::string{}
                                  : message.substr(separator + 1);
    const auto fields = parse_fields(query);
    if (action == "start-capture" || action == "stop-capture" ||
        action == "cancel") {
        const auto version = field(fields, "version");
        if (version != std::to_string(
                transcribe::live::bridge::protocol_version)) {
            throw std::invalid_argument(
                "The capture operation uses an unsupported protocol version.");
        }
    }

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
        post_live_status_json(state, 0,
            transcribe::live::bridge::CaptureState::Idle, "Ready");
        const auto recovery_path = project_autosave_path();
        std::error_code recovery_error;
        if (std::filesystem::is_regular_file(recovery_path, recovery_error) &&
            !recovery_error) {
            post_json(state, L"{\"type\":\"recovery-available\",\"path\":\"" +
                json_escape(recovery_path.wstring()) + L"\"}");
        }
#ifdef TRANSCRIBE_ENABLE_ASIO
        post_json(state, asio_devices_json());
#endif
    } else if (action == "browse") {
        const std::string field_name = field(fields, "field");
        browse_for_path(state, field_name, from_utf8(field(fields, "current")));
    } else if (action == "transcribe") {
        stop_native_playback(state);
        start_transcription(state, fields);
    } else if (action == "start-capture") {
        if (field(fields, "mode") != "asio") {
            post_live_status_json(state, 0,
                transcribe::live::bridge::CaptureState::Failed,
                "Capture requests must select ASIO input.");
            throw std::invalid_argument("Capture requests must select ASIO input.");
        }
        try {
            stop_native_playback(state);
            start_transcription(state, fields);
        } catch (const std::exception& error) {
            post_live_status_json(state, 0,
                transcribe::live::bridge::CaptureState::Failed, error.what());
            throw;
        }
    } else if (action == "stop-capture") {
        if (!state.busy || !state.active_asio ||
            !state.worker.joinable() || !state.live_bridge) {
            post_live_status_json(state, 0,
                transcribe::live::bridge::CaptureState::Idle,
                "No live capture is active");
            return;
        }
        bool capture_complete = false;
        transcribe::live::bridge::CaptureState capture_state;
        std::string capture_status;
        {
            std::lock_guard lock(state.live_bridge->mutex);
            capture_state = state.live_bridge->capture_state;
            capture_status = state.live_bridge->status;
            capture_complete =
                capture_state == transcribe::live::bridge::CaptureState::Finished ||
                capture_state == transcribe::live::bridge::CaptureState::Failed;
        }
        if (capture_complete) {
            post_live_status_json(state, state.live_bridge->job,
                                  capture_state, capture_status);
            return;
        }
        post_live_capture_state(state, state.live_bridge,
            transcribe::live::bridge::CaptureState::Finishing,
            "Stopping capture and finalizing notes");
        state.worker.request_stop();
        post_json(state,
            L"{\"type\":\"progress\",\"status\":\"Stopping capture and finalizing notes\"}");
    } else if (action == "export-edits") {
        export_edited_score(state, fields, ScoreWriteMode::Export);
    } else if (action == "new-project") {
        stop_native_playback(state);
        create_new_project(state);
    } else if (action == "open-project") {
        stop_native_playback(state);
        start_project_open(
            state, std::filesystem::path(from_utf8(field(fields, "path"))));
    } else if (action == "save-project") {
        export_edited_score(state, fields, ScoreWriteMode::SaveProject);
    } else if (action == "save-score-copy") {
        export_edited_score(state, fields, ScoreWriteMode::SaveCopy);
    } else if (action == "autosave-project") {
        export_edited_score(state, fields, ScoreWriteMode::Autosave);
    } else if (action == "discard-recovery") {
        if (state.autosaver) {
            state.autosaver->clear();
        }
        post_json(state, L"{\"type\":\"recovery-discarded\"}");
    } else if (action == "accept-proposal") {
        accept_proposal(state);
    } else if (action == "pause-analysis") {
        if (!state.busy || !state.active_asio)
            throw std::runtime_error("Analysis pause is available only during live capture.");
        const auto paused = field(fields, "paused");
        if (paused != "0" && paused != "1")
            throw std::invalid_argument("Analysis pause requires paused=0 or 1.");
        state.analysis_paused.store(paused == "1", std::memory_order_release);
        post_json(state, L"{\"type\":\"analysis-paused\",\"paused\":" +
            std::wstring(paused == "1" ? L"true" : L"false") + L"}");
    } else if (action == "live-edit") {
        if (!state.busy || !state.active_asio) {
            throw std::runtime_error("Live note editing is available only during capture.");
        }
        const auto bridge = state.live_bridge;
        if (!bridge) {
            throw std::runtime_error("The live capture document is unavailable.");
        }
        {
            std::lock_guard lock(bridge->mutex);
            if (bridge->capture_state !=
                    transcribe::live::bridge::CaptureState::Capturing &&
                bridge->capture_state !=
                    transcribe::live::bridge::CaptureState::Interrupted) {
                throw std::runtime_error(
                    "Live note editing is unavailable while capture is finalizing.");
            }
        }
        std::vector<std::unordered_map<std::string, std::string>> edits;
        if (fields.contains("batch")) {
            const auto payload = field(fields, "batch");
            std::string_view encoded = payload;
            while (!encoded.empty()) {
                const auto end = encoded.find(';');
                edits.push_back(parse_fields(encoded.substr(0, end)));
                if (edits.size() > 256) throw std::length_error("Live edit transaction exceeds 256 commands.");
                if (end == std::string_view::npos) break;
                encoded.remove_prefix(end + 1);
            }
        } else {
            edits.push_back(fields);
        }
        if (edits.empty()) throw std::invalid_argument("Live edit transaction is empty.");
        std::vector<LiveEditCommand> transaction;
        for (const auto& edit : edits) {
        LiveEditCommand command;
        command.note_id = field(edit, "note_id");
        const auto kind = field(edit, "kind");
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
            const auto pitch = field(edit, "midi");
            std::size_t pitch_characters = 0;
            command.midi_pitch = std::stoi(pitch, &pitch_characters);
            if (pitch_characters != pitch.size()) {
                throw std::invalid_argument("Invalid MIDI pitch in live edit.");
            }
            command.onset_seconds = parse_number(field(edit, "start"), "note onset");
            command.duration_seconds =
                parse_number(field(edit, "duration"), "note duration");
        } else if (command.kind == LiveEditKind::Fingering) {
            const auto string_text = field(edit, "string");
            const auto fret_text = field(edit, "fret");
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
        transaction.push_back(std::move(command));
        }
        std::lock_guard lock(state.live_command_mutex);
        std::size_t queued = transaction.size();
        for (const auto& batch : state.live_commands) queued += batch.size();
        if (queued > 256) {
            throw std::runtime_error(
                "The live edit queue is full; wait for pending edits to apply.");
        }
        state.live_commands.push_back(std::move(transaction));
    } else if (action == "refine-live") {
        start_live_refinement(state);
    } else if (action == "apply-refinement") {
        apply_live_refinement(state, fields);
    } else if (action == "discard-refinement") {
        discard_live_refinement(state);
    } else if (action == "undo-refinement") {
        undo_live_refinement(state);
    } else if (action == "audition-event" || action == "audition-source") {
        if (state.busy) throw std::runtime_error("Wait for audio processing to finish before auditioning.");
        const bool preview_source = action == "audition-source";
        if (!preview_source && (!state.last_request || state.last_request->use_asio ||
            state.last_request->input_path.empty())) {
            throw std::runtime_error(
                "Event audition is available after transcribing a file.");
        }
        const auto audition_path = preview_source
            ? std::filesystem::path(from_utf8(field(fields, "path"))) : state.last_request->input_path;
        if (audition_path.empty() || audition_path.wstring().find_first_of(L"\"\r\n") != std::wstring::npos) {
            throw std::invalid_argument("Choose a valid local WAV path for audition.");
        }
        auto extension = audition_path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
        if (extension != L".wav") {
            throw std::runtime_error("Event audition currently supports WAV files.");
        }
        const double start = parse_number(field(fields, "start"), "audition start");
        const auto info = transcribe::inspect_wav(audition_path);
        const double duration = static_cast<double>(info.frames) / info.sample_rate;
        const double end = std::min(duration, parse_number(field(fields, "end"), "audition end"));
        if (start < 0.0 || end <= start ||
            end - start > 600.0) {
            throw std::invalid_argument("The requested WAV audition region is invalid.");
        }
        stop_native_playback(state);
        if (state.review_audio_open) {
            (void)mciSendStringW(L"stop transcriber_review_audio", nullptr, 0, nullptr);
            const auto close_error = mciSendStringW(
                L"close transcriber_review_audio", nullptr, 0, nullptr);
            state.review_audio_open = false;
            if (close_error != 0) {
                throw std::runtime_error("Windows could not close the previous audio audition.");
            }
        }
        const auto file_path = audition_path.wstring();
        const std::wstring open_command =
            L"open \"" + file_path + L"\" type waveaudio alias transcriber_review_audio";
        auto error = mciSendStringW(open_command.c_str(), nullptr, 0, nullptr);
        if (error != 0) {
            throw std::runtime_error(
                "Windows could not open the WAV for event audition (MCI error " +
                std::to_string(error) + ").");
        }
        state.review_audio_open = true;
        error = mciSendStringW(
            L"set transcriber_review_audio time format milliseconds",
            nullptr, 0, nullptr);
        const auto start_ms = std::llround(start * 1000.0);
        const auto end_ms = std::llround(end * 1000.0);
        const std::wstring play_command =
            L"play transcriber_review_audio from " + std::to_wstring(start_ms) +
            L" to " + std::to_wstring(end_ms);
        if (error == 0) {
            error = mciSendStringW(play_command.c_str(), nullptr, 0, nullptr);
        }
        if (error != 0) {
            (void)mciSendStringW(L"close transcriber_review_audio", nullptr, 0, nullptr);
            state.review_audio_open = false;
            throw std::runtime_error(
                "Windows could not play the requested WAV region (MCI error " +
                std::to_string(error) + ").");
        }
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
        stop_native_playback(state);
        state.playback_token = std::stoull(field(fields, "token"));
        const auto command = L"open \"" + state.last_request->input_path.wstring() +
            L"\" type waveaudio alias transcriber_review_audio";
        if (mciSendStringW(command.c_str(), nullptr, 0, nullptr)) {
            throw std::runtime_error("Windows could not open the selected WAV file.");
        }
        state.review_audio_open = true;
        auto error = mciSendStringW(L"set transcriber_review_audio time format milliseconds",
                                    nullptr, 0, nullptr);
        if (!error) error = mciSendStringW(L"play transcriber_review_audio from 0", nullptr, 0, nullptr);
        if (error) {
            stop_native_playback(state);
            throw std::runtime_error("Windows could not start WAV playback.");
        }
        state.original_playback = true;
        if (!SetTimer(state.window, playback_timer, 30, nullptr)) {
            stop_native_playback(state);
            throw std::runtime_error("Windows could not start the WAV playback position timer.");
        }
        poll_native_playback(state);
    } else if (action == "stop-playback") {
        stop_native_playback(state);
    } else if (action == "cancel") {
        if (state.busy && state.active_transcription_job && state.worker.joinable()) {
            if (state.active_asio && state.live_bridge) {
                transcribe::live::bridge::CaptureState current_state;
                std::string current_status;
                {
                    std::lock_guard lock(state.live_bridge->mutex);
                    current_state = state.live_bridge->capture_state;
                    current_status = state.live_bridge->status;
                    if (current_state !=
                            transcribe::live::bridge::CaptureState::Finished &&
                        current_state !=
                            transcribe::live::bridge::CaptureState::Failed) {
                        state.live_bridge->cancel_requested.store(
                            true, std::memory_order_release);
                    }
                }
                if (current_state ==
                        transcribe::live::bridge::CaptureState::Finished ||
                    current_state ==
                        transcribe::live::bridge::CaptureState::Failed) {
                    post_live_status_json(state,
                        state.live_bridge->job, current_state, current_status);
                    return;
                }
                post_live_capture_state(state, state.live_bridge,
                    transcribe::live::bridge::CaptureState::Finishing,
                    "Canceling capture and discarding its notes");
            }
            state.worker.request_stop();
            state.transcription_cancelled = true;
            post_json(state, state.active_asio
                ? L"{\"type\":\"progress\",\"status\":\"Canceling capture\"}"
                : L"{\"type\":\"progress\",\"status\":\"Cancelling analysis at a safe boundary\"}");
        } else {
            post_live_status_json(state, 0,
                transcribe::live::bridge::CaptureState::Idle,
                "No active operation to cancel");
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

void drain_live_bridge(AppState& state) {
    const auto bridge = state.live_bridge;
    if (!bridge || bridge->job != state.active_transcription_job) return;

    std::shared_ptr<transcribe::live::LiveTranscriptionSession> session;
    {
        std::lock_guard lock(bridge->mutex);
        session = bridge->session;
    }
    if (!session) return;

    const auto notification_time_ns =
        bridge->notification_posted_at_ns.exchange(0, std::memory_order_acq_rel);
    if (notification_time_ns > 0) {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const double delay_ms = std::max(
            0.0, static_cast<double>(now_ns - notification_time_ns) / 1'000'000.0);
        bridge->ui_dispatch_samples_ms.push_back(delay_ms);
        if (bridge->ui_dispatch_samples_ms.size() > 256) {
            bridge->ui_dispatch_samples_ms.pop_front();
        }
        ++bridge->ui_dispatch_sample_count;
    }
    const auto ui_dispatch = summarize_ui_dispatch(
        bridge->ui_dispatch_samples_ms, bridge->ui_dispatch_sample_count);

    auto batch = session->poll_events();
    const auto metrics = session->metrics();
    const auto session_state = session->state();
    const double recording_seconds =
        static_cast<double>(session->ingested_frames()) / bridge->sample_rate;
    transcribe::live::bridge::Message message;
    std::unordered_map<std::string, std::pair<int, int>> fingerings;
    double driver_input_latency_ms = -1;
    double callback_period_ms = 0;
    std::string status;
    transcribe::live::bridge::CaptureState capture_state;
    bool final_batch = false;

    {
        std::lock_guard lock(bridge->mutex);
        if (batch.resync_required) {
            bridge->document.resync(batch.resync_snapshot);
        } else {
            bridge->document.apply(batch);
        }

        auto visible_batch = batch;
        const auto is_manual_note = [&](const auto& note) {
            return bridge->manual_note_ids.contains(note.id);
        };
        visible_batch.events.erase(
            std::remove_if(visible_batch.events.begin(), visible_batch.events.end(),
                [&](const auto& event) {
                    return !event.note.id.empty() &&
                           bridge->manual_note_ids.contains(event.note.id);
                }),
            visible_batch.events.end());
        if (visible_batch.resync_required) {
            visible_batch.resync_snapshot.erase(
                std::remove_if(visible_batch.resync_snapshot.begin(),
                              visible_batch.resync_snapshot.end(),
                              is_manual_note),
                visible_batch.resync_snapshot.end());
        }
        bridge->coalescer.append(visible_batch);
        for (const auto& note : bridge->manual_upserts) {
            bridge->coalescer.append_upsert(note);
        }
        for (const auto& [id, revision] : bridge->manual_retractions) {
            bridge->coalescer.append_retraction(id, revision);
        }
        bridge->manual_upserts.clear();
        bridge->manual_retractions.clear();

        if (session_state == transcribe::live::LiveState::InputInterrupted &&
            bridge->capture_state == transcribe::live::bridge::CaptureState::Capturing) {
            bridge->capture_state = transcribe::live::bridge::CaptureState::Interrupted;
            bridge->status = "Audio input interrupted; attempting to reconnect";
        } else if (session_state == transcribe::live::LiveState::Running &&
                   bridge->capture_state == transcribe::live::bridge::CaptureState::Interrupted) {
            bridge->capture_state = transcribe::live::bridge::CaptureState::Capturing;
            bridge->status = "Audio input reconnected";
        }

        capture_state = bridge->capture_state;
        status = bridge->status;
        fingerings = bridge->fingerings;
        driver_input_latency_ms = bridge->driver_input_latency_ms;
        callback_period_ms = bridge->callback_period_ms;
        message = bridge->coalescer.take(metrics, capture_state, status);
        final_batch = session_state == transcribe::live::LiveState::Finished ||
                      session_state == transcribe::live::LiveState::AnalysisFailed ||
                      session_state == transcribe::live::LiveState::Cancelled;
    }

    post_json(state, live_bridge_message_json(
        message, fingerings, recording_seconds, bridge->sample_rate,
        ui_dispatch, driver_input_latency_ms, callback_period_ms, bridge->job));
    if (final_batch) {
        {
            std::lock_guard lock(bridge->mutex);
            bridge->final_batch_applied = true;
        }
        bridge->drained.notify_all();
    }
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
    case message_live_status: {
        std::unique_ptr<LiveStatusMessage> status;
        if (wparam == 1) {
            std::lock_guard lock(state->live_message_mutex);
            status = std::move(state->pending_live_status);
            state->live_status_posted = false;
        } else {
            status.reset(reinterpret_cast<LiveStatusMessage*>(lparam));
        }
        if (!status ||
            (status->job && status->job != state->active_transcription_job)) {
            return 0;
        }
        post_live_status_json(*state, status->job, status->state, status->status);
        if (status->state == transcribe::live::bridge::CaptureState::Capturing ||
            status->state == transcribe::live::bridge::CaptureState::Interrupted ||
            status->state == transcribe::live::bridge::CaptureState::Finishing) {
            const auto bridge = state->live_bridge;
            if (bridge && bridge->job == status->job) {
                bool has_session = false;
                {
                    std::lock_guard lock(bridge->mutex);
                    has_session = static_cast<bool>(bridge->session);
                }
                if (has_session && !state->live_session_timer_active) {
                    if (SetTimer(window, live_session_timer, 17, nullptr) != 0) {
                        state->live_session_timer_active = true;
                    } else {
                        bridge->notification_failed.store(true,
                            std::memory_order_release);
                        bridge->drained.notify_all();
                        if (state->worker.joinable()) state->worker.request_stop();
                        post_error(*state,
                            "Could not start the live event delivery timer.");
                    }
                }
            }
        }
        return 0;
    }
    case message_live_events_ready: {
        const auto bridge = state->live_bridge;
        if (!bridge) return 0;
        bridge->notification_posted.store(false, std::memory_order_release);
        if (bridge->job != state->active_transcription_job) return 0;
        bool has_session = false;
        {
            std::lock_guard lock(bridge->mutex);
            has_session = static_cast<bool>(bridge->session);
        }
        if (has_session && !state->live_session_timer_active) {
            if (SetTimer(window, live_session_timer, 17, nullptr) != 0) {
                state->live_session_timer_active = true;
            } else {
                bridge->notification_failed.store(true, std::memory_order_release);
                bridge->drained.notify_all();
                if (state->worker.joinable()) state->worker.request_stop();
                post_error(*state, "Could not start the live event delivery timer.");
            }
        }
        return 0;
    }
    case message_progress: {
        std::unique_ptr<ProgressMessage> progress(
            reinterpret_cast<ProgressMessage*>(lparam));
        if (wparam == 1) {
            std::lock_guard lock(state->live_message_mutex);
            progress = std::move(state->pending_progress);
            state->progress_posted = false;
        }
        if (!progress) return 0;
        if (progress->job && progress->job != state->active_transcription_job) return 0;
        std::wostringstream json;
        json << L"{\"type\":\"progress\"";
        if (progress->percent >= 0) json << L",\"progress\":" << progress->percent;
        json << L",\"status\":\"" << json_escape(progress->status) << L"\"}";
        post_json(*state, json.str());
        return 0;
    }
    case message_live_preview: {
        std::unique_ptr<LiveMessage> live(reinterpret_cast<LiveMessage*>(lparam));
        if (wparam == 1) {
            std::lock_guard lock(state->live_message_mutex);
            live = std::move(state->pending_preview_notes);
            state->preview_notes_posted = false;
        }
        if (!live) return 0;
        if (live->job && live->job != state->active_transcription_job) return 0;
        if (state->transcription_cancelled || (live->job && state->last_result)) return 0;
        post_json(*state, L"{\"type\":\"live\",\"notes\":" +
                             notes_json(live->notes, live->offset) +
                             L",\"events\":" + events_json(live->events) + L"}");
        return 0;
    }
    case message_audio_preview: {
        std::unique_ptr<AudioPreviewMessage> preview(
            reinterpret_cast<AudioPreviewMessage*>(lparam));
        if (wparam == 1) {
            std::lock_guard lock(state->live_message_mutex);
            preview = std::move(state->pending_audio_preview);
            state->audio_preview_posted = false;
        }
        if (!preview) return 0;
        if (preview->job && preview->job != state->active_transcription_job) return 0;
        if (preview->job && (state->transcription_cancelled || state->last_result)) return 0;
        post_json(*state, audio_preview_json(
                              preview->offset, preview->replace, preview->preview));
        return 0;
    }
    case message_project_autosave: {
        std::unique_ptr<AutosaveCompletion> result(
            reinterpret_cast<AutosaveCompletion*>(lparam));
        if (result->success) {
            post_json(*state, L"{\"type\":\"project-autosaved\","
                L"\"score_revision\":" + std::to_wstring(result->revision) +
                L",\"generation\":" +
                std::to_wstring(result->edit_generation) +
                L",\"summary\":\"" + json_escape(result->summary) + L"\"}");
        } else {
            post_json(*state, L"{\"type\":\"project-autosave-error\","
                L"\"message\":\"" +
                json_escape(from_utf8_lossy(result->error)) + L"\"}");
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == playback_timer) {
            poll_native_playback(*state);
            return 0;
        }
        if (wparam == live_session_timer) {
            const auto bridge = state->live_bridge;
            try {
                drain_live_bridge(*state);
            } catch (const std::exception& error) {
                if (bridge) {
                    bridge->notification_failed.store(true,
                        std::memory_order_release);
                    bridge->drained.notify_all();
                    if (state->worker.joinable()) state->worker.request_stop();
                }
                post_error(*state, error.what());
            }
            bool completed = !bridge;
            if (bridge) {
                std::lock_guard lock(bridge->mutex);
                completed = bridge->final_batch_applied ||
                    bridge->notification_failed.load(std::memory_order_acquire);
            }
            if (completed) {
                KillTimer(window, live_session_timer);
                state->live_session_timer_active = false;
            }
            return 0;
        }
        break;
    case message_complete: {
        std::unique_ptr<CompletionMessage> completion(
            reinterpret_cast<CompletionMessage*>(lparam));
        if (completion->job && completion->job != state->active_transcription_job) return 0;
        if (state->worker.joinable()) {
            state->worker.join();
        }
        if (state->live_session_timer_active) {
            KillTimer(window, live_session_timer);
            state->live_session_timer_active = false;
        }
        if (completion->job && state->transcription_cancelled) {
            completion->success = false;
            completion->cancelled = true;
            completion->error = "Transcription cancelled; accepted score unchanged.";
        }
        if (state->live_bridge &&
            state->live_bridge->job == completion->job) {
            state->live_bridge.reset();
        }
        state->busy = false;
        state->active_transcription_job = 0;
        state->active_asio = false;
        if (completion->project_open) {
            if (completion->success) {
                state->project = completion->project;
                state->project_path = completion->project_path;
                state->last_request = completion->request;
                state->last_result = completion->result;
                state->manual_fingering_corrections =
                    completion->request.guitar_tab_options.manual_corrections;
                state->proposal_request.reset();
                state->proposal_result.reset();
                state->live_document.reset();
                state->live_recording = {};
                state->live_backend_id.clear();
                state->pre_refinement_result.reset();
                state->pre_refinement_document.reset();
                if (!completion->project_recovery && state->autosaver) {
                    state->autosaver->clear();
                }
            }
            post_json(*state, result_json(*completion));
        } else if (completion->refinement_proposal) {
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
                if (completion->has_project) {
                    if (completion->project_save) {
                        state->project = completion->project;
                        state->project_path = completion->project_path;
                        if (state->autosaver) state->autosaver->clear();
                    } else if (completion->score_only_copy) {
                        if (state->project) {
                            state->project->score = completion->result.score;
                        } else {
                            state->project = completion->project;
                        }
                    } else {
                        state->project = completion->project;
                    }
                    if (!completion->project_save && state->autosaver &&
                        state->project) {
                        try {
                            queue_project_autosave(
                                *state, *state->project,
                                completion->edit_generation);
                        } catch (const std::exception& error) {
                            post_error(*state, error.what());
                        }
                    }
                }
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
                export_message += L",\"project_saved\":" +
                    std::wstring(completion->project_save ? L"true" : L"false") +
                    L",\"score_only_copy\":" +
                    std::wstring(completion->score_only_copy ? L"true" : L"false");
                if (completion->project_save || completion->score_only_copy) {
                    export_message += L",\"project_path\":\"" +
                        json_escape(completion->project_path.wstring()) + L"\"";
                }
                if (state->project) {
                    export_message += L",\"project\":" +
                        project_summary_json(*state->project, state->project_path);
                }
                if (completion->request.retain_review_evidence) {
                    export_message += L",\"review\":" +
                       score_review_json(completion->result.score);
                }
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
                    if (completion->has_project) {
                        state->project = completion->project;
                        state->project_path = completion->project_path;
                        if (state->autosaver) {
                            try {
                                queue_project_autosave(
                                    *state, *state->project, 0);
                            } catch (const std::exception& error) {
                                post_error(*state, error.what());
                            }
                        }
                    }
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
        stop_native_playback(*state);
        if (state->live_session_timer_active) {
            KillTimer(window, live_session_timer);
            state->live_session_timer_active = false;
        }
        if (state->live_bridge) {
            state->live_bridge->shutdown_requested.store(
                true, std::memory_order_release);
            state->live_bridge->drained.notify_all();
        }
        state->shutting_down = true;
        PlaySoundW(nullptr, nullptr, 0);
        if (state->review_audio_open) {
            (void)mciSendStringW(L"stop transcriber_review_audio", nullptr, 0, nullptr);
            (void)mciSendStringW(L"close transcriber_review_audio", nullptr, 0, nullptr);
            state->review_audio_open = false;
        }
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
    return DefWindowProcW(window, message, wparam, lparam);
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
