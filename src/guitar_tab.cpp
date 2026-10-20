#include "transcribe/guitar_tab.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace transcribe {
namespace {

struct FretPosition {
    int string;
    int fret;
    double cost{};
};

struct Fingering {
    double cost{};
    std::vector<FretPosition> positions;
};

struct TabEvent {
    double start{};
    double duration{};
    std::array<std::string, 6> strings;
    std::vector<std::size_t> note_indices;
    std::vector<std::vector<FretPosition>> alternatives;
};

std::vector<FretPosition> candidates_for(int midi_pitch, const GuitarTabOptions& options) {
    std::vector<FretPosition> candidates;
    for (std::size_t string = 0; string < options.tuning.size(); ++string) {
        const int fret = midi_pitch - options.tuning[string] - options.capo_fret;
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

std::vector<Fingering> assign_group(const std::vector<const Note*>& group,
                                    double previous_position,
                                    const GuitarTabOptions& options,
                                    std::size_t maximum_solutions,
                                    const std::vector<std::array<bool, 6>>&
                                        blocked_strings = {},
                                    const std::vector<std::optional<FretPosition>>&
                                        fixed_positions = {}) {
    std::vector<FretPosition> current(group.size(), {-1, -1, 0.0});
    std::vector<bool> occupied(options.tuning.size(), false);
    std::vector<Fingering> solutions;
    const std::size_t assignable = std::min(group.size(), options.tuning.size());

    const auto search = [&](auto&& self, std::size_t index, double cost) -> void {
        if (index == assignable) {
            int minimum_fret = options.maximum_fret;
            int maximum_fret = 0;
            for (std::size_t i = 0; i < assignable; ++i) {
                minimum_fret = std::min(minimum_fret, current[i].fret);
                maximum_fret = std::max(maximum_fret, current[i].fret);
            }
            cost += std::max(0, maximum_fret - minimum_fret - 4) * 1.5;
            solutions.push_back({cost, current});
            std::stable_sort(solutions.begin(), solutions.end(),
                             [](const Fingering& left, const Fingering& right) {
                                 return left.cost < right.cost;
                             });
            if (solutions.size() > maximum_solutions) {
                solutions.pop_back();
            }
            return;
        }
        auto candidates = candidates_for(group[index]->midi_pitch, options);
        if (index < fixed_positions.size() && fixed_positions[index]) {
            candidates = {*fixed_positions[index]};
        }
        for (const auto& candidate : candidates) {
            const auto string = static_cast<std::size_t>(candidate.string);
            if (occupied[string] ||
                (index < blocked_strings.size() &&
                 blocked_strings[index][string])) {
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

    if (solutions.empty()) {
        Fingering fallback;
        fallback.positions.resize(group.size(), {-1, -1, 0.0});
        std::vector<bool> fallback_occupied(options.tuning.size(), false);
        for (std::size_t i = 0; i < assignable; ++i) {
            auto candidates = candidates_for(group[i]->midi_pitch, options);
            if (i < fixed_positions.size() && fixed_positions[i]) {
                candidates = {*fixed_positions[i]};
            }
            for (const auto& candidate : candidates) {
                const auto string = static_cast<std::size_t>(candidate.string);
                if (fallback_occupied[string] ||
                    (i < blocked_strings.size() && blocked_strings[i][string])) {
                    continue;
                }
                fallback.positions[i] = candidate;
                fallback_occupied[string] = true;
                break;
            }
        }
        solutions.push_back(std::move(fallback));
    }
    return solutions;
}

void validate_options(const GuitarTabOptions& options) {
    if (options.maximum_fret < 0 || options.maximum_fret > 36) {
        throw std::invalid_argument("Maximum fret must be between 0 and 36.");
    }
    if (options.preferred_position < 0 ||
        options.preferred_position > options.maximum_fret) {
        throw std::invalid_argument("Preferred position must be within the allowed fret range.");
    }
    if (!std::isfinite(options.position_shift_cost) || options.position_shift_cost < 0.0) {
        throw std::invalid_argument("Position shift cost must be finite and non-negative.");
    }
    if (options.capo_fret < 0 || options.capo_fret > 24) {
        throw std::invalid_argument("Capo fret must be between 0 and 24.");
    }
    if (options.alternative_fingerings > 5) {
        throw std::invalid_argument("At most five alternative fingerings can be requested.");
    }
    for (std::size_t i = 0; i < options.tuning.size(); ++i) {
        if (options.tuning[i] < 0 || options.tuning[i] > 127 ||
            (i > 0 && options.tuning[i] >= options.tuning[i - 1])) {
            throw std::invalid_argument(
                "Guitar tuning must contain six descending MIDI pitches from high to low.");
        }
    }
}

std::vector<std::vector<std::size_t>> note_groups(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ordered_indices) {
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t first = 0; first < ordered_indices.size();) {
        std::size_t end = first + 1;
        while (end < ordered_indices.size() &&
               notes[ordered_indices[end]].start_seconds -
                       notes[ordered_indices[first]].start_seconds <=
                   0.03) {
            ++end;
        }
        groups.emplace_back(ordered_indices.begin() + static_cast<std::ptrdiff_t>(first),
                            ordered_indices.begin() + static_cast<std::ptrdiff_t>(end));
        first = end;
    }
    return groups;
}

} // namespace

std::vector<GuitarTabPosition> assign_guitar_tab_positions(
    const std::vector<Note>& notes, const GuitarTabOptions& options) {
    validate_options(options);
    std::vector<std::size_t> ordered_indices(notes.size());
    for (std::size_t i = 0; i < ordered_indices.size(); ++i) {
        ordered_indices[i] = i;
    }
    std::stable_sort(ordered_indices.begin(), ordered_indices.end(),
                     [&notes](std::size_t left, std::size_t right) {
                         return notes[left].start_seconds < notes[right].start_seconds;
                     });

    std::vector<GuitarTabPosition> result(notes.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].note_index = i;
        result[i].string_number = 0;
        result[i].fret = -1;
    }
    std::vector<std::optional<FretPosition>> fixed_positions(notes.size());
    for (const auto& position : options.position_overrides) {
        if (position.note_index >= notes.size() ||
            fixed_positions[position.note_index] ||
            position.string_number < 1 ||
            position.string_number > static_cast<int>(options.tuning.size()) ||
            position.fret < 0 || position.fret > options.maximum_fret) {
            throw std::invalid_argument("A guitar tab position override is invalid.");
        }
        const auto string_index = static_cast<std::size_t>(position.string_number - 1);
        if (notes[position.note_index].midi_pitch !=
            options.tuning[string_index] + options.capo_fret + position.fret) {
            throw std::invalid_argument(
                "A guitar tab position override does not match its note pitch.");
        }
        fixed_positions[position.note_index] =
            FretPosition{static_cast<int>(string_index), position.fret, 0.0};
    }
    for (std::size_t left = 0; left < notes.size(); ++left) {
        if (!fixed_positions[left]) {
            continue;
        }
        const double left_end =
            notes[left].start_seconds + notes[left].duration_seconds;
        for (std::size_t right = left + 1; right < notes.size(); ++right) {
            if (!fixed_positions[right] ||
                fixed_positions[left]->string != fixed_positions[right]->string) {
                continue;
            }
            const double right_end =
                notes[right].start_seconds + notes[right].duration_seconds;
            if (notes[left].start_seconds < right_end &&
                notes[right].start_seconds < left_end) {
                throw std::invalid_argument(
                    "Overlapping notes cannot use the same overridden guitar string.");
            }
        }
    }

    struct ActivePosition {
        double end_seconds{};
        int string{};
    };
    std::vector<ActivePosition> active_positions;
    double previous_position = options.preferred_position;
    for (const auto& group_indices : note_groups(notes, ordered_indices)) {
        const double group_start = notes[group_indices.front()].start_seconds;
        active_positions.erase(
            std::remove_if(active_positions.begin(), active_positions.end(),
                           [group_start](const ActivePosition& active) {
                               return active.end_seconds <= group_start;
                           }),
            active_positions.end());
        std::vector<const Note*> group;
        std::vector<std::array<bool, 6>> blocked_strings(
            group_indices.size(), std::array<bool, 6>{});
        std::vector<std::optional<FretPosition>> group_fixed_positions;
        group.reserve(group_indices.size());
        group_fixed_positions.reserve(group_indices.size());
        for (std::size_t index = 0; index < group_indices.size(); ++index) {
            const auto note_index = group_indices[index];
            for (const auto& active : active_positions) {
                if (active.end_seconds > notes[note_index].start_seconds) {
                    blocked_strings[index][static_cast<std::size_t>(active.string)] = true;
                }
            }
            if (fixed_positions[note_index] &&
                blocked_strings[index][static_cast<std::size_t>(
                    fixed_positions[note_index]->string)]) {
                throw std::invalid_argument(
                    "An overridden guitar string is occupied by an overlapping note.");
            }
            group_fixed_positions.push_back(fixed_positions[note_index]);
            group.push_back(&notes[note_index]);
        }
        const auto solutions = assign_group(group, previous_position, options, 1,
                                            blocked_strings,
                                            group_fixed_positions);
        const auto& positions = solutions.front().positions;
        for (std::size_t i = 0; i < group_indices.size(); ++i) {
            const auto& position = positions[i];
            result[group_indices[i]].string_number =
                position.string < 0 ? 0 : position.string + 1;
            result[group_indices[i]].fret = position.fret;
            if (position.fret >= 0) {
                previous_position = (previous_position + position.fret) * 0.5;
                const auto& note = notes[group_indices[i]];
                if (note.duration_seconds > 0.0) {
                    active_positions.push_back(
                        {note.start_seconds + note.duration_seconds,
                         position.string});
                }
            }
        }
    }
    return result;
}

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes) {
    write_guitar_tab(path, notes, GuitarTabOptions{});
}

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& requested_options) {
    validate_options(requested_options);
    const auto assigned_positions =
        assign_guitar_tab_positions(notes, requested_options);

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create tablature output: " + path.string());
    }

    std::vector<std::size_t> ordered_indices(notes.size());
    for (std::size_t i = 0; i < ordered_indices.size(); ++i) {
        ordered_indices[i] = i;
    }
    std::stable_sort(ordered_indices.begin(), ordered_indices.end(),
                     [&notes](std::size_t left, std::size_t right) {
                         return notes[left].start_seconds < notes[right].start_seconds;
                     });

    std::vector<TabEvent> events;
    double previous_position = requested_options.preferred_position;
    for (const auto& group_indices : note_groups(notes, ordered_indices)) {
        std::vector<const Note*> group;
        group.reserve(group_indices.size());
        for (const auto index : group_indices) {
            group.push_back(&notes[index]);
        }
        const auto solutions = assign_group(
            group, previous_position, requested_options,
            requested_options.alternative_fingerings + 1);
        TabEvent event;
        event.start = group.front()->start_seconds;
        event.note_indices = group_indices;
        for (std::size_t alternative = 1; alternative < solutions.size(); ++alternative) {
            event.alternatives.push_back(solutions[alternative].positions);
        }
        for (std::size_t i = 0; i < group.size(); ++i) {
            const auto& position = assigned_positions[group_indices[i]];
            event.duration = std::max(event.duration, group[i]->duration_seconds);
            if (position.string_number < 1) {
                continue;
            }
            const auto string = static_cast<std::size_t>(position.string_number - 1);
            event.strings[string] =
                std::to_string(position.fret);
            previous_position = (previous_position + position.fret) * 0.5;
        }
        events.push_back(std::move(event));
    }

    for (std::size_t event = 1; event < events.size(); ++event) {
        const double gap = events[event].start - events[event - 1].start;
        if (gap < 0.12) {
            for (std::size_t string = 0; string < requested_options.tuning.size(); ++string) {
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
        lines[string] << midi_note_name(requested_options.tuning[string]) << '|';
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

    output << "Estimated guitar tablature (high-to-low tuning: ";
    for (std::size_t string = requested_options.tuning.size(); string > 0; --string) {
        output << midi_note_name(requested_options.tuning[string - 1])
               << (string == 1 ? "" : "-");
    }
    output << ")\n";
    if (requested_options.capo_fret > 0) {
        output << "Capo: fret " << requested_options.capo_fret << '\n';
    }
    output << "Fingering, legato marks, and rhythm are approximate.\n";
    for (auto& line : lines) {
        output << line.str() << "|\n";
    }
    output << "\nNotes:\n";
    for (const auto index : ordered_indices) {
        const auto* note = &notes[index];
        output << std::fixed << std::setprecision(2) << note->start_seconds << "s  "
               << note->name << "  " << note->duration_seconds << "s  confidence "
               << std::setprecision(2) << note->confidence << '\n';
    }
    if (requested_options.alternative_fingerings > 0) {
        output << "\nAlternative fingerings:\n";
        for (const auto& event : events) {
            for (std::size_t alternative = 0;
                 alternative < event.alternatives.size(); ++alternative) {
                output << std::fixed << std::setprecision(2) << event.start << "s option "
                       << alternative + 1 << ':';
                for (std::size_t i = 0; i < event.note_indices.size(); ++i) {
                    const auto& position = event.alternatives[alternative][i];
                    output << ' ' << notes[event.note_indices[i]].name << '=';
                    if (position.string < 0) {
                        output << "unplayable";
                    } else {
                        output << "string " << position.string + 1 << " fret "
                               << position.fret;
                    }
                }
                output << '\n';
            }
        }
    }
    if (!output) {
        throw std::runtime_error("Failed while writing tablature output.");
    }
}

} // namespace transcribe
