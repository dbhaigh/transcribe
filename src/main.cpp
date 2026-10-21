#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"
#include "transcribe/inference.hpp"
#include "transcribe/live.hpp"
#include "transcribe/score.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cctype>
#include <cmath>
#include <exception>
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
#include <thread>
#include <vector>

namespace {

struct Options {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> batch_directory;
    std::filesystem::path musicxml{"transcription.musicxml"};
    std::filesystem::path tab{"transcription.txt"};
    std::optional<std::filesystem::path> midi;
    std::optional<std::filesystem::path> json;
    std::optional<std::filesystem::path> score_project;
    std::filesystem::path model;
    std::string backend;
    std::filesystem::path output_directory;
    transcribe::GuitarTabOptions guitar_tab;
    std::optional<double> tempo_bpm;
    std::optional<int> key_fifths;
    std::optional<bool> minor_key;
    std::optional<std::array<int, 2>> meter;
    std::optional<double> pickup_quarters;
    int written_transposition_semitones{};
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
    bool live_pipeline{};
    std::optional<std::filesystem::path> live_events;
    bool refine{};
    std::string refine_backend{"builtin:spectral"};
    std::string refine_mode{"auto"};
    std::optional<std::filesystem::path> proposal_json;
};

void print_usage(std::ostream& output) {
    output <<     "Transcriber " << TRANSCRIBE_VERSION
           << " - turn audio into MusicXML, MIDI, and guitar tablature\n\n"
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
              "  --score-project <path>      Write a versioned editable score project\n"
              "  --model <path.onnx>         Use an ONNX model instead of built-in YIN\n"
              "  --backend <registry-id>     Opt-in offline model adapter (e.g. builtin:yin)\n"
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
              "  --meter <meter>              Override meter (e.g. 3/4, 4/4, 5/4, 6/8, 9/8)\n"
              "  --pickup <quarters>          Set an implicit pickup measure length\n"
              "  --written-transposition <n>  Written pitch offset in semitones (-48..48)\n"
              "  --latency-ms <20..50>       ASIO live-analysis latency target (default: 40)\n"
              "  --minimum-confidence <0..1> Drop notes below this confidence\n"
              "  --benchmark                 Report analysis time and real-time factor\n"
              "  --config <path>             Load defaults from simple key=value config\n"
              "  --output-dir <directory>    Batch output directory (default: <input>/transcriber-output)\n"
              "  --quiet                     Suppress progress and per-file summaries\n"
              "  --live-pipeline             Use the incremental live pipeline for WAV replay or --asio\n"
              "                              (--backend selects a windowed model; default incremental YIN)\n"
              "  --live-events <path>        Write the original live event stream as JSON lines\n"
              "  --refine                    After live analysis, run offline refinement\n"
              "  --refine-backend <id>       Refinement model (default: builtin:spectral)\n"
              "  --refine-mode <mode>        auto, full or windowed (auto: windowed for onnx:*)\n"
              "  --proposal-json <path>      Write the refinement proposal; live outputs are unchanged\n\n"
              "Examples:\n"
              "  transcriber guitar.wav --musicxml score.musicxml --tab guitar.txt --midi take.mid\n"
              "  transcriber guitar.wav --model poly.onnx --polyphonic --json notes.json\n"
              "  transcriber --batch samples --output-dir results --benchmark\n"
              "  transcriber --asio 8 --musicxml live.musicxml --tab live.txt\n"
              "  transcriber take.wav --live-pipeline --refine --proposal-json proposal.json\n";
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
    static constexpr std::array<std::array<int, 2>, 10> supported{{
        {2, 4}, {3, 4}, {4, 4}, {5, 4}, {2, 8}, {3, 8}, {5, 8},
        {6, 8}, {7, 8}, {9, 8}}};
    for (const auto& meter : supported) {
        if (value == std::to_string(meter[0]) + "/" + std::to_string(meter[1])) {
            return meter;
        }
    }
    if (value == "12/8") return {12, 8};
    throw std::invalid_argument(
        "Unsupported meter. Choose 2/4, 3/4, 4/4, 5/4, 2/8, 3/8, 5/8, "
        "6/8, 7/8, 9/8, or 12/8.");
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
    static constexpr std::array<std::string_view, 30> supported_keys{
        "input", "musicxml", "tab", "midi", "json", "model", "provider",
        "device-id", "latency-ms", "tempo", "key-fifths", "mode", "meter",
        "quality", "batch", "output-dir", "asio", "tuning", "capo",
        "tab-alternatives", "preserve-expressive-timing", "fast-preview",
        "minimum-confidence", "polyphonic", "instrument", "benchmark", "quiet", "backend",
        "pickup", "written-transposition"};
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
    set_path("backend", options.backend);
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
    const auto pickup = values.find("pickup");
    if (pickup != values.end()) {
        options.pickup_quarters = std::stod(pickup->second);
    }
    const auto written_transposition = values.find("written-transposition");
    if (written_transposition != values.end()) {
        options.written_transposition_semitones = std::stoi(written_transposition->second);
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
        } else if (argument == "--score-project") {
            options.score_project = next_value(i, argc, argv, argument);
        } else if (argument == "--model") {
            options.model = next_value(i, argc, argv, argument);
        } else if (argument == "--backend") {
            options.backend = next_value(i, argc, argv, argument);
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
        } else if (argument == "--pickup") {
            options.pickup_quarters = std::stod(next_value(i, argc, argv, argument));
        } else if (argument == "--written-transposition") {
            options.written_transposition_semitones =
                std::stoi(next_value(i, argc, argv, argument));
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
        } else if (argument == "--live-pipeline") {
            options.live_pipeline = true;
        } else if (argument == "--live-events") {
            options.live_events = next_value(i, argc, argv, argument);
        } else if (argument == "--refine") {
            options.refine = true;
        } else if (argument == "--refine-backend") {
            options.refine_backend = next_value(i, argc, argv, argument);
        } else if (argument == "--refine-mode") {
            options.refine_mode = next_value(i, argc, argv, argument);
            if (options.refine_mode != "auto" && options.refine_mode != "full" &&
                options.refine_mode != "windowed") {
                throw std::invalid_argument("--refine-mode must be auto, full or windowed.");
            }
        } else if (argument == "--proposal-json") {
            options.proposal_json = next_value(i, argc, argv, argument);
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
    if (options.pickup_quarters &&
        (!std::isfinite(*options.pickup_quarters) || *options.pickup_quarters <= 0.0 ||
         *options.pickup_quarters > 16.0)) {
        throw std::invalid_argument("--pickup must be greater than 0 and at most 16 quarters.");
    }
    if (options.written_transposition_semitones < -48 ||
        options.written_transposition_semitones > 48) {
        throw std::invalid_argument(
            "--written-transposition must be between -48 and 48 semitones.");
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
    if (!options.live_pipeline && (options.live_events || options.refine || options.proposal_json)) {
        throw std::invalid_argument("--live-events, --refine and --proposal-json require --live-pipeline.");
    }
    if ((options.proposal_json || options.refine_mode != "auto") && !options.refine) {
        throw std::invalid_argument("--proposal-json and --refine-mode require --refine.");
    }
    if (options.live_pipeline) {
        if (options.batch_directory) {
            throw std::invalid_argument("--live-pipeline cannot be combined with --batch.");
        }
        if (!options.model.empty() && options.backend.empty()) {
            throw std::invalid_argument(
                "--live-pipeline with --model requires an explicit --backend (e.g. onnx:basic-pitch-v1).");
        }
        if (options.fast_preview) {
            throw std::invalid_argument("--fast-preview is not supported by --live-pipeline.");
        }
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

void write_score_project(const std::filesystem::path& path,
                         const transcribe::ScoreDocument& score) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create score project: " + path.string());
    }
    output << transcribe::serialize_score_document(score);
    output << '\n';
    if (!output) {
        throw std::runtime_error("Failed while writing score project.");
    }
}

std::string progress_bar(double progress) {
    constexpr std::size_t width = 32;
    const auto filled = static_cast<std::size_t>(
        std::clamp(progress, 0.0, 1.0) * width);
    return "[" + std::string(filled, '#') + std::string(width - filled, ' ') + "] " +
           std::to_string(static_cast<int>(progress * 100.0)) + "%";
}

const char* live_event_kind(transcribe::live::LiveEventKind kind) {
    using transcribe::live::LiveEventKind;
    switch (kind) {
    case LiveEventKind::NoteOn: return "note_on";
    case LiveEventKind::NoteStabilized: return "note_stabilized";
    case LiveEventKind::NoteOff: return "note_off";
    case LiveEventKind::NoteCorrected: return "note_corrected";
    case LiveEventKind::NoteRetracted: return "note_retracted";
    case LiveEventKind::AnalysisGap: return "analysis_gap";
    case LiveEventKind::Status: return "status";
    }
    return "unknown";
}

const char* lifecycle_name(transcribe::EventLifecycle state) {
    switch (state) {
    case transcribe::EventLifecycle::Provisional: return "provisional";
    case transcribe::EventLifecycle::Stabilized: return "stabilized";
    case transcribe::EventLifecycle::Refined: return "refined";
    case transcribe::EventLifecycle::ManuallyCorrected: return "manually_corrected";
    case transcribe::EventLifecycle::Rejected: return "rejected";
    }
    return "unknown";
}

void write_live_note(std::ostream& output, const transcribe::live::LiveNote& note) {
    output << "{\"id\": \"" << json_escape(note.id) << "\", \"midi\": " << note.midi_pitch
           << ", \"onset_frame\": " << note.onset_frame << ", \"onset_seconds\": " << note.onset_seconds()
           << ", \"offset_seconds\": ";
    if (const auto offset = note.offset_seconds()) output << *offset;
    else output << "null";
    output << ", \"sample_rate\": " << note.sample_rate << ", \"state\": \"" << lifecycle_name(note.state)
           << "\", \"raw_strength\": " << note.raw_strength << ", \"source_model\": \""
           << json_escape(note.source_model) << "\", \"detection_method\": \""
           << json_escape(note.detection_method) << "\", \"onset_truncated\": "
           << (note.onset_truncated ? "true" : "false") << ", \"revision\": " << note.revision << '}';
}

void write_proposal_json(const std::filesystem::path& path,
                         const transcribe::InferenceResult<transcribe::live::ScoreProposal>& proposal,
                         const transcribe::live::ProposalReview* review) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Could not create proposal output: " + path.string());
    output << std::setprecision(9);
    if (const auto* failure = std::get_if<transcribe::InferenceError>(&proposal)) {
        output << "{\"error\": \"" << json_escape(failure->message) << "\"}\n";
        return;
    }
    const auto& value = std::get<transcribe::live::ScoreProposal>(proposal);
    output << "{\n  \"proposal_id\": \"" << json_escape(value.id) << "\", \"backend\": \""
           << json_escape(value.backend_id) << "\", \"sample_rate\": " << value.sample_rate
           << ", \"inference_seconds\": " << value.inference_seconds
           << ",\n  \"applied\": false, \"base_revision\": " << review->base_revision
           << ", \"conflicts\": " << review->conflicts << ",\n  \"notes\": [";
    for (std::size_t i = 0; i < value.notes.size(); ++i) {
        output << (i ? ",\n    " : "\n    ");
        write_live_note(output, value.notes[i]);
    }
    output << "\n  ],\n  \"changes\": [";
    for (std::size_t i = 0; i < review->changes.size(); ++i) {
        const auto& change = review->changes[i];
        output << (i ? ",\n    " : "\n    ") << "{\"change_id\": \"" << json_escape(change.change_id)
               << "\", \"kind\": \""
               << (change.kind == transcribe::live::ProposedChangeKind::Add ? "add" :
                   change.kind == transcribe::live::ProposedChangeKind::Modify ? "modify" : "remove")
               << "\", \"note_id\": \"" << json_escape(change.note_id) << "\", \"conflicts_with_manual_edit\": "
               << (change.conflicts_with_manual_edit ? "true" : "false") << ", \"current\": ";
        if (change.current) write_live_note(output, *change.current);
        else output << "null";
        output << ", \"proposed\": ";
        if (change.proposed) write_live_note(output, *change.proposed);
        else output << "null";
        output << '}';
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("Failed while writing proposal output.");
}

std::string milliseconds(const transcribe::live::LatencySummary& summary) {
    if (!summary.count) return "n/a";
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << summary.p50_seconds * 1000.0 << '/'
         << summary.p95_seconds * 1000.0 << " ms (n=" << summary.count << ')';
    return text.str();
}

std::vector<transcribe::Note> run_live_pipeline(const std::filesystem::path& input_path,
                                                const Options& options, double& audio_seconds) {
    namespace live = transcribe::live;
    live::LiveOptions live_options;
    transcribe::AudioBuffer audio;
    if (options.asio) {
        live_options.format = {44100, 1};
        live_options.lag_policy = live::LagPolicy::SkipAhead;
        live_options.audio_source_id = "asio";
        live_options.max_recording_seconds = std::max(600.0, options.capture_seconds + 1.0);
    } else {
        audio = transcribe::read_audio(input_path);
        live_options.format = {audio.sample_rate, 1};
        live_options.lag_policy = live::LagPolicy::Lossless;
        live_options.audio_source_id = "file:" + input_path.filename().string();
        live_options.max_recording_seconds = std::max(
            600.0, static_cast<double>(audio.samples.size()) / std::max<std::uint32_t>(1, audio.sample_rate) + 1.0);
    }
    live_options.yin.realtime.latency_ms = options.realtime_latency_ms;
    live_options.model_load = {options.onnx_provider, options.device_id, false};
    auto& request = live_options.window.request;
    request.analysis.polyphonic = options.polyphonic;
    request.analysis.minimum_confidence = options.minimum_confidence;
    request.analysis.instrument = options.instrument;
    request.onnx.polyphonic = options.polyphonic;
    request.onnx.independent_probabilities = options.polyphonic;
    request.onnx.minimum_confidence = options.minimum_confidence;
    request.onnx.execution_provider = options.onnx_provider;
    request.onnx.device_id = options.device_id;
    std::unique_ptr<transcribe::ModelBackend> model;
    if (!options.backend.empty() && options.backend != "builtin:yin") {
        transcribe::ModelRegistry registry(options.model);
        auto created = registry.create(options.backend);
        if (const auto* failure = std::get_if<transcribe::InferenceError>(&created)) {
            throw std::runtime_error(failure->message);
        }
        model = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
        live_options.backend = live::LiveBackendKind::OverlappingWindow;
    } else if (!options.model.empty()) {
        throw std::invalid_argument("--model cannot be combined with incremental builtin:yin");
    }

    live::LiveTranscriptionSession session(live_options, std::move(model));
    const auto started = session.start();
    if (const auto* failure = std::get_if<transcribe::InferenceError>(&started)) {
        throw std::runtime_error("Live pipeline failed to start: " + failure->message);
    }
    std::ofstream events_output;
    if (options.live_events) {
        events_output.open(*options.live_events, std::ios::binary);
        if (!events_output) throw std::runtime_error("Could not create live event output: " + options.live_events->string());
        events_output << std::setprecision(9);
    }
    live::TranscriptionDocument document(live_options.format.sample_rate);
    const auto consume = [&] {
        auto batch = session.poll_events();
        for (const auto& event : batch.events) {
            if (events_output.is_open()) {
                events_output << "{\"sequence\": " << event.sequence << ", \"kind\": \""
                              << live_event_kind(event.kind) << "\", \"note\": ";
                if (event.kind == live::LiveEventKind::AnalysisGap || event.kind == live::LiveEventKind::Status) {
                    events_output << "null";
                } else {
                    write_live_note(events_output, event.note);
                }
                events_output << ", \"gap_first_frame\": " << event.gap_first_frame
                              << ", \"gap_end_frame\": " << event.gap_end_frame
                              << ", \"evidence_end_frame\": " << event.timing.evidence_end_frame
                              << ", \"message\": \"" << json_escape(event.message) << "\"}\n";
            }
            if (!options.quiet && event.kind != live::LiveEventKind::NoteStabilized) {
                std::cerr << "\n[live] " << live_event_kind(event.kind);
                if (event.kind == live::LiveEventKind::Status || event.kind == live::LiveEventKind::AnalysisGap) {
                    std::cerr << ": " << event.message;
                } else {
                    std::cerr << ' ' << event.note.id << ' ' << transcribe::midi_note_name(event.note.midi_pitch)
                              << " at " << std::fixed << std::setprecision(3) << event.note.onset_seconds() << 's';
                    if (const auto offset = event.note.offset_seconds()) std::cerr << " to " << *offset << 's';
                }
            }
        }
        document.apply(batch);
        if (document.requires_resync()) document.resync(session.snapshot());
    };

    if (options.asio) {
        std::exception_ptr capture_error;
        std::atomic<bool> capture_done{false};
        std::jthread capture([&](std::stop_token stop) {
            try {
                (void)transcribe::capture_asio_live(options.capture_seconds, session, stop,
                    [&options](double progress) {
                        if (!options.quiet) std::cerr << '\r' << progress_bar(progress) << std::flush;
                    });
            } catch (...) {
                capture_error = std::current_exception();
            }
            capture_done.store(true, std::memory_order_release);
        });
        while (!capture_done.load(std::memory_order_acquire)) {
            consume();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        capture.join();
        if (capture_error) {
            session.cancel();
            std::rethrow_exception(capture_error);
        }
    } else {
        constexpr std::size_t block_frames = 1024;
        for (std::size_t offset = 0; offset < audio.samples.size(); offset += block_frames) {
            const auto count = std::min(block_frames, audio.samples.size() - offset);
            const live::AudioFrameBlock block{live_options.format, offset,
                std::span<const float>(audio.samples).subspan(offset, count),
                std::chrono::duration_cast<live::Nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()), false};
            auto status = session.push(block);
            while (status == live::PushStatus::QueueFull) {
                consume();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                status = session.push(block);
            }
            if (status != live::PushStatus::Accepted) {
                throw std::runtime_error("Live pipeline rejected audio: " + session.last_error());
            }
            if (!options.quiet && offset % (block_frames * 64) == 0) {
                std::cerr << '\r' << progress_bar(static_cast<double>(offset) / audio.samples.size()) << std::flush;
            }
        }
    }
    session.finish();
    consume();
    if (session.state() == live::LiveState::AnalysisFailed) {
        throw std::runtime_error("Live analysis failed: " + session.last_error());
    }
    const auto recording = session.recording();
    audio_seconds = recording.sample_rate
        ? static_cast<double>(recording.samples.size()) / recording.sample_rate : 0.0;
    if (!options.quiet) {
        const auto metrics = session.metrics();
        std::cerr << "\n[live] end-to-end p50/p95 " << milliseconds(metrics.end_to_end_latency)
                  << "; stabilization " << milliseconds(metrics.stabilization_delay)
                  << "; analysis step " << milliseconds(metrics.analysis_latency)
                  << "; input queue high-water " << metrics.input_queue_high_water << '/'
                  << metrics.input_queue_capacity << "; skipped " << metrics.skipped_frames
                  << " frames; dropped events " << metrics.events_dropped << '\n';
    }
    if (options.refine) {
        live::RefinementOptions refinement;
        refinement.backend_id = options.refine_backend;
        refinement.model_path = options.model;
        refinement.load = {options.onnx_provider, options.device_id, false};
        refinement.request.analysis = request.analysis;
        refinement.request.onnx = request.onnx;
        refinement.audio_source_id = live_options.audio_source_id;
        const bool windowed = options.refine_mode == "windowed" ||
            (options.refine_mode == "auto" && options.refine_backend.starts_with("onnx:"));
        if (windowed) refinement.windowed = live_options.window;
        const auto proposal = live::refine_recording(recording, refinement);
        std::optional<live::ProposalReview> review;
        if (const auto* value = std::get_if<live::ScoreProposal>(&proposal)) {
            review = document.review(*value);
            if (!options.quiet) {
                std::cerr << "[refine] " << value->backend_id << (windowed ? " (windowed)" : " (full-context)")
                          << " proposal: " << value->notes.size()
                          << " notes, " << review->changes.size() << " proposed changes ("
                          << review->conflicts << " conflicts); live score left unchanged\n";
            }
        } else if (!options.quiet || !options.proposal_json) {
            std::cerr << "[refine] failed: " << std::get<transcribe::InferenceError>(proposal).message << '\n';
        }
        if (options.proposal_json) {
            write_proposal_json(*options.proposal_json, proposal, review ? &*review : nullptr);
        }
    }
    return document.to_notes(audio_seconds);
}

void process_input(const std::filesystem::path& input_path, const Options& options,
                   const std::filesystem::path& musicxml_path,
                   const std::filesystem::path& tab_path,
                   const std::optional<std::filesystem::path>& midi_path,
                   const std::optional<std::filesystem::path>& json_path,
                   const std::optional<std::filesystem::path>& score_project_path) {
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
    const bool streaming_wav = !options.live_pipeline && !options.asio && options.model.empty() &&
        options.backend.empty() && extension == ".wav";
    if (options.live_pipeline) {
        notes = run_live_pipeline(input_path, options, audio_seconds);
    } else if (streaming_wav) {
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
        if (!options.backend.empty()) {
            transcribe::ModelRegistry registry(options.model);
            auto created = registry.create(options.backend);
            if (const auto* failure = std::get_if<transcribe::InferenceError>(&created)) {
                throw std::runtime_error(failure->message);
            }
            auto instance = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
            if (!options.model.empty() && instance->metadata().definition.path.empty()) {
                throw std::invalid_argument("--model cannot be combined with a built-in --backend");
            }
            const auto loaded = instance->load(
                {options.onnx_provider, options.device_id, options.fast_preview});
            if (const auto* failure = std::get_if<transcribe::InferenceError>(&loaded)) {
                throw std::runtime_error(failure->message);
            }
            transcribe::InferenceRequest request;
            request.analysis.polyphonic = options.polyphonic;
            request.analysis.minimum_confidence = options.minimum_confidence;
            request.analysis.instrument = options.instrument;
            if (options.benchmark) request.analysis.timing_callback = accumulate_timings;
            request.onnx.polyphonic = options.polyphonic;
            request.onnx.independent_probabilities = options.polyphonic;
            request.onnx.minimum_confidence = options.minimum_confidence;
            request.onnx.execution_provider = options.onnx_provider;
            request.onnx.device_id = options.device_id;
            if (!options.quiet) request.progress = report_progress;
            auto result = instance->infer(audio, request);
            if (const auto* failure = std::get_if<transcribe::InferenceError>(&result)) {
                throw std::runtime_error(failure->message);
            }
            notes = std::move(std::get<transcribe::InferenceOutput>(result).notes);
        } else if (options.model.empty()) {
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

    transcribe::InterpretationOptions interpretation_options;
    interpretation_options.tempo_bpm = options.tempo_bpm;
    if (options.meter) {
        interpretation_options.meter = transcribe::MeterConstraint{
            (*options.meter)[0], (*options.meter)[1], {}};
    }
    interpretation_options.key_fifths = options.key_fifths;
    interpretation_options.minor_key = options.minor_key;
    interpretation_options.pickup_quarters = options.pickup_quarters;
    interpretation_options.written_transposition_semitones =
        options.written_transposition_semitones;
    switch (options.instrument) {
    case transcribe::InstrumentPrior::Guitar:
        interpretation_options.instrument = "Guitar";
        break;
    case transcribe::InstrumentPrior::Piano:
        interpretation_options.instrument = "Piano";
        break;
    case transcribe::InstrumentPrior::Voice:
        interpretation_options.instrument = "Voice";
        break;
    case transcribe::InstrumentPrior::None:
        break;
    }
    const auto interpretation = transcribe::interpret_notes(notes, interpretation_options);
    if (interpretation.hypotheses.empty()) {
        throw std::runtime_error("Musical interpretation produced no score hypothesis.");
    }
    const auto metadata =
        transcribe::metadata_for_hypothesis(interpretation.hypotheses.front());
    auto score = transcribe::make_score_document(
        interpretation, 0, input_path.string(), metadata, options.guitar_tab);
    auto score_note_values = transcribe::score_notes(score);
    const auto fingering =
        transcribe::assign_guitar_tab_fingerings(score_note_values, options.guitar_tab);
    for (const auto& position : fingering.positions) {
        if (position.note_index >= score.notes.size()) {
            throw std::runtime_error("Guitar tablature assignment returned an invalid note index.");
        }
        auto tab = std::find_if(score.tab_notes.begin(), score.tab_notes.end(),
            [&](const auto& candidate) {
                return candidate.notation_note_id ==
                    score.notes[position.note_index].id;
            });
        if (tab != score.tab_notes.end() &&
            position.string_number > 0 && position.fret >= 0) {
            tab->string_number = position.string_number;
            tab->fret = position.fret;
            tab->manually_corrected = position.manually_corrected;
        }
    }
    transcribe::write_musicxml(musicxml_path, score,
                               options.preserve_expressive_timing);
    transcribe::write_guitar_tab(tab_path, score, options.guitar_tab);
    if (midi_path) {
        transcribe::write_midi(*midi_path, score);
    }
    if (json_path) {
        write_json(*json_path, notes, metadata);
    }
    if (score_project_path) {
        write_score_project(*score_project_path, score);
    }

    if (!options.quiet) {
        std::cout << input_path.string() << ": " << notes.size() << " notes\n"
                  << "  Tempo: ";
        if (options.tempo_bpm) {
            std::cout << std::fixed << std::setprecision(1) << metadata.tempo_bpm
                      << " BPM (user supplied)";
        } else if (metadata.tempo_inferred) {
            std::cout << std::fixed << std::setprecision(1) << metadata.tempo_bpm
                      << " BPM (confidence " << std::setprecision(2)
                      << metadata.tempo_confidence << ')';
        } else {
            std::cout << "insufficient onset evidence (120 BPM default)";
        }
        std::cout << "\n  Meter: ";
        if (options.meter) {
            std::cout << metadata.time_numerator << '/' << metadata.time_denominator
                      << " (user supplied)";
        } else if (metadata.meter_inferred) {
            std::cout << metadata.time_numerator << '/' << metadata.time_denominator
                      << " (confidence " << std::setprecision(2)
                      << metadata.meter_confidence << ')';
        } else {
            std::cout << "uncertain (4/4 default)";
        }
        std::cout << "\n  Key: ";
        if (options.key_fifths || options.minor_key) {
            std::cout << key_name(metadata) << " (user constrained)";
        } else if (metadata.key_inferred) {
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
        if (score_project_path) {
            std::cout << "  Score project: " << score_project_path->string() << '\n';
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
                  options.json, options.score_project);
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
                                       : std::nullopt,
                          options.score_project
                              ? std::optional(output_directory / (stem + ".score.json"))
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
