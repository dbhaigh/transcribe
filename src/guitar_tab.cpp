#include "transcribe/guitar_tab.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace transcribe {

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create tablature output: " + path.string());
    }

    static constexpr std::array<const char*, 6> labels{"e", "B", "G", "D", "A", "E"};
    static constexpr std::array<int, 6> open_strings{64, 59, 55, 50, 45, 40};
    std::array<std::ostringstream, 6> lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        lines[i] << labels[i] << '|';
    }

    for (const auto& note : notes) {
        int selected_string = -1;
        int fret = 0;
        for (std::size_t string = 0; string < open_strings.size(); ++string) {
            const int candidate = note.midi_pitch - open_strings[string];
            if (candidate >= 0 && candidate <= 24) {
                selected_string = static_cast<int>(string);
                fret = candidate;
                break;
            }
        }

        std::ostringstream token;
        if (selected_string < 0) {
            token << 'x';
        } else {
            token << fret;
        }
        const std::string value = token.str();
        const int width = std::max(4, static_cast<int>(value.size()) + 2);
        for (std::size_t string = 0; string < lines.size(); ++string) {
            if (static_cast<int>(string) == selected_string) {
                const int left_padding = (width - static_cast<int>(value.size())) / 2;
                lines[string] << std::string(static_cast<std::size_t>(left_padding), '-')
                              << value
                              << std::string(static_cast<std::size_t>(width -
                                  left_padding - static_cast<int>(value.size())), '-');
            } else {
                lines[string] << std::string(static_cast<std::size_t>(width), '-');
            }
        }
    }

    output << "Estimated monophonic guitar tablature (standard E-A-D-G-B-E tuning)\n"
              "Timing and fingering are approximate; notes are shown left to right.\n";
    for (auto& line : lines) {
        output << line.str() << "|\n";
    }
    output << "\nNotes:\n";
    for (const auto& note : notes) {
        output << std::fixed << std::setprecision(2) << note.start_seconds << "s  "
               << note.name << "  " << note.duration_seconds << "s\n";
    }
    if (!output) {
        throw std::runtime_error("Failed while writing tablature output.");
    }
}

} // namespace transcribe
