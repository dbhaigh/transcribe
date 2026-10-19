#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int id_mode = 100;
constexpr int id_input = 101;
constexpr int id_input_browse = 102;
constexpr int id_duration_label = 103;
constexpr int id_duration = 104;
constexpr int id_model = 105;
constexpr int id_model_browse = 106;
constexpr int id_musicxml = 107;
constexpr int id_musicxml_browse = 108;
constexpr int id_tab = 109;
constexpr int id_tab_browse = 110;
constexpr int id_transcribe = 111;
constexpr UINT message_complete = WM_APP + 1;

struct WorkResult {
    bool success{};
    std::wstring message;
};

struct AppState {
    HWND window{};
    HWND mode{};
    HWND input{};
    HWND input_browse{};
    HWND duration_label{};
    HWND duration{};
    HWND model{};
    HWND model_browse{};
    HWND musicxml{};
    HWND musicxml_browse{};
    HWND tab{};
    HWND tab_browse{};
    HWND transcribe{};
    HWND status{};
    std::vector<HWND> form_controls;
    std::jthread worker;
    bool busy{};
    bool close_when_done{};
};

std::wstring get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring to_wide(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) {
        return L"Transcription failed with an unrecognized error.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

HWND create_control(AppState& state, const wchar_t* class_name, const wchar_t* text,
                    DWORD style, int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(
        0, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control == nullptr) {
        throw std::runtime_error("Could not create a window control.");
    }
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    if (id != 0) {
        state.form_controls.push_back(control);
    }
    return control;
}

void create_label(AppState& state, const wchar_t* text, int y) {
    create_control(state, L"STATIC", text, SS_LEFT, 20, y, 145, 24, 0);
}

void set_mode_controls(AppState& state) {
    const bool asio_selected = SendMessageW(state.mode, CB_GETCURSEL, 0, 0) == 1;
    EnableWindow(state.input, !asio_selected);
    EnableWindow(state.input_browse, !asio_selected);
    EnableWindow(state.duration_label, asio_selected);
    EnableWindow(state.duration, asio_selected);
#ifndef TRANSCRIBE_ENABLE_ONNX
    EnableWindow(state.model, FALSE);
    EnableWindow(state.model_browse, FALSE);
#endif
}

void browse_for_file(HWND owner, HWND edit, bool save, const wchar_t* filter,
                     const wchar_t* default_extension) {
    std::array<wchar_t, 32768> filename{};
    const auto current = get_text(edit);
    if (current.size() < filename.size()) {
        std::copy(current.begin(), current.end(), filename.begin());
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = default_extension;
    const BOOL accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (accepted) {
        SetWindowTextW(edit, filename.data());
    }
}

void set_form_enabled(AppState& state, bool enabled) {
    for (const HWND control : state.form_controls) {
        EnableWindow(control, enabled);
    }
    set_mode_controls(state);
    if (!enabled) {
        for (const HWND control : state.form_controls) {
            EnableWindow(control, FALSE);
        }
    }
}

void start_transcription(AppState& state) {
    const bool use_asio = SendMessageW(state.mode, CB_GETCURSEL, 0, 0) == 1;
    const std::wstring input_text = get_text(state.input);
    const std::wstring model_text = get_text(state.model);
    const std::wstring musicxml_text = get_text(state.musicxml);
    const std::wstring tab_text = get_text(state.tab);
    double capture_seconds = 0.0;

    if (!use_asio && input_text.empty()) {
        MessageBoxW(state.window, L"Choose a WAV audio file first.", L"Transcribe",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (musicxml_text.empty() || tab_text.empty()) {
        MessageBoxW(state.window, L"Choose both output file paths.", L"Transcribe",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
#ifndef TRANSCRIBE_ENABLE_ONNX
    if (!model_text.empty()) {
        MessageBoxW(state.window,
                    L"ONNX Runtime support is not enabled in this build. "
                    L"Configure with -DTRANSCRIBE_ENABLE_ONNX=ON to use a model.",
                    L"Transcribe", MB_OK | MB_ICONWARNING);
        return;
    }
#endif

    if (use_asio) {
        try {
            std::size_t parsed_characters = 0;
            capture_seconds = std::stod(get_text(state.duration), &parsed_characters);
            if (parsed_characters != get_text(state.duration).size() ||
                capture_seconds <= 0.0 || capture_seconds > 300.0) {
                throw std::invalid_argument("Invalid duration.");
            }
        } catch (const std::exception&) {
            MessageBoxW(state.window, L"Enter a capture duration greater than 0 and at most 300 seconds.",
                        L"Transcribe", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    const std::filesystem::path input_path(input_text);
    const std::filesystem::path model_path(model_text);
    const std::filesystem::path musicxml_path(musicxml_text);
    const std::filesystem::path tab_path(tab_text);
    state.busy = true;
    set_form_enabled(state, false);
    SetWindowTextW(state.status, use_asio
                                     ? L"Capturing from ASIO, then transcribing..."
                                     : L"Transcribing audio...");

    state.worker = std::jthread([window = state.window, use_asio, capture_seconds,
                                 input_path, model_path, musicxml_path, tab_path]
                                (std::stop_token stop_token) {
        auto result = std::make_unique<WorkResult>();
        try {
            transcribe::AudioBuffer audio = use_asio
                                                ? transcribe::capture_asio(capture_seconds, stop_token)
                                                : transcribe::read_wav(input_path);
            std::vector<transcribe::Note> notes;
            if (model_path.empty()) {
                notes = transcribe::transcribe_monophonic(audio);
            } else {
#ifdef TRANSCRIBE_ENABLE_ONNX
                notes = transcribe::transcribe_with_onnx(audio, model_path);
#else
                throw std::runtime_error("ONNX Runtime support is disabled.");
#endif
            }
            if (notes.empty()) {
                throw std::runtime_error("No stable pitches were detected in the input.");
            }
            transcribe::write_musicxml(musicxml_path, notes);
            transcribe::write_guitar_tab(tab_path, notes);
            result->success = true;
            result->message = L"Detected " + std::to_wstring(notes.size()) +
                              L" notes.\r\nMusicXML: " + musicxml_path.wstring() +
                              L"\r\nGuitar tab: " + tab_path.wstring();
        } catch (const std::exception& error) {
            result->message = to_wide(error.what());
        }

        if (PostMessageW(window, message_complete, 0,
                         reinterpret_cast<LPARAM>(result.get()))) {
            result.release();
        }
    });
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE: {
        try {
        create_label(*state, L"Audio input", 24);
        state->mode = create_control(*state, L"COMBOBOX", L"",
                                     CBS_DROPDOWNLIST | WS_VSCROLL, 175, 20, 190, 150, id_mode);
        SendMessageW(state->mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV file"));
#ifdef TRANSCRIBE_ENABLE_ASIO
        SendMessageW(state->mode, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Real-time ASIO"));
#endif
        SendMessageW(state->mode, CB_SETCURSEL, 0, 0);

        create_label(*state, L"Input WAV file", 66);
        state->input = create_control(*state, L"EDIT", L"",
                                      WS_BORDER | ES_AUTOHSCROLL, 175, 62, 455, 25, id_input);
        state->input_browse = create_control(*state, L"BUTTON", L"Browse...",
                                             BS_PUSHBUTTON, 640, 61, 90, 27, id_input_browse);

        state->duration_label = create_control(*state, L"STATIC", L"Capture seconds",
                                               SS_LEFT, 20, 106, 145, 24, id_duration_label);
        state->duration = create_control(*state, L"EDIT", L"10",
                                         WS_BORDER | ES_AUTOHSCROLL,
                                         175, 102, 120, 25, id_duration);

        create_label(*state, L"ONNX model (optional)", 146);
        state->model = create_control(*state, L"EDIT", L"",
                                      WS_BORDER | ES_AUTOHSCROLL, 175, 142, 455, 25, id_model);
        state->model_browse = create_control(*state, L"BUTTON", L"Browse...",
                                             BS_PUSHBUTTON, 640, 141, 90, 27, id_model_browse);
#ifndef TRANSCRIBE_ENABLE_ONNX
        EnableWindow(state->model, FALSE);
        EnableWindow(state->model_browse, FALSE);
#endif

        create_label(*state, L"MusicXML output", 186);
        state->musicxml = create_control(*state, L"EDIT", L"transcription.musicxml",
                                         WS_BORDER | ES_AUTOHSCROLL, 175, 182, 455, 25, id_musicxml);
        state->musicxml_browse = create_control(*state, L"BUTTON", L"Save as...",
                                                BS_PUSHBUTTON, 640, 181, 90, 27, id_musicxml_browse);

        create_label(*state, L"Guitar tab output", 226);
        state->tab = create_control(*state, L"EDIT", L"transcription.txt",
                                    WS_BORDER | ES_AUTOHSCROLL, 175, 222, 455, 25, id_tab);
        state->tab_browse = create_control(*state, L"BUTTON", L"Save as...",
                                           BS_PUSHBUTTON, 640, 221, 90, 27, id_tab_browse);

        state->transcribe = create_control(*state, L"BUTTON", L"Transcribe",
                                           BS_DEFPUSHBUTTON, 175, 267, 140, 34, id_transcribe);
        state->status = create_control(*state, L"STATIC", L"Ready.",
                                       SS_LEFT, 175, 316, 555, 65, 0);
        set_mode_controls(*state);
        return 0;
        } catch (const std::exception& error) {
            MessageBoxW(window, to_wide(error.what()).c_str(), L"Transcribe",
                        MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case id_mode:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                set_mode_controls(*state);
            }
            return 0;
        case id_input_browse:
            browse_for_file(window, state->input, false,
                            L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0", nullptr);
            return 0;
        case id_model_browse:
            browse_for_file(window, state->model, false,
                            L"ONNX models (*.onnx)\0*.onnx\0All files (*.*)\0*.*\0\0", nullptr);
            return 0;
        case id_musicxml_browse:
            browse_for_file(window, state->musicxml, true,
                            L"MusicXML (*.musicxml;*.xml)\0*.musicxml;*.xml\0All files (*.*)\0*.*\0\0",
                            L"musicxml");
            return 0;
        case id_tab_browse:
            browse_for_file(window, state->tab, true,
                            L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0", L"txt");
            return 0;
        case id_transcribe:
            try {
                start_transcription(*state);
            } catch (const std::exception& error) {
                MessageBoxW(window, to_wide(error.what()).c_str(), L"Transcribe",
                            MB_OK | MB_ICONERROR);
            }
            return 0;
        default:
            break;
        }
        break;
    case message_complete: {
        std::unique_ptr<WorkResult> result(reinterpret_cast<WorkResult*>(lparam));
        if (state->worker.joinable()) {
            state->worker.join();
        }
        state->busy = false;
        if (result->success) {
            SetWindowTextW(state->status, result->message.c_str());
        } else {
            SetWindowTextW(state->status, L"Transcription failed.");
            MessageBoxW(window, result->message.c_str(), L"Transcription failed",
                        MB_OK | MB_ICONERROR);
        }
        if (state->close_when_done) {
            DestroyWindow(window);
        } else {
            set_form_enabled(*state, true);
        }
        return 0;
    }
    case WM_CLOSE:
        if (state->busy) {
            state->close_when_done = true;
            state->worker.request_stop();
            SetWindowTextW(state->status, L"Stopping capture or finishing transcription...");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    AppState state;
    const wchar_t class_name[] = L"TranscribeMainWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;
    if (RegisterClassExW(&window_class) == 0) {
        MessageBoxW(nullptr, L"Could not register the application window.", L"Transcribe",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    RECT bounds{0, 0, 760, 405};
    AdjustWindowRect(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND window = CreateWindowExW(
        0, class_name, L"Transcribe", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Could not create the application window.", L"Transcribe",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
