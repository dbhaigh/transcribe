#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> batch_directory;
    std::filesystem::path musicxml{"transcription.musicxml"};
    std::filesystem::path tab{"transcription.txt"};
    std::optional<std::filesystem::path> midi;
    std::optional<std::filesystem::path> json;
    std::filesystem::path model;
    std::filesystem::path output_directory;
    transcribe::GuitarTabOptions guitar_tab;
    std::optional<double> tempo_bpm;
    std::optional<int> key_fifths;
    std::optional<bool> minor_key;
    std::optional<std::array<int, 2>> meter;
    double capture_seconds{};
    double realtime_latency_ms{40.0};
    double minimum_confidence{0.25};
    transcribe::InstrumentPrior instrument{transcribe::InstrumentPrior::None};
    transcribe::OnnxExecutionProvider onnx_provider{
        transcribe::OnnxExecutionProvider::Cpu};
    int device_id{};
    bool asio{};
    bool polyphonic{};
    bool fast_preview{};
    bool benchmark{};
    bool preserve_expressive_timing{};
    bool quiet{};
    bool help{};
};

void print_usage(std::ostream& output) {
    output <<     "Transcriber 0.1.4 - turn audio into MusicXML, MIDI, and guitar tablature\n\n"
              "Usage:\n"
              "  transcriber <input.wav> [options]\n"
              "  transcriber --asio <seconds> [options]\n"
              "  transcriber --live <seconds> [options] (alias for --asio)\n"
              "  transcriber --batch <directory> [options]\n\n"
              "Options:\n"
              "  -h, --help                  Show this help\n"
              "  --musicxml <path>           MusicXML output path\n"
              "  --tab <path>                Guitar tablature output path\n"
              "  --midi <path>               Optional Standard MIDI output path\n"
              "  --json <path>               Write notes and analysis metadata as JSON\n"
              "  --model <path.onnx>         Use an ONNX model instead of built-in YIN\n"
              "  --provider <name>           ONNX provider: cpu, cuda, directml, or rocm\n"
              "  --device-id <number>        GPU device index for the selected provider\n"
              "  --polyphonic                Enable multi-pitch analysis (built-in or ONNX)\n"
              "  --instrument <name>         Apply guitar, piano, or voice pitch/polyphony prior\n"
              "  --tuning <name>             Guitar tuning: standard, drop-d, dadgad, open-d, open-g\n"
              "  --capo <fret>               Guitar capo fret (0-24)\n"
              "  --tab-alternatives <count>  Include up to 5 alternative chord fingerings\n"
              "  --preserve-expressive-timing Annotate unquantized note-onset offsets in MusicXML\n"
              "  --fast-preview              Use lower-resolution analysis for a quick rough score\n"
              "  --quality <high|fast>       Select normal or fast-preview built-in analysis\n"
              "  --tempo <40..240>           Override the score tempo in BPM\n"
              "  --key-fifths <-7..7>        Override the key signature (circle of fifths)\n"
              "  --mode <major|minor>        Set the key mode\n"
              "  --meter <3/4|4/4|6/8>        Override the score time signature\n"
              "  --latency-ms <20..50>       ASIO live-analysis latency target (default: 40)\n"
              "  --minimum-confidence <0..1> Drop notes below this confidence\n"
              "  --benchmark                 Report analysis time and real-time factor\n"
              "  --config <path>             Load defaults from simple key=value config\n"
              "  --output-dir <directory>    Batch output directory (default: <input>/transcriber-output)\n"
              "  --quiet                     Suppress progress and per-file summaries\n\n"
              "Examples:\n"
              "  transcriber guitar.wav --musicxml score.musicxml --tab guitar.txt --midi take.mid\n"
              "  transcriber guitar.wav --model poly.onnx --polyphonic --json notes.json\n"
              "  transcriber --batch samples --output-dir results --benchmark\n"
              "  transcriber --asio 8 --musicxml live.musicxml --tab live.txt\n";
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_config_bool(const std::string& value, const std::string& key) {
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    throw std::invalid_argument(
        "The " + key + " config value must be true, false, 1, or 0.");
}

transcribe::OnnxExecutionProvider parse_onnx_provider(const std::string& value) {
    if (value == "cpu") {
        return transcribe::OnnxExecutionProvider::Cpu;
    }
    if (value == "cuda") {
        return transcribe::OnnxExecutionProvider::Cuda;
    }
    if (value == "directml") {
        return transcribe::OnnxExecutionProvider::DirectML;
    }
    if (value == "rocm") {
        return transcribe::OnnxExecutionProvider::Rocm;
    }
    throw std::invalid_argument(
        "ONNX provider must be one of: cpu, cuda, directml, rocm.");
}

std::array<int, 6> parse_guitar_tuning(const std::string& value) {
    if (value == "standard") return {64, 59, 55, 50, 45, 40};
    if (value == "drop-d") return {64, 59, 55, 50, 45, 38};
    if (value == "dadgad") return {62, 57, 55, 50, 45, 38};
    if (value == "open-d") return {62, 57, 54, 50, 45, 38};
    if (value == "open-g") return {62, 59, 55, 50, 43, 38};
    throw std::invalid_argument(
        "Tuning must be one of: standard, drop-d, dadgad, open-d, open-g.");
}

std::array<int, 2> parse_meter(const std::string& value) {
    if (value == "3/4") return {3, 4};
    if (value == "4/4") return {4, 4};
    if (value == "6/8") return {6, 8};
    throw std::invalid_argument("Meter must be one of: 3/4, 4/4, 6/8.");
}

std::map<std::string, std::string> read_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Could not open config file: " + path.string());
    }
    std::map<std::string, std::string> values;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = trim(std::move(line));
        if (line.empty() || line.front() == '[') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Invalid config entry on line " +
                                     std::to_string(line_number) + ".");
        }
        values[trim(line.substr(0, separator))] = trim(line.substr(separator + 1));
    }
    return values;
}

void apply_config(Options& options, const std::map<std::string, std::string>& values) {
    static constexpr std::array<std::string_view, 27> supported_keys{
        "input", "musicxml", "tab", "midi", "json", "model", "provider",
        "device-id", "latency-ms", "tempo", "key-fifths", "mode", "meter",
        "quality", "batch", "output-dir", "asio", "tuning", "capo",
        "tab-alternatives", "preserve-expressive-timing", "fast-preview",
        "minimum-confidence", "polyphonic", "instrument", "benchmark", "quiet"};
    for (const auto& [key, value] : values) {
        (void)value;
        if (std::find(supported_keys.begin(), supported_keys.end(), key) ==
            supported_keys.end()) {
            throw std::invalid_argument("Unknown config key: " + key + ".");
        }
    }
    const auto set_path = [&values](const char* key, auto& destination) {
        const auto found = values.find(key);
        if (found != values.end()) {
            destination = found->second;
        }
    };
    set_path("input", options.input);
    set_path("batch", options.batch_directory);
    set_path("output-dir", options.output_directory);
    set_path("musicxml", options.musicxml);
    set_path("tab", options.tab);
    set_path("midi", options.midi);
    set_path("json", options.json);
    set_path("model", options.model);
    const auto provider = values.find("provider");
    if (provider != values.end()) {
        options.onnx_provider = parse_onnx_provider(provider->second);
    }
    const auto device_id = values.find("device-id");
    if (device_id != values.end()) {
        options.device_id = std::stoi(device_id->second);
    }
    const auto latency_ms = values.find("latency-ms");
    if (latency_ms != values.end()) {
        options.realtime_latency_ms = std::stod(latency_ms->second);
    }
    const auto asio = values.find("asio");
    if (asio != values.end()) {
        options.asio = true;
        options.capture_seconds = std::stod(asio->second);
    }
    const auto tempo = values.find("tempo");
    if (tempo != values.end()) {
        options.tempo_bpm = std::stod(tempo->second);
    }
    const auto key_fifths = values.find("key-fifths");
    if (key_fifths != values.end()) {
        options.key_fifths = std::stoi(key_fifths->second);
    }
    const auto mode = values.find("mode");
    if (mode != values.end()) {
        if (mode->second == "major") {
            options.minor_key = false;
        } else if (mode->second == "minor") {
            options.minor_key = true;
        } else {
            throw std::invalid_argument("Mode must be either major or minor.");
        }
    }
    const auto meter = values.find("meter");
    if (meter != values.end() && meter->second != "auto") {
        options.meter = parse_meter(meter->second);
    }
    const auto quality = values.find("quality");
    if (quality != values.end()) {
        if (quality->second == "high") {
            options.fast_preview = false;
        } else if (quality->second == "fast") {
            options.fast_preview = true;
        } else {
            throw std::invalid_argument("Quality must be either high or fast.");
        }
    }
    const auto tuning = values.find("tuning");
    if (tuning != values.end()) {
        options.guitar_tab.tuning = parse_guitar_tuning(tuning->second);
    }
    const auto capo = values.find("capo");
    if (capo != values.end()) {
        options.guitar_tab.capo_fret = std::stoi(capo->second);
    }
    const auto alternatives = values.find("tab-alternatives");
    if (alternatives != values.end()) {
        options.guitar_tab.alternative_fingerings =
            static_cast<std::size_t>(std::stoul(alternatives->second));
    }
    const auto expressive_timing = values.find("preserve-expressive-timing");
    if (expressive_timing != values.end()) {
        options.preserve_expressive_timing =
            parse_config_bool(expressive_timing->second,
                              "preserve-expressive-timing");
    }
    const auto fast_preview = values.find("fast-preview");
    if (fast_preview != values.end()) {
        options.fast_preview =
            parse_config_bool(fast_preview->second, "fast-preview");
    }
    const auto confidence = values.find("minimum-confidence");
    if (confidence != values.end()) {
        options.minimum_confidence = std::stod(confidence->second);
    }
    const auto polyphonic = values.find("polyphonic");
    if (polyphonic != values.end()) {
        options.polyphonic = parse_config_bool(polyphonic->second, "polyphonic");
    }
    const auto instrument = values.find("instrument");
    if (instrument != values.end()) {
        if (instrument->second == "guitar") {
            options.instrument = transcribe::InstrumentPrior::Guitar;
        } else if (instrument->second == "piano") {
            options.instrument = transcribe::InstrumentPrior::Piano;
        } else if (instrument->second == "voice") {
            options.instrument = transcribe::InstrumentPrior::Voice;
        } else if (instrument->second != "none") {
            throw std::invalid_argument(
                "Instrument must be one of: none, guitar, piano, voice.");
        }
    }
    const auto benchmark = values.find("benchmark");
    if (benchmark != values.end()) {
        options.benchmark = parse_config_bool(benchmark->second, "benchmark");
    }
    const auto quiet = values.find("quiet");
    if (quiet != values.end()) {
        options.quiet = parse_config_bool(quiet->second, "quiet");
    }
}

std::string next_value(int& index, int argc, char* argv[], const std::string& option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument("Missing value after " + option + ".");
    }
    return argv[++index];
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    std::vector<std::filesystem::path> config_paths;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            config_paths.emplace_back(
                next_value(i, argc, argv, "--config"));
        }
    }
    for (const auto& config_path : config_paths) {
        apply_config(options, read_config(config_path));
    }

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--config") {
            (void)next_value(i, argc, argv, argument);
        } else if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "--musicxml") {
            options.musicxml = next_value(i, argc, argv, argument);
        } else if (argument == "--tab") {
            options.tab = next_value(i, argc, argv, argument);
        } else if (argument == "--midi") {
            options.midi = next_value(i, argc, argv, argument);
        } else if (argument == "--json") {
            options.json = next_value(i, argc, argv, argument);
        } else if (argument == "--model") {
            options.model = next_value(i, argc, argv, argument);
        } else if (argument == "--provider") {
            options.onnx_provider =
                parse_onnx_provider(next_value(i, argc, argv, argument));
        } else if (argument == "--device-id") {
            options.device_id = std::stoi(next_value(i, argc, argv, argument));
        } else if (argument == "--minimum-confidence") {
            options.minimum_confidence = std::stod(next_value(i, argc, argv, argument));
        } else if (argument == "--latency-ms") {
            options.realtime_latency_ms = std::stod(next_value(i, argc, argv, argument));
        } else if (argument == "--tempo") {
            options.tempo_bpm = std::stod(next_value(i, argc, argv, argument));
        } else if (argument == "--key-fifths") {
            options.key_fifths = std::stoi(next_value(i, argc, argv, argument));
        } else if (argument == "--mode") {
            const auto mode = next_value(i, argc, argv, argument);
            if (mode == "major") {
                options.minor_key = false;
            } else if (mode == "minor") {
                options.minor_key = true;
            } else {
                throw std::invalid_argument("Mode must be either major or minor.");
            }
        } else if (argument == "--meter") {
            const auto meter = next_value(i, argc, argv, argument);
            options.meter = meter == "auto"
                                ? std::nullopt
                                : std::optional(parse_meter(meter));
        } else if (argument == "--quality") {
            const auto quality = next_value(i, argc, argv, argument);
            if (quality == "high") {
                options.fast_preview = false;
            } else if (quality == "fast") {
                options.fast_preview = true;
            } else {
                throw std::invalid_argument("Quality must be either high or fast.");
            }
        } else if (argument == "--tuning") {
            options.guitar_tab.tuning =
                parse_guitar_tuning(next_value(i, argc, argv, argument));
        } else if (argument == "--capo") {
            options.guitar_tab.capo_fret =
                std::stoi(next_value(i, argc, argv, argument));
        } else if (argument == "--tab-alternatives") {
            options.guitar_tab.alternative_fingerings =
                static_cast<std::size_t>(
                    std::stoul(next_value(i, argc, argv, argument)));
        } else if (argument == "--preserve-expressive-timing") {
            options.preserve_expressive_timing = true;
        } else if (argument == "--polyphonic") {
            options.polyphonic = true;
        } else if (argument == "--fast-preview") {
            options.fast_preview = true;
        } else if (argument == "--instrument") {
            const auto instrument = next_value(i, argc, argv, argument);
            if (instrument == "guitar") {
                options.instrument = transcribe::InstrumentPrior::Guitar;
            } else if (instrument == "piano") {
                options.instrument = transcribe::InstrumentPrior::Piano;
            } else if (instrument == "voice") {
                options.instrument = transcribe::InstrumentPrior::Voice;
            } else if (instrument == "none") {
                options.instrument = transcribe::InstrumentPrior::None;
            } else {
                throw std::invalid_argument(
                    "Instrument must be one of: none, guitar, piano, voice.");
            }
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else if (argument == "--batch") {
            options.batch_directory = next_value(i, argc, argv, argument);
        } else if (argument == "--output-dir") {
            options.output_directory = next_value(i, argc, argv, argument);
        } else if (argument == "--asio" || argument == "--live") {
            options.asio = true;
            options.capture_seconds = std::stod(next_value(i, argc, argv, argument));
        } else if (argument.starts_with('-')) {
            throw std::invalid_argument("Unknown option: " + argument);
        } else if (!options.input) {
            options.input = argument;
        } else {
            throw std::invalid_argument("Unexpected extra input path: " + argument);
        }
    }

    if (!(options.minimum_confidence >= 0.0 && options.minimum_confidence <= 1.0)) {
        throw std::invalid_argument("Minimum confidence must be between 0 and 1.");
    }
    if (options.fast_preview && !options.model.empty()) {
        throw std::invalid_argument(
            "--fast-preview currently applies only to the built-in analysis path.");
    }
    if (options.tempo_bpm &&
        (!std::isfinite(*options.tempo_bpm) || *options.tempo_bpm < 40.0 ||
         *options.tempo_bpm > 240.0)) {
        throw std::invalid_argument("--tempo must be between 40 and 240 BPM.");
    }
    if (options.key_fifths &&
        (*options.key_fifths < -7 || *options.key_fifths > 7)) {
        throw std::invalid_argument("--key-fifths must be between -7 and 7.");
    }
    if (!std::isfinite(options.realtime_latency_ms) ||
        options.realtime_latency_ms < 20.0 || options.realtime_latency_ms > 50.0) {
        throw std::invalid_argument("--latency-ms must be between 20 and 50.");
    }
    if (options.device_id < 0) {
        throw std::invalid_argument("--device-id must be non-negative.");
    }
    if (options.onnx_provider != transcribe::OnnxExecutionProvider::Cpu &&
        options.model.empty()) {
        throw std::invalid_argument("--provider requires --model.");
    }
    if (options.asio && (options.capture_seconds <= 0.0 || options.capture_seconds > 300.0)) {
        throw std::invalid_argument("ASIO capture duration must be greater than 0 and at most 300 seconds.");
    }
    if (options.asio && options.batch_directory) {
        throw std::invalid_argument("--asio cannot be combined with --batch.");
    }
    return options;
}

std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20) {
                escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(character) << std::dec;
            } else {
                escaped << character;
            }
        }
    }
    return escaped.str();
}

std::string key_name(const transcribe::ScoreMetadata& metadata) {
    static constexpr std::array<const char*, 15> major_names{
        "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C",
        "G", "D", "A", "E", "B", "F#", "C#"};
    static constexpr std::array<const char*, 15> minor_names{
        "Ab", "Eb", "Bb", "F", "C", "G", "D", "A",
        "E", "B", "F#", "C#", "G#", "D#", "A#"};
    const int fifths = std::clamp(metadata.key_fifths, -7, 7);
    const auto index = static_cast<std::size_t>(fifths + 7);
    return std::string(metadata.minor_key ? minor_names[index] : major_names[index]) +
           (metadata.minor_key ? " minor" : " major");
}

void write_json(const std::filesystem::path& path, const std::vector<transcribe::Note>& notes,
                const transcribe::ScoreMetadata& metadata) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create JSON output: " + path.string());
    }
    output << "{\n  \"metadata\": {\"tempo_bpm\": " << metadata.tempo_bpm
           << ", \"tempo_confidence\": " << metadata.tempo_confidence
           << ", \"tempo_inferred\": "
           << (metadata.tempo_inferred ? "true" : "false")
           << ", \"time_signature\": \"" << metadata.time_numerator << '/'
           << metadata.time_denominator << "\", \"meter_confidence\": "
           << metadata.meter_confidence << ", \"meter_inferred\": "
           << (metadata.meter_inferred ? "true" : "false")
           << ", \"key\": \"" << json_escape(key_name(metadata))
           << "\", \"key_confidence\": " << metadata.key_confidence
           << ", \"key_inferred\": " << (metadata.key_inferred ? "true" : "false")
           << "},\n  \"notes\": [\n";
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto& note = notes[index];
        output << "    {\"midi\": " << note.midi_pitch << ", \"name\": \""
               << json_escape(note.name) << "\", \"start\": " << note.start_seconds
               << ", \"duration\": " << note.duration_seconds << ", \"confidence\": "
               << note.confidence << "}" << (index + 1 == notes.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    if (!output) {
        throw std::runtime_error("Failed while writing JSON output.");
    }
}

std::string progress_bar(double progress) {
    constexpr std::size_t width = 32;
    const auto filled = static_cast<std::size_t>(
        std::clamp(progress, 0.0, 1.0) * width);
    return "[" + std::string(filled, '#') + std::string(width - filled, ' ') + "] " +
           std::to_string(static_cast<int>(progress * 100.0)) + "%";
}

void process_input(const std::filesystem::path& input_path, const Options& options,
                   const std::filesystem::path& musicxml_path,
                   const std::filesystem::path& tab_path,
                   const std::optional<std::filesystem::path>& midi_path,
                   const std::optional<std::filesystem::path>& json_path) {
    const auto report_progress = [&options](double progress) {
        if (!options.quiet) {
            std::cerr << '\r' << progress_bar(progress) << std::flush;
        }
    };
    std::vector<transcribe::Note> notes;
    double audio_seconds = 0.0;
    double maximum_realtime_latency_ms = 0.0;
    transcribe::AnalysisTimings stage_timings;
    const auto accumulate_timings = [&stage_timings](
                                        const transcribe::AnalysisTimings& timings) {
        stage_timings.preprocessing_seconds += timings.preprocessing_seconds;
        stage_timings.onset_fft_seconds += timings.onset_fft_seconds;
        stage_timings.resampling_seconds += timings.resampling_seconds;
        stage_timings.pitch_analysis_seconds += timings.pitch_analysis_seconds;
        stage_timings.spectrogram_cache_hit =
            stage_timings.spectrogram_cache_hit || timings.spectrogram_cache_hit;
    };
    const auto analysis_started = std::chrono::steady_clock::now();
    auto extension = input_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    const bool streaming_wav =
        !options.asio && options.model.empty() && extension == ".wav";
    if (streaming_wav) {
        transcribe::AnalysisOptions analysis_options;
        analysis_options.minimum_confidence = options.minimum_confidence;
        analysis_options.fast_preview = options.fast_preview;
        analysis_options.polyphonic = options.polyphonic;
        analysis_options.instrument = options.instrument;
        if (options.benchmark) {
            analysis_options.timing_callback = accumulate_timings;
        }
        if (!options.quiet) {
            analysis_options.progress_callback = report_progress;
        }
        transcribe::StreamingOptions stream_options;
        stream_options.analysis = std::move(analysis_options);
        auto result = transcribe::transcribe_wav_streaming(input_path, stream_options);
        notes = std::move(result.notes);
        audio_seconds = result.duration_seconds;
    } else {
        transcribe::RealtimeOptions realtime_options;
        realtime_options.latency_ms = options.realtime_latency_ms;
        transcribe::AudioBuffer audio = options.asio
            ? transcribe::capture_asio(
                  options.capture_seconds, {},
                  [&options](double progress) {
                      if (!options.quiet) {
                          std::cerr << '\r' << progress_bar(progress) << std::flush;
                      }
                  },
                  {},
                  [&options, &maximum_realtime_latency_ms](
                      const std::vector<transcribe::Note>&,
                      const std::vector<transcribe::NoteEvent>& events) {
                      for (const auto& event : events) {
                          maximum_realtime_latency_ms = std::max(
                              maximum_realtime_latency_ms,
                              std::max(0.0, event.detected_at_seconds -
                                                event.time_seconds) *
                                  1000.0);
                      }
                      if (options.quiet) {
                          return;
                      }
                      for (const auto& event : events) {
                          std::cerr << "\n[realtime] "
                                    << (event.type == transcribe::NoteEventType::Onset
                                            ? "note-on "
                                            : "note-off ")
                                    << transcribe::midi_note_name(event.midi_pitch)
                                    << " at " << std::fixed << std::setprecision(3)
                                    << event.time_seconds << "s (analysis latency "
                                    << std::setprecision(1)
                                    << std::max(0.0, event.detected_at_seconds -
                                                        event.time_seconds) *
                                           1000.0
                                    << " ms)\n";
                      }
                  },
                  realtime_options)
            : transcribe::read_audio(input_path);
        audio_seconds = static_cast<double>(audio.samples.size()) / audio.sample_rate;
        if (options.model.empty()) {
            transcribe::AnalysisOptions analysis_options;
            analysis_options.minimum_confidence = options.minimum_confidence;
            analysis_options.fast_preview = options.fast_preview;
            analysis_options.polyphonic = options.polyphonic;
            analysis_options.instrument = options.instrument;
            if (options.benchmark) {
                analysis_options.timing_callback = accumulate_timings;
            }
            if (!options.quiet) {
                analysis_options.progress_callback = report_progress;
            }
            notes = transcribe::transcribe_monophonic(audio, analysis_options);
        } else {
#ifdef TRANSCRIBE_ENABLE_ONNX
            transcribe::OnnxOptions model_options;
            model_options.polyphonic = options.polyphonic;
            model_options.independent_probabilities = options.polyphonic;
            model_options.minimum_confidence = options.minimum_confidence;
            model_options.execution_provider = options.onnx_provider;
            model_options.device_id = options.device_id;
            if (!options.quiet) {
                model_options.progress_callback = report_progress;
            }
            notes = transcribe::transcribe_with_onnx(audio, options.model, model_options);
#else
            throw std::runtime_error(
                "ONNX support is disabled. Configure with -DTRANSCRIBE_ENABLE_ONNX=ON.");
#endif
        }
    }
    const double analysis_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - analysis_started).count();
    if (!options.quiet) {
        std::cerr << '\n';
    }
    if (notes.empty()) {
        throw std::runtime_error("No stable pitches were detected in " + input_path.string() + ".");
    }

    auto metadata = transcribe::infer_score_metadata(notes);
    if (options.tempo_bpm) {
        metadata.tempo_bpm = *options.tempo_bpm;
        metadata.tempo_confidence = 1.0;
        metadata.tempo_inferred = true;
    }
    if (options.key_fifths) {
        metadata.key_fifths = *options.key_fifths;
        metadata.key_confidence = 1.0;
        metadata.key_inferred = true;
    }
    if (options.minor_key) {
        metadata.minor_key = *options.minor_key;
        metadata.key_confidence = 1.0;
        metadata.key_inferred = true;
    }
    if (options.meter) {
        metadata.time_numerator = (*options.meter)[0];
        metadata.time_denominator = (*options.meter)[1];
        metadata.meter_confidence = 1.0;
        metadata.meter_inferred = true;
    }
    transcribe::write_musicxml(musicxml_path, notes, metadata, options.guitar_tab,
                               options.preserve_expressive_timing);
    transcribe::write_guitar_tab(tab_path, notes, options.guitar_tab);
    if (midi_path) {
        transcribe::write_midi(*midi_path, notes, metadata);
    }
    if (json_path) {
        write_json(*json_path, notes, metadata);
    }

    if (!options.quiet) {
        std::cout << input_path.string() << ": " << notes.size() << " notes\n"
                  << "  Tempo: ";
        if (metadata.tempo_inferred) {
            std::cout << std::fixed << std::setprecision(1) << metadata.tempo_bpm
                      << " BPM (confidence " << std::setprecision(2)
                      << metadata.tempo_confidence << ')';
        } else {
            std::cout << "insufficient onset evidence (120 BPM default)";
        }
        std::cout << "\n  Meter: ";
        if (metadata.meter_inferred) {
            std::cout << metadata.time_numerator << '/' << metadata.time_denominator
                      << " (confidence " << std::setprecision(2)
                      << metadata.meter_confidence << ')';
        } else {
            std::cout << "uncertain (4/4 default)";
        }
        std::cout << "\n  Key: ";
        if (metadata.key_inferred) {
            std::cout << key_name(metadata) << " (confidence " << std::setprecision(2)
                      << metadata.key_confidence << ')';
        } else {
            std::cout << "uncertain (C major default)";
        }
        std::cout << '\n' << "  MusicXML: " << musicxml_path.string() << '\n'
                  << "  Guitar tab: " << tab_path.string() << '\n';
        if (midi_path) {
            std::cout << "  MIDI: " << midi_path->string() << '\n';
        }
        if (json_path) {
            std::cout << "  JSON: " << json_path->string() << '\n';
        }
        if (options.benchmark) {
            std::cout << std::fixed << std::setprecision(3)
                      << "  Analysis: " << analysis_seconds
                      << " s; real-time factor "
                      << analysis_seconds / std::max(audio_seconds, 1e-9) << "x\n";
            if (options.model.empty()) {
                std::cout << "  Stages: preprocess " << stage_timings.preprocessing_seconds
                          << " s; onset/FFT " << stage_timings.onset_fft_seconds
                          << " s; resample " << stage_timings.resampling_seconds
                          << " s; pitch/FFT " << stage_timings.pitch_analysis_seconds
                          << " s";
                if (options.polyphonic) {
                    std::cout << "; spectrogram cache "
                              << (stage_timings.spectrogram_cache_hit ? "hit" : "miss");
                }
                std::cout << '\n';
            }
            if (options.asio) {
                std::cout << "  Maximum realtime event analysis latency: "
                          << std::setprecision(1) << maximum_realtime_latency_ms
                          << " ms (input-time estimate)\n";
            }
        }
    }
}

void process_single(const Options& options) {
    if (!options.asio && !options.input) {
        throw std::invalid_argument("Provide an input WAV path, use --asio, or use --batch.");
    }
    const auto input_path = options.asio ? std::filesystem::path("ASIO input")
                                         : *options.input;
    process_input(input_path, options, options.musicxml, options.tab, options.midi,
                  options.json);
}

void process_batch(const Options& options) {
    if (!std::filesystem::is_directory(*options.batch_directory)) {
        throw std::invalid_argument("Batch input is not a directory: " +
                                    options.batch_directory->string());
    }
    const auto output_directory = options.output_directory.empty()
                                      ? *options.batch_directory / "transcriber-output"
                                      : options.output_directory;
    std::filesystem::create_directories(output_directory);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(*options.batch_directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
#ifdef TRANSCRIBE_ENABLE_FFMPEG
        if (extension == ".wav" || extension == ".mp3" || extension == ".flac" ||
            extension == ".ogg" || extension == ".m4a" || extension == ".aac") {
#else
        if (extension == ".wav") {
#endif
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        throw std::runtime_error("No supported audio files were found in the batch directory.");
    }
    std::size_t failures = 0;
    for (const auto& file : files) {
        const auto stem = file.stem().string();
        try {
            process_input(file, options, output_directory / (stem + ".musicxml"),
                          output_directory / (stem + ".tab.txt"),
                          options.midi ? std::optional(output_directory / (stem + ".mid"))
                                       : std::nullopt,
                          options.json ? std::optional(output_directory / (stem + ".json"))
                                       : std::nullopt);
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << file.string() << ": " << error.what() << '\n';
        }
    }
    if (failures > 0) {
        throw std::runtime_error(std::to_string(failures) + " of " +
                                 std::to_string(files.size()) + " files failed.");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        if (options.batch_directory) {
            process_batch(options);
        } else {
            process_single(options);
        }
    } catch (const std::exception& error) {
        std::cerr << "transcriber: " << error.what() << "\n\n";
        print_usage(std::cerr);
        return 1;
    }
    return 0;
}
