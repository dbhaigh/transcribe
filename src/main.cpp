#include "transcribe/asio_capture.hpp"
#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
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
    double capture_seconds{};
    double minimum_confidence{0.25};
    bool asio{};
    bool polyphonic{};
    bool benchmark{};
    bool quiet{};
    bool help{};
};

void print_usage(std::ostream& output) {
    output << "Transcribe 0.1.3 - turn audio into MusicXML, MIDI, and guitar tablature\n\n"
              "Usage:\n"
              "  transcribe <input.wav> [options]\n"
              "  transcribe --asio <seconds> [options]\n"
              "  transcribe --batch <directory> [options]\n\n"
              "Options:\n"
              "  -h, --help                  Show this help\n"
              "  --musicxml <path>           MusicXML output path\n"
              "  --tab <path>                Guitar tablature output path\n"
              "  --midi <path>               Optional Standard MIDI output path\n"
              "  --json <path>               Write notes and analysis metadata as JSON\n"
              "  --model <path.onnx>         Use an ONNX model instead of built-in YIN\n"
              "  --polyphonic                Peak-pick independent ONNX pitch activations\n"
              "  --minimum-confidence <0..1> Drop notes below this confidence\n"
              "  --benchmark                 Report analysis time and real-time factor\n"
              "  --config <path>             Load defaults from simple key=value config\n"
              "  --output-dir <directory>    Batch output directory (default: <input>/transcribe-output)\n"
              "  --quiet                     Suppress progress and per-file summaries\n\n"
              "Examples:\n"
              "  transcribe guitar.wav --musicxml score.musicxml --tab guitar.txt --midi take.mid\n"
              "  transcribe guitar.wav --model poly.onnx --polyphonic --json notes.json\n"
              "  transcribe --batch samples --output-dir results --benchmark\n"
              "  transcribe --asio 8 --musicxml live.musicxml --tab live.txt\n";
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
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
    const auto set_path = [&values](const char* key, auto& destination) {
        const auto found = values.find(key);
        if (found != values.end()) {
            destination = found->second;
        }
    };
    set_path("input", options.input);
    set_path("musicxml", options.musicxml);
    set_path("tab", options.tab);
    set_path("midi", options.midi);
    set_path("json", options.json);
    set_path("model", options.model);
    const auto confidence = values.find("minimum-confidence");
    if (confidence != values.end()) {
        options.minimum_confidence = std::stod(confidence->second);
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
    std::optional<std::filesystem::path> config_path;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            config_path = next_value(i, argc, argv, "--config");
            break;
        }
    }
    if (config_path) {
        apply_config(options, read_config(*config_path));
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
        } else if (argument == "--minimum-confidence") {
            options.minimum_confidence = std::stod(next_value(i, argc, argv, argument));
        } else if (argument == "--polyphonic") {
            options.polyphonic = true;
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else if (argument == "--batch") {
            options.batch_directory = next_value(i, argc, argv, argument);
        } else if (argument == "--output-dir") {
            options.output_directory = next_value(i, argc, argv, argument);
        } else if (argument == "--asio") {
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
    if (options.polyphonic && options.model.empty()) {
        throw std::invalid_argument("--polyphonic requires --model.");
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
        if (!options.quiet) {
            analysis_options.progress_callback = report_progress;
        }
        transcribe::StreamingOptions stream_options;
        stream_options.analysis = std::move(analysis_options);
        auto result = transcribe::transcribe_wav_streaming(input_path, stream_options);
        notes = std::move(result.notes);
        audio_seconds = result.duration_seconds;
    } else {
        transcribe::AudioBuffer audio = options.asio
                                            ? transcribe::capture_asio(options.capture_seconds)
                                            : transcribe::read_audio(input_path);
        audio_seconds = static_cast<double>(audio.samples.size()) / audio.sample_rate;
        if (options.model.empty()) {
            transcribe::AnalysisOptions analysis_options;
            analysis_options.minimum_confidence = options.minimum_confidence;
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

    const auto metadata = transcribe::infer_score_metadata(notes);
    transcribe::write_musicxml(musicxml_path, notes, metadata);
    transcribe::write_guitar_tab(tab_path, notes);
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
            std::cout << "  Analysis: " << analysis_seconds << " s; real-time factor "
                      << analysis_seconds / std::max(audio_seconds, 1e-9) << "x\n";
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
                                      ? *options.batch_directory / "transcribe-output"
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
        std::cerr << "transcribe: " << error.what() << "\n\n";
        print_usage(std::cerr);
        return 1;
    }
    return 0;
}
