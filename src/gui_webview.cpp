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
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t app_origin[] = L"https://appassets.transcribe/";
constexpr UINT message_complete = WM_APP + 1;
constexpr UINT message_progress = WM_APP + 2;
constexpr UINT message_live_preview = WM_APP + 3;
constexpr UINT message_audio_preview = WM_APP + 4;

struct AppState {
    HWND window{};
    HMODULE loader{};
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken navigation_token{};
    EventRegistrationToken message_token{};
    std::jthread worker;
    bool busy{};
    bool active_asio{};
    bool close_when_done{};
    bool shutting_down{};
    std::optional<transcribe::gui::Request> last_request;
    std::optional<transcribe::gui::Result> last_result;
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

struct AudioPreviewMessage {
    double offset{};
    bool replace{};
    transcribe::AudioPreviewData preview;
};

struct CompletionMessage {
    bool success{};
    bool export_only{};
    std::string error;
    transcribe::gui::Result result;
    transcribe::gui::Request request;
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
        OutputDebugStringW(L"Transcribe could not post a message to the WebView2 page.\n");
    }
}

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
             << L",\"amplitude\":" << note.amplitude << L'}';
    }
    json << L']';
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

std::wstring result_json(const CompletionMessage& completion) {
    if (!completion.success) {
        return L"{\"type\":\"complete\",\"success\":false,\"error\":\"" +
               json_escape(from_utf8_lossy(completion.error)) + L"\"}";
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
    summary << L".\nMusicXML: " << completion.request.musicxml_path.wstring()
            << L"\nGuitar tab: " << completion.request.tab_path.wstring();
    if (!completion.request.midi_path.empty()) {
        summary << L"\nMIDI: " << completion.request.midi_path.wstring();
    }

    std::wostringstream json;
    json << L"{\"type\":\"complete\",\"success\":true,\"notes\":"
         << notes_json(completion.result.notes)
         << L",\"metadata\":{\"tempo\":" << std::fixed << std::setprecision(2)
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
         << L"},\"summary\":\"" << json_escape(summary.str()) << L"\"}";
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }

    using GetAvailableVersionFunction =
        decltype(&GetAvailableCoreWebView2BrowserVersionString);
    const auto get_available_version =
        reinterpret_cast<GetAvailableVersionFunction>(GetProcAddress(
            state.loader, "GetAvailableCoreWebView2BrowserVersionString"));
    if (get_available_version == nullptr) {
        MessageBoxW(nullptr, L"The WebView2 loader is missing a required entry point.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
                    L"Evergreen installer is missing beside Transcribe.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
        return RuntimeStartup::failed;
    }

    const int choice = MessageBoxW(
        nullptr,
        L"The Microsoft Edge WebView2 Runtime is required but is not installed.\n\n"
        L"Would you like to start the included Evergreen installer? Internet access "
        L"is required. Restart Transcribe after installation.",
        L"Transcribe", MB_YESNO | MB_ICONINFORMATION);
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
        MessageBoxW(nullptr, message.c_str(), L"Transcribe", MB_OK | MB_ICONERROR);
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

void start_transcription(AppState& state,
                         const std::unordered_map<std::string, std::string>& fields) {
    if (state.busy) {
        return;
    }

    transcribe::gui::Request request;
    request.use_asio = field(fields, "mode") == "asio";
    request.input_path = from_utf8(field(fields, "input"));
    request.model_path = from_utf8(field(fields, "model"));
    request.musicxml_path = from_utf8(field(fields, "musicxml"));
    request.tab_path = from_utf8(field(fields, "tab"));
    request.midi_path = from_utf8(field(fields, "midi"));
    request.polyphonic = field(fields, "polyphonic") == "1";
    request.fast_preview = field(fields, "quality") == "fast";
    request.instrument = instrument_prior(field(fields, "instrument"));
    request.guitar_tab_options.tuning = guitar_tuning(field(fields, "tuning"));
    request.preserve_expressive_timing = field(fields, "preserve_timing") == "1";

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
#endif
    } else if (request.input_path.empty()) {
        throw std::invalid_argument("Choose an audio file first.");
    }
    if (request.musicxml_path.empty() || request.tab_path.empty()) {
        throw std::invalid_argument("Choose both MusicXML and guitar tab output paths.");
    }
#ifndef TRANSCRIBE_ENABLE_ONNX
    if (!request.model_path.empty() || request.polyphonic) {
        throw std::runtime_error(
            "ONNX support is not enabled. Configure with -DTRANSCRIBE_ENABLE_ONNX=ON.");
    }
#endif
    if (request.polyphonic && request.model_path.empty()) {
        throw std::invalid_argument("Choose an ONNX model to enable polyphonic inference.");
    }

    state.busy = true;
    state.active_asio = request.use_asio;
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
                completion->result = transcribe::gui::run_transcription(
                    request, stop_token, progress_callback, live_callback,
                    audio_preview_callback);
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

std::vector<transcribe::Note> parse_edited_notes(std::string_view encoded) {
    std::vector<transcribe::Note> notes;
    while (!encoded.empty()) {
        const auto end = encoded.find(';');
        const auto record = encoded.substr(0, end);
        std::array<std::string_view, 5> values{};
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
        const double midi_value = parse_number(values[0], "edited MIDI pitch");
        const double start = parse_number(values[1], "edited note start");
        const double duration = parse_number(values[2], "edited note duration");
        const double confidence = parse_number(values[3], "edited note confidence");
        const double amplitude = parse_number(values[4], "edited note amplitude");
        if (midi_value < 0.0 || midi_value > 127.0 ||
            std::floor(midi_value) != midi_value || start < 0.0 ||
            duration <= 0.0 || duration > 600.0 ||
            confidence < 0.0 || confidence > 1.0 ||
            amplitude < 0.0 || amplitude > 1.0) {
            throw std::invalid_argument("Edited note values are outside supported ranges.");
        }
        const int midi = static_cast<int>(midi_value);
        notes.push_back({midi, start, duration, transcribe::midi_note_name(midi),
                         confidence, amplitude});
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
            static_cast<int>(fret)});
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
    transcribe::gui::Result result;
    result.notes = parse_edited_notes(field(fields, "notes"));
    request.guitar_tab_options.position_overrides =
        parse_tab_positions(field(fields, "tab_positions"));
    result.metadata = transcribe::infer_score_metadata(result.notes);
    if (!result.notes.empty()) {
        result.metadata.tempo_bpm = parse_number(field(fields, "tempo"), "tempo");
        if (result.metadata.tempo_bpm < 40.0 || result.metadata.tempo_bpm > 240.0) {
            throw std::invalid_argument("Tempo must be between 40 and 240 BPM.");
        }
        const auto key = field(fields, "key_fifths");
        if (!key.empty() && key != "auto") {
            std::size_t key_characters = 0;
            const int fifths = std::stoi(key, &key_characters);
            if (key_characters != key.size() || fifths < -7 || fifths > 7) {
                throw std::invalid_argument("Key signature must be between -7 and 7 fifths.");
            }
            result.metadata.key_fifths = fifths;
            result.metadata.minor_key = field(fields, "minor") == "1";
            result.metadata.key_inferred = true;
        }
        const auto meter = field(fields, "meter");
        if (meter != "auto") {
            if (meter == "3/4") {
                result.metadata.time_numerator = 3;
                result.metadata.time_denominator = 4;
            } else if (meter == "4/4") {
                result.metadata.time_numerator = 4;
                result.metadata.time_denominator = 4;
            } else if (meter == "6/8") {
                result.metadata.time_numerator = 6;
                result.metadata.time_denominator = 8;
            } else {
                throw std::invalid_argument("Choose a supported time signature.");
            }
            result.metadata.meter_inferred = true;
        }
    }

    state.busy = true;
    post_json(state, L"{\"type\":\"progress\",\"progress\":0,\"status\":\"Exporting edited score\"}");
    try {
        state.worker = std::jthread(
            [&state, request = std::move(request),
             result = std::move(result)](std::stop_token) mutable {
                auto message = std::make_unique<CompletionMessage>();
                message->export_only = true;
                message->request = request;
                message->result = result;
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
                    transcribe::write_musicxml(
                        request.musicxml_path, result.notes, result.metadata,
                        request.guitar_tab_options,
                        request.preserve_expressive_timing);
                    report_export_progress(55, L"Writing guitar tablature");
                    transcribe::write_guitar_tab(
                        request.tab_path, result.notes, request.guitar_tab_options);
                    if (!request.midi_path.empty()) {
                        report_export_progress(80, L"Writing MIDI");
                        transcribe::write_midi(
                            request.midi_path, result.notes, result.metadata);
                    }
                    report_export_progress(100, L"Export complete");
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

void handle_web_message(AppState& state, const std::string& message) {
    const auto separator = message.find('|');
    const std::string action = message.substr(0, separator);
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
        post_json(state, L"{\"type\":\"ready\",\"asio\":" +
                             std::wstring(asio_enabled ? L"true" : L"false") +
                             L",\"onnx\":" +
                             std::wstring(onnx_enabled ? L"true" : L"false") +
                             L",\"ffmpeg\":" +
                             std::wstring(ffmpeg_enabled ? L"true" : L"false") + L"}");
    } else if (action == "browse") {
        const std::string field_name = field(fields, "field");
        browse_for_path(state, field_name, from_utf8(field(fields, "current")));
    } else if (action == "transcribe") {
        start_transcription(state, fields);
    } else if (action == "export-edits") {
        export_edited_score(state, fields);
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return FAILED(error_code) ? error_code : E_FAIL;
    }

    state_.controller = controller;
    HRESULT result = state_.controller->get_CoreWebView2(&state_.webview);
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not access the WebView2 browser control.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }

    auto* message_handler = new WebMessageReceived(state_);
    result = state_.webview->add_WebMessageReceived(
        message_handler, &state_.message_token);
    message_handler->Release();
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not initialize the WebView2 message bridge.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }

    ComPtr<ICoreWebView2_3> webview3;
    result = state_.webview->QueryInterface(
        IID_ICoreWebView2_3,
        reinterpret_cast<void**>(webview3.GetAddressOf()));
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"The installed WebView2 Runtime is too old.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }
    const auto ui_directory = executable_directory() / L"ui";
    result = webview3->SetVirtualHostNameToFolderMapping(
        L"appassets.transcribe", ui_directory.c_str(),
        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not load the local user interface assets.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
        PostQuitMessage(1);
        return result;
    }
    result = state_.webview->Navigate(L"https://appassets.transcribe/index.html");
    if (FAILED(result)) {
        MessageBoxW(state_.window, L"Could not open the local user interface.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
    if (FAILED(args->TryGetWebMessageAsString(&message)) || message == nullptr) {
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
            MessageBoxW(window, from_utf8(error.what()).c_str(), L"Transcribe",
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
        if (completion->export_only) {
            if (completion->success) {
                state->last_result = completion->result;
                state->last_request = completion->request;
            }
            std::wstring export_message =
                std::wstring(L"{\"type\":\"exported\",\"success\":") +
                (completion->success ? L"true" : L"false");
            if (completion->success) {
                export_message += L"}";
            } else {
                export_message += L",\"error\":\"" +
                                  json_escape(from_utf8_lossy(completion->error)) +
                                  L"\"}";
            }
            post_json(*state, export_message);
        } else {
            if (completion->success) {
                state->last_request = completion->request;
                state->last_result = completion->result;
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
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

    const wchar_t class_name[] = L"TranscribeWebViewWindow";
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
                    L"Transcribe", MB_OK | MB_ICONERROR);
        return 1;
    }

    RECT bounds{0, 0, 1240, 900};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(
        0, class_name, L"Transcribe", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        DeleteObject(window_class.hbrBackground);
        CoUninitialize();
        MessageBoxW(nullptr, L"Could not create the application window.",
                    L"Transcribe", MB_OK | MB_ICONERROR);
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
