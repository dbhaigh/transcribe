#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"
#include "transcribe/inference.hpp"
#include "transcribe/live.hpp"
#include "transcribe/score.hpp"
#include "transcribe/output.hpp"
#include "transcribe/engine.hpp"
#include "transcribe/preprocessing.hpp"
#include "transcribe/path.hpp"
#include "transcribe/project.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <set>
#include <thread>
#include <csignal>
#include <type_traits>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using transcribe::path_utf8;
volatile std::sig_atomic_t batch_cancel_requested = 0;

void request_batch_cancel(int) {
    batch_cancel_requested = 1;
}

class BatchCancelled final : public std::runtime_error {
public:
    BatchCancelled() : std::runtime_error("Batch processing cancelled by SIGINT.") {}
};

void check_batch_cancel() {
    if (batch_cancel_requested) throw BatchCancelled();
}

struct Options {
    std::optional<std::filesystem::path> input;
    std::vector<std::filesystem::path> config_sources;
    std::optional<std::filesystem::path> batch_directory;
    std::optional<std::filesystem::path> batch_manifest;
    std::optional<std::filesystem::path> batch_results;
    std::optional<std::filesystem::path> progress_jsonl;
    std::size_t concurrency{1};
    std::size_t memory_limit_mb{2048};
    bool resume{};
    std::filesystem::path musicxml{"transcription.musicxml"};
    std::filesystem::path tab{"transcription.txt"};
    std::optional<std::filesystem::path> midi;
    std::optional<std::filesystem::path> json;
    std::optional<std::filesystem::path> events_json;
    std::optional<std::filesystem::path> score_project;
    std::optional<std::filesystem::path> project;
    std::filesystem::path model;
    std::filesystem::path isolated_audio;
    bool preprocessing_fallback{};
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
    double live_target_ms{200.0};
    double live_stabilized_target_ms{300.0};
    double live_hop_ms{250.0};
    bool live_timing_configured{};
    double minimum_confidence{0.25};
    transcribe::InstrumentPrior instrument{transcribe::InstrumentPrior::None};
    transcribe::OnnxExecutionProvider onnx_provider{
        transcribe::OnnxExecutionProvider::Cpu};
    int device_id{};
    bool asio{};
    bool polyphonic{};
    bool competing_polyphony{};
    transcribe::PitchObjective model_objective{transcribe::PitchObjective::Unspecified};
    transcribe::NeuralNoteDecoder neural_decoder{transcribe::NeuralNoteDecoder::Legacy};
    bool fast_preview{};
    bool benchmark{};
    bool bounded_inference{};
    bool preserve_expressive_timing{};
    transcribe::QuantizationMode quantization_mode{transcribe::QuantizationMode::Legacy};
    double quantization_strength{1.0};
    double minimum_rhythm_evidence{0.65};
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
              "  transcriber --batch-manifest <csv> [batch options]\n\n"
              "Options:\n"
              "  -h, --help                  Show this help\n"
              "  --musicxml <path>           MusicXML output path\n"
              "  --tab <path>                Guitar tablature output path\n"
              "  --midi <path>               Optional Standard MIDI output path\n"
              "  --json <path>               Write notes and analysis metadata as JSON\n"
              "  --events-json <path>        Write versioned v1 timestamped note events\n"
              "  --score-project <path>      Write a versioned editable score project\n"
              "  --project <path>            Write a native project with an external audio reference\n"
              "  --model <path.onnx>         Use an ONNX model instead of built-in YIN\n"
              "  --backend <registry-id>     Opt-in offline model adapter (e.g. builtin:yin)\n"
              "  --neural-decoder <speed|quality>  Basic Pitch legacy/events modes; measured tradeoffs in docs\n"
              "  --bounded-inference         Bounded WAV windows; requires an explicit --backend\n"
              "  --provider <name>           ONNX provider: cpu, cuda, directml, or rocm\n"
              "  --device-id <number>        GPU device index for the selected provider\n"
              "  --polyphonic                Enable multi-pitch analysis (built-in or ONNX)\n"
              "  --polyphonic-decoder <name> legacy or competing (experimental harmonic/ONNX heads)\n"
              "  --model-objective <name>    independent-multi-pitch or monophonic (generic ONNX)\n"
              "  --instrument <name>         Apply guitar, bass, piano, or voice prior (not source recognition)\n"
              "  --isolated-audio <wav>       Transcribe a reviewed, zero-offset aligned stem instead of the mixture\n"
              "  --preprocess-fallback        Explicitly use the mixture if the stem is unavailable\n"
              "  --tuning <name>             Guitar tuning: standard, drop-d, dadgad, open-d, open-g\n"
              "  --capo <fret>               Guitar capo fret (0-24)\n"
              "  --string-tuning <midi,...>  Custom 1-12 strings, string 1 first (overrides preset)\n"
              "  --fret-count <0..36>        Physical neck fret count, including capo offset\n"
              "  --guitar-min-midi <0..127>  Suggestion pitch-range lower bound\n"
              "  --guitar-max-midi <0..127>  Suggestion pitch-range upper bound\n"
              "  --tab-alternatives <count>  Include up to 5 alternative chord fingerings\n"
              "  --preserve-expressive-timing Annotate unquantized note-onset offsets in MusicXML\n"
              "  --quantization <mode>      legacy (default), evidence, or expressive\n"
              "  --quantization-strength <0..1> Blend measured timing toward the selected grid\n"
              "  --minimum-rhythm-evidence <0..1> Heuristic gate for evidence quantization (0.65)\n"
              "  --fast-preview              Use lower-resolution analysis for a quick rough score\n"
              "  --quality <high|fast>       Select normal or fast-preview built-in analysis\n"
              "  --tempo <40..240>           Override the score tempo in BPM\n"
              "  --key-fifths <-7..7>        Override the key signature (circle of fifths)\n"
              "  --mode <major|minor>        Set the key mode\n"
              "  --meter <meter>              Override meter (e.g. 3/4, 4/4, 5/4, 6/8, 9/8)\n"
              "  --pickup <quarters>          Set an implicit pickup measure length\n"
              "  --written-transposition <n>  Written pitch offset in semitones (-48..48)\n"
              "  --latency-ms <20..50>       ASIO live-analysis latency target (default: 40)\n"
              "  --live-target-ms <N>        Provisional publication target; diagnostic, no pitch changes\n"
              "  --live-stabilized-target-ms <N>  Stabilized publication target (>= provisional)\n"
              "  --live-hop-ms <N>           Fixed-window update interval; context/thresholds unchanged\n"
              "  --minimum-confidence <0..1> Drop notes below this confidence\n"
              "  --benchmark                 Report analysis time and real-time factor\n"
              "  --config <path>             Load defaults from simple key=value config\n"
              "  --output-dir <directory>    Batch output directory (default: <input>/transcriber-output)\n"
              "  --batch-manifest <path.csv> Process manifest rows and per-record options/outputs\n"
              "  --batch-results <path.json> Write structured per-file results (default in output-dir)\n"
              "  --progress-jsonl <path>     Append machine-readable batch progress events\n"
              "  --concurrency <1..32>       Independent per-file workers (default 1)\n"
              "  --memory-limit-mb <N>       Scheduler admission budget across concurrent jobs\n"
              "  --resume                    Skip successful journaled files whose outputs remain present\n"
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

template<class Value>
Value parse_number(std::string_view value, std::string_view option) {
    Value result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(static_cast<double>(result))) {
        throw std::invalid_argument("Invalid value for " + std::string(option) + ": '" +
            std::string(value) + "'. Expected a finite " +
            (std::is_integral_v<Value> ? "integer" : "number") + " with no trailing characters.");
    }
    return result;
}

std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
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
        throw std::runtime_error("Could not open config file: " + path_utf8(path));
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
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        if (line.empty() || line.front() == '[') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("Invalid config entry on line " +
                                     std::to_string(line_number) + ".");
        }
        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty() || !values.emplace(key, value).second) {
            throw std::runtime_error("Empty or duplicate config entry on line " +
                std::to_string(line_number) + " in " + path_utf8(path) + ".");
        }
    }
    if (input.bad()) throw std::runtime_error("Could not finish reading config: " + path_utf8(path));
    return values;
}

void apply_config(Options& options, const std::map<std::string, std::string>& values) {
    static constexpr std::array<std::string_view, 53> supported_keys{
        "input", "musicxml", "tab", "midi", "json", "events-json", "project",
        "score-project", "model", "provider",
        "device-id", "latency-ms", "tempo", "key-fifths", "mode", "meter",
        "quality", "batch", "output-dir", "asio", "tuning", "capo",
        "tab-alternatives", "preserve-expressive-timing", "fast-preview",
        "minimum-confidence", "polyphonic", "instrument", "benchmark", "quiet", "backend",
        "pickup", "written-transposition", "quantization", "quantization-strength",
        "minimum-rhythm-evidence", "neural-decoder", "live-target-ms",
        "live-stabilized-target-ms", "live-hop-ms", "string-tuning", "fret-count",
        "guitar-min-midi", "guitar-max-midi", "isolated-audio", "preprocess-fallback",
        "batch-manifest", "batch-results", "progress-jsonl", "concurrency",
        "memory-limit-mb", "resume", "batch"};
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
            destination = utf8_path(found->second);
        }
    };
    set_path("input", options.input);
    set_path("isolated-audio", options.isolated_audio);
    set_path("batch", options.batch_directory);
    set_path("output-dir", options.output_directory);
    set_path("musicxml", options.musicxml);
    set_path("tab", options.tab);
    set_path("midi", options.midi);
    set_path("json", options.json);
    set_path("events-json", options.events_json);
    set_path("project", options.project);
    set_path("score-project", options.score_project);
    set_path("batch-manifest", options.batch_manifest);
    set_path("batch-results", options.batch_results);
    set_path("progress-jsonl", options.progress_jsonl);
    if (const auto found = values.find("concurrency"); found != values.end())
        options.concurrency = parse_number<std::size_t>(found->second, "concurrency");
    if (const auto found = values.find("memory-limit-mb"); found != values.end())
        options.memory_limit_mb = parse_number<std::size_t>(found->second, "memory-limit-mb");
    if (const auto found = values.find("resume"); found != values.end())
        options.resume = parse_config_bool(found->second, "resume");
    set_path("model", options.model);
    if (const auto found = values.find("backend"); found != values.end()) options.backend = found->second;
    if (const auto found = values.find("live-target-ms"); found != values.end())
        options.live_target_ms = parse_number<double>(found->second, "live-target-ms");
    if (const auto found = values.find("live-stabilized-target-ms"); found != values.end())
        options.live_stabilized_target_ms = parse_number<double>(found->second, "live-stabilized-target-ms");
    if (const auto found = values.find("live-hop-ms"); found != values.end())
        options.live_hop_ms = parse_number<double>(found->second, "live-hop-ms");
    if (values.contains("live-target-ms") || values.contains("live-stabilized-target-ms") ||
        values.contains("live-hop-ms"))
        options.live_timing_configured = true;
    if (const auto found = values.find("neural-decoder"); found != values.end()) {
        if (found->second != "legacy" && found->second != "events" &&
            found->second != "speed" && found->second != "quality")
            throw std::invalid_argument("neural-decoder must be speed/legacy or quality/events");
        options.neural_decoder = found->second == "events" || found->second == "quality" ?
            transcribe::NeuralNoteDecoder::MultiHeadEvents : transcribe::NeuralNoteDecoder::Legacy;
    }
    const auto provider = values.find("provider");
    if (provider != values.end()) {
        options.onnx_provider = parse_onnx_provider(provider->second);
    }
    const auto device_id = values.find("device-id");
    if (device_id != values.end()) {
        options.device_id = parse_number<int>(device_id->second, "device-id");
    }
    const auto latency_ms = values.find("latency-ms");
    if (latency_ms != values.end()) {
        options.realtime_latency_ms = parse_number<double>(latency_ms->second, "latency-ms");
    }
    const auto asio = values.find("asio");
    if (asio != values.end()) {
        options.asio = true;
        options.capture_seconds = parse_number<double>(asio->second, "asio");
    }
    const auto tempo = values.find("tempo");
    if (tempo != values.end()) {
        options.tempo_bpm = parse_number<double>(tempo->second, "tempo");
    }
    const auto key_fifths = values.find("key-fifths");
    if (key_fifths != values.end()) {
        options.key_fifths = parse_number<int>(key_fifths->second, "key-fifths");
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
    if (meter != values.end()) {
        options.meter = meter->second == "auto" ? std::nullopt : std::optional(parse_meter(meter->second));
    }
    const auto pickup = values.find("pickup");
    if (pickup != values.end()) {
        options.pickup_quarters = parse_number<double>(pickup->second, "pickup");
    }
    const auto written_transposition = values.find("written-transposition");
    if (written_transposition != values.end()) {
        options.written_transposition_semitones = parse_number<int>(written_transposition->second, "written-transposition");
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
        options.guitar_tab.capo_fret = parse_number<int>(capo->second, "capo");
    }
    if (const auto found = values.find("string-tuning"); found != values.end()) {
        options.guitar_tab.string_tuning = transcribe::parse_string_tuning(found->second);
    }
    if (const auto found = values.find("fret-count"); found != values.end()) {
        options.guitar_tab.search.physical_fret_count = parse_number<int>(found->second, "fret-count");
    }
    if (const auto found = values.find("guitar-min-midi"); found != values.end()) {
        options.guitar_tab.search.minimum_midi_pitch = parse_number<int>(found->second, "guitar-min-midi");
    }
    if (const auto found = values.find("guitar-max-midi"); found != values.end()) {
        options.guitar_tab.search.maximum_midi_pitch = parse_number<int>(found->second, "guitar-max-midi");
    }
    const auto alternatives = values.find("tab-alternatives");
    if (alternatives != values.end()) {
        options.guitar_tab.alternative_fingerings =
            parse_number<std::size_t>(alternatives->second, "tab-alternatives");
    }
    const auto expressive_timing = values.find("preserve-expressive-timing");
    if (expressive_timing != values.end()) {
        options.preserve_expressive_timing =
            parse_config_bool(expressive_timing->second,
                              "preserve-expressive-timing");
    }
    if (const auto found = values.find("quantization"); found != values.end()) {
        options.quantization_mode = transcribe::parse_quantization_mode(found->second);
    }
    if (const auto found = values.find("quantization-strength"); found != values.end()) {
        options.quantization_strength = parse_number<double>(found->second, "Quantization strength");
    }
    if (const auto found = values.find("minimum-rhythm-evidence"); found != values.end()) {
        options.minimum_rhythm_evidence = parse_number<double>(found->second, "Quantization minimum rhythm evidence");
    }
    const auto fast_preview = values.find("fast-preview");
    if (fast_preview != values.end()) {
        options.fast_preview =
            parse_config_bool(fast_preview->second, "fast-preview");
    }
    const auto confidence = values.find("minimum-confidence");
    if (confidence != values.end()) {
        options.minimum_confidence = parse_number<double>(confidence->second, "minimum-confidence");
    }
    const auto polyphonic = values.find("polyphonic");
    if (polyphonic != values.end()) {
        options.polyphonic = parse_config_bool(polyphonic->second, "polyphonic");
    }
    const auto instrument = values.find("instrument");
    if (instrument != values.end()) {
        if (instrument->second == "guitar") {
            options.instrument = transcribe::InstrumentPrior::Guitar;
        } else if (instrument->second == "bass") {
            options.instrument = transcribe::InstrumentPrior::Bass;
        } else if (instrument->second == "piano") {
            options.instrument = transcribe::InstrumentPrior::Piano;
        } else if (instrument->second == "voice") {
            options.instrument = transcribe::InstrumentPrior::Voice;
        } else if (instrument->second == "none") {
            options.instrument = transcribe::InstrumentPrior::None;
        } else {
            throw std::invalid_argument(
                "Instrument must be one of: none, guitar, bass, piano, voice.");
        }
    }
    const auto benchmark = values.find("benchmark");
    if (const auto fallback = values.find("preprocess-fallback"); fallback != values.end()) {
        options.preprocessing_fallback = parse_config_bool(fallback->second, "preprocess-fallback");
    }
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
            config_paths.emplace_back(utf8_path(
                next_value(i, argc, argv, "--config")));
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
            options.musicxml = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--tab") {
            options.tab = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--midi") {
            options.midi = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--json") {
            options.json = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--events-json") {
            options.events_json = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--score-project") {
            options.score_project = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--project") {
            options.project = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--isolated-audio") {
            options.isolated_audio = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--preprocess-fallback") {
            options.preprocessing_fallback = true;
        } else if (argument == "--model") {
            options.model = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--backend") {
            options.backend = next_value(i, argc, argv, argument);
        } else if (argument == "--neural-decoder") {
            apply_config(options, {{"neural-decoder", next_value(i, argc, argv, argument)}});
        } else if (argument == "--provider") {
            options.onnx_provider =
                parse_onnx_provider(next_value(i, argc, argv, argument));
        } else if (argument == "--device-id") {
            options.device_id = parse_number<int>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--minimum-confidence") {
            options.minimum_confidence = parse_number<double>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--latency-ms") {
            options.realtime_latency_ms = parse_number<double>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--live-target-ms" || argument == "--live-stabilized-target-ms" ||
                   argument == "--live-hop-ms") {
            apply_config(options, {{argument.substr(2), next_value(i, argc, argv, argument)}});
        } else if (argument == "--tempo") {
            options.tempo_bpm = parse_number<double>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--key-fifths") {
            options.key_fifths = parse_number<int>(next_value(i, argc, argv, argument), argument);
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
            options.pickup_quarters = parse_number<double>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--written-transposition") {
            options.written_transposition_semitones =
                parse_number<int>(next_value(i, argc, argv, argument), argument);
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
                parse_number<int>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--string-tuning") {
            options.guitar_tab.string_tuning = transcribe::parse_string_tuning(next_value(i, argc, argv, argument));
        } else if (argument == "--fret-count") {
            options.guitar_tab.search.physical_fret_count = parse_number<int>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--guitar-min-midi") {
            options.guitar_tab.search.minimum_midi_pitch = parse_number<int>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--guitar-max-midi") {
            options.guitar_tab.search.maximum_midi_pitch = parse_number<int>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--tab-alternatives") {
            options.guitar_tab.alternative_fingerings =
                parse_number<std::size_t>(next_value(i, argc, argv, argument), argument);
        } else if (argument == "--preserve-expressive-timing") {
            options.preserve_expressive_timing = true;
        } else if (argument == "--quantization") {
            options.quantization_mode = transcribe::parse_quantization_mode(
                next_value(i, argc, argv, argument));
        } else if (argument == "--quantization-strength") {
            options.quantization_strength = parse_number<double>(next_value(i, argc, argv, argument), "Quantization strength");
        } else if (argument == "--minimum-rhythm-evidence") {
            options.minimum_rhythm_evidence = parse_number<double>(next_value(i, argc, argv, argument), "Quantization minimum rhythm evidence");
        } else if (argument == "--polyphonic") {
            options.polyphonic = true;
        } else if (argument == "--polyphonic-decoder") {
            const auto value = next_value(i, argc, argv, argument);
            if (value != "legacy" && value != "competing") {
                throw std::invalid_argument("Polyphonic decoder must be legacy or competing");
            }
            options.competing_polyphony = value == "competing";
        } else if (argument == "--model-objective") {
            const auto value = next_value(i, argc, argv, argument);
            if (value == "independent-multi-pitch") {
                options.model_objective = transcribe::PitchObjective::IndependentMultiPitch;
            } else if (value == "monophonic") {
                options.model_objective = transcribe::PitchObjective::Monophonic;
            } else throw std::invalid_argument("Unknown model training objective");
        } else if (argument == "--fast-preview") {
            options.fast_preview = true;
        } else if (argument == "--instrument") {
            const auto instrument = next_value(i, argc, argv, argument);
            if (instrument == "guitar") {
                options.instrument = transcribe::InstrumentPrior::Guitar;
            } else if (instrument == "bass") {
                options.instrument = transcribe::InstrumentPrior::Bass;
            } else if (instrument == "piano") {
                options.instrument = transcribe::InstrumentPrior::Piano;
            } else if (instrument == "voice") {
                options.instrument = transcribe::InstrumentPrior::Voice;
            } else if (instrument == "none") {
                options.instrument = transcribe::InstrumentPrior::None;
            } else {
                throw std::invalid_argument(
                    "Instrument must be one of: none, guitar, bass, piano, voice.");
            }
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else if (argument == "--bounded-inference") {
            options.bounded_inference = true;
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else if (argument == "--batch") {
            options.batch_directory = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--batch-manifest") {
            options.batch_manifest = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--batch-results") {
            options.batch_results = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--progress-jsonl") {
            options.progress_jsonl = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--concurrency") {
            options.concurrency = parse_number<std::size_t>(
                next_value(i, argc, argv, argument), argument);
        } else if (argument == "--memory-limit-mb") {
            options.memory_limit_mb = parse_number<std::size_t>(
                next_value(i, argc, argv, argument), argument);
        } else if (argument == "--resume") {
            options.resume = true;
        } else if (argument == "--output-dir") {
            options.output_directory = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--live-pipeline") {
            options.live_pipeline = true;
        } else if (argument == "--live-events") {
            options.live_events = utf8_path(next_value(i, argc, argv, argument));
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
            options.proposal_json = utf8_path(next_value(i, argc, argv, argument));
        } else if (argument == "--asio" || argument == "--live") {
            options.asio = true;
            options.capture_seconds = parse_number<double>(next_value(i, argc, argv, argument), argument);
        } else if (argument.starts_with('-')) {
            throw std::invalid_argument("Unknown option: " + argument);
        } else if (!options.input) {
            options.input = utf8_path(argument);
        } else {
            throw std::invalid_argument("Unexpected extra input path: " + argument);
        }
    }

    if (!(options.minimum_confidence >= 0.0 && options.minimum_confidence <= 1.0)) {
        throw std::invalid_argument("Minimum confidence must be between 0 and 1.");
    }
    if (options.live_target_ms <= 0 || options.live_target_ms > 60000 ||
        options.live_stabilized_target_ms < options.live_target_ms ||
        options.live_stabilized_target_ms > 60000 || options.live_hop_ms < 10 || options.live_hop_ms > 1800)
        throw std::invalid_argument("Live targets require 0 < provisional <= stabilized <=60000 ms; hop 10..1800 ms.");
    if (!(options.quantization_strength >= 0.0 && options.quantization_strength <= 1.0) ||
        !(options.minimum_rhythm_evidence >= 0.0 && options.minimum_rhythm_evidence <= 1.0)) {
        throw std::invalid_argument("Quantization strength and minimum rhythm evidence must be between 0 and 1.");
    }
    if (options.fast_preview && !options.model.empty()) {
        throw std::invalid_argument(
            "--fast-preview currently applies only to the built-in analysis path.");
    }
    if (options.neural_decoder == transcribe::NeuralNoteDecoder::MultiHeadEvents &&
        (options.model.empty() || !options.polyphonic || (options.asio && !options.live_pipeline) ||
         (!options.backend.empty() && options.backend != "onnx:basic-pitch-v1"))) {
        throw std::invalid_argument(
            "--neural-decoder events requires --model, --polyphonic and Basic Pitch (ASIO also requires --live-pipeline)");
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
    if (options.asio && (!std::isfinite(options.capture_seconds) ||
                        options.capture_seconds <= 0.0 || options.capture_seconds > 300.0)) {
        throw std::invalid_argument("ASIO capture duration must be greater than 0 and at most 300 seconds.");
    }
    if (options.asio && (options.batch_directory || options.batch_manifest)) {
        throw std::invalid_argument("--asio cannot be combined with batch input.");
    }
    if (options.input && (options.asio || options.batch_directory || options.batch_manifest)) {
        throw std::invalid_argument("Choose exactly one input mode: a file, --asio, or batch input.");
    }
    if (options.batch_directory && options.batch_manifest) {
        throw std::invalid_argument("Choose either --batch or --batch-manifest, not both.");
    }
    if (options.concurrency < 1 || options.concurrency > 32 ||
        options.memory_limit_mb < 128 || options.memory_limit_mb > 1048576) {
        throw std::invalid_argument("--concurrency must be 1-32 and --memory-limit-mb 128-1048576.");
    }
    if ((options.batch_results || options.progress_jsonl || options.resume) &&
        !options.batch_directory && !options.batch_manifest) {
        throw std::invalid_argument("Batch result, progress and resume options require --batch or --batch-manifest.");
    }
    if (options.guitar_tab.capo_fret < 0 || options.guitar_tab.capo_fret > 24 ||
        options.guitar_tab.alternative_fingerings > 5) {
        throw std::invalid_argument("Capo must be 0-24; --tab-alternatives must be 0-5.");
    }
    if (options.guitar_tab.search.physical_fret_count) {
        if (*options.guitar_tab.search.physical_fret_count < options.guitar_tab.capo_fret ||
            *options.guitar_tab.search.physical_fret_count > 36) {
            throw std::invalid_argument("Physical fret count must be between the capo fret and 36.");
        }
        options.guitar_tab.maximum_fret =
            *options.guitar_tab.search.physical_fret_count - options.guitar_tab.capo_fret;
        options.guitar_tab.preferred_position =
            std::min(options.guitar_tab.preferred_position, options.guitar_tab.maximum_fret);
    }
    transcribe::assign_guitar_tab_fingerings({}, options.guitar_tab);
    if (options.preprocessing_fallback && options.isolated_audio.empty()) {
        throw std::invalid_argument("--preprocess-fallback requires --isolated-audio.");
    }
    if (!options.live_pipeline && (options.live_events || options.refine || options.proposal_json ||
                                  options.live_timing_configured)) {
        throw std::invalid_argument("Live events, refinement and live timing options require --live-pipeline.");
    }
    if ((options.proposal_json || options.refine_mode != "auto") && !options.refine) {
        throw std::invalid_argument("--proposal-json and --refine-mode require --refine.");
    }
    if (options.live_pipeline) {
        if (options.batch_directory || options.batch_manifest) {
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
    options.config_sources = std::move(config_paths);
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
    std::ostringstream output;
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
    transcribe::write_output_atomic(path, output.str());
}

const char* evidence_semantics(transcribe::EvidenceSemantics semantics) {
    using transcribe::EvidenceSemantics;
    switch (semantics) {
    case EvidenceSemantics::Unspecified: return "unspecified";
    case EvidenceSemantics::HeuristicScore: return "heuristic_score";
    case EvidenceSemantics::RelativeActivation: return "relative_activation";
    case EvidenceSemantics::RawActivation: return "raw_activation";
    case EvidenceSemantics::CalibratedProbability: return "calibrated_probability";
    }
    return "unspecified";
}

std::string engine_identifier(const Options& options) {
    if (!options.backend.empty()) return options.backend;
    if (!options.model.empty()) return "onnx:legacy";
    return options.polyphonic ? "builtin:spectral" : "builtin:yin";
}

void write_event_json(const std::filesystem::path& path,
                      const transcribe::ScoreDocument& score,
                      const std::filesystem::path& input,
                      const Options& options) {
    std::ostringstream output;
    output << std::setprecision(12)
           << "{\n  \"format\":\"transcriber-note-events\",\n"
           << "  \"schema_version\":1,\n"
           << "  \"timebase\":{\"seconds\":\"recording-relative seconds\","
              "\"quarters\":\"score quarters\",\"sample_frames\":\"zero-based source frames\"},\n"
           << "  \"source\":{\"path\":\"" << json_escape(path_utf8(input))
           << "\",\"sample_rate_hz\":" << score.source_sample_rate << "},\n"
           << "  \"instrument\":{\"name\":\"" << json_escape(score.instrument.name)
           << "\",\"written_transposition_semitones\":"
           << score.instrument.written_transposition_semitones << ",\"tuning_midi\":[";
    for (std::size_t i = 0; i < score.instrument.tuning.size(); ++i) {
        if (i) output << ',';
        output << score.instrument.tuning[i];
    }
    output << "],\"capo_fret\":" << score.instrument.capo_fret << "},\n"
           << "  \"analysis\":{\"engine_id\":\"" << json_escape(engine_identifier(options))
           << "\",\"model_id\":\"" << json_escape(options.model.empty()
                ? engine_identifier(options) : path_utf8(options.model.filename()))
           << "\",\"instrument_prior\":\""
           << (options.instrument == transcribe::InstrumentPrior::Guitar ? "guitar" :
               options.instrument == transcribe::InstrumentPrior::Bass ? "bass" :
               options.instrument == transcribe::InstrumentPrior::Piano ? "piano" :
               options.instrument == transcribe::InstrumentPrior::Voice ? "voice" : "none")
           << "\"},\n  \"notes\":[";
    for (std::size_t i = 0; i < score.notes.size(); ++i) {
        const auto& note = score.notes[i];
        if (i) output << ',';
        output << "\n    {\"id\":\"" << json_escape(note.id)
               << "\",\"midi_pitch\":" << note.sounding_midi_pitch
               << ",\"written_midi_pitch\":" << note.written_midi_pitch
               << ",\"onset_seconds\":" << note.measured_onset_seconds
               << ",\"offset_seconds\":" << note.measured_offset_seconds
               << ",\"duration_seconds\":"
               << note.measured_offset_seconds - note.measured_onset_seconds
               << ",\"onset_quarters\":" << note.onset_quarters
               << ",\"duration_quarters\":" << note.duration_quarters
               << ",\"voice\":" << note.voice << ",\"sample_rate_hz\":"
               << score.source_sample_rate << ",\"onset_frame\":";
        if (score.source_sample_rate)
            output << static_cast<std::uint64_t>(std::llround(
                note.measured_onset_seconds * score.source_sample_rate));
        else output << "null";
        output << ",\"offset_frame\":";
        if (score.source_sample_rate)
            output << static_cast<std::uint64_t>(std::llround(
                note.measured_offset_seconds * score.source_sample_rate));
        else output << "null";
        output << ",\"confidence\":{\"heuristic_score\":";
        if (note.raw_evidence.empty() || !std::isfinite(note.interpretation_evidence))
            output << "null";
        else output << note.interpretation_evidence;
        output << ",\"calibrated_probability\":";
        if (note.calibrated_confidence) output << *note.calibrated_confidence;
        else output << "null";
        output << ",\"semantics\":\""
               << (note.calibrated_confidence ? "calibrated_probability" :
                   note.raw_evidence.empty() ? "unspecified" : "heuristic_score")
               << "\"},\"provenance\":{\"origin\":\"inference\",\"method\":\""
               << json_escape(engine_identifier(options)) << "\",\"source_event_ids\":[";
        for (std::size_t e = 0; e < note.source_event_ids.size(); ++e) {
            if (e) output << ',';
            output << '"' << json_escape(note.source_event_ids[e]) << '"';
        }
        output << "],\"evidence_semantics\":[";
        for (std::size_t e = 0; e < note.raw_evidence.size(); ++e) {
            if (e) output << ',';
            output << '"' << evidence_semantics(note.raw_evidence[e].strength_semantics) << '"';
        }
        output << "]}}";
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("Failed while writing versioned note-event JSON.");
    transcribe::write_output_atomic(path, output.str());
}

void write_score_project(const std::filesystem::path& path,
                         const transcribe::ScoreDocument& score) {
    transcribe::write_output_atomic(path, transcribe::serialize_score_document(score) + '\n');
}

void write_native_project(const std::filesystem::path& path,
                          const std::filesystem::path& input,
                          const transcribe::ScoreDocument& score,
                          const Options& options) {
    transcribe::ProjectDocument project;
    project.id = score.id + "-project";
    project.title = path_utf8(input.stem());
    project.last_saved_path = path_utf8(path);
    project.audio.policy = transcribe::ProjectAudioPolicy::ExternalReference;
    project.audio.reference_path = path_utf8(std::filesystem::absolute(input));
    project.audio.sample_rate = score.source_sample_rate;
    project.audio.channels = 1;
    auto extension = path_utf8(input.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (extension == ".wav") {
        const auto audio = transcribe::inspect_wav(input);
        project.audio.sample_rate = audio.sample_rate;
        project.audio.channels = audio.channels;
        project.audio.frame_count = audio.frames;
    }
    project.score = score;
    project.analysis.backend_identifier = engine_identifier(options);
    project.analysis.model_identifier = options.model.empty()
        ? engine_identifier(options) : path_utf8(options.model.filename());
    project.analysis.model_reference = options.model.empty() ? "" : path_utf8(std::filesystem::absolute(options.model));
    project.analysis.instrument_prior =
        options.instrument == transcribe::InstrumentPrior::Guitar ? "guitar" :
        options.instrument == transcribe::InstrumentPrior::Bass ? "bass" :
        options.instrument == transcribe::InstrumentPrior::Piano ? "piano" :
        options.instrument == transcribe::InstrumentPrior::Voice ? "voice" : "auto";
    project.analysis.live_backend = options.backend.empty() ? "yin" : options.backend;
    project.analysis.provider = options.onnx_provider == transcribe::OnnxExecutionProvider::Cpu ? "cpu" :
        options.onnx_provider == transcribe::OnnxExecutionProvider::Cuda ? "cuda" :
        options.onnx_provider == transcribe::OnnxExecutionProvider::DirectML ? "directml" : "rocm";
    project.analysis.tempo_automatic = !options.tempo_bpm;
    project.analysis.meter_automatic = !options.meter;
    project.analysis.key_automatic = !options.key_fifths;
    project.analysis.polyphonic = options.polyphonic;
    project.analysis.fast_preview = options.fast_preview;
    project.analysis.preserve_expressive_timing = options.preserve_expressive_timing;
    project.analysis.quantization = options.quantization_mode == transcribe::QuantizationMode::Legacy ? "legacy" :
        options.quantization_mode == transcribe::QuantizationMode::EvidenceAware ? "evidence" : "expressive";
    project.analysis.quantization_strength = options.quantization_strength;
    project.analysis.minimum_rhythm_evidence = options.minimum_rhythm_evidence;
    project.analysis.maximum_fret = options.guitar_tab.maximum_fret;
    project.analysis.preferred_position = options.guitar_tab.preferred_position;
    project.analysis.fingering_alternatives = options.guitar_tab.alternative_fingerings;
    transcribe::save_project_file_atomic(path, project);
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
    case LiveEventKind::NoteFinalized: return "note_finalized";
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
           << (note.onset_truncated ? "true" : "false") << ", \"revision\": " << note.revision
           << ", \"finalized\": " << (note.finalized ? "true" : "false") << '}';
}

void write_proposal_json(const std::filesystem::path& path,
                         const transcribe::InferenceResult<transcribe::live::ScoreProposal>& proposal,
                         const transcribe::live::ProposalReview* review) {
    std::ostringstream output;
    output << std::setprecision(9);
    if (const auto* failure = std::get_if<transcribe::InferenceError>(&proposal)) {
        output << "{\"error\": \"" << json_escape(failure->message) << "\"}\n";
        transcribe::write_output_atomic(path, output.str());
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
    transcribe::write_output_atomic(path, output.str());
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
    std::optional<transcribe::WavInfo> wav;
    if (options.asio) {
        live_options.format = {44100, 1};
        live_options.lag_policy = live::LagPolicy::SkipAhead;
        live_options.audio_source_id = "asio";
        live_options.max_recording_seconds = std::max(600.0, options.capture_seconds + 1.0);
    } else {
        auto extension = path_utf8(input_path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".wav") wav = transcribe::inspect_wav(input_path);
        else audio = transcribe::read_audio(input_path);
        const auto rate = wav ? wav->sample_rate : audio.sample_rate;
        const auto count = wav ? wav->frames : audio.samples.size();
        live_options.format = {rate, 1};
        live_options.lag_policy = live::LagPolicy::Lossless;
        live_options.audio_source_id = "file:" + path_utf8(input_path.filename());
        live_options.max_recording_seconds = options.refine ? 600.0 : 0.0;
        audio_seconds = double(count) / rate;
        if (options.refine && audio_seconds > live_options.max_recording_seconds)
            throw std::invalid_argument(
                "Live refinement retains at most 600 seconds; process the full file offline for longer recordings.");
    }
    live_options.yin.realtime.latency_ms = options.realtime_latency_ms;
    live_options.target_provisional_seconds = options.live_target_ms / 1000;
    live_options.target_stabilized_seconds = options.live_stabilized_target_ms / 1000;
    live_options.window.hop_seconds = options.live_hop_ms / 1000;
    if (options.neural_decoder == transcribe::NeuralNoteDecoder::MultiHeadEvents)
        live_options.window.temporal_decoding = false;
    live_options.model_load = {options.onnx_provider, options.device_id, false};
    auto& request = live_options.window.request;
    request.analysis.polyphonic = options.polyphonic;
    request.analysis.minimum_confidence = options.minimum_confidence;
    request.analysis.instrument = options.instrument;
    request.analysis.spectral_decoder = options.competing_polyphony ?
        transcribe::SpectralDecoder::HarmonicCompetition : transcribe::SpectralDecoder::LegacySalience;
    request.onnx.polyphonic = options.polyphonic;
    request.onnx.independent_probabilities = options.polyphonic;
    request.onnx.minimum_confidence = options.minimum_confidence;
    request.onnx.execution_provider = options.onnx_provider;
    request.onnx.device_id = options.device_id;
    request.onnx.objective = options.model_objective;
    request.onnx.aligned_model_heads = options.competing_polyphony;
    request.onnx.note_decoder = options.neural_decoder;
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
        if (!events_output) throw std::runtime_error("Could not create live event output: " + path_utf8(*options.live_events));
        events_output.exceptions(std::ios::failbit | std::ios::badbit);
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
        if (batch.resync_required) document.resync(batch.resync_snapshot);
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
        session.begin_input(0);
        std::uint64_t sequence = 0;
        const auto feed = [&](const transcribe::AudioBlockView& source) {
            const live::AudioFrameBlock block{live_options.format, source.first_frame, source.samples,
                {}, false, sequence, live_options.stream_epoch};
            auto status = session.push(block);
            while (status == live::PushStatus::QueueFull) {
                consume();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                status = session.push(block);
            }
            if (status != live::PushStatus::Accepted) {
                throw std::runtime_error("Live pipeline rejected audio: " + session.last_error());
            }
            ++sequence;
            consume();
            if (!options.quiet && source.total_frames && source.first_frame % (block_frames * 64) == 0) {
                std::cerr << '\r' << progress_bar(double(source.first_frame) / source.total_frames) << std::flush;
            }
        };
        if (wav) transcribe::read_wav_blocks(input_path, block_frames, feed);
        else for (std::size_t offset = 0; offset < audio.samples.size(); offset += block_frames) {
            feed({audio.sample_rate, offset, audio.samples.size(),
                std::span<const float>(audio.samples).subspan(
                    offset, std::min(block_frames, audio.samples.size() - offset))});
        }
        session.end_input(wav ? wav->frames : audio.samples.size());
    }
    session.finish();
    consume();
    if (events_output.is_open()) events_output.close();
    if (session.state() == live::LiveState::AnalysisFailed) {
        throw std::runtime_error("Live analysis failed: " + session.last_error());
    }
    const auto recording = session.recording();
    if (options.asio) audio_seconds = double(session.ingested_frames()) / live_options.format.sample_rate;
    if (!options.quiet) {
        const auto metrics = session.metrics();
        std::cerr << "\n[live] end-to-end p50/p95 " << milliseconds(metrics.end_to_end_latency)
                  << "; p99 " << metrics.end_to_end_latency.p99_seconds * 1000 << " ms"
                  << "; stabilization " << milliseconds(metrics.stabilization_delay)
                  << "; analysis step " << milliseconds(metrics.analysis_latency)
                  << "; input queue high-water " << metrics.input_queue_high_water << '/'
                  << metrics.input_queue_capacity << "; skipped " << metrics.skipped_frames
                  << " frames; dropped events " << metrics.events_dropped << '\n';
        std::cerr << "[live] target misses provisional/stabilized "
                  << metrics.provisional_target_misses << '/' << metrics.stabilized_target_misses << '\n';
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

std::size_t process_input(const std::filesystem::path& input_path, const Options& options,
                   const std::filesystem::path& musicxml_path,
                   const std::filesystem::path& tab_path,
                   const std::optional<std::filesystem::path>& midi_path,
                   const std::optional<std::filesystem::path>& json_path,
                   const std::optional<std::filesystem::path>& score_project_path,
                   const std::optional<std::filesystem::path>& events_json_path,
                   const std::optional<std::filesystem::path>& project_path) {
    check_batch_cancel();
    std::vector<std::filesystem::path> outputs{musicxml_path, tab_path};
    for (const auto& optional : {midi_path, json_path, score_project_path, events_json_path, project_path,
                                options.live_events, options.proposal_json}) {
        if (optional) outputs.push_back(*optional);
    }
    auto protected_paths = options.config_sources;
    if (!options.isolated_audio.empty()) protected_paths.push_back(options.isolated_audio);
    if (!options.asio) protected_paths.push_back(input_path);
    if (!options.model.empty()) protected_paths.push_back(options.model);
    const auto aliases = [](const auto& left, const auto& right) {
        std::error_code error;
        if (std::filesystem::equivalent(left, right, error) && !error) return true;
        auto a = std::filesystem::weakly_canonical(left).native();
        auto b = std::filesystem::weakly_canonical(right).native();
#ifdef _WIN32
        std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return std::towlower(c); });
        std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return std::towlower(c); });
#endif
        return a == b;
    };
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const auto& path = outputs[i];
        if (path.empty()) throw std::invalid_argument("Output path must not be empty.");
        const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
        if (!std::filesystem::is_directory(parent) || std::filesystem::is_directory(path)) {
            throw std::invalid_argument("Output needs an existing directory and a file name: " + path_utf8(path));
        }
        for (const auto& source : protected_paths) {
            if (aliases(path, source)) {
                throw std::invalid_argument("Output path would overwrite an input, model or config file: " + path_utf8(path));
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (aliases(path, outputs[j])) {
                throw std::invalid_argument("Output paths collide: " + path_utf8(path));
            }
        }
    }
    const auto report_progress = [&options](double progress) {
        check_batch_cancel();
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
    if ((!options.isolated_audio.empty() || options.preprocessing_fallback) &&
        (options.asio || options.batch_directory)) {
        throw std::invalid_argument("Isolated audio preprocessing supports one file, not ASIO or batch input.");
    }
    const auto target = options.instrument == transcribe::InstrumentPrior::Guitar ? "guitar" :
        options.instrument == transcribe::InstrumentPrior::Bass ? "bass" :
        options.instrument == transcribe::InstrumentPrior::Piano ? "piano" :
        options.instrument == transcribe::InstrumentPrior::Voice ? "voice" : "";
    const auto prepared = transcribe::prepare_audio(
        {input_path, options.isolated_audio, target, options.preprocessing_fallback});
    if (!options.isolated_audio.empty()) std::cerr << "Preprocessing: " << prepared.diagnostic << '\n';
    const auto& analysis_path = prepared.analysis_path;
    auto extension = path_utf8(analysis_path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    std::uint32_t source_sample_rate = extension == ".wav"
        ? transcribe::inspect_wav(analysis_path).sample_rate : 0;
    const bool streaming_wav = !options.live_pipeline && !options.asio && options.model.empty() &&
        options.backend.empty() && extension == ".wav";
    if (options.bounded_inference && (options.backend.empty() || options.asio ||
        options.live_pipeline || extension != ".wav")) {
        throw std::invalid_argument("--bounded-inference requires WAV file input and an explicit --backend");
    }
    if (options.live_pipeline) {
        notes = run_live_pipeline(analysis_path, options, audio_seconds);
    } else if (streaming_wav) {
        transcribe::AnalysisOptions analysis_options;
        analysis_options.minimum_confidence = options.minimum_confidence;
        analysis_options.fast_preview = options.fast_preview;
        analysis_options.polyphonic = options.polyphonic;
        analysis_options.instrument = options.instrument;
        analysis_options.spectral_decoder = options.competing_polyphony ?
            transcribe::SpectralDecoder::HarmonicCompetition : transcribe::SpectralDecoder::LegacySalience;
        if (options.benchmark) {
            analysis_options.timing_callback = accumulate_timings;
        }
        analysis_options.progress_callback = report_progress;
        transcribe::StreamingOptions stream_options;
        stream_options.analysis = std::move(analysis_options);
        auto result = transcribe::transcribe_wav_streaming(analysis_path, stream_options);
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
            : options.bounded_inference ? transcribe::AudioBuffer{} : transcribe::read_audio(analysis_path);
        if (!options.bounded_inference) {
            audio_seconds = static_cast<double>(audio.samples.size()) / audio.sample_rate;
            source_sample_rate = audio.sample_rate;
        }
        check_batch_cancel();
        if (!options.backend.empty()) {
            transcribe::InferenceRequest request;
            request.analysis.polyphonic = options.polyphonic;
            request.analysis.minimum_confidence = options.minimum_confidence;
            request.analysis.instrument = options.instrument;
            request.analysis.spectral_decoder = options.competing_polyphony ?
                transcribe::SpectralDecoder::HarmonicCompetition : transcribe::SpectralDecoder::LegacySalience;
            if (options.benchmark) request.analysis.timing_callback = accumulate_timings;
            request.onnx.polyphonic = options.polyphonic;
            request.onnx.independent_probabilities = options.polyphonic;
            request.onnx.minimum_confidence = options.minimum_confidence;
            request.onnx.execution_provider = options.onnx_provider;
            request.onnx.device_id = options.device_id;
            request.onnx.objective = options.model_objective;
            request.onnx.aligned_model_heads = options.competing_polyphony;
            request.onnx.note_decoder = options.neural_decoder;
            request.progress = report_progress;
            if (options.bounded_inference) {
                transcribe::ModelRegistry registry(options.model);
                auto created = registry.create(options.backend);
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&created))
                    throw std::runtime_error(failure->message);
                auto instance = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
                if (!options.model.empty() && instance->metadata().definition.path.empty())
                    throw std::invalid_argument("--model cannot be combined with a built-in --backend");
                const auto loaded = instance->load({options.onnx_provider, options.device_id, options.fast_preview});
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&loaded))
                    throw std::runtime_error(failure->message);
                auto result = transcribe::infer_wav_file(*instance, analysis_path, request);
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&result)) {
                    throw std::runtime_error(failure->message);
                }
                auto& file = std::get<transcribe::FileInferenceResult>(result);
                notes = std::move(file.notes);
                audio_seconds = static_cast<double>(file.source_frames) / file.sample_rate;
            } else {
                transcribe::EngineRegistry registry(options.model);
                auto created = registry.create(options.backend);
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&created))
                    throw std::runtime_error(failure->message);
                auto instance = std::move(std::get<std::unique_ptr<transcribe::TranscriptionEngine>>(created));
                if (!options.model.empty() && instance->metadata().model.definition.path.empty())
                    throw std::invalid_argument("--model cannot be combined with a built-in --backend");
                const auto loaded = instance->initialize({options.onnx_provider, options.device_id, options.fast_preview});
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&loaded))
                    throw std::runtime_error(failure->message);
                transcribe::EngineConfiguration configuration;
                configuration.audio_source_id = path_utf8(input_path);
                configuration.inference = request;
                const auto configured = instance->configure(configuration);
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&configured))
                    throw std::runtime_error(failure->message);
                auto result = instance->transcribe(audio);
                if (const auto* failure = std::get_if<transcribe::InferenceError>(&result)) {
                    throw std::runtime_error(failure->message);
                }
                notes = std::move(std::get<transcribe::EngineUpdate>(result).notes);
            }
        } else if (options.model.empty()) {
            transcribe::AnalysisOptions analysis_options;
            analysis_options.minimum_confidence = options.minimum_confidence;
            analysis_options.fast_preview = options.fast_preview;
            analysis_options.polyphonic = options.polyphonic;
            analysis_options.instrument = options.instrument;
            analysis_options.spectral_decoder = options.competing_polyphony ?
                transcribe::SpectralDecoder::HarmonicCompetition : transcribe::SpectralDecoder::LegacySalience;
            if (options.benchmark) {
                analysis_options.timing_callback = accumulate_timings;
            }
            analysis_options.progress_callback = report_progress;
            notes = transcribe::transcribe_monophonic(audio, analysis_options);
        } else {
#ifdef TRANSCRIBE_ENABLE_ONNX
            transcribe::OnnxOptions model_options;
            model_options.polyphonic = options.polyphonic;
            model_options.independent_probabilities = options.polyphonic;
            model_options.minimum_confidence = options.minimum_confidence;
            model_options.execution_provider = options.onnx_provider;
            model_options.device_id = options.device_id;
            model_options.objective = options.model_objective;
            model_options.aligned_model_heads = options.competing_polyphony;
            model_options.note_decoder = options.neural_decoder;
            model_options.progress_callback = report_progress;
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
        throw std::runtime_error("No stable pitches were detected in " + path_utf8(input_path) + ".");
    }
    check_batch_cancel();

    transcribe::InterpretationOptions interpretation_options;
    interpretation_options.quantization_mode = options.quantization_mode;
    interpretation_options.quantization_strength = options.quantization_strength;
    interpretation_options.minimum_rhythm_evidence = options.minimum_rhythm_evidence;
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
    case transcribe::InstrumentPrior::Bass:
        interpretation_options.instrument = "Bass";
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
        interpretation, 0, path_utf8(input_path), metadata, options.guitar_tab);
    score.source_sample_rate = source_sample_rate;
    transcribe::verify_prepared_audio(prepared);
    if (!options.isolated_audio.empty()) {
        score.uncertainties.push_back(prepared.diagnostic + " Stem: " + path_utf8(options.isolated_audio));
    }
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
    check_batch_cancel();
    transcribe::write_guitar_tab(tab_path, score, options.guitar_tab);
    check_batch_cancel();
    if (midi_path) {
        transcribe::write_midi(*midi_path, score);
    }
    if (json_path) {
        write_json(*json_path, notes, metadata);
    }
    if (score_project_path) {
        write_score_project(*score_project_path, score);
    }
    if (events_json_path) write_event_json(*events_json_path, score, input_path, options);
    check_batch_cancel();
    if (project_path) write_native_project(*project_path, input_path, score, options);

    if (!options.quiet) {
        std::cout << path_utf8(input_path) << ": " << notes.size() << " notes\n"
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
        std::cout << '\n' << "  MusicXML: " << path_utf8(musicxml_path) << '\n'
                  << "  Guitar tab: " << path_utf8(tab_path) << '\n';
        if (midi_path) {
            std::cout << "  MIDI: " << path_utf8(*midi_path) << '\n';
        }
        if (json_path) {
            std::cout << "  JSON: " << path_utf8(*json_path) << '\n';
        }
        if (score_project_path) {
            std::cout << "  Score project: " << path_utf8(*score_project_path) << '\n';
        }
        if (events_json_path) std::cout << "  Note events: " << path_utf8(*events_json_path) << '\n';
        if (project_path) std::cout << "  Native project: " << path_utf8(*project_path) << '\n';
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
    return notes.size();
}

void process_single(const Options& options) {
    if (!options.asio && !options.input) {
        throw std::invalid_argument("Provide an input WAV path, use --asio, or use --batch.");
    }
    const auto input_path = options.asio ? std::filesystem::path("ASIO input")
                                         : *options.input;
    process_input(input_path, options, options.musicxml, options.tab, options.midi,
                  options.json, options.score_project, options.events_json, options.project);
}

using CsvRow = std::vector<std::string>;

std::vector<CsvRow> parse_csv(std::string_view text) {
    std::vector<CsvRow> rows;
    CsvRow row;
    std::string cell;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else quoted = false;
            } else cell.push_back(ch);
        } else if (ch == '"' && cell.empty()) {
            quoted = true;
        } else if (ch == ',') {
            row.push_back(std::move(cell));
            cell.clear();
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            row.push_back(std::move(cell));
            cell.clear();
            if (!(row.size() == 1 && row.front().empty())) rows.push_back(std::move(row));
            row.clear();
        } else {
            if (ch == '"')
                throw std::invalid_argument("Batch manifest contains a quote inside an unquoted CSV field.");
            cell.push_back(ch);
        }
    }
    if (quoted) throw std::invalid_argument("Batch manifest has an unterminated quoted CSV field.");
    if (!cell.empty() || !row.empty()) {
        row.push_back(std::move(cell));
        if (!(row.size() == 1 && row.front().empty())) rows.push_back(std::move(row));
    }
    return rows;
}

struct BatchJob {
    std::size_t index{};
    std::filesystem::path input;
    Options options;
    std::filesystem::path musicxml;
    std::filesystem::path tab;
    std::optional<std::filesystem::path> midi;
    std::optional<std::filesystem::path> json;
    std::optional<std::filesystem::path> score_project;
    std::optional<std::filesystem::path> events_json;
    std::optional<std::filesystem::path> project;
    std::uint64_t memory_estimate{};
    std::string key;
    std::string option_signature;
};

struct BatchFileResult {
    std::size_t index{};
    std::filesystem::path input;
    std::string status{"pending"};
    std::string error;
    std::uint64_t memory_estimate{};
    std::optional<std::size_t> note_count;
    std::vector<std::filesystem::path> outputs;
};

std::string hex_hash(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::filesystem::path absolute_normal(const std::filesystem::path& path) {
    std::error_code error;
    auto value = std::filesystem::absolute(path, error);
    if (error) throw std::filesystem::filesystem_error("Could not resolve batch path.", path, error);
    return value.lexically_normal();
}

std::filesystem::path manifest_path(const std::string& value,
                                    const std::filesystem::path& base) {
    auto path = utf8_path(value);
    return path.is_absolute() ? path.lexically_normal() : (base / path).lexically_normal();
}

std::filesystem::path default_output(const std::filesystem::path& directory,
                                     std::size_t index,
                                     const std::filesystem::path& input,
                                     std::string_view suffix) {
    std::ostringstream ordinal;
    ordinal << std::setw(6) << std::setfill('0') << index + 1;
    const auto name = ordinal.str() + "_" + path_utf8(input.stem()) + std::string(suffix);
    return directory / utf8_path(name);
}

std::uint64_t estimate_job_memory(const BatchJob& job) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(job.input, error);
    if (error) throw std::runtime_error("Cannot estimate input memory: " + error.message());
    auto extension = path_utf8(job.input.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool wav = extension == ".wav";
    const bool streaming = wav && job.options.model.empty() && job.options.backend.empty() &&
        !job.options.live_pipeline && !job.options.bounded_inference;
    const auto model_bytes = job.options.model.empty() ? std::uint64_t{0} :
        std::filesystem::file_size(job.options.model, error);
    if (!job.options.model.empty() && error)
        throw std::runtime_error("Cannot estimate model memory: " + error.message());
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    constexpr std::uint64_t base = 128ULL * mib;
    if (streaming) return base;
    std::uint64_t decoded = file_bytes;
    if (wav) {
        const auto info = transcribe::inspect_wav(job.input);
        decoded = static_cast<std::uint64_t>(info.frames) * info.channels * sizeof(float);
    } else {
        decoded = file_bytes > std::numeric_limits<std::uint64_t>::max() / 64
            ? std::numeric_limits<std::uint64_t>::max()
            : std::max<std::uint64_t>(file_bytes * 64, 256ULL * mib);
    }
    const auto multiplier = job.options.bounded_inference ? 2ULL : 8ULL;
    if (decoded > (std::numeric_limits<std::uint64_t>::max() - base) / multiplier)
        throw std::length_error("Estimated decoded input memory overflows.");
    const auto model = model_bytes > (std::numeric_limits<std::uint64_t>::max() - base) / 4
        ? std::numeric_limits<std::uint64_t>::max() : model_bytes * 4;
    return base + decoded * multiplier + model;
}

std::vector<std::filesystem::path> job_outputs(const BatchJob& job);

std::string journal_key(const BatchJob& job) {
    std::error_code error;
    const auto size = std::filesystem::file_size(job.input, error);
    if (error) return hex_hash(path_utf8(job.input) + ":missing");
    const auto modified = std::filesystem::last_write_time(job.input, error);
    const auto stamp = error ? std::int64_t{0} :
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            modified.time_since_epoch()).count();
    std::ostringstream identity;
    identity << std::setprecision(std::numeric_limits<double>::max_digits10)
             << TRANSCRIBE_VERSION << ':' << path_utf8(absolute_normal(job.input))
             << ':' << size << ':' << stamp
             << ':' << path_utf8(job.musicxml) << ':' << path_utf8(job.tab)
             << ':' << (job.options.backend) << ':' << path_utf8(job.options.model)
             << ':' << job.options.polyphonic << ':' << job.options.fast_preview
             << ':' << job.options.competing_polyphony
             << ':' << job.options.minimum_confidence << ':' << job.options.tempo_bpm.value_or(0)
             << ':' << static_cast<int>(job.options.instrument) << ':'
             << static_cast<int>(job.options.onnx_provider) << ':'
             << job.options.device_id << ':' << static_cast<int>(job.options.model_objective)
             << ':' << static_cast<int>(job.options.neural_decoder)
             << ':' << job.options.bounded_inference
             << ':' << job.options.minimum_rhythm_evidence << ':'
             << job.options.quantization_strength << ':'
             << static_cast<int>(job.options.quantization_mode) << ':'
             << job.options.preserve_expressive_timing << ':'
             << job.options.written_transposition_semitones << ':'
             << job.options.guitar_tab.capo_fret << ':'
             << job.options.guitar_tab.maximum_fret << ':'
             << job.options.guitar_tab.preferred_position << ':'
             << job.options.guitar_tab.position_shift_cost << ':'
             << job.options.guitar_tab.duration_proportional_ascii << ':'
             << job.options.guitar_tab.search.maximum_alternatives << ':'
             << job.options.guitar_tab.alternative_fingerings << ':'
             << job.options.guitar_tab.suggest_legato_marks << ':'
             << job.options.meter.has_value() << ':'
             << (job.options.meter ? (*job.options.meter)[0] : 0) << ':'
             << (job.options.meter ? (*job.options.meter)[1] : 0) << ':'
             << job.options.key_fifths.value_or(999) << ':'
             << job.options.minor_key.value_or(false) << ':'
             << job.options.pickup_quarters.value_or(0) << ':'
             << job.option_signature;
    if (!job.options.model.empty()) {
        const auto model_size = std::filesystem::file_size(job.options.model, error);
        const auto model_modified = std::filesystem::last_write_time(job.options.model, error);
        identity << ":model-size:" << model_size << ":model-time:"
                 << (error ? std::int64_t{0} :
                     std::chrono::duration_cast<std::chrono::nanoseconds>(
                         model_modified.time_since_epoch()).count());
    }
    for (const auto pitch : job.options.guitar_tab.tuning) identity << ":t" << pitch;
    for (const auto pitch : job.options.guitar_tab.string_tuning) identity << ":s" << pitch;
    const auto& search = job.options.guitar_tab.search;
    identity << ':' << search.capo_fret << ':' << search.maximum_fret
             << ':' << search.preferred_position << ':' << search.position_shift_cost
             << ':' << search.simultaneous_onset_tolerance_seconds
             << ':' << search.correction_match_tolerance_seconds
             << ':' << search.ambiguity_cost_margin
             << ':' << search.maximum_chord_fret_span
             << ':' << search.maximum_alternatives
             << ':' << search.fret_height_cost << ':' << search.preferred_position_cost
             << ':' << search.string_preference_cost << ':' << search.stretch_cost
             << ':' << search.repeated_string_cost << ':' << search.string_crossing_cost
             << ':' << search.awkward_transition_cost << ':' << search.unresolved_note_cost
             << ':' << search.maximum_fretting_fingers << ':' << search.beam_width
             << ':' << search.local_solution_limit << ':' << search.search_node_limit;
    for (const auto pitch : search.tuning) identity << ":st" << pitch;
    for (const auto pitch : search.string_tuning) identity << ":ss" << pitch;
    for (const auto& position : job.options.guitar_tab.position_overrides)
        identity << ":override" << position.note_index << ',' << position.string_number
                 << ',' << position.fret << ',' << position.manually_corrected;
    for (const auto& correction : job.options.guitar_tab.manual_corrections)
        identity << ":correction" << correction.midi_pitch << ',' << correction.onset_seconds
                 << ',' << correction.duration_seconds << ',' << correction.string_number
                 << ',' << correction.fret;
    for (const auto& position : search.position_overrides)
        identity << ":search-override" << position.note_index << ',' << position.string_number
                 << ',' << position.fret << ',' << position.manually_corrected;
    for (const auto& correction : search.manual_corrections)
        identity << ":search-correction" << correction.midi_pitch << ',' << correction.onset_seconds
                 << ',' << correction.duration_seconds << ',' << correction.string_number
                 << ',' << correction.fret;
    for (const auto& output : job_outputs(job)) identity << ':' << path_utf8(output);
    return hex_hash(identity.str());
}

// List helper kept local to the CLI batch layer.
std::vector<std::filesystem::path> job_outputs(const BatchJob& job) {
    std::vector<std::filesystem::path> paths{job.musicxml, job.tab};
    for (const auto& output : {job.midi, job.json, job.score_project, job.events_json, job.project})
        if (output) paths.push_back(*output);
    return paths;
}

const std::unordered_set<std::string>& manifest_option_keys() {
    static const std::unordered_set<std::string> keys{
        "instrument", "model", "backend", "provider", "device-id", "tempo",
        "key-fifths", "mode", "meter", "quality", "polyphonic",
        "minimum-confidence", "tuning", "capo", "string-tuning", "fret-count",
        "written-transposition", "quantization", "quantization-strength",
        "minimum-rhythm-evidence", "tab-alternatives"};
    return keys;
}

const std::unordered_set<std::string>& manifest_output_keys() {
    static const std::unordered_set<std::string> keys{
        "musicxml", "tab", "midi", "json", "events-json", "score-project", "project"};
    return keys;
}

std::vector<BatchJob> read_manifest_jobs(const Options& options,
                                        const std::filesystem::path& output_directory) {
    const auto path = absolute_normal(*options.batch_manifest);
    const auto size = std::filesystem::file_size(path);
    if (size == 0 || size > 8U * 1024U * 1024U)
        throw std::length_error("Batch manifest must be nonempty and no larger than 8 MiB.");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open batch manifest: " + path_utf8(path));
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Could not read the complete batch manifest.");
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    const auto records = parse_csv(bytes);
    if (records.size() < 2 || records.front().size() != 2 ||
        records.front()[0] != "transcriber-batch-manifest" || records.front()[1] != "1")
        throw std::invalid_argument("Batch manifest must begin with the CSV record transcriber-batch-manifest,1.");
    auto headers = records[1];
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        headers[i] = trim(headers[i]);
        if (headers[i].empty() || !columns.emplace(headers[i], i).second)
            throw std::invalid_argument("Batch manifest has an empty or duplicate column name.");
        if (headers[i] != "input" && !manifest_option_keys().contains(headers[i]) &&
            !manifest_output_keys().contains(headers[i]))
            throw std::invalid_argument("Unsupported batch manifest column: " + headers[i]);
    }
    if (!columns.contains("input"))
        throw std::invalid_argument("Batch manifest requires an input column.");
    if (records.size() > 100001)
        throw std::length_error("Batch manifest exceeds the 100,000-record limit.");
    const auto manifest_directory = path.parent_path();
    std::vector<BatchJob> jobs;
    for (std::size_t record = 2; record < records.size(); ++record) {
        if (records[record].size() != headers.size())
            throw std::invalid_argument("Batch manifest row " + std::to_string(record + 1) +
                " has " + std::to_string(records[record].size()) +
                " columns; expected " + std::to_string(headers.size()) + ".");
        const auto value = [&](std::string_view key) -> const std::string& {
            static const std::string empty;
            const auto found = columns.find(std::string(key));
            return found == columns.end() ? empty : records[record][found->second];
        };
        if (trim(value("input")).empty())
            throw std::invalid_argument("Batch manifest row " + std::to_string(record + 1) +
                " has no input path.");
        BatchJob job;
        job.index = jobs.size();
        job.input = absolute_normal(manifest_path(value("input"), manifest_directory));
        job.options = options;
        job.options.batch_directory.reset();
        job.options.batch_manifest.reset();
        job.options.quiet = true;
        std::map<std::string, std::string> config;
        for (const auto& key : manifest_option_keys()) {
            const auto it = columns.find(key);
            if (it != columns.end() && !trim(records[record][it->second]).empty())
                config.emplace(key, trim(records[record][it->second]));
        }
        if (const auto model = config.find("model"); model != config.end()) {
            auto model_path = utf8_path(model->second);
            if (model_path.is_relative()) model->second = path_utf8(manifest_directory / model_path);
        }
        apply_config(job.options, config);
        job.options.config_sources.push_back(path);
        std::ostringstream signature;
        for (const auto& header : headers)
            signature << header << '=' << records[record][columns.at(header)] << '\n';
        job.option_signature = signature.str();
        const auto output_path = [&](std::string_view key, std::string_view suffix,
                                     bool enabled) -> std::optional<std::filesystem::path> {
            const auto found = columns.find(std::string(key));
            if (found != columns.end() && !trim(records[record][found->second]).empty())
                return absolute_normal(manifest_path(trim(records[record][found->second]), manifest_directory));
            if (enabled) return default_output(output_directory, job.index, job.input, suffix);
            return std::nullopt;
        };
        job.musicxml = output_path("musicxml", ".musicxml", true).value();
        job.tab = output_path("tab", ".tab.txt", true).value();
        job.midi = output_path("midi", ".mid", options.midi.has_value());
        job.json = output_path("json", ".json", options.json.has_value());
        job.events_json = output_path("events-json", ".events.json", options.events_json.has_value());
        job.score_project = output_path("score-project", ".score.json", options.score_project.has_value());
        job.project = output_path("project", ".transcriber", options.project.has_value());
        if (!job.options.model.empty() && job.options.model.is_relative())
            job.options.model = absolute_normal(manifest_directory / job.options.model);
        if (job.options.asio || job.options.live_pipeline || job.options.refine ||
            !job.options.isolated_audio.empty() || job.options.proposal_json ||
            job.options.live_events)
            throw std::invalid_argument("Batch manifest entries cannot request capture, live, refinement or stem preprocessing.");
        jobs.push_back(std::move(job));
    }
    return jobs;
}

std::vector<BatchJob> directory_jobs(const Options& options,
                                     const std::filesystem::path& output_directory) {
    if (!std::filesystem::is_directory(*options.batch_directory))
        throw std::invalid_argument("Batch input is not a directory: " + path_utf8(*options.batch_directory));
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(*options.batch_directory)) {
        if (!entry.is_regular_file()) continue;
        auto extension = path_utf8(entry.path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#ifdef TRANSCRIBE_ENABLE_FFMPEG
        if (extension == ".wav" || extension == ".mp3" || extension == ".flac" ||
            extension == ".ogg" || extension == ".m4a" || extension == ".aac")
#else
        if (extension == ".wav")
#endif
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) throw std::runtime_error("No supported audio files were found in the batch directory.");
    std::vector<BatchJob> jobs;
    std::set<std::filesystem::path::string_type> stems;
    for (const auto& file : files) {
        auto stem = file.stem().native();
#ifdef _WIN32
        std::transform(stem.begin(), stem.end(), stem.begin(),
            [](wchar_t c) { return std::towlower(c); });
#endif
        if (!stems.insert(stem).second)
            throw std::invalid_argument("Batch files have colliding output stems. Use a manifest to assign distinct outputs.");
        BatchJob job;
        job.index = jobs.size();
        job.input = absolute_normal(file);
        job.options = options;
        job.options.quiet = true;
        const auto out = [&](std::string_view suffix) {
            return output_directory / (stem + std::filesystem::path(suffix).native());
        };
        job.musicxml = out(".musicxml");
        job.tab = out(".tab.txt");
        if (options.midi) job.midi = out(".mid");
        if (options.json) job.json = out(".json");
        if (options.events_json) job.events_json = out(".events.json");
        if (options.score_project) job.score_project = out(".score.json");
        if (options.project) job.project = out(".transcriber");
        jobs.push_back(std::move(job));
    }
    return jobs;
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code error;
    if (std::filesystem::equivalent(a, b, error) && !error) return true;
    auto left = std::filesystem::weakly_canonical(a, error).native();
    if (error) left = absolute_normal(a).native();
    auto right = std::filesystem::weakly_canonical(b, error).native();
    if (error) right = absolute_normal(b).native();
#ifdef _WIN32
    const auto lower = [](auto c) { return static_cast<decltype(c)>(std::towlower(c)); };
    std::transform(left.begin(), left.end(), left.begin(), lower);
    std::transform(right.begin(), right.end(), right.begin(), lower);
#endif
    return left == right;
}

class BatchEventWriter {
public:
    explicit BatchEventWriter(const std::optional<std::filesystem::path>& path) {
        if (!path) return;
        const auto parent = path->has_parent_path() ? path->parent_path() : std::filesystem::current_path();
        std::filesystem::create_directories(parent);
        stream_.open(*path, std::ios::binary | std::ios::app);
        if (!stream_) throw std::runtime_error("Could not open progress JSONL: " + path_utf8(*path));
    }
    void write(std::string_view type, std::size_t index, std::size_t completed,
               std::size_t total, const std::filesystem::path& input,
               std::string_view status = {}, std::string_view error = {}) {
        if (!stream_.is_open()) return;
        std::lock_guard lock(mutex_);
        stream_ << "{\"schema_version\":1,\"type\":\"" << type
                << "\",\"index\":" << index << ",\"completed\":" << completed
                << ",\"total\":" << total << ",\"input\":\""
                << json_escape(path_utf8(input)) << "\",\"status\":\"" << status
                << "\",\"error\":\"" << json_escape(std::string(error)) << "\"}\n";
        stream_.flush();
        if (!stream_) throw std::runtime_error("Could not append batch progress event.");
    }
private:
    std::ofstream stream_;
    std::mutex mutex_;
};

void append_journal(const std::filesystem::path& path, std::string_view key,
                    std::string_view status, std::mutex& mutex) {
    std::lock_guard lock(mutex);
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("Could not open resumable batch journal: " + path_utf8(path));
    output << key << '\t' << status << '\n';
    output.flush();
    if (!output) throw std::runtime_error("Could not flush resumable batch journal.");
}

std::unordered_map<std::string, std::string> read_journal(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> completed;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return completed;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 64U * 1024U * 1024U)
        throw std::length_error("Resume journal is unreadable or exceeds 64 MiB.");
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('\t');
        if (separator != std::string::npos)
            completed.insert_or_assign(line.substr(0, separator), line.substr(separator + 1));
    }
    if (input.bad()) throw std::runtime_error("Could not read resume journal.");
    return completed;
}

std::string batch_summary_json(const std::vector<BatchFileResult>& results,
                               std::size_t concurrency, std::size_t memory_limit_mb) {
    std::size_t succeeded = 0, failed = 0, skipped = 0, cancelled = 0;
    for (const auto& item : results) {
        if (item.status == "success") ++succeeded;
        else if (item.status == "failed") ++failed;
        else if (item.status == "skipped") ++skipped;
        else if (item.status == "cancelled") ++cancelled;
    }
    std::ostringstream output;
    output << "{\n  \"format\":\"transcriber-batch-results\",\n"
           << "  \"schema_version\":1,\n  \"summary\":{\"total\":" << results.size()
           << ",\"succeeded\":" << succeeded << ",\"failed\":" << failed
           << ",\"skipped\":" << skipped << ",\"cancelled\":" << cancelled
           << ",\"concurrency\":" << concurrency << ",\"memory_limit_mb\":"
           << memory_limit_mb << "},\n  \"files\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& item = results[i];
        if (i) output << ',';
        output << "\n    {\"index\":" << item.index + 1 << ",\"input\":\""
               << json_escape(path_utf8(item.input)) << "\",\"status\":\""
               << item.status << "\",\"error\":\"" << json_escape(item.error)
               << "\",\"estimated_memory_bytes\":" << item.memory_estimate
               << ",\"note_count\":";
        if (item.note_count) output << *item.note_count;
        else output << "null";
        output << ",\"outputs\":[";
        for (std::size_t j = 0; j < item.outputs.size(); ++j) {
            if (j) output << ',';
            output << '"' << json_escape(path_utf8(item.outputs[j])) << '"';
        }
        output << "]}";
    }
    output << "\n  ]\n}\n";
    return output.str();
}

int process_batch(const Options& options) {
    if (options.isolated_audio.empty() == false || options.preprocessing_fallback)
        throw std::invalid_argument("Audio stem preprocessing is not supported for batch jobs.");
    const auto output_directory = options.output_directory.empty()
        ? options.batch_directory ? *options.batch_directory / "transcriber-output"
            : options.batch_manifest->parent_path() / "transcriber-output"
        : options.output_directory;
    std::filesystem::create_directories(output_directory);
    auto jobs = options.batch_manifest
        ? read_manifest_jobs(options, output_directory)
        : directory_jobs(options, output_directory);
    if (jobs.empty()) throw std::runtime_error("Batch manifest contains no recording rows.");
    const auto results_path = options.batch_results.value_or(output_directory / "batch-results.json");
    auto journal_path = results_path;
    journal_path += std::filesystem::path(".journal");
    const auto results_parent = results_path.has_parent_path()
        ? results_path.parent_path() : std::filesystem::current_path();
    std::filesystem::create_directories(results_parent);
    const auto progress_path = options.progress_jsonl;
    const auto summary_paths = [&] {
        std::vector<std::filesystem::path> values{results_path, journal_path};
        if (progress_path) values.push_back(*progress_path);
        return values;
    }();
    std::vector<BatchFileResult> results(jobs.size());
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        auto& job = jobs[i];
        job.key = journal_key(job);
        results[i].index = i;
        results[i].input = job.input;
        results[i].outputs = job_outputs(job);
        for (const auto& output : results[i].outputs)
            std::filesystem::create_directories(output.has_parent_path()
                ? output.parent_path() : std::filesystem::current_path());
        try {
            job.memory_estimate = estimate_job_memory(job);
            results[i].memory_estimate = job.memory_estimate;
            const auto limit = static_cast<std::uint64_t>(options.memory_limit_mb) * 1024 * 1024;
            if (job.memory_estimate > limit) {
                results[i].status = "failed";
                results[i].error = "Estimated job memory exceeds --memory-limit-mb admission budget.";
            }
        } catch (const std::exception& error) {
            results[i].status = "failed";
            results[i].error = error.what();
        }
    }
    std::vector<std::filesystem::path> inputs;
    inputs.reserve(jobs.size());
    for (const auto& job : jobs) inputs.push_back(job.input);
    std::vector<std::filesystem::path> protected_paths = options.config_sources;
    protected_paths.insert(protected_paths.end(), inputs.begin(), inputs.end());
    if (options.batch_manifest) protected_paths.push_back(*options.batch_manifest);
    if (!options.model.empty()) protected_paths.push_back(options.model);
    protected_paths.insert(protected_paths.end(), summary_paths.begin(), summary_paths.end());
    std::vector<std::filesystem::path> all_outputs;
    for (const auto& result : results) {
        for (const auto& output : result.outputs) {
            for (const auto& protected_path : protected_paths) {
                if (same_path(output, protected_path))
                    throw std::invalid_argument("Batch output would overwrite an input, model, config or batch journal: " +
                        path_utf8(output));
            }
            for (const auto& prior : all_outputs) {
                if (same_path(output, prior))
                    throw std::invalid_argument("Batch output paths collide: " + path_utf8(output));
            }
            all_outputs.push_back(output);
        }
    }
    for (const auto& summary : summary_paths) {
        for (const auto& input : inputs)
            if (same_path(summary, input))
                throw std::invalid_argument("Batch result/progress path would overwrite an input: " + path_utf8(summary));
        for (const auto& output : all_outputs)
            if (same_path(summary, output))
                throw std::invalid_argument("Batch result/progress path collides with an export: " + path_utf8(summary));
    }
    auto previous_status = options.resume ? read_journal(journal_path)
                                          : std::unordered_map<std::string, std::string>{};
    if (options.resume) {
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (results[i].status != "pending" ||
                !previous_status.contains(jobs[i].key) ||
                previous_status.at(jobs[i].key) != "success") continue;
            if (std::all_of(results[i].outputs.begin(), results[i].outputs.end(),
                [](const auto& path) { return std::filesystem::is_regular_file(path); }))
                results[i].status = "skipped";
        }
    }
    BatchEventWriter events(progress_path);
    events.write("batch_started", 0, 0, jobs.size(), {}, "running");
    std::size_t completed = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        if (result.status == "pending") continue;
        ++completed;
        events.write(result.status == "failed" ? "file_failed" : "file_skipped",
            i + 1, completed, jobs.size(), result.input, result.status, result.error);
    }
    const auto prior_handler = std::signal(SIGINT, request_batch_cancel);
    batch_cancel_requested = 0;
    std::mutex schedule_mutex;
    std::condition_variable schedule_changed;
    std::mutex journal_mutex;
    std::size_t active = 0;
    std::uint64_t reserved_memory = 0;
    const auto memory_limit = static_cast<std::uint64_t>(options.memory_limit_mb) * 1024 * 1024;
    std::vector<std::thread> workers;
    const auto worker_count = std::min(options.concurrency, jobs.size());
    try {
        for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
            workers.emplace_back([&] {
            for (;;) {
                std::size_t selected = jobs.size();
                {
                    std::unique_lock lock(schedule_mutex);
                    for (;;) {
                        if (batch_cancel_requested || completed == jobs.size()) return;
                        for (std::size_t i = 0; i < jobs.size(); ++i) {
                            if (results[i].status != "pending") continue;
                            if (jobs[i].memory_estimate <= memory_limit - reserved_memory) {
                                selected = i;
                                break;
                            }
                        }
                        if (selected < jobs.size()) {
                            reserved_memory += jobs[selected].memory_estimate;
                            ++active;
                            results[selected].status = "running";
                            break;
                        }
                        schedule_changed.wait(lock);
                    }
                }
                auto& result = results[selected];
                std::string file_status = "success";
                std::string file_error;
                try {
                    std::size_t completed_snapshot;
                    { std::lock_guard lock(schedule_mutex); completed_snapshot = completed; }
                    events.write("file_started", selected + 1, completed_snapshot, jobs.size(),
                        jobs[selected].input, "running");
                    result.note_count = process_input(jobs[selected].input, jobs[selected].options,
                        jobs[selected].musicxml, jobs[selected].tab, jobs[selected].midi,
                        jobs[selected].json, jobs[selected].score_project,
                        jobs[selected].events_json, jobs[selected].project);
                    check_batch_cancel();
                    for (const auto& output : result.outputs)
                        if (!std::filesystem::is_regular_file(output))
                            throw std::runtime_error("Exporter returned without creating expected output: " + path_utf8(output));
                    file_status = "success";
                } catch (const BatchCancelled& error) {
                    file_status = "cancelled";
                    file_error = error.what();
                } catch (const std::exception& error) {
                    file_status = "failed";
                    file_error = error.what();
                }
                try {
                    append_journal(journal_path, jobs[selected].key, file_status, journal_mutex);
                } catch (const std::exception& error) {
                    file_status = "failed";
                    if (!file_error.empty()) file_error += "; ";
                    file_error += error.what();
                }
                std::size_t completed_snapshot;
                {
                    std::lock_guard lock(schedule_mutex);
                    result.status = file_status;
                    result.error = file_error;
                    reserved_memory -= jobs[selected].memory_estimate;
                    --active;
                    ++completed;
                    completed_snapshot = completed;
                }
                schedule_changed.notify_all();
                try {
                    events.write(file_status == "success" ? "file_succeeded" :
                        file_status == "cancelled" ? "file_cancelled" : "file_failed",
                        selected + 1, completed_snapshot, jobs.size(), jobs[selected].input,
                        file_status, file_error);
                } catch (const std::exception& error) {
                    std::lock_guard lock(schedule_mutex);
                    result.status = "failed";
                    if (!result.error.empty()) result.error += "; ";
                    result.error += error.what();
                    try {
                        append_journal(journal_path, jobs[selected].key, "failed", journal_mutex);
                    } catch (const std::exception& journal_error) {
                        result.error += "; could not record progress failure: ";
                        result.error += journal_error.what();
                    }
                }
            }
            });
        }
    } catch (...) {
        batch_cancel_requested = 1;
        schedule_changed.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        std::signal(SIGINT, prior_handler);
        batch_cancel_requested = 0;
        throw;
    }
    for (auto& worker : workers) worker.join();
    const bool interrupted = batch_cancel_requested != 0;
    if (interrupted) {
        for (auto& result : results) if (result.status == "pending") result.status = "cancelled";
    }
    const auto summary = batch_summary_json(results, options.concurrency, options.memory_limit_mb);
    transcribe::write_output_atomic(results_path, summary);
    std::size_t succeeded = 0, failed = 0, skipped = 0, cancelled = 0;
    for (const auto& result : results) {
        if (result.status == "success") ++succeeded;
        else if (result.status == "failed") {
            ++failed;
            std::cerr << path_utf8(result.input) << ": " << result.error << '\n';
        } else if (result.status == "skipped") ++skipped;
        else if (result.status == "cancelled") ++cancelled;
    }
    events.write(batch_cancel_requested ? "batch_cancelled" : "batch_finished",
        0, results.size(), results.size(), {}, batch_cancel_requested ? "cancelled" : "complete");
    if (!options.quiet) {
        std::cout << "Batch: " << succeeded << " succeeded, " << failed << " failed, "
                  << skipped << " resumed, " << cancelled << " cancelled; results: "
                  << path_utf8(results_path) << '\n';
    }
    if (failed)
        std::cerr << failed << " of " << results.size() << " files failed.\n";
    std::signal(SIGINT, prior_handler);
    batch_cancel_requested = 0;
    if (interrupted || cancelled) return 130;
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
#ifdef _WIN32
        int count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) throw std::runtime_error("Could not read the Windows Unicode command line.");
        std::vector<std::string> arguments;
        try {
            for (int i = 0; i < count; ++i) {
                const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                                    nullptr, 0, nullptr, nullptr);
                if (!size) throw std::runtime_error("Invalid Unicode in command line argument.");
                std::string text(static_cast<std::size_t>(size), '\0');
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1,
                                    text.data(), size, nullptr, nullptr);
                text.pop_back();
                arguments.push_back(std::move(text));
            }
        } catch (...) { LocalFree(wide); throw; }
        LocalFree(wide);
        std::vector<char*> pointers;
        for (auto& argument : arguments) pointers.push_back(argument.data());
        argc = count;
        argv = pointers.data();
#endif
        const auto options = parse_options(argc, argv);
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        if (options.batch_directory || options.batch_manifest) {
            return process_batch(options);
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
