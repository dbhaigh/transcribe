#include "transcribe/guitar_tab.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace transcribe {
namespace {

constexpr std::array<const char*, 6> string_labels{"e", "B", "G", "D", "A", "E"};
constexpr std::array<int, 6> open_strings{64, 59, 55, 50, 45, 40};

struct FretPosition {
    int string;
    int fret;
    double cost{};
};

struct TabEvent {
    double start{};
    double duration{};
    std::array<std::string, 6> strings;
};

std::vector<FretPosition> candidates_for(int midi_pitch, const GuitarTabOptions& options) {
    std::vector<FretPosition> candidates;
    for (std::size_t string = 0; string < open_strings.size(); ++string) {
        const int fret = midi_pitch - open_strings[string];
        if (fret >= 0 && fret <= options.maximum_fret) {
            candidates.push_back({static_cast<int>(string), fret});
        }
    }
    return candidates;
}

double position_cost(const FretPosition& candidate, double previous_position,
                     const GuitarTabOptions& options) {
    const double preferred_distance =
        std::abs(candidate.fret - options.preferred_position);
    const double shift = std::abs(candidate.fret - previous_position);
    return 0.12 * candidate.fret + 0.75 * preferred_distance +
           options.position_shift_cost * shift + 0.1 * candidate.string;
}

std::vector<FretPosition> assign_group(const std::vector<const Note*>& group,
                                      double previous_position,
                                      const GuitarTabOptions& options) {
    std::vector<FretPosition> current(group.size());
    std::vector<FretPosition> best(group.size());
    std::vector<bool> occupied(open_strings.size(), false);
    double best_cost = std::numeric_limits<double>::infinity();
    const std::size_t assignable = std::min(group.size(), open_strings.size());

    const auto search = [&](auto&& self, std::size_t index, double cost) -> void {
        if (index == assignable) {
            if (cost < best_cost) {
                best_cost = cost;
                best = current;
            }
            return;
        }
        const auto candidates = candidates_for(group[index]->midi_pitch, options);
        for (const auto& candidate : candidates) {
            if (occupied[static_cast<std::size_t>(candidate.string)]) {
                continue;
            }
            const auto scored = FretPosition{
                candidate.string, candidate.fret,
                position_cost(candidate, previous_position, options)};
            occupied[static_cast<std::size_t>(candidate.string)] = true;
            current[index] = scored;
            self(self, index + 1, cost + scored.cost);
            occupied[static_cast<std::size_t>(candidate.string)] = false;
        }
    };
    search(search, 0, 0.0);

    if (!std::isfinite(best_cost)) {
        for (std::size_t i = 0; i < assignable; ++i) {
            const auto candidates = candidates_for(group[i]->midi_pitch, options);
            if (!candidates.empty()) {
                best[i] = candidates.front();
            } else {
                best[i] = {-1, -1, 0.0};
            }
        }
    }
    for (std::size_t i = assignable; i < group.size(); ++i) {
        best[i] = {-1, -1, 0.0};
    }
    return best;
}

} // namespace

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes) {
    write_guitar_tab(path, notes, GuitarTabOptions{});
}

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& requested_options) {
    if (requested_options.maximum_fret < 0 || requested_options.maximum_fret > 36) {
        throw std::invalid_argument("Maximum fret must be between 0 and 36.");
    }
    if (requested_options.preferred_position < 0 ||
        requested_options.preferred_position > requested_options.maximum_fret) {
        throw std::invalid_argument("Preferred position must be within the allowed fret range.");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create tablature output: " + path.string());
    }

    std::vector<const Note*> ordered;
    ordered.reserve(notes.size());
    for (const auto& note : notes) {
        ordered.push_back(&note);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Note* left, const Note* right) {
                         return left->start_seconds < right->start_seconds;
                     });

    std::vector<TabEvent> events;
    double previous_position = requested_options.preferred_position;
    std::size_t first = 0;
    while (first < ordered.size()) {
        std::size_t end = first + 1;
        while (end < ordered.size() &&
               ordered[end]->start_seconds - ordered[first]->start_seconds <= 0.03) {
            ++end;
        }
        std::vector<const Note*> group(ordered.begin() + static_cast<std::ptrdiff_t>(first),
                                       ordered.begin() + static_cast<std::ptrdiff_t>(end));
        const auto assignments = assign_group(group, previous_position, requested_options);
        TabEvent event;
        event.start = group.front()->start_seconds;
        for (std::size_t i = 0; i < group.size(); ++i) {
            const auto& position = assignments[i];
            event.duration = std::max(event.duration, group[i]->duration_seconds);
            if (position.string < 0) {
                continue;
            }
            event.strings[static_cast<std::size_t>(position.string)] =
                std::to_string(position.fret);
            previous_position = (previous_position + position.fret) * 0.5;
        }
        events.push_back(std::move(event));
        first = end;
    }

    for (std::size_t event = 1; event < events.size(); ++event) {
        const double gap = events[event].start - events[event - 1].start;
        if (gap < 0.12) {
            for (std::size_t string = 0; string < open_strings.size(); ++string) {
                auto& previous = events[event - 1].strings[string];
                const auto& current = events[event].strings[string];
                if (previous.empty() || current.empty()) {
                    continue;
                }
                const int previous_fret = std::stoi(previous);
                const int current_fret = std::stoi(current);
                const int distance = std::abs(current_fret - previous_fret);
                if (distance == 0) {
                    continue;
                }
                if (distance <= 3) {
                    previous += current_fret > previous_fret ? "h" : "p";
                } else {
                    previous += current_fret > previous_fret ? "/" : "\\";
                }
            }
        }
    }

    std::array<std::ostringstream, 6> lines;
    for (std::size_t string = 0; string < lines.size(); ++string) {
        lines[string] << string_labels[string] << '|';
    }
    for (const auto& event : events) {
        int width = 4;
        for (const auto& value : event.strings) {
            width = std::max(width, static_cast<int>(value.size()) + 2);
        }
        if (requested_options.duration_proportional_ascii) {
            width = std::clamp(static_cast<int>(std::lround(event.duration * 8.0)),
                               width, 24);
        }
        for (std::size_t string = 0; string < lines.size(); ++string) {
            const auto& value = event.strings[string];
            if (value.empty()) {
                lines[string] << std::string(static_cast<std::size_t>(width), '-');
                continue;
            }
            const int padding = std::max(0, width - static_cast<int>(value.size()));
            const int left = padding / 2;
            lines[string] << std::string(static_cast<std::size_t>(left), '-') << value
                          << std::string(static_cast<std::size_t>(padding - left), '-');
        }
    }

    output << "Estimated guitar tablature (standard E-A-D-G-B-E tuning)\n"
              "Fingering, legato marks, and rhythm are approximate.\n";
    for (auto& line : lines) {
        output << line.str() << "|\n";
    }
    output << "\nNotes:\n";
    for (const auto* note : ordered) {
        output << std::fixed << std::setprecision(2) << note->start_seconds << "s  "
               << note->name << "  " << note->duration_seconds << "s  confidence "
               << std::setprecision(2) << note->confidence << '\n';
    }
    if (!output) {
        throw std::runtime_error("Failed while writing tablature output.");
    }
}

} // namespace transcribe
