#include "transcribe/audio.hpp"
#include "transcribe/asio_capture.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout << "Usage:\n"
                 "  transcribe <input.wav> [--model model.onnx] [--musicxml output.musicxml] [--tab output.txt]\n"
                 "  transcribe --asio <seconds> [--model model.onnx] [--musicxml output.musicxml] [--tab output.txt]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    try {
        bool use_asio = std::string(argv[1]) == "--asio";
        int option_start = 2;
#ifdef TRANSCRIBE_ENABLE_ASIO
        double capture_seconds = 0.0;
#endif
        if (use_asio) {
#ifndef TRANSCRIBE_ENABLE_ASIO
            throw std::runtime_error("ASIO support is disabled. Configure with -DTRANSCRIBE_ENABLE_ASIO=ON.");
#else
            if (argc < 3) {
                print_usage();
                return 2;
            }
            capture_seconds = std::stod(argv[2]);
            if (capture_seconds <= 0.0 || capture_seconds > 300.0) {
                throw std::invalid_argument("ASIO capture duration must be greater than 0 and at most 300 seconds.");
            }
            option_start = 3;
#endif
        }

        std::filesystem::path musicxml_path = "transcription.musicxml";
        std::filesystem::path tab_path = "transcription.txt";
        std::filesystem::path model_path;
        for (int index = option_start; index < argc; ++index) {
            const std::string option = argv[index];
            if (index + 1 >= argc) {
                throw std::invalid_argument("Missing output path after " + option);
            }
            if (option == "--musicxml") {
                musicxml_path = argv[++index];
            } else if (option == "--tab") {
                tab_path = argv[++index];
            } else if (option == "--model") {
                model_path = argv[++index];
            } else {
                throw std::invalid_argument("Unknown option: " + option);
            }
        }

#ifndef TRANSCRIBE_ENABLE_ONNX
        if (!model_path.empty()) {
            throw std::runtime_error("ONNX Runtime support is disabled. Configure with -DTRANSCRIBE_ENABLE_ONNX=ON.");
        }
#endif

        transcribe::AudioBuffer audio;
        if (use_asio) {
#ifdef TRANSCRIBE_ENABLE_ASIO
            audio = transcribe::capture_asio(capture_seconds);
#endif
        } else {
            audio = transcribe::read_wav(argv[1]);
        }
        const auto notes = model_path.empty()
                               ? transcribe::transcribe_monophonic(audio)
#ifdef TRANSCRIBE_ENABLE_ONNX
                               : transcribe::transcribe_with_onnx(audio, model_path);
#else
                               : std::vector<transcribe::Note>{};
#endif
        if (notes.empty()) {
            throw std::runtime_error("No stable pitches were detected in the input.");
        }
        transcribe::write_musicxml(musicxml_path, notes);
        transcribe::write_guitar_tab(tab_path, notes);
        std::cout << "Detected " << notes.size() << " notes.\n"
                  << "MusicXML: " << musicxml_path.string() << '\n'
                  << "Guitar tab: " << tab_path.string() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "transcribe: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
