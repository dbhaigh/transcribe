#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>

#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gui_transcription.hpp"

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
constexpr int id_midi = 112;
constexpr int id_midi_browse = 113;
constexpr int id_open_output = 114;
constexpr int id_progress = 115;
constexpr int id_preview = 116;
constexpr int id_polyphonic = 117;
constexpr int id_latency = 118;
constexpr int id_guitar_tuning = 119;
constexpr int id_capo = 120;
constexpr int id_tab_alternatives = 121;
constexpr int id_expressive_timing = 122;
constexpr UINT message_complete = WM_APP + 1;
constexpr UINT message_progress = WM_APP + 2;
constexpr UINT message_live_preview = WM_APP + 3;

struct WorkResult {
    bool success{};
    std::wstring message;
    std::wstring preview;
};

struct LivePreview {
    std::wstring text;
};

struct AppState {
    HWND window{};
    HWND mode{};
    HWND input{};
    HWND input_browse{};
    HWND duration_label{};
    HWND duration{};
    HWND realtime_latency{};
    HWND model{};
    HWND model_browse{};
    HWND musicxml{};
    HWND musicxml_browse{};
    HWND tab{};
    HWND tab_browse{};
    HWND midi{};
    HWND midi_browse{};
    HWND open_output{};
    HWND polyphonic{};
    HWND guitar_tuning{};
    HWND capo{};
    HWND tab_alternatives{};
    HWND expressive_timing{};
    HWND transcribe{};
    HWND status{};
    HWND progress{};
    HWND preview{};
    std::vector<HWND> form_controls;
    std::jthread worker;
    bool busy{};
    bool close_when_done{};
    std::filesystem::path preferences_path;
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

std::wstring key_name(const transcribe::ScoreMetadata& metadata) {
    static constexpr std::array<const wchar_t*, 15> major_names{
        L"Cb", L"Gb", L"Db", L"Ab", L"Eb", L"Bb", L"F", L"C",
        L"G", L"D", L"A", L"E", L"B", L"F#", L"C#"};
    static constexpr std::array<const wchar_t*, 15> minor_names{
        L"Ab", L"Eb", L"Bb", L"F", L"C", L"G", L"D", L"A",
        L"E", L"B", L"F#", L"C#", L"G#", L"D#", L"A#"};
    const auto index = static_cast<std::size_t>(std::clamp(metadata.key_fifths, -7, 7) + 7);
    return std::wstring(metadata.minor_key ? minor_names[index] : major_names[index]) +
           (metadata.minor_key ? L" minor" : L" major");
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
    EnableWindow(state.realtime_latency, asio_selected);
#ifndef TRANSCRIBE_ENABLE_ONNX
    EnableWindow(state.model, FALSE);
    EnableWindow(state.model_browse, FALSE);
    EnableWindow(state.polyphonic, FALSE);
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

std::filesystem::path preferences_path() {
    std::array<wchar_t, 32768> appdata{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata.data(),
                                                 static_cast<DWORD>(appdata.size()));
    if (length == 0 || length >= appdata.size()) {
        return {};
    }
    return std::filesystem::path(appdata.data()) / L"Transcribe" / L"preferences.ini";
}

void load_preferences(AppState& state) {
    state.preferences_path = preferences_path();
    if (state.preferences_path.empty()) {
        return;
    }
    std::array<wchar_t, 32768> value{};
    const auto read = [&state, &value](const wchar_t* key, HWND control) {
        value.fill(L'\0');
        GetPrivateProfileStringW(L"Recent", key, L"", value.data(),
                                 static_cast<DWORD>(value.size()),
                                 state.preferences_path.c_str());
        if (value[0] != L'\0') {
            SetWindowTextW(control, value.data());
        }
    };
    read(L"input", state.input);
    read(L"model", state.model);
    read(L"musicxml", state.musicxml);
    read(L"tab", state.tab);
    read(L"midi", state.midi);
}

bool save_preferences(const AppState& state) {
    if (state.preferences_path.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(state.preferences_path.parent_path(), error);
    if (error) {
        return false;
    }
    const auto write = [&state](const wchar_t* key, HWND control) {
        const auto value = get_text(control);
        return WritePrivateProfileStringW(L"Recent", key, value.c_str(),
                                          state.preferences_path.c_str()) != FALSE;
    };
    return write(L"input", state.input) && write(L"model", state.model) &&
           write(L"musicxml", state.musicxml) && write(L"tab", state.tab) &&
           write(L"midi", state.midi);
}

void save_preferences_or_warn(const AppState& state) {
    if (!save_preferences(state)) {
        MessageBoxW(state.window,
                    L"Recent paths could not be saved and will not be remembered.",
                    L"Transcribe", MB_OK | MB_ICONWARNING);
    }
}

void start_transcription(AppState& state) {
    const bool use_asio = SendMessageW(state.mode, CB_GETCURSEL, 0, 0) == 1;
    const std::wstring input_text = get_text(state.input);
    const std::wstring model_text = get_text(state.model);
    const std::wstring musicxml_text = get_text(state.musicxml);
    const std::wstring tab_text = get_text(state.tab);
    const std::wstring midi_text = get_text(state.midi);
    const bool polyphonic = IsDlgButtonChecked(state.window, id_polyphonic) == BST_CHECKED;
    double capture_seconds = 0.0;
    double latency_ms = 40.0;
    int capo_fret = 0;
    int tab_alternatives = 0;
    static constexpr std::array<std::array<int, 6>, 5> tunings{{
        {{64, 59, 55, 50, 45, 40}},
        {{64, 59, 55, 50, 45, 38}},
        {{62, 57, 55, 50, 45, 38}},
        {{62, 57, 54, 50, 45, 38}},
        {{62, 59, 55, 50, 43, 38}},
    }};
    const auto tuning_selection = SendMessageW(state.guitar_tuning, CB_GETCURSEL, 0, 0);
    if (tuning_selection < 0 ||
        tuning_selection >= static_cast<LRESULT>(tunings.size())) {
        MessageBoxW(state.window, L"Select a supported guitar tuning.", L"Transcribe",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    try {
        const auto capo_text = get_text(state.capo);
        std::size_t parsed_characters = 0;
        capo_fret = std::stoi(capo_text, &parsed_characters);
        if (parsed_characters != capo_text.size() || capo_fret < 0 || capo_fret > 24) {
            throw std::invalid_argument("Invalid capo fret.");
        }
        const auto alternatives_text = get_text(state.tab_alternatives);
        tab_alternatives = std::stoi(alternatives_text, &parsed_characters);
        if (parsed_characters != alternatives_text.size() ||
            tab_alternatives < 0 || tab_alternatives > 5) {
            throw std::invalid_argument("Invalid alternative fingering count.");
        }
    } catch (const std::exception&) {
        MessageBoxW(state.window, L"Enter a capo fret from 0 to 24 and 0 to 5 alternative fingerings.",
                    L"Transcribe", MB_OK | MB_ICONWARNING);
        return;
    }

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
    if (!model_text.empty() || polyphonic) {
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
            const auto latency_text = get_text(state.realtime_latency);
            latency_ms = std::stod(latency_text, &parsed_characters);
            if (parsed_characters != latency_text.size() || !std::isfinite(latency_ms) ||
                latency_ms < 20.0 || latency_ms > 50.0) {
                throw std::invalid_argument("Invalid realtime latency.");
            }
        } catch (const std::exception&) {
            MessageBoxW(state.window,
                        L"Enter a capture duration up to 300 seconds and a live latency from 20 to 50 ms.",
                        L"Transcribe", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    const std::filesystem::path input_path(input_text);
    const std::filesystem::path model_path(model_text);
    const std::filesystem::path musicxml_path(musicxml_text);
    const std::filesystem::path tab_path(tab_text);
    const std::filesystem::path midi_path(midi_text);
    save_preferences_or_warn(state);
    state.busy = true;
    set_form_enabled(state, false);
    auto input_extension = input_path.extension().wstring();
    std::transform(input_extension.begin(), input_extension.end(), input_extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    const bool streaming_wav = !use_asio && model_path.empty() && input_extension == L".wav";
    SetWindowTextW(state.status, use_asio ? L"Capturing from ASIO..."
                          : streaming_wav ? L"Streaming and analyzing WAV..."
                                          : L"Loading and analyzing audio...");
    SendMessageW(state.progress, PBM_SETPOS, 0, 0);

    transcribe::gui::Request request;
    request.use_asio = use_asio;
    request.polyphonic = polyphonic;
    request.capture_seconds = capture_seconds;
    request.realtime_options.latency_ms = latency_ms;
    request.guitar_tab_options.tuning =
        tunings[static_cast<std::size_t>(tuning_selection)];
    request.guitar_tab_options.capo_fret = capo_fret;
    request.guitar_tab_options.alternative_fingerings =
        static_cast<std::size_t>(tab_alternatives);
    request.preserve_expressive_timing =
        IsDlgButtonChecked(state.window, id_expressive_timing) == BST_CHECKED;
    request.input_path = input_path;
    request.model_path = model_path;
    request.musicxml_path = musicxml_path;
    request.tab_path = tab_path;
    request.midi_path = midi_path;

    state.worker = std::jthread([window = state.window, request = std::move(request)]
                                (std::stop_token stop_token) {
        auto result = std::make_unique<WorkResult>();
        try {
            const auto progress_callback = [window](double progress, const std::string&) {
                PostMessageW(window, message_progress, static_cast<WPARAM>(
                    std::clamp(static_cast<int>(progress * 100.0), 0, 100)), 0);
            };
            const auto live_preview_callback =
                [window](double offset, const std::vector<transcribe::Note>& notes,
                         const std::vector<transcribe::NoteEvent>& events) {
                    std::wostringstream text;
                    text << L"Detected notes:\r\n";
                    const std::size_t first = notes.size() > 35 ? notes.size() - 35 : 0;
                    for (std::size_t i = first; i < notes.size(); ++i) {
                        text << std::fixed << std::setprecision(2)
                             << offset + notes[i].start_seconds << L"s  "
                             << to_wide(notes[i].name.c_str()) << L"\r\n";
                    }
                    if (!events.empty()) {
                        text << L"\r\nRealtime events:\r\n";
                        const std::size_t event_start =
                            events.size() > 5 ? events.size() - 5 : 0;
                        for (std::size_t i = event_start; i < events.size(); ++i) {
                            text << (events[i].type == transcribe::NoteEventType::Onset
                                         ? L"Note on  "
                                         : L"Note off ")
                                 << std::fixed << std::setprecision(2)
                                 << events[i].time_seconds << L"s  "
                                 << to_wide(transcribe::midi_note_name(
                                                events[i].midi_pitch)
                                                .c_str())
                                 << L"  "
                                 << std::setprecision(0)
                                 << std::max(0.0, events[i].detected_at_seconds -
                                                      events[i].time_seconds) *
                                        1000.0
                                 << L" ms analysis"
                                 << L"\r\n";
                        }
                    }
                    auto preview = std::make_unique<LivePreview>();
                    preview->text = text.str();
                    if (PostMessageW(window, message_live_preview, 0,
                                     reinterpret_cast<LPARAM>(preview.get()))) {
                        preview.release();
                    }
                };
            const auto transcription = transcribe::gui::run_transcription(
                request, stop_token, progress_callback, live_preview_callback);
            const auto& notes = transcription.notes;
            const auto& metadata = transcription.metadata;
            result->success = true;
            std::wostringstream summary;
            summary << L"Detected " << notes.size() << L" notes. Tempo: ";
            if (metadata.tempo_inferred) {
                summary << std::fixed << std::setprecision(1) << metadata.tempo_bpm
                        << L" BPM (confidence " << std::setprecision(2)
                        << metadata.tempo_confidence << L')';
            } else {
                summary << L"uncertain; 120 BPM default";
            }
            summary << L". Meter: ";
            if (metadata.meter_inferred) {
                summary << metadata.time_numerator << L'/'
                        << metadata.time_denominator << L" (confidence "
                        << std::setprecision(2) << metadata.meter_confidence << L')';
            } else {
                summary << L"uncertain; 4/4 default";
            }
            summary << L". Key: ";
            if (metadata.key_inferred) {
                summary << key_name(metadata) << L" (confidence "
                        << std::setprecision(2) << metadata.key_confidence << L')';
            } else {
                summary << L"uncertain; C major default";
            }
            summary << L".\r\nMusicXML: " << request.musicxml_path.wstring()
                    << L"\r\nGuitar tab: " << request.tab_path.wstring();
            if (!request.midi_path.empty()) {
                summary << L"\r\nMIDI: " << request.midi_path.wstring();
            }
            result->message = summary.str();
            std::wostringstream note_list;
            const std::size_t shown = std::min<std::size_t>(notes.size(), 80);
            for (std::size_t index = 0; index < shown; ++index) {
                note_list << std::fixed << std::setprecision(2)
                          << notes[index].start_seconds << L"s  "
                          << to_wide(notes[index].name.c_str()) << L"  "
                          << notes[index].duration_seconds << L"s  confidence "
                          << notes[index].confidence << L"\r\n";
            }
            if (notes.size() > shown) {
                note_list << L"... " << notes.size() - shown << L" more notes";
            }
            result->preview = note_list.str();
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

        create_label(*state, L"Audio file", 66);
        state->input = create_control(*state, L"EDIT", L"",
                                      WS_BORDER | ES_AUTOHSCROLL, 175, 62, 455, 25, id_input);
        state->input_browse = create_control(*state, L"BUTTON", L"Browse...",
                                             BS_PUSHBUTTON, 640, 61, 90, 27, id_input_browse);

        state->duration_label = create_control(*state, L"STATIC", L"Capture seconds",
                                               SS_LEFT, 20, 106, 145, 24, id_duration_label);
        state->duration = create_control(*state, L"EDIT", L"10",
                                         WS_BORDER | ES_AUTOHSCROLL,
                                         175, 102, 120, 25, id_duration);
        create_control(*state, L"STATIC", L"Live latency (ms)", SS_LEFT,
                       330, 106, 120, 24, 0);
        state->realtime_latency = create_control(
            *state, L"EDIT", L"40", WS_BORDER | ES_AUTOHSCROLL,
            455, 102, 80, 25, id_latency);

        create_label(*state, L"ONNX model (optional)", 146);
        state->model = create_control(*state, L"EDIT", L"",
                                      WS_BORDER | ES_AUTOHSCROLL, 175, 142, 455, 25, id_model);
        state->model_browse = create_control(*state, L"BUTTON", L"Browse...",
                                             BS_PUSHBUTTON, 640, 141, 90, 27, id_model_browse);
#ifndef TRANSCRIBE_ENABLE_ONNX
        EnableWindow(state->model, FALSE);
        EnableWindow(state->model_browse, FALSE);
#endif

        state->polyphonic = create_control(*state, L"BUTTON", L"Polyphonic ONNX",
                                           BS_AUTOCHECKBOX, 175, 171, 180, 24,
                                           id_polyphonic);
#ifndef TRANSCRIBE_ENABLE_ONNX
        EnableWindow(state->polyphonic, FALSE);
#endif

        create_label(*state, L"MusicXML output", 210);
        state->musicxml = create_control(*state, L"EDIT", L"transcription.musicxml",
                                         WS_BORDER | ES_AUTOHSCROLL, 175, 206, 455, 25, id_musicxml);
        state->musicxml_browse = create_control(*state, L"BUTTON", L"Save as...",
                                                BS_PUSHBUTTON, 640, 205, 90, 27, id_musicxml_browse);

        create_label(*state, L"Guitar tab output", 250);
        state->tab = create_control(*state, L"EDIT", L"transcription.txt",
                                    WS_BORDER | ES_AUTOHSCROLL, 175, 246, 455, 25, id_tab);
        state->tab_browse = create_control(*state, L"BUTTON", L"Save as...",
                                           BS_PUSHBUTTON, 640, 245, 90, 27, id_tab_browse);

        create_label(*state, L"MIDI output (optional)", 290);
        state->midi = create_control(*state, L"EDIT", L"",
                                     WS_BORDER | ES_AUTOHSCROLL, 175, 286, 455, 25, id_midi);
        state->midi_browse = create_control(*state, L"BUTTON", L"Save as...",
                                            BS_PUSHBUTTON, 640, 285, 90, 27, id_midi_browse);

        create_label(*state, L"Guitar tuning", 335);
        state->guitar_tuning = create_control(
            *state, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            175, 331, 190, 150, id_guitar_tuning);
        for (const wchar_t* tuning : {L"Standard", L"Drop D", L"DADGAD",
                                      L"Open D", L"Open G"}) {
            SendMessageW(state->guitar_tuning, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(tuning));
        }
        SendMessageW(state->guitar_tuning, CB_SETCURSEL, 0, 0);
        create_control(*state, L"STATIC", L"Capo fret", SS_LEFT,
                       390, 335, 70, 24, 0);
        state->capo = create_control(*state, L"EDIT", L"0",
                                     WS_BORDER | ES_AUTOHSCROLL,
                                     465, 331, 65, 25, id_capo);
        create_control(*state, L"STATIC", L"Tab alternatives (0-5)", SS_LEFT,
                       20, 374, 145, 24, 0);
        state->tab_alternatives = create_control(
            *state, L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL,
            175, 370, 65, 25, id_tab_alternatives);
        state->expressive_timing = create_control(
            *state, L"BUTTON", L"Annotate expressive timing in MusicXML",
            BS_AUTOCHECKBOX, 390, 370, 310, 24, id_expressive_timing);

        state->transcribe = create_control(*state, L"BUTTON", L"Transcribe",
                                           BS_DEFPUSHBUTTON, 175, 410, 140, 34, id_transcribe);
        state->open_output = create_control(*state, L"BUTTON", L"Open output folder",
                                            BS_PUSHBUTTON, 325, 410, 170, 34, id_open_output);
        state->progress = create_control(*state, PROGRESS_CLASSW, L"",
                                         PBS_SMOOTH, 175, 457, 555, 20, id_progress);
        SendMessageW(state->progress, PBM_SETRANGE32, 0, 100);
        state->status = create_control(*state, L"STATIC", L"Ready.",
                                       SS_LEFT, 175, 484, 555, 24, 0);
        state->preview = create_control(*state, L"EDIT", L"",
                                        WS_BORDER | ES_MULTILINE | ES_READONLY |
                                            ES_AUTOVSCROLL | WS_VSCROLL,
                                        175, 514, 555, 124, id_preview);
        load_preferences(*state);
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
#ifdef TRANSCRIBE_ENABLE_FFMPEG
            browse_for_file(window, state->input, false,
                            L"Audio files (*.wav;*.mp3;*.flac;*.ogg;*.m4a)\0*.wav;*.mp3;*.flac;*.ogg;*.m4a\0All files (*.*)\0*.*\0\0",
                            nullptr);
#else
            browse_for_file(window, state->input, false,
                            L"Wave audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0\0", nullptr);
#endif
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
        case id_midi_browse:
            browse_for_file(window, state->midi, true,
                            L"MIDI files (*.mid)\0*.mid\0All files (*.*)\0*.*\0\0", L"mid");
            return 0;
        case id_open_output: {
            auto folder = std::filesystem::path(get_text(state->musicxml)).parent_path();
            if (folder.empty()) {
                folder = std::filesystem::current_path();
            }
            const auto result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                MessageBoxW(window, L"Could not open the output folder.", L"Transcribe",
                            MB_OK | MB_ICONERROR);
            }
            return 0;
        }
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
            SetWindowTextW(state->preview, result->preview.c_str());
            SendMessageW(state->progress, PBM_SETPOS, 100, 0);
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
    case message_progress:
        SendMessageW(state->progress, PBM_SETPOS, wparam, 0);
        {
            const std::wstring status =
                L"Analyzing audio... " + std::to_wstring(static_cast<int>(wparam)) + L"%";
            SetWindowTextW(state->status, status.c_str());
        }
        return 0;
    case message_live_preview: {
        std::unique_ptr<LivePreview> preview(reinterpret_cast<LivePreview*>(lparam));
        if (state->busy) {
            SetWindowTextW(state->preview, preview->text.c_str());
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        std::array<wchar_t, 32768> dropped_path{};
        if (DragQueryFileW(drop, 0, dropped_path.data(),
                           static_cast<UINT>(dropped_path.size())) > 0) {
#ifndef TRANSCRIBE_ENABLE_FFMPEG
            const auto extension = std::filesystem::path(dropped_path.data()).extension();
            if (extension != L".wav" && extension != L".WAV") {
                MessageBoxW(window, L"This build accepts WAV files only.", L"Transcribe",
                            MB_OK | MB_ICONINFORMATION);
                DragFinish(drop);
                return 0;
            }
#endif
            if (!state->busy) {
                SendMessageW(state->mode, CB_SETCURSEL, 0, 0);
                set_mode_controls(*state);
                SetWindowTextW(state->input, dropped_path.data());
                SetWindowTextW(state->status, L"Audio file selected.");
            }
        }
        DragFinish(drop);
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
        save_preferences_or_warn(*state);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&common_controls)) {
        MessageBoxW(nullptr, L"Could not initialize common controls.", L"Transcribe",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
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

    RECT bounds{0, 0, 760, 660};
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

    DragAcceptFiles(window, TRUE);
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
